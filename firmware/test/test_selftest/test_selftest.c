#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif

/* Redirect the scratch file into a host temp dir. */
#define SELFTEST_TMP_PATH "st_tmp/.selftest.tmp"

/* Inject file-I/O failures (exercises every storage branch). */
static int g_fwrite_fail;
static int g_fread_fail;
static int g_fread_mismatch;

static size_t mock_selftest_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (g_fwrite_fail) {
        return 0;
    }
    return fwrite(p, sz, n, f);
}

static size_t mock_selftest_fread(void *p, size_t sz, size_t n, FILE *f)
{
    if (g_fread_fail) {
        return 0;
    }
    size_t r = fread(p, sz, n, f);
    if (g_fread_mismatch) {
        memset(p, 0xAA, sz * n);
        return sz * n; /* claim success with corrupted data */
    }
    return r;
}

#define SELFTEST_FWRITE(p, sz, n, f) mock_selftest_fwrite((p), (sz), (n), (f))
#define SELFTEST_FREAD(p, sz, n, f) mock_selftest_fread((p), (sz), (n), (f))

/* White-box: expose s_tmp_path and the check helpers. */
#define static
#include "../../components/selftest/src/selftest.c"
#undef static

#include "../../test_libs/teststubs/stubs.c"

/* ---- injectable collaborators ---- */
static uint32_t g_heap = 100000u;
static esp_err_t g_pinmap_err = ESP_OK;
static int g_cv_mode; /* 0 ok, 1 valid-read fail, 2/3 boundary not rejected */
static uint16_t g_adc_b1 = 1, g_adc_b2 = 2, g_adc_rail = 3;
static esp_err_t g_adc_err = ESP_OK;
static uint8_t g_volume = 20;
static esp_err_t g_aux_err = ESP_OK;
static bool g_storage_mounted = true;

/* Actuation collaborators. */
static esp_err_t g_aux_set_err = ESP_OK;
static bool g_aux_enabled = false;
static int g_aux_set_calls;
static esp_err_t g_play_err = ESP_OK;
static int g_play_calls;
static int g_stop_calls;
static esp_err_t g_tracks_load_err = ESP_OK;
static size_t g_tracks_count;
static settings_track_t g_tracks[SETTINGS_MAX_TRACKS];
static esp_err_t g_motor_set_err = ESP_OK;
static int g_motor_stop_calls;
static bool g_motor_inhibited;
static esp_err_t g_motor_release_err;
static bool g_applied_nonzero;
static int g_motor_set_calls;
static int g_motor_ramp_stop_calls;
static int g_motor_release_calls;
static bool g_maintenance_exclusive;
static esp_err_t g_maintenance_err = ESP_OK;
static int g_maintenance_depth;
static int g_apply_calls;
static uint8_t g_apply_last_fn;
static bool g_apply_last_on;

uint32_t esp_get_free_heap_size(void)
{
    return g_heap;
}

esp_err_t pinmap_validate(void)
{
    return g_pinmap_err;
}

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (g_cv_mode == 1 && idx == 1) {
        return ESP_FAIL;
    }
    if (g_cv_mode == 2 && idx == 0) {
        return ESP_OK; /* should have been rejected */
    }
    if (g_cv_mode == 3 && idx == SETTINGS_CV_COUNT + 1U) {
        return ESP_OK; /* should have been rejected */
    }
    if (idx < 1 || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out != NULL) {
        *out = 0;
    }
    return ESP_OK;
}

bool storage_is_mounted(void)
{
    return g_storage_mounted;
}

esp_err_t motor_bemf_adc_dump_checked(uint16_t *b1, uint16_t *b2, uint16_t *rail)
{
    if (b1 != NULL) {
        *b1 = g_adc_b1;
    }
    if (b2 != NULL) {
        *b2 = g_adc_b2;
    }
    if (rail != NULL) {
        *rail = g_adc_rail;
    }
    return g_adc_err;
}

uint8_t audio_get_volume(void)
{
    return g_volume;
}

esp_err_t auxio_get_enabled(uint8_t channel, bool *out)
{
    (void)channel;
    if (out != NULL) {
        *out = g_aux_enabled;
    }
    return g_aux_err;
}

esp_err_t auxio_set_enabled(uint8_t channel, bool enabled)
{
    (void)channel;
    if (g_aux_set_err != ESP_OK) {
        return g_aux_set_err;
    }
    g_aux_enabled = enabled;
    g_aux_set_calls++;
    return ESP_OK;
}

esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume)
{
    (void)voice;
    (void)path;
    (void)loop;
    (void)volume;
    g_play_calls++;
    return g_play_err;
}

esp_err_t audio_voice_stop(uint8_t voice)
{
    (void)voice;
    g_stop_calls++;
    return ESP_OK;
}

esp_err_t settings_tracks_load(settings_track_t *tracks, size_t *count)
{
    if (g_tracks_load_err != ESP_OK) {
        return g_tracks_load_err;
    }
    memcpy(tracks, g_tracks, sizeof(g_tracks));
    *count = g_tracks_count;
    return ESP_OK;
}

const char *storage_get_root(void)
{
    return "/userdata";
}

esp_err_t motor_set_speed(uint8_t speed, bool forward)
{
    (void)speed;
    (void)forward;
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_depth);
    TEST_ASSERT_FALSE(g_motor_inhibited);
    ++g_motor_set_calls;
    return g_motor_set_err;
}

void motor_stop(void)
{
    ++g_motor_ramp_stop_calls;
    g_motor_stop_calls++;
}

esp_err_t web_maintenance_begin(bool exclusive_fs)
{
    g_maintenance_exclusive = exclusive_fs;
    if (g_maintenance_err != ESP_OK) { return g_maintenance_err; }
    ++g_maintenance_depth;
    return ESP_OK;
}

void web_maintenance_end(void)
{
    --g_maintenance_depth;
}

esp_err_t motor_set_inhibited(bool inhibited)
{
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_depth);
    if (!inhibited) {
        ++g_motor_release_calls;
        if (g_motor_release_err != ESP_OK) { return g_motor_release_err; }
    }
    g_motor_inhibited = inhibited;
    if (inhibited) { ++g_motor_stop_calls; }
    return ESP_OK;
}

void motor_get_applied_speed(uint8_t *speed, bool *forward)
{
    if (speed != NULL) { *speed = g_motor_inhibited && !g_applied_nonzero ? 0U : 60U; }
    if (forward != NULL) { *forward = true; }
}

void web_apply_function(uint8_t fn, bool state)
{
    g_apply_calls++;
    g_apply_last_fn = fn;
    g_apply_last_on = state;
}

int64_t dcc_last_packet_us(void)
{
    return 0;
}

/* ---- helpers ---- */
static selftest_state_t state_of(const selftest_report_t *r, const char *name)
{
    for (uint8_t i = 0; i < r->count; ++i) {
        if (strcmp(r->items[i].name, name) == 0) {
            return r->items[i].state;
        }
    }
    return SELFTEST_FAIL;
}

void setUp(void)
{
    (void)MKDIR("st_tmp");
    g_fwrite_fail = 0;
    g_fread_fail = 0;
    g_fread_mismatch = 0;
    g_heap = 100000u;
    g_pinmap_err = ESP_OK;
    g_cv_mode = 0;
    g_adc_b1 = 1;
    g_adc_b2 = 2;
    g_adc_rail = 3;
    g_adc_err = ESP_OK;
    g_volume = 20;
    g_aux_err = ESP_OK;
    g_storage_mounted = true;
    s_tmp_path = SELFTEST_TMP_PATH;
    mock_nvs_reset();
    mock_nvs_open_fail = 0;
    mock_nvs_get_u32_err = 0;
    mock_nvs_set_u32_err = 0;
    mock_nvs_u32_corrupt = 0;
    g_aux_set_err = ESP_OK;
    g_aux_enabled = false;
    g_aux_set_calls = 0;
    g_play_err = ESP_OK;
    g_play_calls = 0;
    g_stop_calls = 0;
    g_tracks_load_err = ESP_OK;
    g_tracks_count = 0;
    memset(g_tracks, 0, sizeof(g_tracks));
    g_motor_set_err = ESP_OK;
    g_motor_stop_calls = 0;
    g_motor_inhibited = false;
    g_motor_release_err = ESP_OK;
    g_applied_nonzero = false;
    g_motor_set_calls = g_motor_ramp_stop_calls = g_motor_release_calls = 0;
    g_maintenance_exclusive = false;
    g_maintenance_err = ESP_OK;
    g_maintenance_depth = 0;
    g_apply_calls = 0;
    g_apply_last_fn = 0;
    g_apply_last_on = false;
}

void tearDown(void)
{
    remove(SELFTEST_TMP_PATH);
    (void)RMDIR("st_tmp");
}

/* ---- tests ---- */

static void test_selftest_state_name(void)
{
    TEST_ASSERT_EQUAL_STRING("PASS", selftest_state_name(SELFTEST_PASS));
    TEST_ASSERT_EQUAL_STRING("FAIL", selftest_state_name(SELFTEST_FAIL));
    TEST_ASSERT_EQUAL_STRING("SKIP", selftest_state_name(SELFTEST_SKIP));
}

static void test_selftest_all_pass(void)
{
    selftest_report_t r;
    TEST_ASSERT_EQUAL_UINT8(9, selftest_run(&r));
    TEST_ASSERT_EQUAL_UINT8(9, r.count);
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "heap"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "pinmap"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "settings_cv"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "nvs"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "storage"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "adc"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "audio"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "auxio"));
    TEST_ASSERT_EQUAL(SELFTEST_PASS, state_of(&r, "dcc"));

    /* A NULL report is allowed and reports zero passed. */
    TEST_ASSERT_EQUAL_UINT8(0, selftest_run(NULL));
}

static void test_selftest_storage_skip(void)
{
    g_storage_mounted = false;
    selftest_report_t r;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_SKIP, state_of(&r, "storage"));
}

static void test_selftest_storage_fail_paths(void)
{
    selftest_report_t r;

    /* fopen fails (missing directory). */
    s_tmp_path = "st_missing/.selftest.tmp";
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "storage"));
    s_tmp_path = SELFTEST_TMP_PATH;

    /* fwrite returns short. */
    g_fwrite_fail = 1;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "storage"));
    g_fwrite_fail = 0;

    /* fread fails. */
    g_fread_fail = 1;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "storage"));
    g_fread_fail = 0;

    /* fread succeeds but the payload differs. */
    g_fread_mismatch = 1;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "storage"));
    g_fread_mismatch = 0;
}

static void test_selftest_heap_and_pinmap_fail(void)
{
    selftest_report_t r;
    g_heap = 1;
    g_pinmap_err = ESP_FAIL;
    TEST_ASSERT_EQUAL_UINT8(7, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "heap"));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "pinmap"));
}

static void test_selftest_settings_cv_fail_modes(void)
{
    selftest_report_t r;
    for (g_cv_mode = 1; g_cv_mode <= 3; ++g_cv_mode) {
        TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
        TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "settings_cv"));
    }
    g_cv_mode = 0;
}

static void test_selftest_nvs_failures(void)
{
    selftest_report_t r;

    mock_nvs_open_fail = 1;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "nvs"));
    mock_nvs_open_fail = 0;

    mock_nvs_set_u32_err = ESP_FAIL;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "nvs"));
    mock_nvs_set_u32_err = 0;

    mock_nvs_get_u32_err = ESP_FAIL;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "nvs"));
    mock_nvs_get_u32_err = 0;

    /* Read succeeds but returns the wrong value. */
    mock_nvs_u32_corrupt = 1;
    TEST_ASSERT_EQUAL_UINT8(8, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "nvs"));
    mock_nvs_u32_corrupt = 0;
}

static void test_selftest_adc_audio_auxio_fail(void)
{
    selftest_report_t r;
    g_adc_b1 = 5000; /* out of 12-bit range */
    g_volume = 200;  /* above the valid range */
    g_aux_err = ESP_FAIL;
    TEST_ASSERT_EQUAL_UINT8(6, selftest_run(&r));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "adc"));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "audio"));
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, state_of(&r, "auxio"));
}

/* ---- actuating commands ---- */

static void test_selftest_act_aux(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_aux(AUXIO_CH_COUNT, 100));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_aux(0, SELFTEST_ACT_MS_MIN - 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_aux(0, SELFTEST_ACT_MS_MAX + 1U));

    g_aux_err = ESP_FAIL; /* previous state unreadable */
    TEST_ASSERT_EQUAL(ESP_FAIL, selftest_act_aux(0, 100));
    g_aux_err = ESP_OK;

    g_aux_set_err = ESP_FAIL; /* cannot turn on */
    TEST_ASSERT_EQUAL(ESP_FAIL, selftest_act_aux(0, 100));
    g_aux_set_err = ESP_OK;

    g_aux_enabled = false;
    TEST_ASSERT_EQUAL(ESP_OK, selftest_act_aux(2, 100));
    TEST_ASSERT_EQUAL_INT(2, g_aux_set_calls); /* on, then restore */
    TEST_ASSERT_FALSE(g_aux_enabled);

    g_aux_enabled = true; /* restore to on */
    TEST_ASSERT_EQUAL(ESP_OK, selftest_act_aux(2, 100));
    TEST_ASSERT_TRUE(g_aux_enabled);
}

static void test_selftest_act_sound(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_sound(0, 100));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_sound(SETTINGS_MAX_TRACKS + 1U, 100));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_sound(1, SELFTEST_ACT_MS_MIN - 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_sound(1, SELFTEST_ACT_MS_MAX + 1U));

    g_tracks_load_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, selftest_act_sound(1, 100));
    g_tracks_load_err = ESP_OK;

    /* No such slot. */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, selftest_act_sound(1, 100));

    /* Slot present but disabled / no file is skipped. */
    g_tracks_count = 1;
    g_tracks[0].slot = 1;
    g_tracks[0].enabled = false;
    strcpy(g_tracks[0].file, "audio/slot1.wav");
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, selftest_act_sound(1, 100));

    g_tracks[0].enabled = true;
    strcpy(g_tracks[0].file, ""); /* empty file name */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, selftest_act_sound(1, 100));

    /* Play failure propagates. */
    strcpy(g_tracks[0].file, "audio/slot1.wav");
    g_play_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, selftest_act_sound(1, 100));
    g_play_err = ESP_OK;

    /* Success: play and stop the test voice. */
    g_play_calls = 0;
    g_stop_calls = 0;
    TEST_ASSERT_EQUAL(ESP_OK, selftest_act_sound(1, 100));
    TEST_ASSERT_EQUAL_INT(1, g_play_calls);
    TEST_ASSERT_EQUAL_INT(1, g_stop_calls);
}

static void test_selftest_act_motor(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_motor(0, 100));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      selftest_act_motor(SELFTEST_MOTOR_SPEED_MAX + 1U, 100));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_motor(10, SELFTEST_ACT_MS_MIN - 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, selftest_act_motor(10, SELFTEST_ACT_MS_MAX + 1U));

    g_motor_set_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, selftest_act_motor(10, 100));
    g_motor_set_err = ESP_OK;
    g_motor_stop_calls = 0;

    TEST_ASSERT_EQUAL(ESP_OK, selftest_act_motor(10, 100));
    TEST_ASSERT_EQUAL_INT(1, g_motor_stop_calls);
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
    g_maintenance_err = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, selftest_act_motor(10, 100));
}

static void test_selftest_act_function(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      selftest_act_function(SETTINGS_FUNC_MAP_COUNT, true));
    TEST_ASSERT_EQUAL(ESP_OK, selftest_act_function(3, true));
    TEST_ASSERT_EQUAL_INT(1, g_apply_calls);
    TEST_ASSERT_EQUAL_UINT8(3, g_apply_last_fn);
    TEST_ASSERT_TRUE(g_apply_last_on);
    TEST_ASSERT_EQUAL(ESP_OK, selftest_act_function(3, false));
    TEST_ASSERT_FALSE(g_apply_last_on);
}

static void test_motor_maintenance_release_failure_still_coasts(void)
{
    g_motor_release_err = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, selftest_act_motor(10, 100));
    TEST_ASSERT_EQUAL_INT(0, g_motor_set_calls);
    TEST_ASSERT_EQUAL_INT(1, g_motor_release_calls);
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
    TEST_ASSERT_EQUAL_INT(0, g_motor_ramp_stop_calls);
}

static void test_motor_deadline_coasts_without_cv4_ramp(void)
{
    int64_t start = mock_timer_now_us;
    TEST_ASSERT_EQUAL(ESP_OK, selftest_act_motor(10, 100));
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_FALSE(g_maintenance_exclusive);
    TEST_ASSERT_EQUAL_INT(1, g_motor_set_calls);
    TEST_ASSERT_EQUAL_INT(1, g_motor_stop_calls);
    TEST_ASSERT_EQUAL_INT(0, g_motor_ramp_stop_calls);
    TEST_ASSERT_EQUAL_INT64(100000, mock_timer_now_us - start);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
}

static void test_motor_nonzero_applied_output_fails_and_releases_maintenance(void)
{
    g_applied_nonzero = true;
    TEST_ASSERT_EQUAL(ESP_FAIL, selftest_act_motor(10, 100));
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
}

static void test_selftest_act_fn_sweep(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, selftest_act_fn_sweep(SELFTEST_ACT_MS_MIN - 1U));
    TEST_ASSERT_EQUAL_UINT8(0, selftest_act_fn_sweep(SELFTEST_SWEEP_MS_MAX + 1U));
    g_apply_calls = 0;
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_FUNC_MAP_COUNT, selftest_act_fn_sweep(50));
    TEST_ASSERT_EQUAL_INT(SETTINGS_FUNC_MAP_COUNT * 2, g_apply_calls); /* on + off */
}

static void test_selftest_act_aux_sweep(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, selftest_act_aux_sweep(SELFTEST_ACT_MS_MIN - 1U));
    TEST_ASSERT_EQUAL_UINT8(0, selftest_act_aux_sweep(SELFTEST_SWEEP_MS_MAX + 1U));
    g_aux_set_calls = 0;
    TEST_ASSERT_EQUAL_UINT8(AUXIO_CH_COUNT, selftest_act_aux_sweep(50));
    TEST_ASSERT_EQUAL_INT(AUXIO_CH_COUNT * 2, g_aux_set_calls); /* on + restore */
    TEST_ASSERT_FALSE(g_aux_enabled);
}

static void test_adc_failure_cannot_be_reported_as_pass(void)
{
    g_adc_b1 = g_adc_b2 = g_adc_rail = 0;
    TEST_ASSERT_EQUAL(SELFTEST_PASS, check_adc());
    g_adc_err = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, check_adc());
    g_adc_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(SELFTEST_FAIL, check_adc());
    g_adc_err = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(SELFTEST_SKIP, check_adc());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_selftest_state_name);
    RUN_TEST(test_selftest_all_pass);
    RUN_TEST(test_selftest_storage_skip);
    RUN_TEST(test_selftest_storage_fail_paths);
    RUN_TEST(test_selftest_heap_and_pinmap_fail);
    RUN_TEST(test_selftest_settings_cv_fail_modes);
    RUN_TEST(test_selftest_nvs_failures);
    RUN_TEST(test_selftest_adc_audio_auxio_fail);
    RUN_TEST(test_adc_failure_cannot_be_reported_as_pass);
    RUN_TEST(test_selftest_act_aux);
    RUN_TEST(test_selftest_act_sound);
    RUN_TEST(test_selftest_act_motor);
    RUN_TEST(test_motor_maintenance_release_failure_still_coasts);
    RUN_TEST(test_motor_deadline_coasts_without_cv4_ramp);
    RUN_TEST(test_motor_nonzero_applied_output_fails_and_releases_maintenance);
    RUN_TEST(test_selftest_act_function);
    RUN_TEST(test_selftest_act_fn_sweep);
    RUN_TEST(test_selftest_act_aux_sweep);
    return UNITY_END();
}
