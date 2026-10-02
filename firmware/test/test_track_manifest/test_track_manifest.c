#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <unistd.h>
#include "freertos/task.h"
#include "freertos/semphr.h"
static TaskHandle_t g_current_task = (TaskHandle_t)1;
#define xTaskGetCurrentTaskHandle() g_current_task

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
int fsync(int fd); /* MinGW has no fsync(); provided at the bottom */
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif

/* Host directory for the manifest (must be defined before the white-box include
 * of track_manifest.c so MANIFEST_DIR/MANIFEST_PATH point at a writable path). */
#define MANIFEST_DIR "tm_tmp"

#include "nvs.h"
static const char *g_fail_key;
static bool g_rename_fail;
static TaskHandle_t g_settings_lock_owner;
static unsigned g_recursive_locks;
static bool g_competing_save;
static esp_err_t g_competing_result;
static BaseType_t test_manifest_sem_take(SemaphoreHandle_t handle, TickType_t ticks);
static BaseType_t test_manifest_sem_give(SemaphoreHandle_t handle);
static esp_err_t test_manifest_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);
static int test_manifest_rename(const char *old_name, const char *new_name);
#ifdef _WIN32
static BOOL WINAPI test_manifest_replace(LPCSTR old_name, LPCSTR new_name, DWORD flags);
#endif

#define static
#define TAG storage_TAG
#include "../../components/storage/src/storage.c"
#undef TAG
#undef static

/* settings.c and track_manifest.c each declare a static TAG; here `static` is
 * stripped for the white-box build, so give them distinct names. */
#define static
#define TAG settings_TAG
#define nvs_set_blob test_manifest_set_blob
#define xSemaphoreTake test_manifest_sem_take
#define xSemaphoreGive test_manifest_sem_give
#include "../../components/settings/src/settings.c"
#undef nvs_set_blob
#undef xSemaphoreTake
#undef xSemaphoreGive
#undef TAG
#undef static

#define static
#define TAG manifest_TAG
#define rename test_manifest_rename
#ifdef _WIN32
#define MoveFileExA test_manifest_replace
#endif
#include "../../components/settings/src/track_manifest.c"
#undef rename
#ifdef _WIN32
#undef MoveFileExA
#endif
#undef TAG
#undef static
#undef xTaskGetCurrentTaskHandle

#include "../../test_libs/teststubs/stubs.c"

static esp_err_t test_manifest_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    if (g_competing_save && strcmp(key, "tracks") == 0) {
        g_competing_save = false;
        g_current_task = (TaskHandle_t)2;
        g_competing_result = settings_func_bind_save(NULL, 0);
        g_current_task = (TaskHandle_t)1;
    }
    if (g_fail_key != NULL && strcmp(g_fail_key, key) == 0) return ESP_FAIL;
    return nvs_set_blob(h, key, value, len);
}
static BaseType_t test_manifest_sem_take(SemaphoreHandle_t handle, TickType_t ticks)
{
    if (g_settings_lock_owner != NULL) {
        if (g_settings_lock_owner == g_current_task) ++g_recursive_locks;
        return pdFALSE;
    }
    BaseType_t ok = xSemaphoreTake(handle, ticks);
    if (ok == pdTRUE) g_settings_lock_owner = g_current_task;
    return ok;
}
static BaseType_t test_manifest_sem_give(SemaphoreHandle_t handle)
{
    g_settings_lock_owner = NULL;
    return xSemaphoreGive(handle);
}
static int test_manifest_rename(const char *old_name, const char *new_name)
{
    return g_rename_fail ? -1 : rename(old_name, new_name);
}
#ifdef _WIN32
static BOOL WINAPI test_manifest_replace(LPCSTR old_name, LPCSTR new_name, DWORD flags)
{
    return g_rename_fail ? FALSE : MoveFileExA(old_name, new_name, flags);
}
#endif

#ifdef _WIN32
int fsync(int fd) { (void)fd; return 0; }
#endif

static void wipe_manifest(void)
{
    (void)remove(MANIFEST_PATH);
    (void)remove(MANIFEST_PATH ".tmp");
}

static void seed_track(settings_track_t *t, uint8_t slot, const char *file,
                       const char *label, bool enabled)
{
    memset(t, 0, sizeof(*t));
    t->slot = slot;
    snprintf(t->file, sizeof(t->file), "%s", file);
    snprintf(t->label, sizeof(t->label), "%s", label);
    t->enabled = enabled;
}

void setUp(void)
{
    mock_nvs_reset();
    g_fail_key = NULL;
    g_rename_fail = false;
    g_current_task = (TaskHandle_t)1;
    g_settings_lock_owner = NULL;
    g_recursive_locks = 0;
    g_competing_save = false;
    g_competing_result = ESP_OK;
    s_lock = (SemaphoreHandle_t)1;
    s_manifest_owner = NULL;
    s_manifest_loading = false;
    s_manifest_pending = false;
    s_leases = 0;
    s_maintenance_owner = NULL;
    s_lifecycle_busy = false;
    s_mounted = false;
    TEST_ASSERT_EQUAL(ESP_OK, storage_init());
    TEST_ASSERT_EQUAL(ESP_OK, storage_mount());
    (void)MKDIR(MANIFEST_DIR);
    wipe_manifest();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, g_recursive_locks);
    TEST_ASSERT_NULL(g_settings_lock_owner);
    wipe_manifest();
}

/* ---- roundtrip: tracks (slot/file/label/enabled) ---- */

static void test_manifest_roundtrip_tracks(void)
{
    settings_track_t t[2];
    seed_track(&t[0], 1, "audio/slot1.wav", "Гудок", true);
    seed_track(&t[1], 3, "audio/slot3.wav", "Bell", false);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(t, 2));

    FILE *f = fopen(MANIFEST_PATH, "r");
    TEST_ASSERT_NOT_NULL(f);
    char buf[512] = { 0 };
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    TEST_ASSERT_TRUE(rd > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "AURA-TRACKS"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Гудок"));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, back[0].slot);
    TEST_ASSERT_EQUAL_STRING("audio/slot1.wav", back[0].file);
    TEST_ASSERT_EQUAL_STRING("Гудок", back[0].label);
    TEST_ASSERT_TRUE(back[0].enabled);
    TEST_ASSERT_EQUAL_UINT8(3, back[1].slot);
    TEST_ASSERT_FALSE(back[1].enabled);
}

/* ---- roundtrip: per-slot categories ---- */

static void test_manifest_roundtrip_categories(void)
{
    /* The category is stored as part of each track record, so exercise two
     * occupied slots (1 and 3). */
    settings_track_t t[2];
    seed_track(&t[0], 1, "audio/slot1.wav", "One", true);
    seed_track(&t[1], 3, "audio/slot3.wav", "Three", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(t, 2));

    uint8_t cats[SETTINGS_MAX_TRACKS];
    memset(cats, 0, sizeof(cats));
    cats[0] = 1; /* slot 1 -> effects */
    cats[2] = 0; /* slot 3 -> engine */
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_save(cats, SETTINGS_MAX_TRACKS));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    uint8_t back[SETTINGS_MAX_TRACKS];
    size_t cn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_load(back, &cn));
    TEST_ASSERT_EQUAL_UINT8(1, back[0]);
    TEST_ASSERT_EQUAL_UINT8(0, back[2]);
}

/* ---- roundtrip: function map ---- */

static void test_manifest_roundtrip_func_map(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "One", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    settings_func_map_t m[SETTINGS_FUNC_MAP_COUNT];
    for (size_t i = 0; i < SETTINGS_FUNC_MAP_COUNT; ++i) {
        m[i].slot_a = (uint8_t)(i + 1U);
        m[i].slot_b = (uint8_t)(i & 0x03U);
        m[i].aux_mask = (uint16_t)(i * 3U);
        m[i].dir = (uint8_t)(i % 3U);
        m[i].speed = (uint8_t)(i % 4U);
    }
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_map_save(m, SETTINGS_FUNC_MAP_COUNT));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_func_map_t back[SETTINGS_FUNC_MAP_COUNT];
    size_t fn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_map_load(back, &fn));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_FUNC_MAP_COUNT, fn);
    for (size_t i = 0; i < SETTINGS_FUNC_MAP_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT8(m[i].slot_a, back[i].slot_a);
        TEST_ASSERT_EQUAL_UINT8(m[i].slot_b, back[i].slot_b);
        TEST_ASSERT_EQUAL_UINT16(m[i].aux_mask, back[i].aux_mask);
        TEST_ASSERT_EQUAL_UINT8(m[i].dir, back[i].dir);
        TEST_ASSERT_EQUAL_UINT8(m[i].speed, back[i].speed);
    }
}

/* ---- labels with a semicolon are sanitised (no field break) ---- */

static void test_manifest_sanitizes_semicolon_in_label(void)
{
    settings_track_t t;
    seed_track(&t, 2, "audio/slot2.wav", "a;b;c", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_STRING("a b c", back[0].label);
}

/* ---- UTF-8 label survives byte-for-byte ---- */

static void test_manifest_utf8_label_roundtrip(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "Перед-Задний", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_STRING("Перед-Задний", back[0].label);
}

/* ---- long label is truncated safely, no crash ---- */

static void test_manifest_long_label_truncated(void)
{
    settings_track_t t;
    char big[200];
    memset(big, 'X', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    seed_track(&t, 1, "audio/slot1.wav", big, true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT(strlen(t.label), strlen(back[0].label));
}

/* ---- error paths ---- */

static void test_manifest_absent_returns_not_found(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

static void test_manifest_header_only_returns_not_found(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("AURA-TRACKS 1\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

static void test_manifest_without_magic_returns_not_found(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("T;1;0;1;audio/slot1.wav;One\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

static void test_manifest_bad_track_lines_are_skipped(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("AURA-TRACKS 1\n", f);
    fputs("garbage line\n", f);
    fputs("T;zzz;0;1;audio/x.wav;Bad\n", f); /* invalid slot -> skipped */
    fputs("T;0;0;1;audio/y.wav;Zero\n", f);  /* slot 0 -> skipped */
    fputs("F;999;1;2;3;4;5\n", f);           /* idx out of range -> skipped */
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

/* A manifest with only function metadata (no T; records) must still restore the
 * function map and bindings, while reporting NOT_FOUND so the caller rebuilds
 * the track list from the audio files (REV-ST1). */
static void test_manifest_metadata_without_tracks(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("AURA-TRACKS 1\n", f);
    fputs("F;0;3;4;5;1;2\n", f);
    fputs("B;0;7;1;2;0;1;2;3;4;5;6;7\n", f);
    fclose(f);

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());

    settings_func_map_t back[SETTINGS_FUNC_MAP_COUNT];
    size_t fn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_map_load(back, &fn));
    TEST_ASSERT_EQUAL_UINT8(3, back[0].slot_a);
    TEST_ASSERT_EQUAL_UINT8(4, back[0].slot_b);
    TEST_ASSERT_EQUAL_UINT16(5, back[0].aux_mask);
    TEST_ASSERT_EQUAL_UINT8(1, back[0].dir);
    TEST_ASSERT_EQUAL_UINT8(2, back[0].speed);

    func_binding_t binds[FUNC_BIND_MAX];
    size_t bn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_load(binds, &bn));
    TEST_ASSERT_EQUAL_UINT32(1, bn);
    TEST_ASSERT_EQUAL_UINT8(7, binds[0].fn);
    TEST_ASSERT_EQUAL_UINT8(4, binds[0].short_table);
    TEST_ASSERT_EQUAL_UINT16(5, binds[0].short_ms);
    TEST_ASSERT_EQUAL_UINT16(7, binds[0].fade_ms);
}

/* ---- sync is a no-op (no crash) when storage is not mounted ---- */

static void test_manifest_sync_without_dir_no_crash(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "One", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    wipe_manifest();
    (void)RMDIR(MANIFEST_DIR); /* manifest dir gone: fopen must simply fail */

    settings_manifest_sync(); /* must not crash */

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
}

/* A track whose slot is beyond the stored categories must fall back to the
 * default category (no out-of-range read). */
static void test_manifest_track_slot_without_category(void)
{
    uint8_t cats[5] = { 1, 0, 1, 0, 1 };
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_save(cats, 5));

    settings_track_t t;
    seed_track(&t, 10, "audio/slot10.wav", "Ten", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT8(10, back[0].slot);
}

/* ---- roundtrip: function bindings (manifest v2) ---- */

static void test_manifest_roundtrip_bindings(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "One", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    func_binding_t b[2];
    memset(b, 0, sizeof(b));
    b[0].used = 1;
    b[0].fn = 2;
    b[0].target_type = FUNC_TARGET_SOUND;
    b[0].target_id = 5;
    b[0].mode = SOUND_MODE_SHORT_LONG;
    b[0].flags = FUNC_FLAG_DUCK;
    b[0].short_table = 7;
    b[0].short_ms = 400;
    b[0].min_ms = 150;
    b[0].fade_ms = 80;
    b[1].used = 1;
    b[1].fn = 1;
    b[1].target_type = FUNC_TARGET_OUTPUT;
    b[1].target_id = 3;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_save(b, 2));

    FILE *f = fopen(MANIFEST_PATH, "r");
    TEST_ASSERT_NOT_NULL(f);
    char buf[2048] = { 0 };
    (void)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "AURA-TRACKS 3"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "B;"));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    func_binding_t back[FUNC_BIND_MAX];
    size_t bn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_load(back, &bn));
    TEST_ASSERT_EQUAL_UINT32(2, bn);
    TEST_ASSERT_EQUAL_UINT8(2, back[0].fn);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_SOUND, back[0].target_type);
    TEST_ASSERT_EQUAL_UINT8(5, back[0].target_id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_MODE_SHORT_LONG, back[0].mode);
    TEST_ASSERT_EQUAL_UINT8(FUNC_FLAG_DUCK, back[0].flags);
    TEST_ASSERT_EQUAL_UINT8(7, back[0].short_table);
    TEST_ASSERT_EQUAL_UINT16(400, back[0].short_ms);
    TEST_ASSERT_EQUAL_UINT16(150, back[0].min_ms);
    TEST_ASSERT_EQUAL_UINT16(80, back[0].fade_ms);
    TEST_ASSERT_EQUAL_UINT8(1, back[1].fn);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_OUTPUT, back[1].target_type);
    TEST_ASSERT_EQUAL_UINT8(3, back[1].target_id);
}

static void test_rev_s23_rename_failure_preserves_live_and_retry(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/old.wav", "Old", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));
    char old[8192] = { 0 }, current[8192] = { 0 };
    FILE *f = fopen(MANIFEST_PATH, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t old_size = fread(old, 1, sizeof(old), f);
    fclose(f);
    g_rename_fail = true;
    seed_track(&t, 1, "audio/new.wav", "New", true);
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_tracks_save(&t, 1));
    f = fopen(MANIFEST_PATH, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t new_size = fread(current, 1, sizeof(current), f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT32(old_size, new_size);
    TEST_ASSERT_EQUAL_MEMORY(old, current, old_size);
    settings_flush_status_t status;
    settings_flush_status(&status);
    TEST_ASSERT_TRUE(status.manifest_pending);
    TEST_ASSERT_EQUAL(ESP_FAIL, status.last_error);
    g_rename_fail = false;
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    TEST_ASSERT_EQUAL(ESP_OK, settings_pending_flush());
    settings_flush_status(&status);
    TEST_ASSERT_FALSE(status.manifest_pending);
}

static void test_rev_s24_s25_manifest_explicit_empty_roundtrip(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(NULL, 0));
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_save(NULL, 0));
    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());
    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    func_binding_t binds[FUNC_BIND_MAX];
    size_t count = 99;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(tracks, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_load(binds, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

static void test_rev_s26_partial_restore_marker_and_worker_retry(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "Restore", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));
    func_binding_t b = { .used = 1, .fn = 7, .target_type = FUNC_TARGET_SLOT, .target_id = 1 };
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_save(&b, 1));
    const char *keys[] = { "tracks", "track_cat", "func_map", "func_bind" };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        mock_nvs_reset(); /* reboot/powercut retains external backup */
        g_fail_key = keys[i];
        TEST_ASSERT_EQUAL(ESP_FAIL, settings_manifest_load());
        bool pending = false;
        TEST_ASSERT_EQUAL(ESP_OK, settings_recovery_pending(&pending));
        TEST_ASSERT_TRUE(pending);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, settings_manifest_sync());
        g_fail_key = NULL;
        mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
        TEST_ASSERT_EQUAL(ESP_OK, settings_pending_flush());
        TEST_ASSERT_EQUAL(ESP_OK, settings_recovery_pending(&pending));
        TEST_ASSERT_FALSE(pending);
        func_binding_t back[FUNC_BIND_MAX];
        size_t count = 0;
        TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_load(back, &count));
        TEST_ASSERT_EQUAL_UINT32(1, count);
        TEST_ASSERT_EQUAL_UINT8(7, back[0].fn);
    }
}

static void test_rev_s27_storage_gate_retains_manifest_dirty(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "Pending", true);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, settings_tracks_save(&t, 1));
    settings_flush_status_t status;
    settings_flush_status(&status);
    TEST_ASSERT_TRUE(status.manifest_pending);
    TEST_ASSERT_TRUE(storage_is_quiescent());
    storage_maintenance_end();
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    TEST_ASSERT_EQUAL(ESP_OK, settings_pending_flush());
    settings_flush_status(&status);
    TEST_ASSERT_FALSE(status.manifest_pending);
}

static void test_manifest_rejects_truncated_declared_records(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("AURA-TRACKS 3\nC;1;0\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, settings_manifest_load());
    TEST_ASSERT_TRUE(storage_is_quiescent());
}

static void test_manifest_missing_tracks_is_not_intentional_empty(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_save(NULL, 0));
    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_tracks_load(tracks, &count));
    func_binding_t binds[FUNC_BIND_MAX];
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_load(binds, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

static void test_recovery_excludes_competing_writes_without_recursive_lock(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "Restore", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));
    mock_nvs_reset();
    g_competing_save = true;
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, g_competing_result);
    TEST_ASSERT_EQUAL_UINT32(0, g_recursive_locks);
    g_current_task = (TaskHandle_t)2;
    s_manifest_loading = true;
    s_manifest_owner = (TaskHandle_t)1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, settings_func_bind_save(NULL, 0));
    s_manifest_loading = false;
    s_manifest_owner = NULL;
    g_current_task = (TaskHandle_t)1;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_manifest_roundtrip_tracks);
    RUN_TEST(test_manifest_roundtrip_categories);
    RUN_TEST(test_manifest_roundtrip_func_map);
    RUN_TEST(test_manifest_sanitizes_semicolon_in_label);
    RUN_TEST(test_manifest_utf8_label_roundtrip);
    RUN_TEST(test_manifest_long_label_truncated);
    RUN_TEST(test_manifest_absent_returns_not_found);
    RUN_TEST(test_manifest_header_only_returns_not_found);
    RUN_TEST(test_manifest_without_magic_returns_not_found);
    RUN_TEST(test_manifest_bad_track_lines_are_skipped);
    RUN_TEST(test_manifest_metadata_without_tracks);
    RUN_TEST(test_manifest_sync_without_dir_no_crash);
    RUN_TEST(test_manifest_track_slot_without_category);
    RUN_TEST(test_manifest_roundtrip_bindings);
    RUN_TEST(test_rev_s23_rename_failure_preserves_live_and_retry);
    RUN_TEST(test_rev_s24_s25_manifest_explicit_empty_roundtrip);
    RUN_TEST(test_rev_s26_partial_restore_marker_and_worker_retry);
    RUN_TEST(test_rev_s27_storage_gate_retains_manifest_dirty);
    RUN_TEST(test_manifest_rejects_truncated_declared_records);
    RUN_TEST(test_manifest_missing_tracks_is_not_intentional_empty);
    RUN_TEST(test_recovery_excludes_competing_writes_without_recursive_lock);
    return UNITY_END();
}
