#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "freertos/task.h"
#define xTaskGetCurrentTaskHandle() ((TaskHandle_t)1)

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif

#define static
#define TAG storage_TAG
#include "../../components/storage/src/storage.c"
#undef TAG
#undef static

#define static
#include "../../components/track/src/track_recover.c"
#undef static
#undef xTaskGetCurrentTaskHandle

#include "../../test_libs/teststubs/stubs.c"

/* ---- settings.c substitute ---- */
static settings_track_t g_tracks[SETTINGS_MAX_TRACKS];
static size_t g_count;
static bool g_manifest_ok;
static int g_save_calls;
static bool g_tracks_present;
static bool g_recovery_pending;
static esp_err_t g_manifest_error;
static esp_err_t g_save_error;
static esp_err_t g_load_error;

esp_err_t settings_recovery_pending(bool *out) { *out = g_recovery_pending; return ESP_OK; }

esp_err_t settings_tracks_load(settings_track_t *tracks, size_t *count)
{
    if (tracks == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_load_error != ESP_OK) { *count = 0; return g_load_error; }
    if (g_count == 0 && !g_tracks_present) {
        *count = 0;
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(tracks, g_tracks, g_count * sizeof(settings_track_t));
    *count = g_count;
    return ESP_OK;
}

esp_err_t settings_tracks_save(const settings_track_t *tracks, size_t count)
{
    if (g_save_error != ESP_OK) return g_save_error;
    memcpy(g_tracks, tracks, count * sizeof(settings_track_t));
    g_tracks_present = true;
    g_count = count;
    g_save_calls++;
    return ESP_OK;
}

esp_err_t settings_manifest_load(void)
{
    if (g_manifest_error != ESP_OK) return g_manifest_error;
    if (!g_manifest_ok) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Simulate restoring two slots from the manifest. */
    memset(g_tracks, 0, sizeof(g_tracks));
    g_tracks[0].slot = 1;
    snprintf(g_tracks[0].file, sizeof(g_tracks[0].file), "audio/slot1.wav");
    snprintf(g_tracks[0].label, sizeof(g_tracks[0].label), "Из манифеста");
    g_tracks[0].enabled = true;
    g_tracks[1].slot = 2;
    g_count = 2;
    g_recovery_pending = false;
    return ESP_OK;
}

#define DIR_AUDIO "tr_tmp"
#define DIR_ROOT  "tr_tmp"

static void rm_tree(void)
{
    static const char *files[] = {
        DIR_AUDIO "/slot1.wav", DIR_AUDIO "/slot2.wav", DIR_AUDIO "/slot3.wav",
        DIR_AUDIO "/horn.wav", DIR_AUDIO "/slot1.WAV", DIR_AUDIO "/slot99.wav",
        DIR_AUDIO "/notes.txt", DIR_AUDIO "/xwav.wav", DIR_AUDIO "/a.wav",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        remove(files[i]);
    }
    (void)RMDIR(DIR_AUDIO);
}

static void make_file(const char *name)
{
    FILE *f = fopen(name, "wb");
    if (f != NULL) {
        fwrite("x", 1, 1, f);
        fclose(f);
    }
}

void setUp(void)
{
    g_count = 0;
    g_manifest_ok = false;
    g_save_calls = 0;
    g_tracks_present = false;
    g_recovery_pending = false;
    g_manifest_error = ESP_OK;
    g_save_error = ESP_OK;
    g_load_error = ESP_OK;
    s_leases = 0;
    s_maintenance_owner = NULL;
    s_lifecycle_busy = false;
    s_mounted = false;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    memset(g_tracks, 0, sizeof(g_tracks));
    rm_tree();
    (void)MKDIR(DIR_AUDIO);
}

void tearDown(void)
{
    rm_tree();
}

static int find_slot(const settings_track_t *t, size_t n, uint8_t slot,
                     const char **out_file, const char **out_label)
{
    for (size_t i = 0; i < n; ++i) {
        if (t[i].slot == slot) {
            if (out_file) {
                *out_file = t[i].file;
            }
            if (out_label) {
                *out_label = t[i].label;
            }
            return 1;
        }
    }
    return 0;
}

/* ---- NVS already populated ---- */

static void test_recover_keeps_nvs_list(void)
{
    g_count = 3;
    track_recover_result_t r;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_TRUE(r.had_nvs);
    TEST_ASSERT_EQUAL_UINT32(3, r.count);
    TEST_ASSERT_FALSE(r.rebuilt);
}

/* ---- manifest restore ---- */

static void test_recover_from_manifest(void)
{
    g_count = 0;
    g_manifest_ok = true;
    track_recover_result_t r;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_TRUE(r.from_manifest);
    TEST_ASSERT_EQUAL_UINT32(2, r.count);
    TEST_ASSERT_FALSE(r.rebuilt);
}

/* ---- rebuild from file names ---- */

static void test_recover_rebuilds_from_files(void)
{
    make_file(DIR_AUDIO "/slot1.wav");
    make_file(DIR_AUDIO "/slot3.wav");
    make_file(DIR_AUDIO "/horn.wav");
    make_file(DIR_AUDIO "/notes.txt"); /* ignored */

    track_recover_result_t r;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_TRUE(r.rebuilt);
    TEST_ASSERT_EQUAL_UINT32(3, r.count);
    TEST_ASSERT_EQUAL_INT(1, g_save_calls);

    const char *file = NULL;
    const char *label = NULL;
    TEST_ASSERT_TRUE(find_slot(g_tracks, g_count, 1, &file, &label));
    TEST_ASSERT_EQUAL_STRING("audio/slot1.wav", file);
    TEST_ASSERT_EQUAL_STRING("Слот 1", label);

    TEST_ASSERT_TRUE(find_slot(g_tracks, g_count, 3, &file, NULL));
    TEST_ASSERT_EQUAL_STRING("audio/slot3.wav", file);

    /* Label for a file without a slot prefix is the name without extension. */
    bool found_horn = false;
    for (size_t i = 0; i < g_count; ++i) {
        if (strcmp(g_tracks[i].label, "horn") == 0) {
            found_horn = true;
        }
    }
    TEST_ASSERT_TRUE(found_horn);
}

/* ---- duplicate slot names are de-duplicated ---- */

static void test_recover_dedups_and_ignores_invalid(void)
{
    make_file(DIR_AUDIO "/slot1.wav");
    make_file(DIR_AUDIO "/slot1.WAV");  /* same slot -> skipped */
    make_file(DIR_AUDIO "/slot99.wav"); /* out of range -> pass 2, label slot99 */
    track_recover_result_t r;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_TRUE(r.rebuilt);
    TEST_ASSERT_EQUAL_UINT32(2, r.count);

    int slot1 = 0;
    for (size_t i = 0; i < g_count; ++i) {
        if (g_tracks[i].slot == 1) {
            slot1++;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, slot1);
}

/* ---- no metadata, no files ---- */

static void test_recover_no_files(void)
{
    track_recover_result_t r;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_FALSE(r.had_nvs);
    TEST_ASSERT_FALSE(r.from_manifest);
    TEST_ASSERT_FALSE(r.rebuilt);
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    TEST_ASSERT_EQUAL_UINT32(0, r.files_found);
}

static void test_recover_missing_dir(void)
{
    track_recover_result_t r;
    track_recover_from_storage("tr_nope", "tr_nope", &r);
    TEST_ASSERT_FALSE(r.rebuilt);
    TEST_ASSERT_EQUAL_UINT32(0, r.files_found);
}

static void test_recover_null_out(void)
{
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, NULL); /* no crash */
    TEST_ASSERT_TRUE(true);
}

/* ---- root-dir fallback when the audio dir is empty ---- */

static void test_recover_root_fallback(void)
{
    /* audio_dir missing -> scans root_dir (same temp tree). */
    make_file(DIR_ROOT "/slot2.wav");
    track_recover_result_t r;
    track_recover_from_storage("tr_empty", DIR_ROOT, &r);
    TEST_ASSERT_TRUE(r.rebuilt);
    TEST_ASSERT_EQUAL_UINT32(1, r.count);
    TEST_ASSERT_EQUAL_STRING("slot2.wav", g_tracks[0].file);
}

static void test_rev_s24_intentionally_empty_nvs_survives_orphan_wav(void)
{
    g_tracks_present = true;
    make_file(DIR_AUDIO "/horn.wav");
    g_manifest_ok = true;
    track_recover_result_t r;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_TRUE(r.had_nvs);
    TEST_ASSERT_EQUAL_UINT32(0, r.count);
    TEST_ASSERT_EQUAL_INT(0, g_save_calls);
    TEST_ASSERT_FALSE(r.from_manifest);
    TEST_ASSERT_EQUAL(ESP_OK, r.error);
}

static void test_rev_s26_existing_tracks_do_not_hide_partial_restore(void)
{
    g_count = 1;
    g_recovery_pending = true;
    g_manifest_error = ESP_FAIL;
    track_recover_result_t r;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_FALSE(r.had_nvs);
    TEST_ASSERT_TRUE(r.retry_pending);
    TEST_ASSERT_EQUAL(ESP_FAIL, r.error);
    g_manifest_error = ESP_OK;
    g_manifest_ok = true;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_TRUE(r.from_manifest);
    TEST_ASSERT_EQUAL_UINT32(2, r.count);
    TEST_ASSERT_FALSE(r.retry_pending);
}

static void test_recover_errors_never_trigger_destructive_rebuild(void)
{
    make_file(DIR_AUDIO "/horn.wav");
    track_recover_result_t r;
    g_load_error = ESP_FAIL;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_EQUAL(ESP_FAIL, r.error);
    TEST_ASSERT_TRUE(r.retry_pending);
    TEST_ASSERT_EQUAL_INT(0, g_save_calls);
    g_load_error = ESP_OK;
    g_manifest_error = ESP_FAIL;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_EQUAL(ESP_FAIL, r.error);
    TEST_ASSERT_EQUAL_INT(0, g_save_calls);
    g_manifest_error = ESP_OK;
    g_save_error = ESP_FAIL;
    track_recover_from_storage(DIR_AUDIO, DIR_ROOT, &r);
    TEST_ASSERT_EQUAL(ESP_FAIL, r.error);
    TEST_ASSERT_TRUE(r.retry_pending);
    TEST_ASSERT_FALSE(r.rebuilt);
    TEST_ASSERT_TRUE(storage_is_quiescent());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rev_s24_intentionally_empty_nvs_survives_orphan_wav);
    RUN_TEST(test_rev_s26_existing_tracks_do_not_hide_partial_restore);
    RUN_TEST(test_recover_errors_never_trigger_destructive_rebuild);
    RUN_TEST(test_recover_keeps_nvs_list);
    RUN_TEST(test_recover_from_manifest);
    RUN_TEST(test_recover_rebuilds_from_files);
    RUN_TEST(test_recover_dedups_and_ignores_invalid);
    RUN_TEST(test_recover_no_files);
    RUN_TEST(test_recover_missing_dir);
    RUN_TEST(test_recover_null_out);
    RUN_TEST(test_recover_root_fallback);
    return UNITY_END();
}
