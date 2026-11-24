#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "freertos/task.h"
static TaskHandle_t g_current_task = (TaskHandle_t)1;
#define xTaskGetCurrentTaskHandle() g_current_task

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
int fsync(int fd); /* MinGW has no fsync(); provided at the bottom */
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif

/* Host mount point for the storage tests (must be defined before the white-box
 * include so MOUNT_POINT and the benchmark path point at a temp directory). */
#define STORAGE_MOUNT_POINT "st_tmp"

#define static
#include "../../components/storage/src/storage.c"
#undef static
#undef xTaskGetCurrentTaskHandle

/* Rename the shared stubs whose failure paths the host tests need to inject. */
#define esp_flash_read_id stub_esp_flash_read_id
#define esp_vfs_littlefs_unregister_partition stub_esp_vfs_littlefs_unregister_partition
#include "../../test_libs/teststubs/stubs.c"
#undef esp_flash_read_id
#undef esp_vfs_littlefs_unregister_partition

esp_err_t stub_esp_flash_read_id(esp_flash_t *chip, uint32_t *out_id);
esp_err_t stub_esp_vfs_littlefs_unregister_partition(const esp_partition_t *partition);

static int g_flash_id_fail;
static esp_err_t g_unregister_err = ESP_OK;

esp_err_t esp_flash_read_id(esp_flash_t *chip, uint32_t *out_id)
{
    if (g_flash_id_fail) {
        return ESP_FAIL;
    }
    return stub_esp_flash_read_id(chip, out_id);
}

esp_err_t esp_vfs_littlefs_unregister_partition(const esp_partition_t *partition)
{
    if (g_unregister_err != ESP_OK) {
        return g_unregister_err;
    }
    return stub_esp_vfs_littlefs_unregister_partition(partition);
}

#ifdef _WIN32
int fsync(int fd)
{
    (void)fd;
    return 0;
}
#endif

static void wipe_bench_file(void)
{
    remove(STORAGE_MOUNT_POINT "/.bench.bin");
}

void setUp(void)
{
    mock_spi_bus_init_err = 0;
    mock_spi_add_flash_err = 0;
    mock_flash_init_err = 0;
    mock_partition_register_err = 0;
    mock_lfs_register_err = 0;
    mock_lfs_format_err = 0;
    mock_lfs_info_err = 0;
    mock_flash_read_err = 0;
    mock_flash_write_err = 0;
    mock_flash_erase_err = 0;
    mock_flash_size = 16u * 1024u * 1024u;
    mock_partition_registered_size = 0;
    mock_lfs_total = 1024u * 1024u;
    mock_lfs_used = 256u * 1024u;
    mock_lfs_register_calls = 0;
    mock_lfs_unregister_calls = 0;
    mock_lfs_format_calls = 0;
    mock_timer_now_us = 0;

    s_ext_flash = NULL;
    s_ext_partition = NULL;
    s_mounted = false;
    s_backend = STORAGE_BACKEND_NONE;
    s_leases = 0;
    s_maintenance_owner = NULL;
    s_lifecycle_busy = false;
    g_current_task = (TaskHandle_t)1;
    g_flash_id_fail = 0;
    g_unregister_err = ESP_OK;

    (void)MKDIR(STORAGE_MOUNT_POINT);
    wipe_bench_file();
}

void tearDown(void)
{
    wipe_bench_file();
}

/* ---- init ---- */

static void test_storage_init_ok(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_EXTERNAL_NOR, storage_get_backend());
    TEST_ASSERT_EQUAL_STRING(STORAGE_MOUNT_POINT, storage_get_root());
}

static void test_storage_init_spi_fail(void)
{
    mock_spi_bus_init_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_NONE, storage_get_backend());
}

static void test_storage_init_add_device_fail(void)
{
    mock_spi_add_flash_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_NONE, storage_get_backend());
}

static void test_storage_init_flash_fail(void)
{
    mock_flash_init_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_NONE, storage_get_backend());
}

static void test_storage_init_register_fail(void)
{
    mock_partition_register_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_NONE, storage_get_backend());
}

/* The registered partition must not exceed the detected chip size. */
static void test_storage_init_small_chip(void)
{
    mock_flash_size = 8u * 1024u * 1024u;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_EXTERNAL_NOR, storage_get_backend());
    TEST_ASSERT_EQUAL_UINT32(8u * 1024u * 1024u, mock_partition_registered_size);
}

static void test_storage_init_size_probe_fail(void)
{
    mock_flash_size = 0;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_NONE, storage_get_backend());
}

/* A chip whose JEDEC ID cannot be read still mounts with its detected size. */
static void test_storage_init_jedec_id_read_fail(void)
{
    g_flash_id_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_EXTERNAL_NOR, storage_get_backend());
}

/* ---- mount ---- */

static void test_storage_mount_unavailable(void)
{
    /* No init -> no external partition. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, storage_mount());
    TEST_ASSERT_FALSE(storage_is_mounted());
}

static void test_storage_mount_ok_and_idempotent(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_TRUE(storage_is_mounted());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount()); /* already mounted */
    TEST_ASSERT_EQUAL_INT(1, mock_lfs_register_calls);
}

static void test_storage_mount_lfs_fail(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    mock_lfs_register_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, storage_mount());
    TEST_ASSERT_FALSE(storage_is_mounted());
}

/* ---- format ---- */

static void test_storage_format_unavailable(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, storage_format());
}

static void test_storage_format_ok(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    TEST_ASSERT_EQUAL(ESP_OK, storage_format());
    TEST_ASSERT_TRUE(storage_is_mounted());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_EXTERNAL_NOR, storage_get_backend());
    TEST_ASSERT_TRUE(mock_lfs_format_calls >= 1);
    TEST_ASSERT_TRUE(mock_lfs_unregister_calls >= 1);
}

static void test_storage_format_lfs_fail(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    mock_lfs_format_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, storage_format());
}

static void test_storage_format_remount_fail(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    mock_lfs_register_err = ESP_FAIL; /* format_partition OK, remount fails */
    TEST_ASSERT_EQUAL(ESP_FAIL, storage_format());
}

static uint32_t g_erase_done, g_erase_total;
static void erase_progress_cb(uint32_t done, uint32_t total)
{
    g_erase_done = done;
    g_erase_total = total;
}

static void test_storage_erase_chip_ok(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    g_erase_done = g_erase_total = 0;
    TEST_ASSERT_EQUAL(ESP_OK, storage_erase_chip(erase_progress_cb));
    TEST_ASSERT_EQUAL_UINT32(16u * 1024u * 1024u, g_erase_done);
    TEST_ASSERT_EQUAL_UINT32(16u * 1024u * 1024u, g_erase_total);
    TEST_ASSERT_FALSE(storage_is_mounted());
    TEST_ASSERT_TRUE(mock_lfs_unregister_calls >= 1);
    /* The chip is blank: a second erase is still allowed and idempotent. */
    TEST_ASSERT_EQUAL(ESP_OK, storage_erase_chip(NULL));
}

static void test_storage_erase_chip_requires_maintenance(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_erase_chip(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_erase_chip(NULL));
}

static void test_storage_erase_chip_fail(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    mock_flash_erase_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, storage_erase_chip(NULL));
    TEST_ASSERT_FALSE(storage_is_mounted());
}

static void test_storage_erase_chip_unavailable(void)
{
    /* Maintenance is drained and owned, but no external NOR was probed. */
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, storage_erase_chip(NULL));
    storage_maintenance_end();
}

static void test_storage_erase_chip_size_unknown(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    mock_flash_size = 0; /* get_size succeeds but reports an unusable size */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, storage_erase_chip(NULL));
}

static void test_storage_erase_chip_unmount_fail(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    g_unregister_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, storage_erase_chip(NULL));
    TEST_ASSERT_TRUE(storage_is_mounted());
}

/* ---- free bytes ---- */

static void test_storage_free_bytes(void)
{
    uint64_t freeb = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, storage_get_free_bytes(NULL));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_get_free_bytes(&freeb));

    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_EQUAL(ESP_OK, storage_get_free_bytes(&freeb));
    TEST_ASSERT_EQUAL_UINT64(mock_lfs_total - mock_lfs_used, freeb);

    /* Used > total must clamp to 0. */
    mock_lfs_used = mock_lfs_total + 100;
    TEST_ASSERT_EQUAL(ESP_OK, storage_get_free_bytes(&freeb));
    TEST_ASSERT_EQUAL_UINT64(0, freeb);

    mock_lfs_info_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, storage_get_free_bytes(&freeb));
}

static void test_format_requires_drained_maintenance(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_format());
    TEST_ASSERT_EQUAL(ESP_OK, storage_access_begin());
    TEST_ASSERT_FALSE(storage_is_quiescent());
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_access_begin());
    g_current_task = (TaskHandle_t)2;
    storage_maintenance_end(); /* wrong task cannot reopen admission */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_format());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_access_begin());
    g_current_task = (TaskHandle_t)1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_format());
    TEST_ASSERT_EQUAL_INT(0, mock_lfs_unregister_calls);
    storage_access_end();
    TEST_ASSERT_TRUE(storage_is_quiescent());
    TEST_ASSERT_EQUAL(ESP_OK, storage_format());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_access_begin());
    storage_maintenance_end();
    TEST_ASSERT_EQUAL(ESP_OK, storage_access_begin());
    storage_access_end();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_storage_init_ok);
    RUN_TEST(test_storage_init_spi_fail);
    RUN_TEST(test_storage_init_add_device_fail);
    RUN_TEST(test_storage_init_flash_fail);
    RUN_TEST(test_storage_init_register_fail);
    RUN_TEST(test_storage_init_small_chip);
    RUN_TEST(test_storage_init_size_probe_fail);
    RUN_TEST(test_storage_init_jedec_id_read_fail);
    RUN_TEST(test_storage_mount_unavailable);
    RUN_TEST(test_storage_mount_ok_and_idempotent);
    RUN_TEST(test_storage_mount_lfs_fail);
    RUN_TEST(test_storage_format_unavailable);
    RUN_TEST(test_storage_format_ok);
    RUN_TEST(test_storage_format_lfs_fail);
    RUN_TEST(test_storage_format_remount_fail);
    RUN_TEST(test_storage_erase_chip_ok);
    RUN_TEST(test_storage_erase_chip_requires_maintenance);
    RUN_TEST(test_storage_erase_chip_fail);
    RUN_TEST(test_storage_erase_chip_unavailable);
    RUN_TEST(test_storage_erase_chip_size_unknown);
    RUN_TEST(test_storage_erase_chip_unmount_fail);
    RUN_TEST(test_storage_free_bytes);
    RUN_TEST(test_format_requires_drained_maintenance);
    return UNITY_END();
}
