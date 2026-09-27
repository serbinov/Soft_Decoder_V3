#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* White-box include of the CV store (CRC + factory reset + read-only CVs).
 * Standard headers are pulled in before `static` is redefined so the test
 * stubs' includes (esp_log.h -> stdio.h) cannot leak external-linkage inline
 * definitions into Unity's translation unit. */
#define static
#include "../../components/settings/src/settings.c"
#undef static

/* Host stubs for ESP-IDF services (nvs, freertos, ...). */
#include "../../test_libs/teststubs/stubs.c"

/* settings.c calls the metadata-manifest sync on every save; the host build has
 * no VFS, so provide no-op stubs (the manifest itself is not under test here). */
esp_err_t settings_manifest_sync(void) { return ESP_OK; }
esp_err_t settings_manifest_load(void) { return ESP_ERR_NOT_FOUND; }

void setUp(void)
{
    mock_nvs_reset();
    mock_timer_now_us = 0;
    memset(&s_cv, 0, sizeof(s_cv));
    cv_set_defaults();
    s_lock = (SemaphoreHandle_t)1;
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
    TEST_ASSERT_EQUAL_UINT8(8, s_cv[7]);           /* decoder version = version.txt */
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
    TEST_ASSERT_EQUAL_UINT8(8, v); /* unchanged default */
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

    uint8_t blob[SETTINGS_CV_COUNT + 1];
    size_t len = sizeof(blob);
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_blob(s_h, "cv", blob, &len));
    TEST_ASSERT_EQUAL_UINT32(sizeof(s_cv), (uint32_t)len);
    TEST_ASSERT_EQUAL_UINT8(42, blob[1]);

    uint32_t crc = 0;
    TEST_ASSERT_EQUAL(ESP_OK, nvs_get_u32(s_h, "cv_crc", &crc));
    TEST_ASSERT_EQUAL_UINT32(cv_crc32(blob, len), crc);
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
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_bemf_cal_save(&cal));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_bemf_cal_clear());
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_bemf_use_save(true));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_factory_reset());
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_cv_write(5, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, settings_cv_reset_to_factory());
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc32_deterministic);
    RUN_TEST(test_crc32_differs_on_change);
    RUN_TEST(test_defaults);
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
    return UNITY_END();
}
