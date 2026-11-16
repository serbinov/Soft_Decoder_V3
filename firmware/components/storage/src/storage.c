#include "storage.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
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
static portMUX_TYPE s_lifecycle_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_leases;
static TaskHandle_t s_maintenance_owner;
static bool s_lifecycle_busy;

esp_err_t storage_access_begin(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_mounted && s_maintenance_owner == NULL && !s_lifecycle_busy && s_leases < UINT32_MAX) {
        ++s_leases;
        err = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return err;
}

void storage_access_end(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_leases > 0U) --s_leases;
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

esp_err_t storage_maintenance_begin(void)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_lifecycle_mux);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_maintenance_owner == NULL && !s_lifecycle_busy) {
        s_maintenance_owner = caller;
        err = ESP_OK;
    }
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return err;
}

void storage_maintenance_end(void)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_lifecycle_mux);
    if (s_maintenance_owner == caller && !s_lifecycle_busy) s_maintenance_owner = NULL;
    portEXIT_CRITICAL(&s_lifecycle_mux);
}

bool storage_is_quiescent(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    bool quiet = s_leases == 0U && !s_lifecycle_busy;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return quiet;
}

/* Boards ship with different 16 MiB-class SPI NOR chips (Winbond, Zetta,
 * GigaDevice, ...). ESP-IDF selects a chip-specific driver when it knows the
 * JEDEC ID and otherwise falls back to its generic driver, but the bus speed
 * and read command still have to work for the specific part. Try the common
 * combinations from fastest to most conservative so a slower or quirky chip
 * still mounts instead of silently disabling the sound library. */
typedef struct {
    int freq_mhz;
    esp_flash_io_mode_t io_mode;
    const char *label;
} nor_profile_t;

static const nor_profile_t NOR_PROFILES[] = {
    { 40, SPI_FLASH_FASTRD, "fast-read" },
    { 40, SPI_FLASH_SLOWRD, "slow-read" },
    { 20, SPI_FLASH_FASTRD, "fast-read" },
    { 20, SPI_FLASH_SLOWRD, "slow-read" },
    { 10, SPI_FLASH_SLOWRD, "slow-read" },
};

static void nor_log_chip(const nor_profile_t *profile, uint32_t size)
{
    uint32_t id = 0;
    if (esp_flash_read_id(s_ext_flash, &id) == ESP_OK) {
        ESP_LOGI(TAG,
                 "External NOR: JEDEC %02X %02X %02X, %lu KiB (%d MHz %s)",
                 (unsigned)((id >> 16) & 0xFFU), (unsigned)((id >> 8) & 0xFFU),
                 (unsigned)(id & 0xFFU), (unsigned long)(size / 1024),
                 profile->freq_mhz, profile->label);
    } else {
        ESP_LOGW(TAG, "External NOR: %lu KiB (%d MHz %s), JEDEC ID read failed",
                 (unsigned long)(size / 1024), profile->freq_mhz, profile->label);
    }
}

/* Returns ESP_OK and fills s_ext_flash/s_size, or an error after cleaning up
 * the half-initialised bus device so the next profile can be tried. */
static esp_err_t external_nor_try(const nor_profile_t *profile, uint32_t *out_size)
{
    esp_flash_spi_device_config_t dev = {
        .host_id = SPI2_HOST,
        .cs_io_num = PIN_STORAGE_CS,
        .io_mode = profile->io_mode,
        .freq_mhz = profile->freq_mhz,
        .input_delay_ns = 0,
    };
    esp_flash_t *chip = NULL;
    esp_err_t err = spi_bus_add_flash_device(&chip, &dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add flash device failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_flash_init(chip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "External NOR init at %d MHz %s failed: %s",
                 profile->freq_mhz, profile->label, esp_err_to_name(err));
        (void)spi_bus_remove_flash_device(chip);
        return err;
    }

    uint32_t chip_size = 0;
    (void)esp_flash_get_size(chip, &chip_size);
    /* Reject a probe that cannot address at least a sound library. A zero or
     * absurd size means the chip answered but its ID/geometry was not usable. */
    if (chip_size < (1024U * 1024U) || chip_size > (64U * 1024U * 1024U)) {
        ESP_LOGW(TAG, "External NOR size unusable at %d MHz %s (%lu KiB)",
                 profile->freq_mhz, profile->label, (unsigned long)(chip_size / 1024));
        (void)spi_bus_remove_flash_device(chip);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_ext_flash = chip;
    *out_size = chip_size;
    nor_log_chip(profile, chip_size);
    return ESP_OK;
}

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
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t chip_size = 0;
    err = ESP_ERR_NOT_SUPPORTED;
    for (size_t i = 0; i < sizeof(NOR_PROFILES) / sizeof(NOR_PROFILES[0]); ++i) {
        err = external_nor_try(&NOR_PROFILES[i], &chip_size);
        if (err == ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no usable external NOR profile: sound features disabled");
        return err;
    }

    /* Never register more than the chip physically has: on a smaller device
     * a fixed 16 MiB window would run past the end of the flash and corrupt
     * the filesystem. Bigger chips are capped at the library partition size. */
    uint32_t part_size = EXT_PARTITION_SIZE;
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
    ESP_LOGI(TAG, "External NOR ready: %lu KiB partition at offset 0",
             (unsigned long)(part_size / 1024));
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

static esp_err_t storage_mount_owned(void)
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
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_mounted = true;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return ESP_OK;
}

esp_err_t storage_mount(void)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_lifecycle_mux);
    bool allowed = !s_lifecycle_busy && (s_maintenance_owner == NULL || s_maintenance_owner == caller);
    if (allowed) s_lifecycle_busy = true;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    esp_err_t err = storage_mount_owned();
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_lifecycle_busy = false;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return err;
}

storage_backend_t storage_get_backend(void)
{
    return s_backend;
}

bool storage_is_mounted(void)
{
    portENTER_CRITICAL(&s_lifecycle_mux);
    bool mounted = s_mounted;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return mounted;
}

const char *storage_get_root(void)
{
    return MOUNT_POINT;
}

/* ------------------------------------------------------------------ */
/* External NOR format                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t storage_format_owned(void)
{
    if (s_ext_partition == NULL) {
        ESP_LOGW(TAG, "format: external NOR not available");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Unmount the external NOR so it can be reformatted and remounted. */
    if (s_mounted) {
        esp_err_t err = esp_vfs_littlefs_unregister_partition(s_ext_partition);
        if (err != ESP_OK) return err;
        portENTER_CRITICAL(&s_lifecycle_mux);
        s_mounted = false;
        portEXIT_CRITICAL(&s_lifecycle_mux);
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
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_mounted = true;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    ESP_LOGI(TAG, "External NOR formatted and remounted");
    return ESP_OK;
}

esp_err_t storage_format(void)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_lifecycle_mux);
    bool allowed = s_maintenance_owner == caller && caller != NULL &&
                   s_leases == 0U && !s_lifecycle_busy;
    if (allowed) s_lifecycle_busy = true;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    if (!allowed) return ESP_ERR_INVALID_STATE;
    esp_err_t err = storage_format_owned();
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_lifecycle_busy = false;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return err;
}

/* Full-chip erase for the "erase both chips" production action. Erase in
 * 64 KiB blocks instead of esp_flash_erase_chip(): the latter blocks for tens
 * of seconds and would trip the provisioning task watchdog. The callback runs
 * after every block so the caller can feed the WDT and report progress. */
esp_err_t storage_erase_chip(storage_progress_cb_t progress)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_lifecycle_mux);
    bool allowed = s_maintenance_owner == caller && caller != NULL &&
                   s_leases == 0U && !s_lifecycle_busy;
    if (allowed) s_lifecycle_busy = true;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    if (!allowed) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (s_ext_flash == NULL) {
        ESP_LOGW(TAG, "chip erase: external NOR not available");
        goto out;
    }

    uint32_t size = 0;
    err = esp_flash_get_size(s_ext_flash, &size);
    if (err != ESP_OK || size == 0U) {
        ESP_LOGE(TAG, "chip erase: cannot determine size (%s)",
                 esp_err_to_name(err));
        err = (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
        goto out;
    }

    if (s_mounted) {
        err = esp_vfs_littlefs_unregister_partition(s_ext_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "chip erase: unmount failed (%s)", esp_err_to_name(err));
            goto out;
        }
        portENTER_CRITICAL(&s_lifecycle_mux);
        s_mounted = false;
        portEXIT_CRITICAL(&s_lifecycle_mux);
    }

    ESP_LOGW(TAG, "Erasing the entire external NOR chip (%lu KiB)...",
             (unsigned long)(size / 1024U));
    const uint32_t chunk = 64U * 1024U;
    for (uint32_t off = 0; off < size; off += chunk) {
        uint32_t len = size - off;
        if (len > chunk) len = chunk;
        err = esp_flash_erase_region(s_ext_flash, off, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "chip erase failed at 0x%lx (%s)",
                     (unsigned long)off, esp_err_to_name(err));
            goto out;
        }
        if (progress != NULL) {
            progress(off + len, size);
        }
    }
    ESP_LOGW(TAG, "External NOR chip erased (%lu KiB, blank)", (unsigned long)(size / 1024U));
    err = ESP_OK;

out:
    portENTER_CRITICAL(&s_lifecycle_mux);
    s_lifecycle_busy = false;
    portEXIT_CRITICAL(&s_lifecycle_mux);
    return err;
}

esp_err_t storage_get_free_bytes(uint64_t *out_free_bytes)
{
    if (out_free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) return err;

    size_t total = 0;
    size_t used = 0;
    err = esp_littlefs_partition_info(s_ext_partition, &total, &used);
    storage_access_end();
    if (err != ESP_OK) {
        return err;
    }
    *out_free_bytes = (uint64_t)(total > used ? total - used : 0);
    return ESP_OK;
}
