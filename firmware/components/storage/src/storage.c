#include "storage.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pinmap.h"

static const char *TAG = "storage";

#ifndef STORAGE_MOUNT_POINT
#define STORAGE_MOUNT_POINT "/userdata"
#endif
#define EXT_PARTITION_LABEL "ext_userdata"
#define EXT_PARTITION_SIZE  (16 * 1024 * 1024)
#define MOUNT_POINT         STORAGE_MOUNT_POINT

static esp_flash_t *s_ext_flash = NULL;
static const esp_partition_t *s_ext_partition = NULL;
static bool s_mounted = false;
static storage_backend_t s_backend = STORAGE_BACKEND_NONE;

static esp_err_t external_nor_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_STORAGE_MOSI,
        .miso_io_num = PIN_STORAGE_MISO,
        .sclk_io_num = PIN_STORAGE_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_flash_spi_device_config_t dev = {
        .host_id = SPI2_HOST,
        .cs_io_num = PIN_STORAGE_CS,
        .io_mode = SPI_FLASH_FASTRD,
        .freq_mhz = 40,
        .input_delay_ns = 0,
    };
    err = spi_bus_add_flash_device(&s_ext_flash, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add flash device failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_flash_init(s_ext_flash);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t chip_size = 0;
    (void)esp_flash_get_size(s_ext_flash, &chip_size);
    ESP_LOGI(TAG, "External NOR detected: size=%lu KiB", (unsigned long)(chip_size / 1024));

    /* Never register more than the chip physically has: on a smaller device
     * (or when the size probe fails) a fixed 16 MiB window would run past the
     * end of the flash and corrupt the filesystem. */
    uint32_t part_size = EXT_PARTITION_SIZE;
    if (chip_size == 0U || chip_size < (1024U * 1024U)) {
        ESP_LOGW(TAG, "External NOR size unusable (%lu KiB): sound features disabled",
                 (unsigned long)(chip_size / 1024));
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (chip_size < part_size) {
        part_size = chip_size;
    }

    const esp_partition_t *part = NULL;
    err = esp_partition_register_external(
        s_ext_flash, 0, part_size, EXT_PARTITION_LABEL,
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &part);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "external partition registration failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ext_partition = part;
    return ESP_OK;
}

static esp_err_t mount_littlefs_ext(bool format_on_fail)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition = s_ext_partition,
        .format_if_mount_failed = format_on_fail,
    };
    return esp_vfs_littlefs_register(&conf);
}

esp_err_t storage_init(void)
{
    esp_err_t err = external_nor_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "External NOR init failed: sound features disabled");
        s_backend = STORAGE_BACKEND_NONE;
    } else {
        s_backend = STORAGE_BACKEND_EXTERNAL_NOR;
    }
    return ESP_OK;
}

esp_err_t storage_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    if (s_backend != STORAGE_BACKEND_EXTERNAL_NOR || s_ext_partition == NULL) {
        ESP_LOGW(TAG, "External NOR not available: sound features disabled");
        return ESP_ERR_NOT_FOUND;
    }

    /* Never auto-format the external chip: a transient or format-version
     * mismatch on mount would silently erase every sound file. Formatting
     * happens only explicitly in storage_format() (provisioning). If the mount
     * fails the device keeps running, just without sound. */
    esp_err_t err = mount_littlefs_ext(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "External NOR LittleFS mount failed (%s); data NOT formatted "
                      "(run provisioning to reformat)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Mounted external NOR user data at %s", MOUNT_POINT);
    s_mounted = true;
    return ESP_OK;
}

storage_backend_t storage_get_backend(void)
{
    return s_backend;
}

bool storage_is_mounted(void)
{
    return s_mounted;
}

bool storage_ext_available(void)
{
    return s_ext_flash != NULL && s_ext_partition != NULL;
}

const char *storage_get_root(void)
{
    return MOUNT_POINT;
}

/* ------------------------------------------------------------------ */
/* External NOR format                                                 */
/* ------------------------------------------------------------------ */

esp_err_t storage_format(void)
{
    if (s_ext_partition == NULL) {
        ESP_LOGW(TAG, "format: external NOR not available");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Unmount the external NOR so it can be reformatted and remounted. */
    if (s_mounted) {
        (void)esp_vfs_littlefs_unregister_partition(s_ext_partition);
        s_mounted = false;
    }

    ESP_LOGI(TAG, "Formatting external NOR (LittleFS)...");
    esp_err_t err = esp_littlefs_format_partition(s_ext_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "External NOR format failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mount_littlefs_ext(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Remount after format failed: %s", esp_err_to_name(err));
        return err;
    }
    s_backend = STORAGE_BACKEND_EXTERNAL_NOR;
    s_mounted = true;
    ESP_LOGI(TAG, "External NOR formatted and remounted");
    return ESP_OK;
}

esp_err_t storage_get_free_bytes(uint64_t *out_free_bytes)
{
    if (out_free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = 0;
    size_t used = 0;
    esp_err_t err = esp_littlefs_partition_info(s_ext_partition, &total, &used);
    if (err != ESP_OK) {
        return err;
    }
    *out_free_bytes = (uint64_t)(total > used ? total - used : 0);
    return ESP_OK;
}
