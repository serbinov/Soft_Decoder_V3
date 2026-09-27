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
static size_t mock_prov_fwrite(const void *p, size_t sz, size_t n, FILE *f);
#define PROV_FWRITE(p, sz, n, f) mock_prov_fwrite((p), (sz), (n), (f))

#define static
#include "../../components/provision/src/provision.c"
#undef static
#undef malloc

#include "../../test_libs/teststubs/stubs.c"

static size_t mock_prov_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (g_prov_fwrite_fail) {
        return 0;
    }
    return fwrite(p, sz, n, f);
}

/* ---- collaborator stubs ---- */
static esp_err_t g_cal_start_err = ESP_OK;
static esp_err_t g_cal_clear_err = ESP_OK;
static esp_err_t g_cal_save_err = ESP_OK;
static esp_err_t g_storage_format_err = ESP_OK;
static int g_storage_format_calls;
static int g_tracks_save_calls;
static int g_cv_write_calls;

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

void motor_bemf_adc_dump(uint16_t *b1, uint16_t *b2, uint16_t *rail)
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

esp_err_t motor_bemf_cal_clear(void)
{
    return g_cal_clear_err;
}

esp_err_t motor_bemf_cal_reload(void)
{
    return ESP_OK;
}

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    (void)speed128;
    (void)forward;
    return ESP_OK;
}

static int g_motor_es_calls;
static int g_audio_stop_calls;
static int g_web_busy;

void motor_emergency_stop(void)
{
    g_motor_es_calls++;
}

void audio_stop_all(void)
{
    g_audio_stop_calls++;
}

bool web_fs_busy(void)
{
    return g_web_busy != 0;
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
    return ESP_OK;
}

esp_err_t storage_format(void)
{
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
    g_web_busy = 0;
    g_audio_stop_calls = 0;
    g_motor_es_calls = 0;
    s_usbjtag_ok = false;
    s_provisioning = false;
    s_listen_iter_cap = 0;
    (void)MKDIR(PROV_AUDIO_DIR);
}

void tearDown(void)
{
    remove(PROV_AUDIO_DIR "/slot1.wav");
    remove(PROV_AUDIO_DIR "/slot99.wav");
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

    tx_reset();
    handle_bemf_cmd("BEMF-TEST 60 rev");
    TEST_ASSERT_TRUE(tx_has("BEMF-TEST-OK 60 rev"));

    tx_reset();
    handle_bemf_cmd("BEMF-TEST 200");
    TEST_ASSERT_TRUE(tx_has("BEMF-ERR speed"));
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
    TEST_ASSERT_EQUAL(ESP_FAIL, provision_fw("FW 4"));
    TEST_ASSERT_TRUE(tx_has("FW-ERR boot"));
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
    feed("PROV-CONFIRM\n");
    TEST_ASSERT_FALSE(provision_run());
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_TRUE(tx_has("PROV-ERR storage"));
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
    TEST_ASSERT_EQUAL_INT(1, g_audio_stop_calls);
    g_web_busy = 0;
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
}

static void test_ensure_uart_driver_idempotent(void)
{
    s_uart_driver_installed = false;
    ensure_uart_driver();
    ensure_uart_driver(); /* second call returns on the installed guard */
    s_uart_driver_installed = false;
    TEST_PASS();
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
    provision_listener_start();
    mock_task_create_ok = 0;
    provision_listener_start(); /* task-create failure branch */
    mock_task_create_ok = 1;
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
    provision_listener_start(); /* covers the ESP_LOGW branch */
    mock_uart_driver_install_err = 0;
    /* The failed install does not set the installed flag; mark it so later
     * tests do not retry the (now mocked) driver install. */
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
    /* A directory where the WAV should go makes fopen("...slot1.wav","wb")
     * fail (provision_run recreates the parent dir, so removing it is not
     * enough). */
    (void)MKDIR(PROV_AUDIO_DIR "/slot1.wav");
    provision_run();
    TEST_ASSERT_TRUE(tx_has("PROV-OK"));
    TEST_ASSERT_FALSE(tx_has("FILE-OK"));
    (void)RMDIR(PROV_AUDIO_DIR "/slot1.wav");
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
    TEST_ASSERT_FALSE(tx_has("FILE-OK"));
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bemf_info_commands);
    RUN_TEST(test_bemf_cal_commands);
    RUN_TEST(test_bemf_cvset_and_test);
    RUN_TEST(test_bemf_set_command);
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
    RUN_TEST(test_provision_try_paths);
    RUN_TEST(test_provision_listener_start);
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
    return UNITY_END();
}
