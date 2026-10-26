#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif

/* Host audio dir + ISR-free UART mock (defined before the white-box include). */
#define PROV_AUDIO_DIR "prov_tmp"

/* malloc failure injection for the out-of-memory guards. */
static int g_malloc_fail = 0;
static void *mock_malloc(size_t n)
{
    if (g_malloc_fail) {
        return NULL;
    }
    return malloc(n);
}
#define malloc(n) mock_malloc(n)

/* fwrite failure injection for the PUT write-error branch. */
static int g_prov_fwrite_fail = 0;
static int g_prov_sync_fail, g_prov_flush_fail, g_prov_close_fail;
static size_t mock_prov_fwrite(const void *p, size_t sz, size_t n, FILE *f);
static int mock_prov_sync(int fd);
static int mock_prov_flush(FILE *f);
static int mock_prov_close(FILE *f);
#define PROV_FWRITE(p, sz, n, f) mock_prov_fwrite((p), (sz), (n), (f))
#define fflush mock_prov_flush
#define fclose mock_prov_close
#ifdef _WIN32
#define _commit mock_prov_sync
#else
#define fsync mock_prov_sync
#endif

#include "../../components/web/src/audio_pack.c"

#define static
#include "../../components/provision/src/provision.c"
#undef static
#undef malloc
#undef fflush
#undef fclose
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif

#define esp_ota_set_boot_partition sdk_set_boot_partition
#define uart_set_baudrate sdk_uart_set_baudrate
#include "../../test_libs/teststubs/stubs.c"
#undef esp_ota_set_boot_partition
#undef uart_set_baudrate

static size_t mock_prov_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (g_prov_fwrite_fail) {
        return 0;
    }
    return fwrite(p, sz, n, f);
}

static int mock_prov_flush(FILE *f)
{
    return g_prov_flush_fail ? EOF : fflush(f);
}

static int mock_prov_close(FILE *f)
{
    int result = fclose(f);
    return g_prov_close_fail ? EOF : result;
}

static int mock_prov_sync(int fd)
{
    if (g_prov_sync_fail) { return -1; }
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}

/* ---- collaborator stubs ---- */
static esp_err_t g_cal_start_err = ESP_OK;
static esp_err_t g_cal_clear_err = ESP_OK;
static esp_err_t g_cal_save_err = ESP_OK;
static esp_err_t g_storage_format_err = ESP_OK;
static int g_storage_format_calls;
static int g_tracks_save_calls;
static int g_cv_write_calls;
static int g_web_busy;
static bool tx_has(const char *s);
static esp_err_t g_adc_err, g_maintenance_err, g_wav_err, g_tracks_save_err;
static int g_maintenance_depth, g_maintenance_begin_calls, g_maintenance_end_calls;
static bool g_fs_closed;
static int g_fs_release_calls, g_boot_calls, g_validate_calls;
static uint32_t g_baud;
static bool g_web_exclusive;
static bool g_web_ready = true;
static int g_storage_maint_depth;
static uint64_t g_free_bytes = 8U * 1024U * 1024U;
static esp_err_t g_free_err = ESP_OK;
static esp_err_t g_motor_speed_err = ESP_OK;
static esp_err_t g_motor_inhibit_err = ESP_OK;
static bool g_motor_inhibited;
static int g_motor_speed_calls;
static int g_motor_inhibit_calls;
static uint8_t g_motor_last_speed;
static bool g_motor_last_fwd;

esp_err_t web_maintenance_begin(bool exclusive_fs)
{
    ++g_maintenance_begin_calls;
    if (g_web_busy || g_maintenance_err != ESP_OK) {
        return g_maintenance_err != ESP_OK ? g_maintenance_err : ESP_ERR_INVALID_STATE;
    }
    g_web_exclusive = exclusive_fs;
    ++g_maintenance_depth;
    if (exclusive_fs) {
        TEST_ASSERT_EQUAL(ESP_OK, storage_maintenance_begin());
        g_fs_closed = true;
    }
    return ESP_OK;
}

void web_maintenance_end(void)
{
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_depth);
    if (g_web_exclusive && g_storage_maint_depth > 0) {
        storage_maintenance_end();
    }
    --g_maintenance_depth;
    ++g_maintenance_end_calls;
    g_web_exclusive = false;
    g_fs_closed = false;
}

esp_err_t storage_maintenance_begin(void)
{
    ++g_storage_maint_depth;
    g_fs_closed = true;
    return ESP_OK;
}

void storage_maintenance_end(void)
{
    if (g_storage_maint_depth > 0) {
        --g_storage_maint_depth;
        g_fs_closed = false;
        ++g_fs_release_calls;
    }
}

esp_err_t storage_get_free_bytes(uint64_t *out_free_bytes)
{
    if (g_free_err != ESP_OK) {
        return g_free_err;
    }
    TEST_ASSERT_NOT_NULL(out_free_bytes);
    *out_free_bytes = g_free_bytes;
    return ESP_OK;
}

bool storage_is_mounted(void)
{
    return true;
}

esp_err_t storage_mount(void)
{
    return ESP_OK;
}

bool web_actuation_ready(void)
{
    return g_web_ready;
}

bool web_maintenance_active(void)
{
    return g_maintenance_depth > 0;
}

const char *storage_get_root(void) { return "prov_tmp"; }

esp_err_t audio_validate_wav(const char *path)
{
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_FALSE(g_fs_closed);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_depth);
    TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
    ++g_validate_calls;
    return g_wav_err;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_depth);
    TEST_ASSERT_FALSE(g_fs_closed);
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
    ++g_boot_calls;
    return sdk_set_boot_partition(partition);
}

esp_err_t uart_set_baudrate(uart_port_t port, uint32_t baud)
{
    g_baud = baud;
    return sdk_uart_set_baudrate(port, baud);
}

void motor_bemf_cal_info(motor_bemf_cal_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->count = 2;
    info->speed[0] = 12;
    info->frac[0] = 100;
    info->speed[1] = 126;
    info->frac[1] = 700;
}

void motor_bemf_diag(motor_bemf_diag_t *d)
{
    memset(d, 0, sizeof(*d));
    d->rail_mv = 1000;
    d->error = -5;
}

esp_err_t motor_bemf_adc_dump_checked(uint16_t *b1, uint16_t *b2, uint16_t *rail)
{
    if (b1) {
        *b1 = 1;
    }
    if (b2) {
        *b2 = 2;
    }
    if (rail) {
        *rail = 3;
    }
    return g_adc_err;
}

void motor_bemf_coast_read(uint16_t *b1, uint16_t *b2)
{
    if (b1) {
        *b1 = 4;
    }
    if (b2) {
        *b2 = 5;
    }
}

esp_err_t motor_bemf_cal_start(void)
{
    return g_cal_start_err;
}

void motor_bemf_cal_default_config(motor_bemf_cal_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MOTOR_BEMF_MODE_AUTO;
}

esp_err_t motor_bemf_cal_start_ex(const motor_bemf_cal_config_t *cfg)
{
    (void)cfg;
    return g_cal_start_err;
}

esp_err_t motor_bemf_cal_clear(void)
{
    return g_cal_clear_err;
}

esp_err_t motor_bemf_cal_reload(void)
{
    return ESP_OK;
}

static esp_err_t g_bemf_check_err = ESP_OK;
static motor_bemf_check_t g_bemf_check_result;
static int g_bemf_check_calls;
static bool g_bemf_check_spin;

esp_err_t motor_bemf_check(motor_bemf_check_t *out, bool spin)
{
    ++g_bemf_check_calls;
    g_bemf_check_spin = spin;
    if (out != NULL) {
        *out = g_bemf_check_result;
    }
    return g_bemf_check_err;
}

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    ++g_motor_speed_calls;
    g_motor_last_speed = speed128;
    g_motor_last_fwd = forward;
    return g_motor_speed_err;
}

esp_err_t motor_set_inhibited(bool inhibited)
{
    ++g_motor_inhibit_calls;
    if (g_motor_inhibit_err != ESP_OK) {
        return g_motor_inhibit_err;
    }
    g_motor_inhibited = inhibited;
    return ESP_OK;
}

void motor_get_applied_speed(uint8_t *speed, bool *forward)
{
    if (speed != NULL) {
        *speed = g_motor_inhibited ? 0U : 60U;
    }
    if (forward != NULL) {
        *forward = true;
    }
}

static int g_motor_es_calls;
static int g_audio_stop_calls;

void motor_emergency_stop(void)
{
    g_motor_es_calls++;
}

void audio_stop_all(void)
{
    g_audio_stop_calls++;
}

esp_err_t settings_cv_write(uint16_t idx, uint8_t val)
{
    (void)idx;
    (void)val;
    g_cv_write_calls++;
    return ESP_OK;
}

esp_err_t settings_cv_commit(void)
{
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save(const settings_bemf_cal_t *cal)
{
    (void)cal;
    return g_cal_save_err;
}

esp_err_t settings_tracks_save(const settings_track_t *tracks, size_t count)
{
    (void)tracks;
    (void)count;
    g_tracks_save_calls++;
    TEST_ASSERT_FALSE(g_fs_closed);
    TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
    return g_tracks_save_err;
}

esp_err_t storage_format(void)
{
    TEST_ASSERT_EQUAL_INT(1, g_storage_maint_depth);
    TEST_ASSERT_TRUE(g_fs_closed);
    g_storage_format_calls++;
    return g_storage_format_err;
}

/* Self-test collaborators: the listener prints the report produced here. */
uint8_t selftest_run(selftest_report_t *report)
{
    if (report == NULL) {
        return 0;
    }
    memset(report, 0, sizeof(*report));
    report->count = 2;
    report->items[0].name = "fake_a";
    report->items[0].state = SELFTEST_PASS;
    report->items[1].name = "fake_b";
    report->items[1].state = SELFTEST_FAIL;
    return 1;
}

const char *selftest_state_name(selftest_state_t state)
{
    switch (state) {
        case SELFTEST_PASS: return "PASS";
        case SELFTEST_FAIL: return "FAIL";
        default: return "SKIP";
    }
}

static esp_err_t g_act_aux_err = ESP_OK;
static esp_err_t g_act_sound_err = ESP_OK;
static esp_err_t g_act_motor_err = ESP_OK;
static uint8_t g_act_last_ch;
static uint16_t g_act_last_ms;

esp_err_t selftest_act_aux(uint8_t channel, uint16_t ms)
{
    g_act_last_ch = channel;
    g_act_last_ms = ms;
    return g_act_aux_err;
}

esp_err_t selftest_act_sound(uint8_t slot, uint16_t ms)
{
    (void)slot;
    (void)ms;
    return g_act_sound_err;
}

esp_err_t selftest_act_motor(uint8_t speed, uint16_t ms)
{
    (void)speed;
    (void)ms;
    return g_act_motor_err;
}

static esp_err_t g_act_fn_err = ESP_OK;
static uint8_t g_fn_sweep_ret = 29;
static uint8_t g_aux_sweep_ret = 9;
static uint8_t g_act_last_fn;
static bool g_act_last_on;

esp_err_t selftest_act_function(uint8_t fn, bool on)
{
    g_act_last_fn = fn;
    g_act_last_on = on;
    return g_act_fn_err;
}

uint8_t selftest_act_fn_sweep(uint16_t ms)
{
    (void)ms;
    return g_fn_sweep_ret;
}

uint8_t selftest_act_aux_sweep(uint16_t ms)
{
    (void)ms;
    return g_aux_sweep_ret;
}

/* ---- sound engine stubs (HIL-ENGINE / HIL-SCHEME-SPEED) ---- */
static int g_sound_engine_set_calls;
static bool g_sound_engine_last;
static int g_sound_speed_calls;
static uint8_t g_sound_speed_last;
static bool g_sound_speed_fwd_last;

void sound_engine_power(bool on)
{
    g_sound_engine_set_calls++;
    g_sound_engine_last = on;
}

void sound_set_speed(uint8_t speed, bool forward)
{
    g_sound_speed_calls++;
    g_sound_speed_last = speed;
    g_sound_speed_fwd_last = forward;
}

/* ---- helpers ---- */
static bool tx_has(const char *s)
{
    return strstr(mock_uart_tx, s) != NULL;
}

static void tx_reset(void)
{
    mock_uart_tx_len = 0;
    mock_uart_tx[0] = '\0';
}

static void feed(const char *s)
{
    mock_uart_feed(s, strlen(s));
}

static void set_prov_flag(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, 1, &h) == ESP_OK) {
        (void)nvs_set_u8(h, NVS_KEY_PROV, 1);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

void setUp(void)
{
    mock_uart_reset();
    mock_nvs_reset();
    mock_timer_now_us = 0;
    g_cal_start_err = ESP_OK;
    g_cal_clear_err = ESP_OK;
    g_cal_save_err = ESP_OK;
    g_storage_format_err = ESP_OK;
    g_storage_format_calls = 0;
    g_tracks_save_calls = 0;
    g_cv_write_calls = 0;
    g_adc_err = g_maintenance_err = g_wav_err = g_tracks_save_err = ESP_OK;
    g_maintenance_depth = g_maintenance_begin_calls = g_maintenance_end_calls = 0;
    g_fs_release_calls = g_boot_calls = g_validate_calls = 0;
    g_fs_closed = false;
    g_baud = 115200;
    s_pending_boot = NULL;
    g_act_aux_err = ESP_OK;
    g_act_sound_err = ESP_OK;
    g_act_motor_err = ESP_OK;
    g_act_last_ch = 0;
    g_act_last_ms = 0;
    g_act_fn_err = ESP_OK;
    g_fn_sweep_ret = 29;
    g_aux_sweep_ret = 9;
    g_act_last_fn = 0;
    g_act_last_on = false;
    mock_uart_driver_install_err = 0;
    mock_usbjtag_install_ok = 0;
    mock_usbjtag_byte = -1;
    mock_ota_begin_err = 0;
    mock_ota_write_err = 0;
    mock_ota_end_err = 0;
    mock_ota_set_boot_err = 0;
    mock_ota_partition_absent = 0;
    mock_ota_next_size = 4u * 1024u * 1024u;
    mock_esp_restart_calls = 0;
    g_malloc_fail = 0;
    g_prov_fwrite_fail = g_prov_sync_fail = g_prov_flush_fail = g_prov_close_fail = 0;
    g_web_busy = 0;
    g_audio_stop_calls = 0;
    g_motor_es_calls = 0;
    g_web_ready = true;
    g_storage_maint_depth = 0;
    g_free_bytes = 8U * 1024U * 1024U;
    g_free_err = ESP_OK;
    g_motor_speed_err = ESP_OK;
    g_motor_inhibit_err = ESP_OK;
    g_motor_inhibited = false;
    g_motor_speed_calls = g_motor_inhibit_calls = 0;
    g_motor_last_speed = 0;
    g_motor_last_fwd = false;
    g_bemf_check_err = ESP_OK;
    memset(&g_bemf_check_result, 0, sizeof(g_bemf_check_result));
    g_bemf_check_result.ok = true;
    g_bemf_check_result.static_ok = true;
    g_bemf_check_result.optimal_us = 340;
    g_bemf_check_result.rail_mv = 1000;
    g_bemf_check_calls = 0;
    g_bemf_check_spin = false;
    install_session_reset();
    s_usbjtag_ok = false;
    s_provisioning = false;
    s_listener_wdt = false;
    s_listen_iter_cap = 0;
    mock_task_wdt_add_err = 0;
    mock_task_wdt_reset_calls = 0;
    (void)RMDIR(PROV_AUDIO_DIR ".prov_stage");
    (void)MKDIR(PROV_AUDIO_DIR);
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
    TEST_ASSERT_EQUAL_INT(0, g_storage_maint_depth);
    TEST_ASSERT_FALSE(g_fs_closed);
    TEST_ASSERT_EQUAL_UINT32(115200, g_baud);
    remove(PROV_AUDIO_DIR "/slot1.wav");
    remove(PROV_AUDIO_DIR "/slot99.wav");
    remove(PROV_AUDIO_DIR "/test.wav");
    (void)RMDIR(PROV_AUDIO_DIR ".prov_stage");
    install_session_reset();
}

/* ---- BEMF commands ---- */

static void test_bemf_info_commands(void)
{
    tx_reset();
    handle_bemf_cmd("BEMF?");
    TEST_ASSERT_TRUE(tx_has("BEMF-OK 2 12 100 126 700"));

    tx_reset();
    handle_bemf_cmd("BEMF-HDR");
    TEST_ASSERT_TRUE(tx_has("BEMF_CAL_BASE"));
    TEST_ASSERT_TRUE(tx_has("BEMF-HDR-END"));

    tx_reset();
    handle_bemf_cmd("BEMF-RAW");
    TEST_ASSERT_TRUE(tx_has("BEMF-RAW 1000"));

    tx_reset();
    handle_bemf_cmd("BEMF-ADC");
    TEST_ASSERT_TRUE(tx_has("BEMF-ADC 1 2 3"));

    tx_reset();
    handle_bemf_cmd("BEMF-COAST");
    TEST_ASSERT_TRUE(tx_has("BEMF-COAST 4 5"));

    tx_reset();
    handle_bemf_cmd("BEMF-WHAT");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR unknown"));
}

static void test_bemf_cal_commands(void)
{
    tx_reset();
    handle_bemf_cmd("BEMF-CAL");
    TEST_ASSERT_TRUE(tx_has("BEMF-CAL-OK"));

    g_cal_start_err = ESP_ERR_INVALID_STATE;
    tx_reset();
    handle_bemf_cmd("BEMF-CAL");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR busy"));

    g_cal_start_err = ESP_FAIL;
    tx_reset();
    handle_bemf_cmd("BEMF-CAL");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR start"));

    tx_reset();
    handle_bemf_cmd("BEMF-CLR");
    TEST_ASSERT_TRUE(tx_has("BEMF-CLR-OK"));

    g_cal_clear_err = ESP_FAIL;
    tx_reset();
    handle_bemf_cmd("BEMF-CLR");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR busy"));
}

static void test_bemf_cvset_and_test(void)
{
    tx_reset();
    handle_bemf_cmd("BEMF-CVSET 3 100");
    TEST_ASSERT_TRUE(tx_has("CVSET-OK 3=100"));
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);

    tx_reset();
    handle_bemf_cmd("BEMF-CVSET 0 100");
    TEST_ASSERT_TRUE(tx_has("CVSET-ERR range"));

    tx_reset();
    handle_bemf_cmd("BEMF-CVSET 513 0");
    TEST_ASSERT_TRUE(tx_has("CVSET-ERR range"));

    tx_reset();
    handle_bemf_cmd("BEMF-TEST 60");
    TEST_ASSERT_TRUE(tx_has("BEMF-TEST-OK 60 fwd"));
    TEST_ASSERT_EQUAL_UINT8(60, g_motor_last_speed);
    TEST_ASSERT_TRUE(g_motor_last_fwd);
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);

    tx_reset();
    handle_bemf_cmd("BEMF-TEST 60 rev");
    TEST_ASSERT_TRUE(tx_has("BEMF-TEST-OK 60 rev"));
    TEST_ASSERT_FALSE(g_motor_last_fwd);
    TEST_ASSERT_TRUE(g_motor_inhibited);

    tx_reset();
    handle_bemf_cmd("BEMF-TEST 200");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR speed"));
    tx_reset();
    handle_bemf_cmd("BEMF-TEST 61");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR speed"));

    /* A drive failure is propagated and still leaves the bridge inhibited. */
    g_motor_speed_err = ESP_FAIL;
    tx_reset();
    handle_bemf_cmd("BEMF-TEST 40");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR motor"));
    TEST_ASSERT_FALSE(tx_has("BEMF-TEST-OK"));
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
    g_motor_speed_err = ESP_OK;

    /* Admission/source is closed while another operation owns actuation. */
    g_web_ready = false;
    tx_reset();
    handle_bemf_cmd("BEMF-TEST 40");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR busy"));
    g_web_ready = true;

    /* Duration is clamped to the bounded maximum. */
    tx_reset();
    handle_bemf_cmd("BEMF-TEST 10 999999");
    TEST_ASSERT_TRUE(tx_has("BEMF-TEST-OK 10 fwd 5000"));
    TEST_ASSERT_TRUE(g_motor_inhibited);
}

static void test_bemf_set_command(void)
{
    tx_reset();
    handle_bemf_cmd("BEMF=12,100,126,700");
    TEST_ASSERT_TRUE(tx_has("BEMF-OK 2"));

    tx_reset();
    handle_bemf_cmd("BEMF=12");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR parse"));

    tx_reset();
    handle_bemf_cmd("BEMF=200,10");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR range"));

    g_cal_save_err = ESP_FAIL;
    tx_reset();
    handle_bemf_cmd("BEMF=12,100,126,700");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR save"));
}

static void test_bemf_check_command(void)
{
    g_bemf_check_result.optimal_us = 340;
    g_bemf_check_result.offset_mv = 3;
    g_bemf_check_result.noise_mv = 2;
    g_bemf_check_result.bemf_mv = 400;
    g_bemf_check_result.rail_mv = 1000;

    tx_reset();
    handle_bemf_cmd("BEMF-CHECK");
    TEST_ASSERT_TRUE(tx_has("BEMF-CHECK-OK optimal_us=340 offset=3 noise=2 bemf=400 rail=1000"));
    TEST_ASSERT_EQUAL_INT(1, g_bemf_check_calls);
    TEST_ASSERT_FALSE(g_bemf_check_spin);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_begin_calls);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_end_calls);

    tx_reset();
    handle_bemf_cmd("BEMF-CHECK spin");
    TEST_ASSERT_TRUE(tx_has("BEMF-CHECK-OK"));
    TEST_ASSERT_TRUE(g_bemf_check_spin);
    TEST_ASSERT_EQUAL_INT(2, g_bemf_check_calls);

    tx_reset();
    handle_bemf_cmd("BEMF-CHECK foo");
    TEST_ASSERT_TRUE(tx_has("BEMF-CHECK-ERR reason=args"));
    TEST_ASSERT_EQUAL_INT(2, g_bemf_check_calls);

    /* A completed check failure is reported with its reason. */
    g_bemf_check_err = ESP_FAIL;
    g_bemf_check_result.ok = false;
    g_bemf_check_result.reason = MOTOR_BEMF_CHECK_ERR_NO_RISE;
    tx_reset();
    handle_bemf_cmd("BEMF-CHECK spin");
    TEST_ASSERT_TRUE(tx_has("BEMF-CHECK-ERR reason=no-rise"));
    TEST_ASSERT_TRUE(g_motor_inhibited);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);

    /* Busy admission does not touch the motor. */
    g_web_ready = false;
    tx_reset();
    handle_bemf_cmd("BEMF-CHECK");
    TEST_ASSERT_TRUE(tx_has("BEMF-CHECK-ERR reason=busy"));
    g_web_ready = true;

    /* Uninhibit failure is reported and the cleanup still runs. */
    g_bemf_check_err = ESP_OK;
    g_bemf_check_result.ok = true;
    g_motor_inhibit_err = ESP_FAIL;
    tx_reset();
    handle_bemf_cmd("BEMF-CHECK");
    TEST_ASSERT_TRUE(tx_has("BEMF-CHECK-ERR reason=motor"));
    TEST_ASSERT_EQUAL_INT(1, g_motor_es_calls);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
    g_motor_inhibit_err = ESP_OK;
}

/* ---- FW upload ---- */

static void test_provision_fw_errors(void)
{
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, provision_fw("FW 0"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR parse"));

    tx_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, provision_fw("FW 99999999"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR size"));

    mock_ota_partition_absent = 1;
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, provision_fw("FW 100"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR nopart"));
    mock_ota_partition_absent = 0;

    mock_ota_next_size = 10;
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, provision_fw("FW 100"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR big"));
    mock_ota_next_size = 4u * 1024u * 1024u;

    mock_ota_begin_err = ESP_FAIL;
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_FAIL, provision_fw("FW 4"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR begin"));
    mock_ota_begin_err = 0;

    /* No data fed -> read_exact times out. */
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, provision_fw("FW 4"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR timeout"));
}

static void test_provision_fw_success_and_write_errors(void)
{
    feed("ABCD");
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_OK, provision_fw("FW 4"));
    TEST_ASSERT_TRUE(tx_has("FW-OK"));
    TEST_ASSERT_EQUAL_UINT32(4, mock_ota_bytes);

    feed("ABCD");
    mock_ota_write_err = ESP_FAIL;
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_FAIL, provision_fw("FW 4"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR write"));
    mock_ota_write_err = 0;

    feed("ABCD");
    mock_ota_end_err = ESP_FAIL;
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_FAIL, provision_fw("FW 4"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR end"));
    mock_ota_end_err = 0;

    feed("ABCD");
    mock_ota_set_boot_err = ESP_FAIL;
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_OK, provision_fw("FW 4"));
    TEST_ASSERT_NOT_NULL(s_pending_boot);
    TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
    TEST_ASSERT_FALSE(tx_has("FW-ERR boot"));
    mock_ota_set_boot_err = 0;
}

static void test_provision_fw_chunk_ack(void)
{
    static char big[PROV_CHUNK + 1];
    memset(big, 'A', sizeof(big));
    mock_uart_feed(big, sizeof(big));
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_OK, provision_fw("FW 4097"));
    TEST_ASSERT_TRUE(tx_has("FW-OK"));
    TEST_ASSERT_TRUE(tx_has("CHUNK"));
    TEST_ASSERT_EQUAL_UINT32(PROV_CHUNK + 1, mock_ota_bytes);
}

/* ---- provisioning run ---- */

static void test_provision_run_requires_confirm(void)
{
    /* No PROV-CONFIRM -> abort before the destructive format. */
    feed("DONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-CONFIRM?"));
    TEST_ASSERT_TRUE(tx_has("PROV-ABORT"));
    TEST_ASSERT_FALSE(tx_has("PROV-OK"));
    TEST_ASSERT_EQUAL_INT(0, g_storage_format_calls);
}

static void test_provision_run_confirm_crlf(void)
{
    /* A CRLF-terminated confirmation must be accepted (trailing CR stripped). */
    feed("PROV-CONFIRM\r\nDONE\n");
    TEST_ASSERT_TRUE(provision_run());
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));
}

static void test_provision_run_storage_fail(void)
{
    g_storage_format_err = ESP_FAIL;
    feed("PROV-CONFIRM\nPUT 1 4 x\nABCD\nDONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_TRUE(tx_has("PROV-ERR storage"));
    TEST_ASSERT_EQUAL_INT(1, g_storage_format_calls);
}

static void test_provision_run_web_busy(void)
{
    /* An in-flight web upload/OTA must block the destructive format. */
    g_web_busy = 1;
    g_storage_format_calls = 0;
    feed("PROV-CONFIRM\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-ERR busy"));
    TEST_ASSERT_EQUAL_INT(0, g_storage_format_calls);
    TEST_ASSERT_EQUAL_INT(0, g_audio_stop_calls);
}

static void test_provision_run_put_and_done(void)
{
    feed("PROV-CONFIRM\nPUT 1 4 Lbl\nABCD\nDONE\n");
    TEST_ASSERT_TRUE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_TRUE(tx_has("PUT-OK"));
    TEST_ASSERT_TRUE(tx_has("FILE-OK"));
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));
    TEST_ASSERT_EQUAL_INT(1, g_tracks_save_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_calls);
}

static void test_provision_run_put_write_fail(void)
{
    g_prov_fwrite_fail = 1;
    feed("PROV-CONFIRM\nPUT 1 4 Lbl\nABCD\nDONE\n");
    provision_run();
    g_prov_fwrite_fail = 0;
    TEST_ASSERT_TRUE(tx_has("PUT-OK"));
    TEST_ASSERT_FALSE(tx_has("FILE-OK"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
    FILE *f = fopen(PROV_AUDIO_DIR "/slot1.wav", "rb");
    TEST_ASSERT_NULL(f);
    f = fopen(PROV_AUDIO_DIR ".prov_stage/slot1.wav", "rb");
    TEST_ASSERT_NULL(f);
}

static void test_ensure_uart_driver_idempotent(void)
{
    s_uart_driver_installed = false;
    mock_uart_driver_install_calls = 0;
    ensure_uart_driver();
    ensure_uart_driver(); /* second call returns on the installed guard */
    TEST_ASSERT_EQUAL_INT(1, mock_uart_driver_install_calls);
    TEST_ASSERT_TRUE(s_uart_driver_installed);
    s_uart_driver_installed = false;
}

static void test_provision_run_bad_put_slot(void)
{
    feed("PROV-CONFIRM\nPUT 99 4 x\nDONE\n");
    provision_run();
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
}

static void test_provision_run_command_timeout(void)
{
    feed("PROV-CONFIRM\n"); /* confirmed, then no commands at all */
    provision_run();
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
}

static void test_provision_run_fw_path(void)
{
    feed("PROV-CONFIRM\nFW 4\nABCD\nDONE\n");
    provision_run();
    TEST_ASSERT_TRUE(tx_has("FW-OK"));
    TEST_ASSERT_TRUE(tx_has("FW-DONE"));
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));
}

static void test_provision_run_wipe(void)
{
    feed("PROV-CONFIRM\nWIPE\nDONE\n");
    TEST_ASSERT_TRUE(provision_run());
    TEST_ASSERT_TRUE(tx_has("WIPE-OK"));
    TEST_ASSERT_EQUAL_INT(1, g_storage_format_calls);
    TEST_ASSERT_EQUAL_INT(1, g_tracks_save_calls);
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_calls);
}

static void test_provision_run_wipe_failure(void)
{
    g_storage_format_err = ESP_FAIL;
    feed("PROV-CONFIRM\nWIPE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("WIPE-ERR storage"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_calls);
}

/* ---- provision_try / listener ---- */

static void test_provision_try_paths(void)
{
    TEST_ASSERT_FALSE(provision_try()); /* no flag */

    set_prov_flag();
    feed("NOPE\n");
    TEST_ASSERT_FALSE(provision_try()); /* flag but no PROV in window */

    set_prov_flag();
    feed("PROV\nPROV-CONFIRM\nDONE\n");
    TEST_ASSERT_TRUE(provision_try());
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));
}

static void test_provision_listener_start(void)
{
    int before = mock_task_create_calls;
    provision_listener_start();
    TEST_ASSERT_EQUAL_INT(before + 1, mock_task_create_calls);
    mock_task_create_ok = 0;
    provision_listener_start(); /* task-create failure branch */
    TEST_ASSERT_EQUAL_INT(before + 2, mock_task_create_calls);
    mock_task_create_ok = 1;
}

/* G-RTOS-5: the provisioning worker owns the maintenance lease; subscribe it
 * to the task watchdog so a hung transfer panics/resets instead of leaving the
 * lease held forever. */
static void test_provision_listener_subscribes_to_twdt(void)
{
    TEST_ASSERT_EQUAL_UINT32(8192U, PROV_LISTENER_STACK);
    TEST_ASSERT_EQUAL_UINT32(4U, PROV_LISTENER_PRIO);

    s_listener_wdt = false;
    mock_task_wdt_add_err = 0;
    mock_task_wdt_reset_calls = 0;
    s_listen_iter_cap = 2;
    listener_task(NULL);
    s_listen_iter_cap = 0;
    TEST_ASSERT_TRUE(s_listener_wdt);
    TEST_ASSERT_GREATER_THAN(0, mock_task_wdt_reset_calls);
}

static void test_provision_listener_wdt_failure_does_not_feed(void)
{
    s_listener_wdt = false;
    mock_task_wdt_add_err = ESP_FAIL;
    mock_task_wdt_reset_calls = 0;
    s_listen_iter_cap = 1;
    listener_task(NULL);
    s_listen_iter_cap = 0;
    TEST_ASSERT_FALSE(s_listener_wdt);
    TEST_ASSERT_EQUAL_INT(0, mock_task_wdt_reset_calls);
    mock_task_wdt_add_err = 0;
}

static void test_provision_listener_task_paths(void)
{
    provision_listener_start(); /* install drivers */

    feed("BEMF?\n");
    s_listen_iter_cap = 12;
    listener_task(NULL);
    s_listen_iter_cap = 0;
    TEST_ASSERT_TRUE(tx_has("BEMF-OK"));

    /* PROV line triggers provision_run. */
    mock_uart_reset();
    feed("PROV\nPROV-CONFIRM\nDONE\n");
    s_listen_iter_cap = 20;
    listener_task(NULL);
    s_listen_iter_cap = 0;
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));

    /* While provisioning is already active the line is ignored. */
    mock_uart_reset();
    feed("PROV\n");
    s_provisioning = true;
    s_listen_iter_cap = 8;
    listener_task(NULL);
    s_listen_iter_cap = 0;
    s_provisioning = false;
    TEST_ASSERT_FALSE(tx_has("PROV-OK"));
}

static void test_provision_listener_selftest(void)
{
    provision_listener_start();
    mock_uart_reset();
    feed("SELFTEST\n");
    s_listen_iter_cap = 16;
    listener_task(NULL);
    s_listen_iter_cap = 0;
    TEST_ASSERT_TRUE(tx_has("SELFTEST-BEGIN"));
    TEST_ASSERT_TRUE(tx_has("TEST fake_a PASS"));
    TEST_ASSERT_TRUE(tx_has("TEST fake_b FAIL"));
    TEST_ASSERT_TRUE(tx_has("SELFTEST-END 1/2"));
}

static void test_provision_listener_hil_cmd(void)
{
    provision_listener_start();
    mock_uart_reset();
    feed("HIL-AUX 2 300\n");
    s_listen_iter_cap = 16;
    listener_task(NULL);
    s_listen_iter_cap = 0;
    TEST_ASSERT_TRUE(tx_has("HIL-AUX-OK 2 300"));
}

static void test_provision_hil_act_commands(void)
{
    mock_uart_reset();
    run_hil_act_console("HIL-AUX 2 300");
    TEST_ASSERT_TRUE(tx_has("HIL-AUX-OK 2 300"));
    TEST_ASSERT_EQUAL_UINT8(2, g_act_last_ch);
    TEST_ASSERT_EQUAL_UINT16(300, g_act_last_ms);

    run_hil_act_console("HIL-SOUND 1 500");
    TEST_ASSERT_TRUE(tx_has("HIL-SOUND-OK 1 500"));

    run_hil_act_console("HIL-MOTOR 30 800");
    TEST_ASSERT_TRUE(tx_has("HIL-MOTOR-OK 30 800"));

    run_hil_act_console("HIL-FN 5 1");
    TEST_ASSERT_TRUE(tx_has("HIL-FN-OK 5 1"));
    TEST_ASSERT_EQUAL_UINT8(5, g_act_last_fn);
    TEST_ASSERT_TRUE(g_act_last_on);

    run_hil_act_console("HIL-FN-SWEEP 100");
    TEST_ASSERT_TRUE(tx_has("HIL-FN-SWEEP-OK 29"));

    run_hil_act_console("HIL-AUX-SWEEP 100");
    TEST_ASSERT_TRUE(tx_has("HIL-AUX-SWEEP-OK 9"));

    run_hil_act_console("HIL-ENGINE 1");
    TEST_ASSERT_TRUE(tx_has("HIL-ENGINE-OK 1"));
    TEST_ASSERT_EQUAL_INT(1, g_sound_engine_set_calls);
    TEST_ASSERT_TRUE(g_sound_engine_last);

    run_hil_act_console("HIL-SCHEME-SPEED 120 1");
    TEST_ASSERT_TRUE(tx_has("HIL-SCHEME-SPEED-OK 120 1"));
    TEST_ASSERT_EQUAL_INT(1, g_sound_speed_calls);
    TEST_ASSERT_EQUAL_UINT8(120, g_sound_speed_last);
    TEST_ASSERT_TRUE(g_sound_speed_fwd_last);

    run_hil_act_console("HIL-NONSENSE");
    TEST_ASSERT_TRUE(tx_has("HIL-ERR"));
}

static void test_provision_hil_act_errors(void)
{
    mock_uart_reset();

    g_act_aux_err = ESP_ERR_INVALID_ARG;
    run_hil_act_console("HIL-AUX 99 300");
    TEST_ASSERT_TRUE(tx_has("HIL-AUX-ERR 99"));
    g_act_aux_err = ESP_OK;

    g_act_sound_err = ESP_ERR_NOT_FOUND;
    run_hil_act_console("HIL-SOUND 9 300");
    TEST_ASSERT_TRUE(tx_has("HIL-SOUND-ERR 9 no-track"));
    g_act_sound_err = ESP_FAIL;
    run_hil_act_console("HIL-SOUND 9 300");
    TEST_ASSERT_TRUE(tx_has("HIL-SOUND-ERR 9 bad-arg"));
    g_act_sound_err = ESP_OK;

    g_act_motor_err = ESP_FAIL;
    run_hil_act_console("HIL-MOTOR 70 300");
    TEST_ASSERT_TRUE(tx_has("HIL-MOTOR-ERR 70"));
    g_act_motor_err = ESP_OK;

    /* Out-of-range args are rejected before the uint8_t cast (REV-P1). */
    run_hil_act_console("HIL-MOTOR 300 800");
    TEST_ASSERT_TRUE(tx_has("HIL-MOTOR-ERR 300"));
    run_hil_act_console("HIL-MOTOR 0 800");
    TEST_ASSERT_TRUE(tx_has("HIL-MOTOR-ERR 0"));
    run_hil_act_console("HIL-MOTOR 30 -5");
    TEST_ASSERT_TRUE(tx_has("HIL-MOTOR-ERR 30"));
    g_sound_speed_calls = 0;
    run_hil_act_console("HIL-SCHEME-SPEED 300 1");
    TEST_ASSERT_TRUE(tx_has("HIL-SCHEME-SPEED-ERR 300"));
    TEST_ASSERT_EQUAL_INT(0, g_sound_speed_calls);

    g_act_fn_err = ESP_ERR_INVALID_ARG;
    run_hil_act_console("HIL-FN 99 1");
    TEST_ASSERT_TRUE(tx_has("HIL-FN-ERR 99"));
    g_act_fn_err = ESP_OK;

    g_fn_sweep_ret = 0;
    run_hil_act_console("HIL-FN-SWEEP 5");
    TEST_ASSERT_TRUE(tx_has("HIL-FN-SWEEP-ERR"));
    g_fn_sweep_ret = 29;

    g_aux_sweep_ret = 0;
    run_hil_act_console("HIL-AUX-SWEEP 5");
    TEST_ASSERT_TRUE(tx_has("HIL-AUX-SWEEP-ERR"));
    g_aux_sweep_ret = 9;
}

/* ---- UART/USB read helpers ---- */

static void test_read_helpers_usbjtag_paths(void)
{
    mock_usbjtag_install_ok = 1;
    ensure_usbjtag_driver(); /* s_usbjtag_ok = true */

    /* read_byte_any via the USB port (no UART data). */
    mock_usbjtag_byte = 'X';
    int64_t deadline = esp_timer_get_time() + 100000;
    uint8_t c = 0;
    TEST_ASSERT_TRUE(read_byte_any(&c, deadline));
    TEST_ASSERT_EQUAL_UINT8('X', c);

    /* read_exact via the USB port. */
    mock_usbjtag_byte = 'Y';
    uint8_t buf[1] = { 0 };
    TEST_ASSERT_TRUE(read_exact(buf, 1, 50));
    TEST_ASSERT_EQUAL_UINT8('Y', buf[0]);
}

static void test_uart_driver_install_failure_logged(void)
{
    s_uart_driver_installed = false; /* force the install path */
    mock_uart_driver_install_err = ESP_FAIL;
    mock_uart_driver_install_calls = 0;
    provision_listener_start(); /* covers the ESP_LOGW branch */
    TEST_ASSERT_EQUAL_INT(1, mock_uart_driver_install_calls);
    TEST_ASSERT_FALSE(s_uart_driver_installed); /* failed install is retried */
    mock_uart_driver_install_err = 0;
    /* Mark it so later tests do not retry the (now mocked) driver install. */
    s_uart_driver_installed = true;
}

static void test_get_clear_prov_flag_errors(void)
{
    mock_nvs_open_fail = 1;
    TEST_ASSERT_FALSE(get_prov_flag()); /* 56: open failure */
    clear_prov_flag();                  /* 68: open failure, no crash */
    mock_nvs_open_fail = 0;
}

static void test_ensure_usbjtag_idempotent(void)
{
    mock_usbjtag_install_ok = 1;
    ensure_usbjtag_driver();
    ensure_usbjtag_driver(); /* 160: already installed -> early return */
    TEST_ASSERT_TRUE(s_usbjtag_ok);
}

static void test_send_line_usb_branch(void)
{
    mock_usbjtag_install_ok = 1;
    ensure_usbjtag_driver();
    tx_reset();
    send_line("hello"); /* 184-185: USB write branch */
    TEST_ASSERT_TRUE(tx_has("hello"));
}

static void test_provision_fw_no_mem(void)
{
    g_malloc_fail = 1;
    tx_reset();
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, provision_fw("FW 4"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR mem"));
    g_malloc_fail = 0;
}

static void test_provision_run_fw_abort(void)
{
    feed("PROV-CONFIRM\nFW 0\nDONE\n"); /* provision_fw rejects size -> run aborts */
    provision_run();
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
}

static void test_provision_run_put_open_fail(void)
{
    feed("PROV-CONFIRM\nPUT\nPUT 1 4 x\n");
    /* A directory where the staged WAV should go makes fopen("...slot1.wav",
     * "wb") fail after the format/preflight succeeded. */
    (void)MKDIR(PROV_AUDIO_DIR ".prov_stage");
    (void)MKDIR(PROV_AUDIO_DIR ".prov_stage/slot1.wav");
    provision_run();
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_FALSE(tx_has("FILE-OK"));
    (void)RMDIR(PROV_AUDIO_DIR ".prov_stage/slot1.wav");
    (void)RMDIR(PROV_AUDIO_DIR ".prov_stage");
}

static void test_provision_run_put_no_mem(void)
{
    feed("PROV-CONFIRM\nPUT 1 4 x\n");
    g_malloc_fail = 1; /* 543-544: chunk buffer allocation fails */
    provision_run();
    g_malloc_fail = 0;
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_FALSE(tx_has("PUT-OK")); /* returned before the data phase */
}

static void test_provision_run_put_timeout(void)
{
    feed("PROV-CONFIRM\nPUT 1 4 x\n"); /* claims 4 bytes, none follow */
    provision_run();
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_TRUE(tx_has("PROV-ERR timeout"));
    TEST_ASSERT_FALSE(tx_has("FILE-OK"));
    FILE *f = fopen(PROV_AUDIO_DIR ".prov_stage/slot1.wav", "rb");
    TEST_ASSERT_NULL(f);
}

static void test_provision_run_put_chunk_ack(void)
{
    static char big[PROV_CHUNK + 1];
    memset(big, 'A', sizeof(big));
    feed("PROV-CONFIRM\nPUT 1 4097 x\n");
    mock_uart_feed(big, sizeof(big));
    feed("\nDONE\n");
    provision_run();
    TEST_ASSERT_TRUE(tx_has("PUT-OK"));
    TEST_ASSERT_TRUE(tx_has("CHUNK")); /* 565 */
    TEST_ASSERT_TRUE(tx_has("FILE-OK"));
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Minimal one-entry PCM16 pack: header (16) + index (16 + "test") + payload. */
static size_t build_test_pack(uint8_t *pack)
{
    const uint32_t data_off = 36;
    memcpy(pack, "AURASP01", 8);
    put16(pack + 8, 1);
    put16(pack + 10, 1);
    put32(pack + 12, data_off);
    pack[16] = 4;      /* name length */
    pack[17] = 100;    /* volume */
    pack[18] = 1;      /* channels */
    pack[19] = 16;     /* PCM16 */
    put32(pack + 20, 8000);
    put32(pack + 24, data_off);
    put32(pack + 28, 4); /* two PCM16 samples */
    memcpy(pack + 32, "test", 4);
    pack[36] = 1;
    pack[37] = 2;
    pack[38] = 3;
    pack[39] = 4;
    return 40;
}

/* One-entry pack carrying an embedded sound scheme (bits = 0). */
static size_t build_scheme_pack(uint8_t *pack)
{
    const char *name = AUDIO_PACK_SCHEME_NAME;
    const uint8_t nlen = (uint8_t)strlen(name);
    const uint32_t data_off = AUDIO_PACK_HDR_LEN + AUDIO_PACK_ENTRY_HDR_LEN + nlen;
    const uint32_t data_len = 2; /* "{}" */
    memcpy(pack, "AURASP01", 8);
    put16(pack + 8, 1);
    put16(pack + 10, 1);
    put32(pack + 12, data_off);
    pack[16] = nlen;
    pack[17] = 0;
    pack[18] = 0;
    pack[19] = AUDIO_PACK_BITS_SCHEME;
    put32(pack + 20, 0);
    put32(pack + 24, data_off);
    put32(pack + 28, data_len);
    memcpy(pack + 32, name, nlen);
    memcpy(pack + data_off, "{}", data_len);
    return data_off + data_len;
}

static void test_provision_run_pack_success(void)
{
    uint8_t pack[64];
    uint8_t stream[128];
    size_t plen = build_test_pack(pack);
    size_t n = 0;

    memcpy(stream + n, "PROV-CONFIRM\n", 13); n += 13;
    memcpy(stream + n, "PACK 40\n", 8); n += 8;
    memcpy(stream + n, "PACK-DATA 40\n", 13); n += 13;
    memcpy(stream + n, pack, plen); n += plen;
    memcpy(stream + n, "DONE\n", 5); n += 5;
    mock_uart_feed(stream, n);
    tx_reset();

    TEST_ASSERT_TRUE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PACK-READY"));
    TEST_ASSERT_TRUE(tx_has("PACK-OK 1"));
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));

    FILE *f = fopen(PROV_AUDIO_DIR "/test.wav", "rb");
    TEST_ASSERT_NOT_NULL(f);
    uint8_t wav[48] = { 0 };
    TEST_ASSERT_EQUAL_UINT32(sizeof(wav), fread(wav, 1, sizeof(wav), f));
    fclose(f);
    TEST_ASSERT_EQUAL_MEMORY("RIFF", wav, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVE", wav + 8, 4);
    TEST_ASSERT_EQUAL_MEMORY("data", wav + 36, 4);
    TEST_ASSERT_EQUAL_UINT8(4, wav[40]);
    TEST_ASSERT_EQUAL_UINT8(1, wav[44]);
    TEST_ASSERT_EQUAL_UINT8(4, wav[47]);
    TEST_ASSERT_EQUAL_INT(1, g_tracks_save_calls);
}

static void test_provision_pack_errors(void)
{
    feed("PROV-CONFIRM\nPACK 0\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PACK-ERR parse"));

    static const uint8_t zeros[16] = { 0 };
    feed("PROV-CONFIRM\nPACK 16\nPACK-DATA 16\n");
    mock_uart_feed(zeros, sizeof(zeros));
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PACK-READY"));
    TEST_ASSERT_TRUE(tx_has("PACK-ERR header"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
}

static void test_provision_pack_scheme_is_rejected(void)
{
    uint8_t pack[128];
    size_t plen = build_scheme_pack(pack);
    char head[64];
    snprintf(head, sizeof(head), "PROV-CONFIRM\nPACK %u\nPACK-DATA %u\n",
             (unsigned)plen, (unsigned)plen);
    feed(head);
    mock_uart_feed(pack, plen);
    tx_reset();
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PACK-READY"));
    TEST_ASSERT_TRUE(tx_has("PACK-ERR scheme"));
    TEST_ASSERT_FALSE(tx_has("PACK-OK"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
    TEST_ASSERT_EQUAL_INT(0, g_storage_format_calls);
}

static void test_provision_pack_failure_leaves_no_library(void)
{
    uint8_t pack[64];
    uint8_t stream[128];
    size_t plen = build_test_pack(pack);
    size_t n = 0;
    const char *head = "PROV-CONFIRM\nPACK 40\nPACK-DATA 40\n";
    memcpy(stream + n, head, strlen(head)); n += strlen(head);
    memcpy(stream + n, pack, plen); n += plen;
    mock_uart_feed(stream, n);
    g_prov_fwrite_fail = 1;
    tx_reset();
    TEST_ASSERT_FALSE(provision_run());
    g_prov_fwrite_fail = 0;
    TEST_ASSERT_TRUE(tx_has("PACK-ERR write"));
    TEST_ASSERT_FALSE(tx_has("PACK-OK"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
    TEST_ASSERT_EQUAL_INT(1, g_storage_format_calls);
    FILE *f = fopen(PROV_AUDIO_DIR "/test.wav", "rb");
    TEST_ASSERT_NULL(f);
    f = fopen(PROV_AUDIO_DIR ".prov_stage/test.wav", "rb");
    TEST_ASSERT_NULL(f);
}

static void test_provision_space_preflight_rejects_before_format(void)
{
    g_free_bytes = 1024; /* less than the reserve, let alone the payload */
    feed("PROV-CONFIRM\nPUT 1 4 x\nABCD\nDONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-ERR space"));
    TEST_ASSERT_EQUAL_INT(0, g_storage_format_calls);
    TEST_ASSERT_FALSE(tx_has("PUT-OK"));
    TEST_ASSERT_FALSE(tx_has("FILE-OK"));

    uint8_t pack[64];
    uint8_t stream[128];
    size_t plen = build_test_pack(pack);
    size_t n = 0;
    mock_uart_reset();
    const char *head = "PROV-CONFIRM\nPACK 40\nPACK-DATA 40\n";
    memcpy(stream + n, head, strlen(head)); n += strlen(head);
    memcpy(stream + n, pack, plen); n += plen;
    mock_uart_feed(stream, n);
    tx_reset();
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PACK-ERR space"));
    TEST_ASSERT_EQUAL_INT(0, g_storage_format_calls);
}

static void test_provision_install_marker_recovers_on_boot(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, install_marker_set(1));
    TEST_ASSERT_TRUE(install_marker_get());
    TEST_ASSERT_FALSE(provision_try()); /* no PROV flag: recovery only */
    TEST_ASSERT_FALSE(install_marker_get());
    TEST_ASSERT_EQUAL_INT(1, g_storage_format_calls);
    TEST_ASSERT_EQUAL_INT(1, g_tracks_save_calls);
    TEST_ASSERT_EQUAL_INT(0, g_storage_maint_depth);
}

static void test_provision_quiescence_failure_never_formats(void)
{
    g_maintenance_err = ESP_ERR_TIMEOUT;
    feed("PROV-CONFIRM\nDONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-ERR busy"));
    TEST_ASSERT_EQUAL_INT(0, g_storage_format_calls);
    TEST_ASSERT_EQUAL_INT(0, g_maintenance_end_calls);
    TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
}

static void test_provision_failed_put_paths_release_maintenance(void)
{
    const char *sessions[] = {
        "PROV-CONFIRM\nPUT 99 4 x\n",
        "PROV-CONFIRM\nPUT 1 4 x\n",
        "PROV-CONFIRM\nPUT 1 4 x\nABCD\nDONE\n",
        "PROV-CONFIRM\nPUT 1 4 x\nABCD\nDONE\n"
    };
    for (size_t i = 0; i < sizeof(sessions) / sizeof(sessions[0]); ++i) {
        mock_uart_reset();
        g_malloc_fail = i == 2;
        g_prov_fwrite_fail = i == 3;
        feed(sessions[i]);
        TEST_ASSERT_FALSE(provision_run());
        TEST_ASSERT_EQUAL_INT(i + 1, g_maintenance_end_calls);
        TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
        TEST_ASSERT_EQUAL_UINT32(115200, g_baud);
        TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
        TEST_ASSERT_FALSE(tx_has("DONE-OK"));
    }
    g_malloc_fail = g_prov_fwrite_fail = 0;
}

static void test_provision_wav_failure_after_fw_never_selects_boot(void)
{
    g_wav_err = ESP_FAIL;
    feed("PROV-CONFIRM\nFW 4\nABCD\nPUT 1 4 x\nABCD\nDONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("FW-DONE"));
    TEST_ASSERT_TRUE(tx_has("PROV-ERR wav"));
    TEST_ASSERT_EQUAL_INT(1, g_fs_release_calls);
    TEST_ASSERT_EQUAL_INT(1, g_validate_calls);
    TEST_ASSERT_EQUAL_INT(1, g_tracks_save_calls); /* rollback clears metadata */
    TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_end_calls);
    TEST_ASSERT_NULL(s_pending_boot);
}

static void test_provision_put_durability_failures_release_without_boot(void)
{
    for (int failure = 0; failure < 3; ++failure) {
        mock_uart_reset();
        g_prov_flush_fail = failure == 0;
        g_prov_sync_fail = failure == 1;
        g_prov_close_fail = failure == 2;
        feed("PROV-CONFIRM\nFW 4\nABCD\nPUT 1 4 x\nABCD\nDONE\n");
        TEST_ASSERT_FALSE(provision_run());
        TEST_ASSERT_TRUE(tx_has("PROV-ERR write"));
        TEST_ASSERT_EQUAL_INT(failure + 1, g_maintenance_end_calls);
        TEST_ASSERT_EQUAL_INT(0, g_maintenance_depth);
        TEST_ASSERT_EQUAL_UINT32(115200, g_baud);
        TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
        TEST_ASSERT_EQUAL_INT(failure + 1, g_tracks_save_calls); /* rollback clears metadata */
        TEST_ASSERT_FALSE(tx_has("DONE-OK"));
        TEST_ASSERT_NULL(s_pending_boot);
    }
    g_prov_flush_fail = g_prov_sync_fail = g_prov_close_fail = 0;
}

static void test_provision_metadata_failure_after_fw_never_selects_boot(void)
{
    g_tracks_save_err = ESP_FAIL;
    feed("PROV-CONFIRM\nFW 4\nABCD\nPUT 1 4 x\nABCD\nDONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-ERR metadata"));
    TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_end_calls);
    TEST_ASSERT_NULL(s_pending_boot);
}

static void test_provision_later_put_failure_does_not_select_received_fw(void)
{
    feed("PROV-CONFIRM\nFW 4\nABCD\nPUT 99 4 x\nDONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("FW-DONE"));
    TEST_ASSERT_EQUAL_INT(0, g_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, g_tracks_save_calls);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_end_calls);
}

static void test_provision_done_selects_boot_once_after_valid_metadata(void)
{
    feed("PROV-CONFIRM\nFW 4\nABCD\nPUT 1 4 x\nABCD\nDONE\n");
    TEST_ASSERT_TRUE(provision_run());
    TEST_ASSERT_EQUAL_INT(1, g_validate_calls);
    TEST_ASSERT_EQUAL_INT(1, g_tracks_save_calls);
    TEST_ASSERT_EQUAL_INT(1, g_boot_calls);
    TEST_ASSERT_EQUAL_INT(1, g_fs_release_calls);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_end_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_calls);
    TEST_ASSERT_TRUE(tx_has("DONE-OK"));
    TEST_ASSERT_NULL(s_pending_boot);
}

static void test_provision_done_boot_selection_failure_cleans_up(void)
{
    mock_ota_set_boot_err = ESP_FAIL;
    feed("PROV-CONFIRM\nFW 4\nABCD\nDONE\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("FW-ERR boot"));
    TEST_ASSERT_FALSE(tx_has("DONE-OK"));
    TEST_ASSERT_EQUAL_INT(1, g_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_calls);
    TEST_ASSERT_EQUAL_INT(1, g_maintenance_end_calls);
}

static void test_bemf_adc_checked_error_is_not_zero_success(void)
{
    g_adc_err = ESP_ERR_TIMEOUT;
    handle_bemf_cmd("BEMF-ADC");
    TEST_ASSERT_TRUE(tx_has("BEMF-ADC-ERR"));
    TEST_ASSERT_FALSE(tx_has("BEMF-ADC 1 2 3"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bemf_info_commands);
    RUN_TEST(test_bemf_cal_commands);
    RUN_TEST(test_bemf_cvset_and_test);
    RUN_TEST(test_bemf_set_command);
    RUN_TEST(test_bemf_check_command);
    RUN_TEST(test_provision_fw_errors);
    RUN_TEST(test_provision_fw_success_and_write_errors);
    RUN_TEST(test_provision_fw_chunk_ack);
    RUN_TEST(test_provision_run_requires_confirm);
    RUN_TEST(test_provision_run_confirm_crlf);
    RUN_TEST(test_provision_run_storage_fail);
    RUN_TEST(test_provision_run_web_busy);
    RUN_TEST(test_provision_run_put_and_done);
    RUN_TEST(test_provision_run_put_write_fail);
    RUN_TEST(test_ensure_uart_driver_idempotent);
    RUN_TEST(test_provision_run_bad_put_slot);
    RUN_TEST(test_provision_run_command_timeout);
    RUN_TEST(test_provision_run_fw_path);
    RUN_TEST(test_provision_run_wipe);
    RUN_TEST(test_provision_run_wipe_failure);
    RUN_TEST(test_provision_try_paths);
    RUN_TEST(test_provision_listener_start);
    RUN_TEST(test_provision_listener_subscribes_to_twdt);
    RUN_TEST(test_provision_listener_wdt_failure_does_not_feed);
    RUN_TEST(test_provision_listener_task_paths);
    RUN_TEST(test_provision_listener_selftest);
    RUN_TEST(test_provision_listener_hil_cmd);
    RUN_TEST(test_provision_hil_act_commands);
    RUN_TEST(test_provision_hil_act_errors);
    RUN_TEST(test_read_helpers_usbjtag_paths);
    RUN_TEST(test_uart_driver_install_failure_logged);
    RUN_TEST(test_get_clear_prov_flag_errors);
    RUN_TEST(test_ensure_usbjtag_idempotent);
    RUN_TEST(test_send_line_usb_branch);
    RUN_TEST(test_provision_fw_no_mem);
    RUN_TEST(test_provision_run_fw_abort);
    RUN_TEST(test_provision_run_put_open_fail);
    RUN_TEST(test_provision_run_put_no_mem);
    RUN_TEST(test_provision_run_put_timeout);
    RUN_TEST(test_provision_run_put_chunk_ack);
    RUN_TEST(test_provision_run_pack_success);
    RUN_TEST(test_provision_pack_errors);
    RUN_TEST(test_provision_pack_scheme_is_rejected);
    RUN_TEST(test_provision_pack_failure_leaves_no_library);
    RUN_TEST(test_provision_space_preflight_rejects_before_format);
    RUN_TEST(test_provision_install_marker_recovers_on_boot);
    RUN_TEST(test_provision_quiescence_failure_never_formats);
    RUN_TEST(test_provision_failed_put_paths_release_maintenance);
    RUN_TEST(test_provision_wav_failure_after_fw_never_selects_boot);
    RUN_TEST(test_provision_put_durability_failures_release_without_boot);
    RUN_TEST(test_provision_metadata_failure_after_fw_never_selects_boot);
    RUN_TEST(test_provision_later_put_failure_does_not_select_received_fw);
    RUN_TEST(test_provision_done_selects_boot_once_after_valid_metadata);
    RUN_TEST(test_provision_done_boot_selection_failure_cleans_up);
    RUN_TEST(test_bemf_adc_checked_error_is_not_zero_success);
    return UNITY_END();
}
