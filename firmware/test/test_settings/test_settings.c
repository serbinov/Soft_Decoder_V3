#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static esp_err_t g_set_error;
static esp_err_t g_commit_error;
static unsigned g_set_calls;
static unsigned g_commit_calls;
static unsigned g_fail_at;
static void (*g_blob_hook)(void);
static void (*g_before_take_hook)(void);
static unsigned g_lock_depth;
static BaseType_t test_settings_take(SemaphoreHandle_t lock, TickType_t ticks);
static BaseType_t test_settings_give(SemaphoreHandle_t lock);
static esp_err_t test_set_u8(nvs_handle_t h, const char *key, uint8_t value);
static esp_err_t test_set_u16(nvs_handle_t h, const char *key, uint16_t value);
static esp_err_t test_set_str(nvs_handle_t h, const char *key, const char *value);
static esp_err_t test_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len);
static esp_err_t test_commit(nvs_handle_t h);
#define nvs_set_u8 test_set_u8
#define nvs_set_u16 test_set_u16
#define nvs_set_str test_set_str
#define nvs_set_blob test_set_blob
#define nvs_commit test_commit
#define xSemaphoreTake test_settings_take
#define xSemaphoreGive test_settings_give

/* White-box include of the CV store (CRC + factory reset + read-only CVs).
 * Standard headers are pulled in before `static` is redefined so the test
 * stubs' includes (esp_log.h -> stdio.h) cannot leak external-linkage inline
 * definitions into Unity's translation unit. */
#define static
#include "../../components/settings/src/settings.c"
#undef static
#undef nvs_set_u8
#undef nvs_set_u16
#undef nvs_set_str
#undef nvs_set_blob
#undef nvs_commit
#undef xSemaphoreTake
#undef xSemaphoreGive

/* Host stubs for ESP-IDF services (nvs, freertos, ...). */
#include "../../test_libs/teststubs/stubs.c"

static BaseType_t test_settings_take(SemaphoreHandle_t lock, TickType_t ticks)
{
    if (g_before_take_hook != NULL) g_before_take_hook();
    BaseType_t result = xSemaphoreTake(lock, ticks);
    if (result == pdTRUE) ++g_lock_depth;
    return result;
}

static BaseType_t test_settings_give(SemaphoreHandle_t lock)
{
    TEST_ASSERT_TRUE(g_lock_depth > 0U);
    --g_lock_depth;
    return xSemaphoreGive(lock);
}

static esp_err_t test_set_error(void)
{
    ++g_set_calls;
    return g_set_error != ESP_OK && (g_fail_at == 0U || g_fail_at == g_set_calls) ? g_set_error : ESP_OK;
}
static esp_err_t test_set_u8(nvs_handle_t h, const char *key, uint8_t value)
{
    esp_err_t err = test_set_error();
    return err == ESP_OK ? nvs_set_u8(h, key, value) : err;
}
static esp_err_t test_set_u16(nvs_handle_t h, const char *key, uint16_t value)
{
    esp_err_t err = test_set_error();
    return err == ESP_OK ? nvs_set_u16(h, key, value) : err;
}
static esp_err_t test_set_str(nvs_handle_t h, const char *key, const char *value)
{
    esp_err_t err = test_set_error();
    return err == ESP_OK ? nvs_set_str(h, key, value) : err;
}
static esp_err_t test_set_blob(nvs_handle_t h, const char *key, const void *value, size_t len)
{
    esp_err_t err = test_set_error();
    if (g_blob_hook != NULL) g_blob_hook();
    return err == ESP_OK ? nvs_set_blob(h, key, value, len) : err;
}
static esp_err_t test_commit(nvs_handle_t h)
{
    ++g_commit_calls;
    return g_commit_error == ESP_OK ? nvs_commit(h) : g_commit_error;
}

/* settings.c calls the metadata-manifest sync on every save; the host build has
 * no VFS, so provide no-op stubs (the manifest itself is not under test here). */
esp_err_t settings_manifest_sync(void) { return ESP_OK; }
esp_err_t settings_manifest_load(void) { return ESP_ERR_NOT_FOUND; }
bool settings_manifest_write_allowed(void) { return true; }

/* Guard against an out-of-band version bump: version.txt is the single source
 * of truth for the firmware version, and CV7 (decoder version) must match its
 * minor. If someone edits version.txt without updating settings.c, this fails. */
static int version_txt_minor(void)
{
    const char *paths[] = { "version.txt", "firmware/version.txt" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "r");
        if (f == NULL) {
            continue;
        }
        char buf[32] = { 0 };
        size_t n = fread(buf, 1, sizeof(buf) - 1U, f);
        fclose(f);
        buf[n] = '\0';
        const char *dot = strchr(buf, '.');
        if (dot != NULL) {
            return atoi(dot + 1);
        }
    }
    return -1;
}

static void test_cv7_matches_version_txt(void)
{
    int minor = version_txt_minor();
    if (minor < 0) {
        TEST_IGNORE_MESSAGE("version.txt not reachable from CWD");
        return;
    }
    TEST_ASSERT_EQUAL_INT(minor, (int)s_cv[7]);
}

void setUp(void)
{
    mock_sem_take_fail = 0;
    mock_nvs_reset();
    g_set_error = ESP_OK;
    g_commit_error = ESP_OK;
    g_set_calls = 0;
    g_commit_calls = 0;
    g_fail_at = 0;
    g_blob_hook = NULL;
    g_before_take_hook = NULL;
    g_lock_depth = 0;
    mock_timer_now_us = 0;
    memset(&s_cv, 0, sizeof(s_cv));
    cv_set_defaults();
    s_lock = (SemaphoreHandle_t)1;
    s_pending = false;
    s_cv_pending = false;
    s_manifest_pending = false;
    s_pending_us = 0;
    s_last_error = ESP_OK;
    s_retries = 0;
    s_cv_generation = 0;
    s_cfg_valid = false;
    s_ready = true;
    settings_config_t cfg;
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&cfg));
}

void tearDown(void)
{
}

/* ---- CRC ---- */

static void test_crc32_deterministic(void)
{
    uint8_t values[4] = {1, 2, 3, 4};
    uint32_t h1 = cv_crc32(values, sizeof(values));
    uint32_t h2 = cv_crc32(values, sizeof(values));
    TEST_ASSERT_EQUAL_UINT32(h1, h2);
}

static void test_crc32_differs_on_change(void)
{
    uint8_t a[4] = {1, 2, 3, 4};
    uint8_t b[4] = {1, 2, 3, 5};
    TEST_ASSERT_NOT_EQUAL(cv_crc32(a, sizeof(a)), cv_crc32(b, sizeof(b)));
}

/* ---- CV defaults ---- */

static void test_defaults(void)
{
    TEST_ASSERT_EQUAL_UINT8(3, s_cv[1]);
    TEST_ASSERT_EQUAL_UINT8(0, s_cv[2]);           /* Vstart (slow low steps) */
    TEST_ASSERT_EQUAL_UINT8(255, s_cv[5]);         /* Vhigh = full */
    TEST_ASSERT_EQUAL_UINT8(128, s_cv[6]);         /* Vmid = half */
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_CV7_VERSION, s_cv[7]);
    TEST_ASSERT_EQUAL_UINT8(0, s_cv[8]);           /* manufacturer (read-only) */
    TEST_ASSERT_EQUAL_UINT8(0x02, s_cv[29]);       /* 28 steps, DCC */
    TEST_ASSERT_EQUAL_UINT8(128, s_cv[54]);
    TEST_ASSERT_EQUAL_UINT8(60, s_cv[55]);
    TEST_ASSERT_EQUAL_UINT8(32, s_cv[56]);
}

/* CV67..CV94 are the 28-point speed table, generated as (i*255)/27. */
static void test_defaults_speed_table(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, s_cv[67]);
    TEST_ASSERT_EQUAL_UINT8(255, s_cv[94]);
    for (uint16_t i = 0; i < 28; ++i) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((i * 255U) / 27U), s_cv[67 + i]);
    }
    for (uint16_t i = 1; i < 28; ++i) {
        TEST_ASSERT_TRUE(s_cv[67 + i] >= s_cv[67 + i - 1]);
    }
}

/* Old factory curve (Vstart=24, Vmid/Vhigh=0) is migrated to the linear curve
 * on load, while a curve the user tuned is preserved. */
static void test_cv_curve_migration(void)
{
    s_cv[2] = 24;
    s_cv[5] = 0;
    s_cv[6] = 0;
    cv_migrate_curve();
    TEST_ASSERT_EQUAL_UINT8(0, s_cv[2]);
    TEST_ASSERT_EQUAL_UINT8(255, s_cv[5]);
    TEST_ASSERT_EQUAL_UINT8(128, s_cv[6]);

    /* Tuned values must not be rewritten. */
    s_cv[2] = 10;
    s_cv[5] = 200;
    s_cv[6] = 90;
    cv_migrate_curve();
    TEST_ASSERT_EQUAL_UINT8(10, s_cv[2]);
    TEST_ASSERT_EQUAL_UINT8(200, s_cv[5]);
    TEST_ASSERT_EQUAL_UINT8(90, s_cv[6]);
}

/* ---- CV read/write ---- */

static void test_cv_read_write(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 42));
    uint8_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(1, &v));
    TEST_ASSERT_EQUAL_UINT8(42, v);
}

static void test_cv7_readonly(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_write(7, 5));
    uint8_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(7, &v));
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_CV7_VERSION, v);
}

static void test_cv8_not_factory_reset(void)
{
    /* Any value other than 8 is ignored (manufacturer id is read-only). */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_write(8, 42));
}

static void test_cv8_factory_reset(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 42));
    TEST_ASSERT_EQUAL_UINT8(42, s_cv[1]);

    /* NMRA: writing 8 to CV8 restores factory defaults. */
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(8, 8));
    TEST_ASSERT_EQUAL_UINT8(3, s_cv[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, s_cv[29]);
}

static void test_cv_out_of_range(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_write(0, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_write(SETTINGS_CV_COUNT + 1, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_read(0, &s_cv[0]));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_read(1, NULL));
}

static void test_cv_reset_to_factory_direct(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 42));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(2, 99));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_reset_to_factory());
    TEST_ASSERT_EQUAL_UINT8(3, s_cv[1]);
    TEST_ASSERT_EQUAL_UINT8(0, s_cv[2]);
    TEST_ASSERT_EQUAL_UINT8(255, s_cv[5]);
    TEST_ASSERT_EQUAL_UINT8(128, s_cv[6]);
}

/* Commit must persist the CV blob and a matching CRC to NVS. */
static void test_cv_commit_persists_blob_and_crc(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 42));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_commit());

    uint8_t blob[CV_RECORD_SIZE];
    size_t len = sizeof(blob);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(s_h, "cv_record", blob, &len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(blob), (uint32_t)len);
    TEST_ASSERT_EQUAL_UINT8(42, blob[CV_RECORD_HEADER + 1U]);

    uint32_t crc = 0;
    for (unsigned i = 0; i < 4; ++i) crc |= (uint32_t)blob[sizeof(blob) - 4U + i] << (8U * i);
    TEST_ASSERT_EQUAL_UINT32(record_crc32(blob, len - 4U), crc);
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_get_u32(s_h, "cv_crc", &crc));
}

/* ---- settings_config_t ---- */

static void test_config_defaults(void)
{
    settings_config_t cfg;
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&cfg));
    TEST_ASSERT_EQUAL_UINT8(1, cfg.wifi_mode);
    TEST_ASSERT_EQUAL_STRING("ADDITIPUS AURA-X", cfg.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("", cfg.ap_password);
    TEST_ASSERT_EQUAL_STRING("", cfg.sta_ssid);
    TEST_ASSERT_EQUAL_UINT16(80, cfg.port);
    TEST_ASSERT_EQUAL_UINT8(1, cfg.hold);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.auto_off_min);
    TEST_ASSERT_EQUAL_UINT8(20, cfg.master_volume);
    TEST_ASSERT_EQUAL_UINT8(20, cfg.engine_volume);
    TEST_ASSERT_EQUAL_UINT8(20, cfg.effects_volume);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.active_slot);
    TEST_ASSERT_EQUAL_UINT8(0, cfg.control_source);
    TEST_ASSERT_EQUAL_STRING("DECODER", cfg.device_name);
}

static void test_config_roundtrip(void)
{
    settings_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.wifi_mode = 3;
    strncpy(cfg.ap_ssid, "MyAP", sizeof(cfg.ap_ssid) - 1);
    strncpy(cfg.ap_password, "secret123", sizeof(cfg.ap_password) - 1);
    strncpy(cfg.sta_ssid, "Home", sizeof(cfg.sta_ssid) - 1);
    strncpy(cfg.sta_password, "wpa2pass", sizeof(cfg.sta_password) - 1);
    cfg.port = 8080;
    cfg.hold = 0;
    cfg.auto_off_min = 15;
    cfg.master_volume = 55;
    cfg.engine_volume = 70;
    cfg.effects_volume = 30;
    cfg.active_slot = 7;
    cfg.control_source = 1;
    strncpy(cfg.device_name, "Loco-1", sizeof(cfg.device_name) - 1);

    TEST_ASSERT_EQUAL(ESP_OK, settings_save(&cfg));

    settings_config_t got;
    memset(&got, 0, sizeof(got));
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&got));
    TEST_ASSERT_EQUAL_UINT8(cfg.wifi_mode, got.wifi_mode);
    TEST_ASSERT_EQUAL_STRING(cfg.ap_ssid, got.ap_ssid);
    TEST_ASSERT_EQUAL_STRING(cfg.ap_password, got.ap_password);
    TEST_ASSERT_EQUAL_STRING(cfg.sta_ssid, got.sta_ssid);
    TEST_ASSERT_EQUAL_STRING(cfg.sta_password, got.sta_password);
    TEST_ASSERT_EQUAL_UINT16(cfg.port, got.port);
    TEST_ASSERT_EQUAL_UINT8(cfg.hold, got.hold);
    TEST_ASSERT_EQUAL_UINT8(cfg.auto_off_min, got.auto_off_min);
    TEST_ASSERT_EQUAL_UINT8(cfg.master_volume, got.master_volume);
    TEST_ASSERT_EQUAL_UINT8(cfg.engine_volume, got.engine_volume);
    TEST_ASSERT_EQUAL_UINT8(cfg.effects_volume, got.effects_volume);
    TEST_ASSERT_EQUAL_UINT8(cfg.active_slot, got.active_slot);
    TEST_ASSERT_EQUAL_UINT8(cfg.control_source, got.control_source);
    TEST_ASSERT_EQUAL_STRING(cfg.device_name, got.device_name);
}

static void test_config_null_args(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_load(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_save(NULL));
}

/* ---- Sound tracks ---- */

static void test_tracks_missing_returns_not_found(void)
{
    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_tracks_load(tracks, &count));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)count);
}

static void test_tracks_roundtrip(void)
{
    settings_track_t in[2];
    memset(in, 0, sizeof(in));
    in[0].slot = 1;
    strncpy(in[0].file, "audio/slot1.wav", sizeof(in[0].file) - 1);
    strncpy(in[0].label, "Engine", sizeof(in[0].label) - 1);
    in[0].enabled = true;
    in[1].slot = 5;
    strncpy(in[1].file, "audio/slot5.wav", sizeof(in[1].file) - 1);
    strncpy(in[1].label, "Horn", sizeof(in[1].label) - 1);
    in[1].enabled = false;

    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(in, 2));

    settings_track_t out[SETTINGS_MAX_TRACKS];
    memset(out, 0, sizeof(out));
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(out, &count));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)count);
    TEST_ASSERT_EQUAL_UINT8(1, out[0].slot);
    TEST_ASSERT_EQUAL_STRING("audio/slot1.wav", out[0].file);
    TEST_ASSERT_EQUAL_STRING("Engine", out[0].label);
    TEST_ASSERT_TRUE(out[0].enabled);
    TEST_ASSERT_EQUAL_UINT8(5, out[1].slot);
    TEST_ASSERT_FALSE(out[1].enabled);
}

static void test_tracks_empty_and_full(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(NULL, 0));
    settings_track_t out[SETTINGS_MAX_TRACKS];
    size_t count = 7;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(out, &count));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)count);

    settings_track_t in[SETTINGS_MAX_TRACKS];
    memset(in, 0, sizeof(in));
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        in[i].slot = (uint8_t)(i + 1U);
        in[i].enabled = true;
    }
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(in, SETTINGS_MAX_TRACKS));
    count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(out, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_MAX_TRACKS, (uint32_t)count);
}

static void test_tracks_null_args(void)
{
    settings_track_t t;
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_tracks_load(NULL, &count));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_tracks_load(&t, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_tracks_save(NULL, 3));
}

/* ---- Track categories ---- */

static void test_track_cats_default_layout(void)
{
    uint8_t cats[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_track_cats_load(cats, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_MAX_TRACKS, (uint32_t)count);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_ENGINE, cats[0]);
    for (size_t i = 1; i < SETTINGS_MAX_TRACKS; ++i) {
        TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_EFFECTS, cats[i]);
    }
}

static void test_track_cats_roundtrip(void)
{
    uint8_t in[SETTINGS_MAX_TRACKS];
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        in[i] = SETTINGS_TRACK_CAT_EFFECTS;
    }
    in[4] = SETTINGS_TRACK_CAT_ENGINE;
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_save(in, SETTINGS_MAX_TRACKS));

    uint8_t out[SETTINGS_MAX_TRACKS];
    memset(out, 0xFF, sizeof(out));
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_load(out, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_MAX_TRACKS, (uint32_t)count);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_EFFECTS, out[0]);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_ENGINE, out[4]);
}

/* ---- Function map ---- */

static void test_func_map_defaults(void)
{
    settings_func_map_t map[SETTINGS_FUNC_MAP_COUNT];
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_func_map_load(map, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_FUNC_MAP_COUNT, (uint32_t)count);

    /* F0 = directional head light. */
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R, map[0].aux_mask);
    TEST_ASSERT_EQUAL_UINT8(0, map[0].slot_a);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_FUNC_DIR_NONE, map[0].dir);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_FUNC_SPD_NONE, map[0].speed);

    /* F1..F20 map to the matching sound slot; F21+ are off. */
    for (uint8_t f = 1; f <= 20U; ++f) {
        TEST_ASSERT_EQUAL_UINT8(f, map[f].slot_a);
        TEST_ASSERT_EQUAL_UINT8(0, map[f].slot_b);
        TEST_ASSERT_EQUAL_UINT16(0, map[f].aux_mask);
    }
    for (uint8_t f = 21; f < SETTINGS_FUNC_MAP_COUNT; ++f) {
        TEST_ASSERT_EQUAL_UINT8(0, map[f].slot_a);
    }
}

static void test_func_map_roundtrip(void)
{
    settings_func_map_t map[SETTINGS_FUNC_MAP_COUNT];
    memset(map, 0, sizeof(map));
    size_t count = 0;
    (void)settings_func_map_load(map, &count);

    map[3].slot_a = 4;
    map[3].slot_b = 9;
    map[3].aux_mask = SETTINGS_FUNC_OUT_AUX1 | SETTINGS_FUNC_OUT_AUX7;
    map[3].dir = SETTINGS_FUNC_DIR_FWD;
    map[3].speed = SETTINGS_FUNC_SPD_MOVING;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_map_save(map, SETTINGS_FUNC_MAP_COUNT));

    settings_func_map_t got[SETTINGS_FUNC_MAP_COUNT];
    memset(got, 0, sizeof(got));
    count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_map_load(got, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_FUNC_MAP_COUNT, (uint32_t)count);
    TEST_ASSERT_EQUAL_UINT8(4, got[3].slot_a);
    TEST_ASSERT_EQUAL_UINT8(9, got[3].slot_b);
    TEST_ASSERT_EQUAL_UINT16(map[3].aux_mask, got[3].aux_mask);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_FUNC_DIR_FWD, got[3].dir);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_FUNC_SPD_MOVING, got[3].speed);
}

/* A blob written by an older firmware (wrong size) must be ignored and the
 * hobby defaults returned instead of a partially valid map. */
static void test_func_map_stale_blob_uses_defaults(void)
{
    uint8_t stale[3] = {1, 2, 3};
    TEST_ASSERT_EQUAL(ESP_OK, mock_nvs_force_blob("func_map", stale, sizeof(stale)));

    settings_func_map_t map[SETTINGS_FUNC_MAP_COUNT];
    memset(map, 0xFF, sizeof(map));
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_func_map_load(map, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_FUNC_MAP_COUNT, (uint32_t)count);
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R, map[0].aux_mask);
    TEST_ASSERT_EQUAL_UINT8(1, map[1].slot_a);
}

/* ---- AUX configuration ---- */

static void test_aux_cfg_defaults(void)
{
    settings_aux_cfg_t cfg[SETTINGS_AUX_COUNT];
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_aux_cfg_load(cfg, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_AUX_COUNT, (uint32_t)count);
    for (size_t i = 0; i < SETTINGS_AUX_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT8(100, cfg[i].level);
        TEST_ASSERT_EQUAL_UINT8(0, cfg[i].effect);
    }
}

static void test_aux_cfg_roundtrip(void)
{
    settings_aux_cfg_t cfg[SETTINGS_AUX_COUNT];
    memset(cfg, 0, sizeof(cfg));
    cfg[2].level = 40;
    cfg[2].effect = 3;
    cfg[8].level = 90;
    cfg[8].effect = 6;
    TEST_ASSERT_EQUAL(ESP_OK, settings_aux_cfg_save(cfg, SETTINGS_AUX_COUNT));

    settings_aux_cfg_t got[SETTINGS_AUX_COUNT];
    memset(got, 0, sizeof(got));
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_aux_cfg_load(got, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_AUX_COUNT, (uint32_t)count);
    TEST_ASSERT_EQUAL_UINT8(40, got[2].level);
    TEST_ASSERT_EQUAL_UINT8(3, got[2].effect);
    TEST_ASSERT_EQUAL_UINT8(90, got[8].level);
    TEST_ASSERT_EQUAL_UINT8(6, got[8].effect);
}

static void test_aux_cfg_stale_blob_uses_defaults(void)
{
    uint8_t stale[5] = {9, 9, 9, 9, 9};
    TEST_ASSERT_EQUAL(ESP_OK, mock_nvs_force_blob("aux_cfg", stale, sizeof(stale)));

    settings_aux_cfg_t cfg[SETTINGS_AUX_COUNT];
    memset(cfg, 0, sizeof(cfg));
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_aux_cfg_load(cfg, &count));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_AUX_COUNT, (uint32_t)count);
    TEST_ASSERT_EQUAL_UINT8(100, cfg[0].level);
}

/* ---- BEMF calibration persistence ---- */

static void test_bemf_cal_roundtrip(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 3;
    cal.speed[0] = 12;
    cal.frac[0] = 400;
    cal.speed[1] = 64;
    cal.frac[1] = 620;
    cal.speed[2] = 126;
    cal.frac[2] = 700;

    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&cal));

    settings_bemf_cal_t got;
    memset(&got, 0, sizeof(got));
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_load(&got));
    TEST_ASSERT_EQUAL_UINT8(3, got.count);
    TEST_ASSERT_EQUAL_UINT8(12, got.speed[0]);
    TEST_ASSERT_EQUAL_UINT16(400, got.frac[0]);
    TEST_ASSERT_EQUAL_UINT16(700, got.frac[2]);

    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_clear());
    TEST_ASSERT_NOT_EQUAL(ESP_OK, settings_bemf_cal_load(&got));
}

static void test_bemf_cal_missing_and_invalid_count(void)
{
    settings_bemf_cal_t got;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, settings_bemf_cal_load(&got));

    /* A stored point count beyond the array bound must be rejected. */
    settings_bemf_cal_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.count = SETTINGS_BEMF_CAL_MAX_POINTS + 1U;
    TEST_ASSERT_EQUAL(ESP_OK, mock_nvs_force_blob("bemf_cal", &bad, sizeof(bad)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, settings_bemf_cal_load(&got));
}

static void test_bemf_cal_null_args(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_load(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_save(NULL));
}

/* ---- BEMF regulation flag ---- */

static void test_bemf_use_default_and_roundtrip(void)
{
    bool enabled = false;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_load(&enabled));
    TEST_ASSERT_TRUE(enabled); /* default: closed loop enabled */

    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_save(false));
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_load(&enabled));
    TEST_ASSERT_FALSE(enabled);

    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_save(true));
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_load(&enabled));
    TEST_ASSERT_TRUE(enabled);
}

static void test_bemf_use_null_arg(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_use_load(NULL));
}

/* ---- Factory reset ---- */

static void test_factory_reset(void)
{
    /* Change a CV and store a calibration, then wipe everything. */
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 42));
    TEST_ASSERT_EQUAL_UINT8(42, s_cv[1]);

    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 2;
    cal.speed[0] = 12;
    cal.frac[0] = 100;
    cal.speed[1] = 126;
    cal.frac[1] = 700;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&cal));

    /* Disable closed-loop regulation so the reset has something to clear. */
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_save(false));

    TEST_ASSERT_EQUAL(ESP_OK, settings_factory_reset());

    /* CVs are back to factory defaults. */
    TEST_ASSERT_EQUAL_UINT8(3, s_cv[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, s_cv[29]);
    /* Stored calibration is gone. */
    settings_bemf_cal_t got;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, settings_bemf_cal_load(&got));
    /* The regulation flag is back to its enabled default. */
    bool bemf_use = false;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_load(&bemf_use));
    TEST_ASSERT_TRUE(bemf_use);
}

/* Factory reset also clears config, tracks, categories, func map and AUX cfg. */
static void test_factory_reset_clears_all_stores(void)
{
    settings_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.wifi_mode = 2;
    cfg.port = 9000;
    strncpy(cfg.device_name, "X", sizeof(cfg.device_name) - 1);
    (void)settings_save(&cfg);

    settings_track_t track;
    memset(&track, 0, sizeof(track));
    track.slot = 1;
    track.enabled = true;
    (void)settings_tracks_save(&track, 1);

    uint8_t cats[SETTINGS_MAX_TRACKS];
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        cats[i] = SETTINGS_TRACK_CAT_ENGINE;
    }
    (void)settings_track_cats_save(cats, SETTINGS_MAX_TRACKS);

    TEST_ASSERT_EQUAL(ESP_OK, settings_factory_reset());

    settings_config_t got;
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&got));
    TEST_ASSERT_EQUAL_UINT8(1, got.wifi_mode);
    TEST_ASSERT_EQUAL_UINT16(80, got.port);
    TEST_ASSERT_EQUAL_STRING("DECODER", got.device_name);

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_tracks_load(tracks, &count));

    uint8_t got_cats[SETTINGS_MAX_TRACKS];
    count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_track_cats_load(got_cats, &count));
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_ENGINE, got_cats[0]);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_EFFECTS, got_cats[1]);
}

/* ---- settings_init / deferred write / error branches ---- */

static void test_nvs_read_u8_error(void)
{
    uint8_t v = 0;
    mock_nvs_get_u8_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, nvs_read_u8("k", &v, 0));
    mock_nvs_get_u8_err = 0;
}

static void test_settings_init_branches(void)
{
    mock_nvs_reset();
    mock_nvs_flash_init_err = 0;
    mock_nvs_open_fail = 0;
    mock_mutex_create_fail = 0;

    TEST_ASSERT_EQUAL(ESP_OK, settings_init()); /* fresh NVS -> defaults */

    mock_nvs_flash_init_err = ESP_ERR_NVS_NO_FREE_PAGES;
    TEST_ASSERT_EQUAL(ESP_OK, settings_init()); /* erase + reinit */
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_flash_init_err);

    mock_nvs_flash_init_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_init());
    mock_nvs_flash_init_err = 0;

    mock_nvs_open_fail = 1;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_init());
    mock_nvs_open_fail = 0;

    mock_mutex_create_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, settings_init());
    mock_mutex_create_fail = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_init()); /* restore s_lock */
}

static void test_settings_init_loads_valid_cv(void)
{
    mock_nvs_reset();
    s_lock = (SemaphoreHandle_t)1;
    memset(&s_cv, 0, sizeof(s_cv));
    cv_set_defaults();
    s_cv[1] = 42;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_commit()); /* writes cv + crc */
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());      /* valid blob -> loaded */
    TEST_ASSERT_EQUAL_UINT8(42, s_cv[1]);
}

static void test_settings_save_deferred_and_flush(void)
{
    settings_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.wifi_mode = 1;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_save_deferred(NULL));

    mock_timer_now_us = 1000000;
    TEST_ASSERT_EQUAL(ESP_OK, settings_save_deferred(&cfg));
    settings_pending_flush(); /* fresh -> no-op */
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    settings_pending_flush(); /* idle reached -> commit */
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    settings_pending_flush(); /* nothing pending -> early return */

    /* Timeout paths. */
    TEST_ASSERT_EQUAL(ESP_OK, settings_save_deferred(&cfg));
    mock_sem_take_fail = 1;
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    settings_pending_flush(); /* take fails -> early return */
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_save(&cfg));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_save_deferred(&cfg));
    mock_sem_take_fail = 0;
}

static void test_settings_save_api_invalid_args(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_track_cats_load(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_map_load(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_aux_cfg_load(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_track_cats_save(NULL, 3));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_map_save(NULL, 3));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_aux_cfg_save(NULL, 3));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_save(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_load(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_use_load(NULL));
}

static void test_settings_save_api_timeouts(void)
{
    settings_track_t t;
    memset(&t, 0, sizeof(t));
    t.slot = 1;
    uint8_t cats[1] = { 1 };
    settings_func_map_t m[SETTINGS_FUNC_MAP_COUNT];
    memset(m, 0, sizeof(m));
    settings_aux_cfg_t a[SETTINGS_AUX_COUNT];
    memset(a, 0, sizeof(a));
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));

    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_cv_commit());
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_tracks_save(&t, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_track_cats_save(cats, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_func_map_save(m, SETTINGS_FUNC_MAP_COUNT));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_aux_cfg_save(a, SETTINGS_AUX_COUNT));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_save(&cal));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_bemf_cal_clear());
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_bemf_use_save(true));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_factory_reset());
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(5, 1));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_reset_to_factory());
    mock_sem_take_fail = 0;
}

/* A deferred CV commit is flushed (blob + crc + commit) only after the
 * stability delay, and is a no-op when nothing is pending. */
static void test_cv_commit_deferred_flush(void)
{
    s_lock = (SemaphoreHandle_t)1;
    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(5, 42));
    settings_cv_commit_deferred();
    mock_timer_now_us = 0;
    settings_pending_flush();          /* too early: not committed yet */
    mock_timer_now_us = 3000000;       /* beyond SETTINGS_FLUSH_DELAY_US */
    settings_pending_flush();
    uint8_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(5, &v));
    TEST_ASSERT_EQUAL_UINT8(42, v);
    settings_pending_flush();          /* nothing pending: no-op */
}

static void test_bemf_cal_clear_missing_key_is_ok(void)
{
    mock_nvs_reset();
    s_lock = (SemaphoreHandle_t)1;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_clear());
}

/* ---- CV63 master-volume alias (R6.1) ---- */

static void test_cv63_master_volume_alias(void)
{
    uint8_t v = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(63, 255));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(63, &v));
    TEST_ASSERT_EQUAL_UINT8(255, v); /* 100 % <-> CV 255 */

    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(63, 128));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(63, &v));
    TEST_ASSERT_EQUAL_UINT8(127, v); /* 128 -> 50 % -> 127 */

    mock_nvs_reset();
    s_lock = (SemaphoreHandle_t)1;
    s_cfg_valid = false;
    settings_config_t cfg;
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(63, &v));
    TEST_ASSERT_EQUAL_UINT8(51, v); /* default 20 % when mvol is absent */

    (void)nvs_set_u8(1, "mvol", 200); /* out-of-range % is clamped */
    s_cfg_valid = false;
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&cfg));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(63, &v));
    TEST_ASSERT_EQUAL_UINT8(255, v);

    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_cv_write(63, 10));
    mock_sem_take_fail = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_read(63, NULL));
}

static void test_cv7_migrated_from_old_blob(void)
{
    s_cv[7] = 7; /* simulate a device upgraded from an older firmware */
    cv_migrate_version();
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_CV7_VERSION, s_cv[7]);
    cv_migrate_version(); /* idempotent */
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_CV7_VERSION, s_cv[7]);
}

/* ---- function bindings (R1) ---- */

static void test_func_bind_roundtrip(void)
{
    func_binding_t b[FUNC_BIND_MAX];
    size_t n = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_func_bind_load(b, &n));
    TEST_ASSERT_EQUAL_UINT32(0, n);
    TEST_ASSERT_EQUAL_UINT8(0, b[0].used);

    memset(b, 0, sizeof(b));
    b[0].used = 1;
    b[0].fn = 2;
    b[0].target_type = FUNC_TARGET_SOUND;
    b[0].target_id = 3;
    b[0].mode = SOUND_MODE_SHORT_LONG;
    b[0].flags = FUNC_FLAG_DUCK;
    b[0].short_table = 7;
    b[0].short_ms = 400;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_save(b, 1));

    func_binding_t back[FUNC_BIND_MAX];
    size_t bn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_load(back, &bn));
    TEST_ASSERT_EQUAL_UINT32(1, bn);
    TEST_ASSERT_EQUAL_UINT8(2, back[0].fn);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_SOUND, back[0].target_type);
    TEST_ASSERT_EQUAL_UINT8(3, back[0].target_id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_MODE_SHORT_LONG, back[0].mode);
    TEST_ASSERT_EQUAL_UINT8(FUNC_FLAG_DUCK, back[0].flags);
    TEST_ASSERT_EQUAL_UINT16(400, back[0].short_ms);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_load(NULL, &bn));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_load(back, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_save(NULL, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_save(back, FUNC_BIND_MAX + 1U));

    /* A blob that is not a whole number of bindings is rejected. */
    uint8_t junk[3] = { 1, 2, 3 };
    TEST_ASSERT_EQUAL(ESP_OK, mock_nvs_force_blob("func_bind", junk, sizeof(junk)));
    size_t jn = 99;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, settings_func_bind_load(back, &jn));
    TEST_ASSERT_EQUAL_UINT32(0, jn);

    /* Lock timeout paths. */
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_func_bind_save(b, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_active_scheme_set("x"));
    mock_sem_take_fail = 0;
}

static void test_func_bind_legacy_convert(void)
{
    settings_func_map_t m[SETTINGS_FUNC_MAP_COUNT];
    memset(m, 0, sizeof(m));
    /* F0: directional head light with a reverse gate -> only F0R. */
    m[0].aux_mask = SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R;
    m[0].dir = SETTINGS_FUNC_DIR_REV;
    m[0].speed = SETTINGS_FUNC_SPD_MOVING;
    /* F1: AUX1 (bit 2) forward-only while stopped + two sound slots. */
    m[1].aux_mask = SETTINGS_FUNC_OUT_AUX1;
    m[1].dir = SETTINGS_FUNC_DIR_FWD;
    m[1].speed = SETTINGS_FUNC_SPD_STOP;
    m[1].slot_a = 1;
    m[1].slot_b = 2;
    /* F2: head light with no function gate -> both directions. */
    m[2].aux_mask = SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R;

    func_binding_t out[FUNC_BIND_MAX];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_legacy_convert(m, SETTINGS_FUNC_MAP_COUNT,
                                                                 out, &n));
    TEST_ASSERT_EQUAL_UINT32(6, n);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_OUTPUT, out[0].target_type);
    TEST_ASSERT_EQUAL_UINT8(1, out[0].target_id); /* F0R only */
    TEST_ASSERT_EQUAL_UINT8(FUNC_DIR_REV, out[0].dir);
    TEST_ASSERT_EQUAL_UINT8(FUNC_STATE_MOVING, out[0].state);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_OUTPUT, out[1].target_type);
    TEST_ASSERT_EQUAL_UINT8(2, out[1].target_id);
    TEST_ASSERT_EQUAL_UINT8(FUNC_DIR_FWD, out[1].dir);
    TEST_ASSERT_EQUAL_UINT8(FUNC_STATE_STOPPED, out[1].state);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_SLOT, out[2].target_type);
    TEST_ASSERT_EQUAL_UINT8(1, out[2].target_id);
    TEST_ASSERT_EQUAL_UINT8(SOUND_MODE_LATCHED, out[2].mode);
    TEST_ASSERT_EQUAL_UINT8(2, out[3].target_id);
    TEST_ASSERT_EQUAL_UINT8(0, out[4].target_id); /* F2F forward */
    TEST_ASSERT_EQUAL_UINT8(FUNC_DIR_FWD, out[4].dir);
    TEST_ASSERT_EQUAL_UINT8(1, out[5].target_id); /* F2R reverse */
    TEST_ASSERT_EQUAL_UINT8(FUNC_DIR_REV, out[5].dir);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_legacy_convert(NULL, 29, out, &n));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_legacy_convert(m, 29, NULL, &n));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_legacy_convert(m, 29, out, NULL));

    /* Saturated map: the output is capped at FUNC_BIND_MAX. */
    for (size_t i = 0; i < SETTINGS_FUNC_MAP_COUNT; ++i) {
        m[i].aux_mask = SETTINGS_FUNC_OUT_ALL;
        m[i].slot_a = 1;
        m[i].slot_b = 2;
    }
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_legacy_convert(m, SETTINGS_FUNC_MAP_COUNT,
                                                                 out, &n));
    TEST_ASSERT_EQUAL_UINT32(FUNC_BIND_MAX, n);
}

static void test_func_bind_add_remove_find(void)
{
    func_binding_t list[FUNC_BIND_MAX];
    memset(list, 0, sizeof(list));
    size_t n = 0;
    func_binding_t b;
    memset(&b, 0, sizeof(b));
    b.used = 1;
    b.fn = 3;
    b.target_type = FUNC_TARGET_SOUND;
    b.target_id = 4;

    TEST_ASSERT_EQUAL(-1, settings_func_bind_find(list, n, &b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_add(NULL, &n, &b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_add(list, NULL, &b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_add(list, &n, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_add(list, &n, &b));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL(0, settings_func_bind_find(list, n, &b));

    func_binding_t c = b;
    c.target_id = 9;
    TEST_ASSERT_EQUAL(-1, settings_func_bind_find(list, n, &c));
    TEST_ASSERT_EQUAL(-1, settings_func_bind_find(NULL, n, &b));
    TEST_ASSERT_EQUAL(-1, settings_func_bind_find(list, n, NULL));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_remove(NULL, &n, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_remove(list, NULL, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_func_bind_remove(list, &n, 5));

    func_binding_t b2 = b;
    b2.target_id = 5;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_add(list, &n, &b2));
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_remove(list, &n, 0)); /* shifts b2 down */
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL(0, settings_func_bind_find(list, n, &b2));

    n = FUNC_BIND_MAX;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, settings_func_bind_add(list, &n, &b));
}

/* ---- active scheme pointer (R1) ---- */

static void test_active_scheme(void)
{
    char buf[64];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_active_scheme_get(NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_active_scheme_get(buf, 0));

    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, settings_active_scheme_get(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_active_scheme_set(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, settings_active_scheme_set("diesel"));
    TEST_ASSERT_EQUAL(ESP_OK, settings_active_scheme_get(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("diesel", buf);
}

static void test_rev_s20_deferred_is_ram_only(void)
{
    settings_config_t cfg;
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&cfg));
    cfg.master_volume = 75;
    TEST_ASSERT_EQUAL(ESP_OK, settings_save_deferred(&cfg));
    TEST_ASSERT_EQUAL_UINT32(0, g_set_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_commit_calls);
    uint8_t value;
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, nvs_get_u8(s_h, "mvol", &value));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(63, 255));
    TEST_ASSERT_EQUAL_UINT32(0, g_set_calls);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(8, 8));
    TEST_ASSERT_EQUAL_UINT32(0, g_set_calls);
    mock_timer_now_us = SETTINGS_FLUSH_DELAY_US + 1;
    TEST_ASSERT_EQUAL(ESP_OK, settings_pending_flush());
    TEST_ASSERT_TRUE(g_set_calls > 0);
}

static void test_rev_s21_all_config_setter_failures_retry(void)
{
    settings_config_t cfg;
    TEST_ASSERT_EQUAL(ESP_OK, settings_load(&cfg));
    cfg.port = 8181;
    for (unsigned failed = 1; failed <= 15U; ++failed) {
        g_set_calls = g_commit_calls = 0;
        g_set_error = ESP_FAIL;
        g_fail_at = failed;
        TEST_ASSERT_EQUAL(ESP_FAIL, settings_save(&cfg));
        TEST_ASSERT_EQUAL_UINT32(0, g_commit_calls);
        settings_flush_status_t state;
        settings_flush_status(&state);
        TEST_ASSERT_TRUE(state.config_pending);
        TEST_ASSERT_EQUAL(ESP_FAIL, state.last_error);
    }
    g_set_error = ESP_OK;
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    TEST_ASSERT_EQUAL(ESP_OK, settings_pending_flush());
    settings_flush_status_t state;
    settings_flush_status(&state);
    TEST_ASSERT_FALSE(state.config_pending);
    uint16_t port = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u16(s_h, "port", &port));
    TEST_ASSERT_EQUAL_UINT16(8181, port);
}

static void test_rev_s21_cv_failure_retains_dirty_bounded_retry(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(5, 91));
    g_set_error = ESP_FAIL;
    mock_timer_now_us = SETTINGS_FLUSH_DELAY_US + 1;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_pending_flush());
    unsigned attempts = g_set_calls;
    TEST_ASSERT_EQUAL(ESP_OK, settings_pending_flush());
    TEST_ASSERT_EQUAL_UINT32(attempts, g_set_calls);
    settings_flush_status_t state;
    settings_flush_status(&state);
    TEST_ASSERT_TRUE(state.cv_pending);
    TEST_ASSERT_EQUAL_UINT32(1, state.retries);
    g_set_error = ESP_OK;
    g_commit_error = ESP_FAIL;
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_pending_flush());
    settings_flush_status(&state);
    TEST_ASSERT_TRUE(state.cv_pending);
    g_commit_error = ESP_OK;
    mock_timer_now_us += SETTINGS_FLUSH_DELAY_US + 1;
    TEST_ASSERT_EQUAL(ESP_OK, settings_pending_flush());
    settings_flush_status(&state);
    TEST_ASSERT_FALSE(state.cv_pending);
}

static void test_rev_s22_legacy_migration_preserves_shipped_keys(void)
{
    s_cv[1] = 77;
    TEST_ASSERT_EQUAL(ESP_OK, mock_nvs_force_blob("cv", s_cv, sizeof(s_cv)));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv))));
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_EQUAL_UINT8(77, s_cv[1]);
    uint8_t legacy[sizeof(s_cv)];
    size_t len = sizeof(legacy);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(s_h, "cv", legacy, &len));
    TEST_ASSERT_EQUAL_UINT8(77, legacy[1]);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 88));
    g_set_error = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_cv_commit());
    g_set_error = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, settings_init()); /* power cut before atomic setter */
    TEST_ASSERT_EQUAL_UINT8(77, s_cv[1]);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 99));
    g_commit_error = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_cv_commit());
    g_commit_error = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, settings_init()); /* setter physically wrote before commit */
    TEST_ASSERT_EQUAL_UINT8(99, s_cv[1]);
}

static void test_rev_s22_power_cut_during_legacy_migration(void)
{
    uint8_t legacy[sizeof(s_cv)];
    memcpy(legacy, s_cv, sizeof(legacy));
    legacy[1] = 61;
    TEST_ASSERT_EQUAL(ESP_OK, mock_nvs_force_blob("cv", legacy, sizeof(legacy)));
    uint32_t crc = cv_crc32(legacy, sizeof(legacy));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_set_u32(s_h, "cv_crc", crc));
    g_set_error = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_init());
    settings_flush_status_t state;
    settings_flush_status(&state);
    TEST_ASSERT_FALSE(state.ready);
    TEST_ASSERT_TRUE(state.cv_pending);
    uint8_t unchanged[sizeof(legacy)];
    size_t len = sizeof(unchanged);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(s_h, "cv", unchanged, &len));
    TEST_ASSERT_EQUAL_MEMORY(legacy, unchanged, sizeof(legacy));
    uint32_t saved_crc = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(s_h, "cv_crc", &saved_crc));
    TEST_ASSERT_EQUAL_UINT32(crc, saved_crc);
    g_set_error = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_EQUAL_UINT8(61, s_cv[1]);
    settings_flush_status(&state);
    TEST_ASSERT_TRUE(state.ready);
}

static void concurrent_cv_update(void) { (void)settings_cv_write(5, 123); }
static void test_cv_publish_generation_retains_newer_write(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(5, 71));
    g_blob_hook = concurrent_cv_update;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_commit());
    TEST_ASSERT_TRUE(s_cv_pending);
    g_blob_hook = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_commit());
    TEST_ASSERT_FALSE(s_cv_pending);
    TEST_ASSERT_EQUAL(ESP_OK, settings_init());
    TEST_ASSERT_EQUAL_UINT8(123, s_cv[5]);
}

static void test_cv_snapshot_and_nmra_side_effects(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(29, 0x22));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(19, 25));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 37));
    uint8_t snapshot[SETTINGS_CV_COUNT + 1];
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_snapshot(snapshot));
    TEST_ASSERT_EQUAL_UINT8(37, snapshot[1]);
    TEST_ASSERT_EQUAL_UINT8(2, snapshot[29]);
    TEST_ASSERT_EQUAL_UINT8(0, snapshot[19]);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(8, 8));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_snapshot(snapshot));
    TEST_ASSERT_EQUAL_UINT8(3, snapshot[1]);
    TEST_ASSERT_EQUAL_UINT8(255, snapshot[5]);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_cv_snapshot(NULL));
}

static void test_rev_m11_reject_curve_as_unit(void)
{
    settings_bemf_cal_t cal = { .count = 2, .speed = { 0, 126 }, .frac = { 0, 1024 } };
    TEST_ASSERT_TRUE(settings_bemf_cal_validate(&cal));
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&cal));
    for (unsigned defect = 0; defect < 5; ++defect) {
        settings_bemf_cal_t bad = cal;
        if (defect == 0) bad.count = 1;
        if (defect == 1) bad.speed[1] = 127;
        if (defect == 2) bad.speed[1] = 0;
        if (defect == 3) bad.frac[0] = 1025;
        if (defect == 4) { bad.frac[0] = 10; bad.frac[1] = 9; }
        TEST_ASSERT_FALSE(settings_bemf_cal_validate(&bad));
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_save(&bad));
        TEST_ASSERT_EQUAL(ESP_OK, mock_nvs_force_blob("bemf_cal", &bad, sizeof(bad)));
        settings_bemf_cal_t out;
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_load(&out));
        TEST_ASSERT_EQUAL_UINT8(0, out.count);
    }
    settings_bemf_cal_t full = { .count = SETTINGS_BEMF_CAL_MAX_POINTS };
    for (unsigned i = 0; i < SETTINGS_BEMF_CAL_MAX_POINTS; ++i) {
        full.speed[i] = (uint8_t)(i * 126U / (SETTINGS_BEMF_CAL_MAX_POINTS - 1U));
        full.frac[i] = (uint16_t)(i * 1024U / (SETTINGS_BEMF_CAL_MAX_POINTS - 1U));
    }
    TEST_ASSERT_TRUE(settings_bemf_cal_validate(&full));
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&full));
}

static void test_rev_s25_empty_bindings_are_present(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_save(NULL, 0));
    func_binding_t bindings[FUNC_BIND_MAX];
    size_t count = 99;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_bind_load(bindings, &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

static void test_hard_reset_changes_only_nmra_configuration_cvs(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(1, 45));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(19, 42));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(29, 0x37));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(31, 7));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(32, 9));
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_write(54, 75));
    int before = mock_nvs_set_calls;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_hard_reset());
    TEST_ASSERT_EQUAL_INT(before, mock_nvs_set_calls);
    uint8_t value = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(1, &value));
    TEST_ASSERT_EQUAL_UINT8(45, value);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(19, &value));
    TEST_ASSERT_EQUAL_UINT8(0, value);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(29, &value));
    TEST_ASSERT_EQUAL_UINT8(2, value);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(31, &value));
    TEST_ASSERT_EQUAL_UINT8(0, value);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(32, &value));
    TEST_ASSERT_EQUAL_UINT8(0, value);
    TEST_ASSERT_EQUAL(ESP_OK, settings_cv_read(54, &value));
    TEST_ASSERT_EQUAL_UINT8(75, value);
}

static bool g_save_cancelled;
static unsigned g_authorize_calls;

static bool authorize_cal_save(void *context)
{
    TEST_ASSERT_EQUAL_PTR(&g_save_cancelled, context);
    TEST_ASSERT_EQUAL_UINT32(1, g_lock_depth); /* Persistence lock BEFORE mux check. */
    ++g_authorize_calls;
    return !*(bool *)context;
}

static void cancel_waiting_cal_save(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
    g_save_cancelled = true;
}

static void cancel_admitted_cal_save(void)
{
    TEST_ASSERT_EQUAL_UINT32(1, g_lock_depth);
    TEST_ASSERT_EQUAL_UINT32(1, g_authorize_calls);
    g_save_cancelled = true;
}

static void test_cal_save_guard_rejects_cancel_between_validation_and_lock(void)
{
    settings_bemf_cal_t old = { .count = 2, .speed = {12, 126}, .frac = {100, 700} };
    settings_bemf_cal_t next = old;
    next.frac[1] = 800;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&old));
    unsigned writes = g_set_calls, commits = g_commit_calls;
    g_save_cancelled = false;
    g_authorize_calls = 0;
    g_before_take_hook = cancel_waiting_cal_save;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
        settings_bemf_cal_save_guarded(&next, authorize_cal_save, &g_save_cancelled));
    g_before_take_hook = NULL;
    TEST_ASSERT_EQUAL_UINT32(1, g_authorize_calls);
    TEST_ASSERT_EQUAL_UINT32(writes, g_set_calls);
    TEST_ASSERT_EQUAL_UINT32(commits, g_commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
    settings_bemf_cal_t got;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_load(&got));
    TEST_ASSERT_EQUAL_MEMORY(&old, &got, sizeof(old));
}

static void test_cal_save_guard_cancel_after_admission_finishes_valid_curve(void)
{
    settings_bemf_cal_t cal = { .count = 2, .speed = {12, 126}, .frac = {100, 800} };
    g_save_cancelled = false;
    g_authorize_calls = 0;
    g_blob_hook = cancel_admitted_cal_save;
    TEST_ASSERT_EQUAL(ESP_OK,
        settings_bemf_cal_save_guarded(&cal, authorize_cal_save, &g_save_cancelled));
    TEST_ASSERT_TRUE(g_save_cancelled);
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
    settings_bemf_cal_t got;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_load(&got));
    TEST_ASSERT_EQUAL_MEMORY(&cal, &got, sizeof(cal));
}

static void test_cal_save_guard_never_authorizes_invalid_or_unlocked_save(void)
{
    settings_bemf_cal_t cal = { .count = 1 };
    g_authorize_calls = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
        settings_bemf_cal_save_guarded(&cal, authorize_cal_save, &g_save_cancelled));
    cal.count = 2;
    cal.speed[1] = 126;
    cal.frac[1] = 100;
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
        settings_bemf_cal_save_guarded(&cal, authorize_cal_save, &g_save_cancelled));
    mock_sem_take_fail = 0;
    TEST_ASSERT_EQUAL_UINT32(0, g_authorize_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save_guarded(&cal, NULL, NULL));
}

static void test_unusable_bemf_curve_cannot_replace_previous_record(void)
{
    settings_bemf_cal_t good = { .count = 2, .speed = {12, 126}, .frac = {100, 900} };
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&good));
    settings_bemf_cal_t bad = { .count = 2, .speed = {12, 126}, .frac = {0, 0} };
    TEST_ASSERT_FALSE(settings_bemf_cal_validate(&bad));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, settings_bemf_cal_save(&bad));
    settings_bemf_cal_t loaded = {0};
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_load(&loaded));
    TEST_ASSERT_EQUAL_MEMORY(&good, &loaded, sizeof(good));
    bad.frac[1] = 50;
    TEST_ASSERT_FALSE(settings_bemf_cal_validate(&bad));
    bad.frac[1] = SETTINGS_BEMF_CAL_MIN_END_FRAC;
    TEST_ASSERT_TRUE(settings_bemf_cal_validate(&bad));
}

static void test_bemf_mode_save_propagates_persistence_errors(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_save(true));
    unsigned writes = g_set_calls, commits = g_commit_calls;
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_bemf_use_save(false));
    mock_sem_take_fail = 0;
    TEST_ASSERT_EQUAL_UINT32(writes, g_set_calls);
    TEST_ASSERT_EQUAL_UINT32(commits, g_commit_calls);
    g_set_error = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_bemf_use_save(false));
    TEST_ASSERT_EQUAL_UINT32(commits, g_commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
    bool enabled = false;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_load(&enabled));
    TEST_ASSERT_TRUE(enabled);
    g_set_error = ESP_OK;
    g_commit_error = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_bemf_use_save(false));
    TEST_ASSERT_EQUAL_UINT32(commits + 1U, g_commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
    /* The mock publishes setters immediately. A failed physical commit's
     * durable value cannot be inferred from this host backend. */
    g_commit_error = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_use_save(true));
}

static void test_bemf_curve_save_propagates_io_errors(void)
{
    settings_bemf_cal_t old = { .count = 2, .speed = {12, 126}, .frac = {100, 700} };
    settings_bemf_cal_t next = old;
    next.frac[1] = 800;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&old));
    unsigned commits = g_commit_calls;
    g_set_error = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_bemf_cal_save(&next));
    TEST_ASSERT_EQUAL_UINT32(commits, g_commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
    settings_bemf_cal_t loaded;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_load(&loaded));
    TEST_ASSERT_EQUAL_MEMORY(&old, &loaded, sizeof(old));
    g_set_error = ESP_OK;
    g_commit_error = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, settings_bemf_cal_save(&next));
    TEST_ASSERT_EQUAL_UINT32(commits + 1U, g_commit_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_lock_depth);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bemf_mode_save_propagates_persistence_errors);
    RUN_TEST(test_bemf_curve_save_propagates_io_errors);
    RUN_TEST(test_cal_save_guard_rejects_cancel_between_validation_and_lock);
    RUN_TEST(test_cal_save_guard_cancel_after_admission_finishes_valid_curve);
    RUN_TEST(test_cal_save_guard_never_authorizes_invalid_or_unlocked_save);
    RUN_TEST(test_hard_reset_changes_only_nmra_configuration_cvs);
    RUN_TEST(test_unusable_bemf_curve_cannot_replace_previous_record);
    RUN_TEST(test_crc32_deterministic);
    RUN_TEST(test_crc32_differs_on_change);
    RUN_TEST(test_defaults);
    RUN_TEST(test_cv7_matches_version_txt);
    RUN_TEST(test_defaults_speed_table);
    RUN_TEST(test_cv_curve_migration);
    RUN_TEST(test_cv_read_write);
    RUN_TEST(test_cv7_readonly);
    RUN_TEST(test_cv8_not_factory_reset);
    RUN_TEST(test_cv8_factory_reset);
    RUN_TEST(test_cv_out_of_range);
    RUN_TEST(test_cv_reset_to_factory_direct);
    RUN_TEST(test_cv_commit_persists_blob_and_crc);
    RUN_TEST(test_config_defaults);
    RUN_TEST(test_config_roundtrip);
    RUN_TEST(test_config_null_args);
    RUN_TEST(test_tracks_missing_returns_not_found);
    RUN_TEST(test_tracks_roundtrip);
    RUN_TEST(test_tracks_empty_and_full);
    RUN_TEST(test_tracks_null_args);
    RUN_TEST(test_track_cats_default_layout);
    RUN_TEST(test_track_cats_roundtrip);
    RUN_TEST(test_func_map_defaults);
    RUN_TEST(test_func_map_roundtrip);
    RUN_TEST(test_func_map_stale_blob_uses_defaults);
    RUN_TEST(test_aux_cfg_defaults);
    RUN_TEST(test_aux_cfg_roundtrip);
    RUN_TEST(test_aux_cfg_stale_blob_uses_defaults);
    RUN_TEST(test_bemf_cal_roundtrip);
    RUN_TEST(test_bemf_cal_missing_and_invalid_count);
    RUN_TEST(test_bemf_cal_null_args);
    RUN_TEST(test_bemf_use_default_and_roundtrip);
    RUN_TEST(test_bemf_use_null_arg);
    RUN_TEST(test_factory_reset);
    RUN_TEST(test_factory_reset_clears_all_stores);
    RUN_TEST(test_nvs_read_u8_error);
    RUN_TEST(test_settings_init_branches);
    RUN_TEST(test_settings_init_loads_valid_cv);
    RUN_TEST(test_settings_save_deferred_and_flush);
    RUN_TEST(test_settings_save_api_invalid_args);
    RUN_TEST(test_settings_save_api_timeouts);
    RUN_TEST(test_cv_commit_deferred_flush);
    RUN_TEST(test_bemf_cal_clear_missing_key_is_ok);
    RUN_TEST(test_cv63_master_volume_alias);
    RUN_TEST(test_cv7_migrated_from_old_blob);
    RUN_TEST(test_func_bind_roundtrip);
    RUN_TEST(test_func_bind_legacy_convert);
    RUN_TEST(test_func_bind_add_remove_find);
    RUN_TEST(test_active_scheme);
    RUN_TEST(test_rev_s20_deferred_is_ram_only);
    RUN_TEST(test_rev_s21_all_config_setter_failures_retry);
    RUN_TEST(test_rev_s21_cv_failure_retains_dirty_bounded_retry);
    RUN_TEST(test_rev_s22_legacy_migration_preserves_shipped_keys);
    RUN_TEST(test_rev_s22_power_cut_during_legacy_migration);
    RUN_TEST(test_cv_publish_generation_retains_newer_write);
    RUN_TEST(test_cv_snapshot_and_nmra_side_effects);
    RUN_TEST(test_rev_m11_reject_curve_as_unit);
    RUN_TEST(test_rev_s25_empty_bindings_are_present);
    return UNITY_END();
}
