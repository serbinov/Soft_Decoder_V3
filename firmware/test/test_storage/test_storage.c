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

#include "../../test_libs/teststubs/stubs.c"

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
    TEST_ASSERT_TRUE(storage_ext_available());
    TEST_ASSERT_EQUAL_STRING(STORAGE_MOUNT_POINT, storage_get_root());
}

static void test_storage_init_spi_fail(void)
{
    mock_spi_bus_init_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_NONE, storage_get_backend());
    TEST_ASSERT_FALSE(storage_ext_available());
}

static void test_storage_init_add_device_fail(void)
{
    mock_spi_add_flash_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(STORAGE_BACKEND_NONE, storage_get_backend());
    TEST_ASSERT_FALSE(storage_ext_available());
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
    TEST_ASSERT_FALSE(storage_ext_available());
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
    TEST_ASSERT_FALSE(storage_ext_available());
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
    RUN_TEST(test_storage_mount_unavailable);
    RUN_TEST(test_storage_mount_ok_and_idempotent);
    RUN_TEST(test_storage_mount_lfs_fail);
    RUN_TEST(test_storage_format_unavailable);
    RUN_TEST(test_storage_format_ok);
    RUN_TEST(test_storage_format_lfs_fail);
    RUN_TEST(test_storage_format_remount_fail);
    RUN_TEST(test_storage_free_bytes);
    RUN_TEST(test_format_requires_drained_maintenance);
    return UNITY_END();
}
