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

    const esp_partition_t *part = NULL;
    err = esp_partition_register_external(
        s_ext_flash, 0, EXT_PARTITION_SIZE, EXT_PARTITION_LABEL,
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

/* ------------------------------------------------------------------ */
/* Throughput benchmark (diagnostic)                                    */
/*                                                                      */
/* Order:                                                               */
/*  1. raw flash READ   (esp_flash_read, safe)                          */
/*  2. raw flash ERASE  (esp_flash_erase_region @ 8 MiB)                */
/*  3. raw flash WRITE  (esp_flash_write @ 8 MiB, after erase)          */
/*  4. raw read-back + verify                                           */
/*  5. re-format LittleFS (restores a clean FS)                          */
/*  6. LittleFS file WRITE 1 MiB (fresh FS)                              */
/*  7. LittleFS file READ  1 MiB (fresh FS)                              */
/* ------------------------------------------------------------------ */

static unsigned int bench_kib_per_s(uint64_t bytes, uint64_t us)
{
    if (us == 0) {
        return 0;
    }
    return (unsigned int)((bytes * 1000000ULL) / us / 1024ULL);
}

static void bench_fill(uint8_t *buf, size_t n, uint32_t off)
{
    uint8_t seed = (uint8_t)(off * 31u + 7u);
    for (size_t i = 0; i < n; ++i) {
        seed = (uint8_t)(seed * 31u + (uint8_t)(i & 0xFFu) + 7u);
        buf[i] = seed;
    }
}

esp_err_t storage_benchmark(void)
{
    const size_t bench_size = 1 * 1024 * 1024;
    const size_t chunk = 4096;
    const uint32_t bench_addr = 8 * 1024 * 1024; /* middle of the 16 MiB chip */

    if (s_ext_flash == NULL) {
        ESP_LOGE(TAG, "bench: external flash not available");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *buf = malloc(chunk);
    uint8_t *ref = malloc(chunk);
    if (buf == NULL || ref == NULL) {
        ESP_LOGE(TAG, "bench: malloc failed");
        free(buf);
        free(ref);
        return ESP_ERR_NO_MEM;
    }

    int64_t t0;
    uint64_t us;
    bool ok = true;

    /* --- 1. raw read (address 0, read-only) --- */
    t0 = esp_timer_get_time();
    for (uint32_t off = 0; off < bench_size && ok; off += chunk) {
        ok = esp_flash_read(s_ext_flash, buf, off, chunk) == ESP_OK;
    }
    us = (uint64_t)(esp_timer_get_time() - t0);
    ESP_LOGI(TAG, "BENCH raw READ:  %u KiB in %u us = %u KiB/s",
             (unsigned)(bench_size / 1024), (unsigned)us, bench_kib_per_s(bench_size, us));

    /* --- 2. raw erase --- */
    t0 = esp_timer_get_time();
    ok = esp_flash_erase_region(s_ext_flash, bench_addr, bench_size) == ESP_OK;
    us = (uint64_t)(esp_timer_get_time() - t0);
    ESP_LOGI(TAG, "BENCH raw ERASE: %u KiB in %u us = %u KiB/s (ok=%d)",
             (unsigned)(bench_size / 1024), (unsigned)us, bench_kib_per_s(bench_size, us), (int)ok);

    /* --- 3. raw write --- */
    t0 = esp_timer_get_time();
    for (uint32_t off = 0; off < bench_size && ok; off += chunk) {
        bench_fill(buf, chunk, off);
        ok = esp_flash_write(s_ext_flash, buf, bench_addr + off, chunk) == ESP_OK;
    }
    us = (uint64_t)(esp_timer_get_time() - t0);
    ESP_LOGI(TAG, "BENCH raw WRITE: %u KiB in %u us = %u KiB/s (ok=%d)",
             (unsigned)(bench_size / 1024), (unsigned)us, bench_kib_per_s(bench_size, us), (int)ok);

    /* --- 4. raw read-back + verify --- */
    t0 = esp_timer_get_time();
    bool match = true;
    for (uint32_t off = 0; off < bench_size && ok && match; off += chunk) {
        ok = esp_flash_read(s_ext_flash, buf, bench_addr + off, chunk) == ESP_OK;
        bench_fill(ref, chunk, off);
        if (ok && memcmp(buf, ref, chunk) != 0) {
            match = false;
        }
    }
    us = (uint64_t)(esp_timer_get_time() - t0);
    ESP_LOGI(TAG, "BENCH raw VERIFY: %u KiB in %u us, data %s",
             (unsigned)(bench_size / 1024), (unsigned)us, match ? "OK" : "MISMATCH");

    /* --- 5. re-format LittleFS (restores a clean FS) --- */
    ESP_LOGI(TAG, "bench: re-formatting LittleFS for clean-FS test...");
    if (storage_format() != ESP_OK) {
        ESP_LOGW(TAG, "bench: re-format failed, skipping FS test");
        free(buf);
        free(ref);
        return ESP_OK;
    }

    /* --- 6. LittleFS write on fresh FS --- */
    const char *path = MOUNT_POINT "/.bench.bin";
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "bench: open for write failed");
        free(buf);
        free(ref);
        return ESP_FAIL;
    }
    (void)setvbuf(f, NULL, _IOFBF, 16384);
    t0 = esp_timer_get_time();
    size_t remaining = bench_size;
    ok = true;
    while (remaining > 0 && ok) {
        size_t n = remaining < chunk ? remaining : chunk;
        bench_fill(buf, n, (uint32_t)(bench_size - remaining));
        ok = fwrite(buf, 1, n, f) == n;
        remaining -= n;
    }
    (void)fflush(f);
    (void)fsync(fileno(f));
    us = (uint64_t)(esp_timer_get_time() - t0);
    fclose(f);
    ESP_LOGI(TAG, "BENCH FS write (fresh): %u KiB in %u us = %u KiB/s (ok=%d)",
             (unsigned)(bench_size / 1024), (unsigned)us, bench_kib_per_s(bench_size, us), (int)ok);

    /* --- 7. LittleFS read on fresh FS --- */
    f = fopen(path, "rb");
    if (f != NULL) {
        t0 = esp_timer_get_time();
        remaining = bench_size;
        ok = true;
        while (remaining > 0 && ok) {
            size_t n = remaining < chunk ? remaining : chunk;
            ok = fread(buf, 1, n, f) == n;
            remaining -= n;
        }
        us = (uint64_t)(esp_timer_get_time() - t0);
        fclose(f);
        ESP_LOGI(TAG, "BENCH FS read (fresh): %u KiB in %u us = %u KiB/s (ok=%d)",
                 (unsigned)(bench_size / 1024), (unsigned)us, bench_kib_per_s(bench_size, us), (int)ok);
        (void)unlink(path);
    }

    free(buf);
    free(ref);
    return ESP_OK;
}
