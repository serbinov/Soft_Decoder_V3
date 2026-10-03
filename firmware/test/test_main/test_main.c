#include <unity.h>
#include <setjmp.h>
#include <sys/stat.h>
#include "esp_ota_ops.h"

/* Host SDK header omissions have been reported to the shared-stub owner. */
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
#ifndef ESP_ERR_OTA_ROLLBACK_INVALID_STATE
#define ESP_ERR_OTA_ROLLBACK_INVALID_STATE 0x1506
#endif

/* No host filesystem or hardware operations are allowed in the boot suite. */
static int no_files_mkdir(const char *path, int mode)
{
    (void)path;
    (void)mode;
    return 0;
}
#define mkdir no_files_mkdir
#include "../../main/app_main.c"
#undef mkdir
#define esp_ota_mark_app_valid_cancel_rollback sdk_mark_app_valid_cancel_rollback
#define vTaskDelay sdk_task_delay
#include "../../test_libs/teststubs/stubs.c"
#undef esp_ota_mark_app_valid_cancel_rollback
#undef vTaskDelay

static esp_err_t g_web_err, g_track_err, g_ready_err, g_motor_err, g_cv_err;
static esp_err_t g_hard_reset_err, g_safety_ack;
static bool g_send_ack, g_rails, g_dc, g_enabled, g_admitted;
static bool g_boot_mode, g_bound_worker;
static int g_worker_delays;
static jmp_buf g_worker_exit;
static int g_lease_depth, g_lease_ends, g_motor_calls, g_notes, g_motion;
static int g_functions, g_sound_stops, g_sound_requests, g_reload, g_deferred, g_flush, g_volume;
static int g_graph_faults, g_graph_installs;
static unsigned g_cal_watchdog_calls;
static bool g_bound_safety;
static unsigned g_safety_delays;
static jmp_buf g_safety_exit;
static bool g_graph_selected;
static bool g_storage_mounted;
static esp_err_t g_selection_err, g_graph_read_err, g_graph_parse_err, g_graph_assets_err;
static esp_err_t g_sound_init_err;
static int g_listener, g_confirm, g_hard_reset_calls;
static uint32_t g_mask;
static int64_t g_tick, g_packet;
static uint8_t g_cv[SETTINGS_CV_COUNT + 1];
static dcc_speed_cb_t g_speed_cb;
static dcc_function_cb_t g_function_cb;
static dcc_cv_write_cb_t g_write_cb;
static dcc_cv_read_cb_t g_read_cb;
static dcc_reset_cb_t g_reset_cb, g_estop_cb, g_hard_cb;

esp_err_t pinmap_validate(void) { return ESP_OK; }
void motor_boot_safe(void) { g_mask = MOTOR_INHIBIT_CONTROL; }
esp_err_t motor_init(void) { return ESP_OK; }
esp_err_t storage_init(void) { return ESP_OK; }
esp_err_t storage_mount(void) { return ESP_OK; }
storage_backend_t storage_get_backend(void) { return STORAGE_BACKEND_NONE; }
bool storage_is_mounted(void) { return g_storage_mounted; }
void motor_bemf_cal_watchdog(void) { ++g_cal_watchdog_calls; }
esp_err_t settings_init(void) { return ESP_OK; }
esp_err_t audio_init(void) { return ESP_OK; }
esp_err_t sound_init(void) { return g_sound_init_err; }
esp_err_t sg_store_selection(char *id, size_t capacity, uint32_t *revision, bool *selected)
{
    snprintf(id, capacity, "%s", "saved_graph");
    *revision = 2;
    *selected = g_graph_selected;
    return g_selection_err;
}
esp_err_t sg_store_read(const char *id, uint32_t revision, char **out, size_t *length, uint32_t *actual)
{
    (void)id;
    if (g_graph_read_err != ESP_OK) { return g_graph_read_err; }
    *out = malloc(3);
    memcpy(*out, "{}", 3);
    *length = 2;
    *actual = revision;
    return ESP_OK;
}
esp_err_t sg_parse(const char *json, size_t length, sg_graph_t *out, sg_diagnostic_t *diag)
{
    (void)json; (void)length; (void)diag;
    memset(out, 0, sizeof(*out));
    return g_graph_parse_err;
}
esp_err_t sg_assets_validate(const sg_graph_t *graph, sg_diagnostic_t *diag)
{
    (void)graph; (void)diag;
    return g_graph_assets_err;
}
esp_err_t sound_graph_install(const sg_graph_t *graph, const char *id, uint32_t revision)
{
    (void)graph; (void)id;
    TEST_ASSERT_EQUAL(2, revision);
    ++g_graph_installs;
    return ESP_OK;
}
void sound_graph_fail_closed(void) { ++g_graph_faults; }
esp_err_t dcc_init(void) { g_enabled = false; return ESP_OK; }
bool provision_try(void) { return false; }
void provision_listener_start(void) { ++g_listener; }
void track_recover_from_storage(const char *audio, const char *root,
                                track_recover_result_t *out)
{
    TEST_ASSERT_EQUAL_STRING("/userdata/audio", audio);
    TEST_ASSERT_EQUAL_STRING("/userdata", root);
    memset(out, 0, sizeof(*out));
}
esp_err_t web_init(void) { g_boot_mode = true; return g_web_err; }
esp_err_t track_init(void) { return g_track_err; }
esp_err_t web_set_actuation_ready(bool ready)
{
    TEST_ASSERT_TRUE(ready);
    TEST_ASSERT_NOT_NULL(g_speed_cb);
    TEST_ASSERT_NOT_NULL(g_estop_cb);
    TEST_ASSERT_NOT_NULL(g_hard_cb);
    TEST_ASSERT_EQUAL_INT(2, mock_task_create_calls);
    if (g_ready_err == ESP_OK) {
        g_admitted = true;
        g_mask &= ~MOTOR_INHIBIT_CONTROL;
    }
    return g_ready_err;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    TEST_ASSERT_TRUE(g_admitted);
    TEST_ASSERT_TRUE(g_enabled);
    ++g_confirm;
    return ESP_OK;
}
bool track_is_dc_mode(void) { return g_dc; }
bool web_control_is_rails(void) { return g_rails && (!g_boot_mode || g_admitted); }
bool web_control_rails_begin(uint32_t *generation)
{
    (void)generation;
    if (!web_control_is_rails()) { return false; }
    ++g_lease_depth;
    return true;
}
void web_control_rails_end(void)
{
    TEST_ASSERT_EQUAL_INT(1, g_lease_depth);
    --g_lease_depth;
    ++g_lease_ends;
}
esp_err_t motor_set_speed(uint8_t speed, bool forward)
{
    (void)speed;
    (void)forward;
    TEST_ASSERT_EQUAL_INT(1, g_lease_depth);
    ++g_motor_calls;
    return g_motor_err;
}
void motor_emergency_stop(void)
{
    TEST_ASSERT_EQUAL_INT(1, g_lease_depth);
    ++g_motor_calls;
}
void track_note_dcc_motor_command(void)
{
    TEST_ASSERT_EQUAL_INT(1, g_lease_depth);
    ++g_notes;
}
void web_motion_changed(uint8_t speed, bool forward)
{
    (void)speed;
    (void)forward;
    TEST_ASSERT_EQUAL_INT(1, g_lease_depth);
    ++g_motion;
}
void web_apply_function(uint8_t fn, bool state)
{
    (void)fn;
    (void)state;
    ++g_functions;
}
void sound_stop_all(void) { ++g_sound_stops; }
void sound_request_stop(void) { ++g_sound_requests; }
void web_log_event(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}
esp_err_t motor_set_inhibit_reason(motor_inhibit_reason_t reason, bool inhibited)
{
    if (inhibited) { g_mask |= reason; } else { g_mask &= ~reason; }
    return ESP_OK;
}
int64_t motor_last_tick_us(void) { return g_tick; }
int64_t dcc_last_packet_us(void) { return g_packet; }
void dcc_set_control_enabled(bool enabled) { g_enabled = enabled; }
void dcc_reload_config(void) { ++g_reload; }
void dcc_register_speed_cb(dcc_speed_cb_t cb) { g_speed_cb = cb; }
void dcc_register_function_cb(dcc_function_cb_t cb) { g_function_cb = cb; }
void dcc_register_cv_write_cb(dcc_cv_write_cb_t cb) { g_write_cb = cb; }
void dcc_register_cv_read_cb(dcc_cv_read_cb_t cb) { g_read_cb = cb; }
void dcc_register_reset_cb(dcc_reset_cb_t cb) { g_reset_cb = cb; }
void dcc_register_emergency_stop_cb(void (*cb)(void)) { g_estop_cb = cb; }
void dcc_register_hard_reset_cb(void (*cb)(void)) { g_hard_cb = cb; }
esp_err_t settings_cv_read(uint16_t cv, uint8_t *out)
{
    if (cv == 0 || cv > SETTINGS_CV_COUNT || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = g_cv[cv];
    return ESP_OK;
}
esp_err_t settings_cv_write(uint16_t cv, uint8_t value)
{
    if (g_cv_err != ESP_OK) { return g_cv_err; }
    if (cv == 8 && value == 8) { g_cv[63] = 128; }
    else { g_cv[cv] = value; }
    return ESP_OK;
}
void settings_cv_commit_deferred(void) { ++g_deferred; }
esp_err_t settings_pending_flush(void) { ++g_flush; return ESP_OK; }
esp_err_t settings_cv_hard_reset(void)
{
    ++g_hard_reset_calls;
    if (g_hard_reset_err != ESP_OK) { return g_hard_reset_err; }
    g_cv[19] = 0;
    g_cv[29] = 2;
    g_cv[31] = g_cv[32] = 0;
    return ESP_OK;
}
void web_master_volume_changed(uint8_t volume) { g_volume = volume; }

void vTaskDelay(TickType_t ticks)
{
    if (g_bound_safety) {
        TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(10), ticks);
        if (++g_safety_delays > 3U) { longjmp(g_safety_exit, 1); }
    }
    if (g_bound_worker) {
        TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(100), ticks);
        if (++g_worker_delays > 1) { longjmp(g_worker_exit, 1); }
    }
    sdk_task_delay(ticks);
    if (g_bound_safety) { g_tick = esp_timer_get_time(); }
}

static void startup_hook(void (*task)(void *), void *param)
{
    (void)param;
    if (task == safety_task && g_send_ack) {
        TEST_ASSERT_EQUAL(ESP_OK, esp_task_wdt_add(NULL));
        TEST_ASSERT_TRUE(g_tick != 0);
        TEST_ASSERT_TRUE(esp_timer_get_time() - g_tick <= 200000);
        TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(s_safety_ready, &g_safety_ack, 0));
    }
}

void setUp(void)
{
    g_web_err = g_track_err = g_ready_err = g_motor_err = g_cv_err = ESP_OK;
    g_hard_reset_err = g_safety_ack = ESP_OK;
    g_send_ack = g_rails = true;
    g_dc = g_enabled = g_admitted = false;
    g_boot_mode = g_bound_worker = false;
    g_worker_delays = 0;
    g_lease_depth = g_lease_ends = g_motor_calls = g_notes = g_motion = 0;
    g_functions = g_sound_stops = g_reload = g_deferred = g_flush = 0;
    g_sound_requests = g_graph_faults = g_graph_installs = 0;
    g_cal_watchdog_calls = 0;
    g_bound_safety = false;
    g_safety_delays = 0;
    g_graph_selected = false;
    g_storage_mounted = true;
    g_selection_err = g_graph_read_err = g_graph_parse_err = g_graph_assets_err = ESP_OK;
    g_sound_init_err = ESP_OK;
    g_volume = g_listener = g_confirm = g_hard_reset_calls = 0;
    g_mask = MOTOR_INHIBIT_CONTROL;
    mock_timer_now_us = 1000000;
    g_tick = g_packet = mock_timer_now_us;
    memset(g_cv, 0, sizeof(g_cv));
    g_speed_cb = NULL; g_function_cb = NULL; g_write_cb = NULL;
    g_read_cb = NULL; g_reset_cb = g_estop_cb = g_hard_cb = NULL;
    s_motor_stall_latched = s_function_cleanup_requested = false;
    s_safety_ready = NULL;
    mock_task_create_ok = 1;
    mock_task_create_fail_after = -1;
    mock_task_create_calls = 0;
    mock_task_create_hook = startup_hook;
    mock_queue_create_fail = mock_queue_send_fail = 0;
    mock_queue_create_fail_after = mock_queue_send_fail_after = -1;
    mock_queue_create_calls = mock_queue_send_calls = 0;
    mock_task_wdt_add_err = mock_task_wdt_reset_calls = 0;
    mock_nvs_reset();
}
void tearDown(void)
{
    TEST_ASSERT_EQUAL_INT(0, g_lease_depth);
    if (s_safety_ready != NULL) { vQueueDelete(s_safety_ready); }
    mock_task_create_hook = NULL;
}

static void assert_boot_denied(void)
{
    TEST_ASSERT_FALSE(g_enabled);
    TEST_ASSERT_FALSE(g_admitted);
    TEST_ASSERT_EQUAL_INT(0, g_confirm);
    TEST_ASSERT_EQUAL_INT(0, g_listener);
    TEST_ASSERT_BITS_HIGH(MOTOR_INHIBIT_SAFETY, g_mask);
    if (g_speed_cb != NULL) { g_speed_cb(60, true); }
    if (g_function_cb != NULL) { g_function_cb(1, true); }
    if (g_reset_cb != NULL) { g_reset_cb(); }
    if (g_estop_cb != NULL) { g_estop_cb(); }
    TEST_ASSERT_EQUAL_INT(0, g_motor_calls);
    TEST_ASSERT_EQUAL_INT(0, g_functions);
}
static void test_boot_required_consumers_fail_closed(void)
{
    g_web_err = ESP_FAIL;
    app_main(); assert_boot_denied();
    TEST_ASSERT_EQUAL_INT(0, mock_task_create_calls);
    g_web_err = ESP_OK; g_track_err = ESP_FAIL;
    app_main(); assert_boot_denied();
    TEST_ASSERT_EQUAL_INT(0, mock_task_create_calls);
}
static void test_boot_persistence_allocation_fails_closed(void)
{
    mock_task_create_fail_after = 0;
    app_main(); assert_boot_denied();
}
static void test_boot_safety_allocation_fails_closed(void)
{
    mock_task_create_fail_after = 1;
    app_main(); assert_boot_denied();
    TEST_ASSERT_NOT_NULL(s_safety_ready);
}
static void test_boot_ready_queue_allocation_fails_closed(void)
{
    mock_queue_create_fail = 1;
    app_main(); assert_boot_denied();
}
static void test_boot_missing_ack_fails_closed_queue_survives(void)
{
    g_send_ack = false;
    app_main(); assert_boot_denied();
    TEST_ASSERT_NOT_NULL(s_safety_ready);
    esp_err_t late = ESP_OK;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(s_safety_ready, &late, 0));
    TEST_ASSERT_EQUAL_INT(0, g_confirm);
}
static void test_boot_error_ack_fails_closed(void)
{
    g_safety_ack = ESP_ERR_TIMEOUT;
    app_main(); assert_boot_denied();
}
static void test_boot_web_admission_fails_closed(void)
{
    g_ready_err = ESP_ERR_TIMEOUT;
    app_main(); assert_boot_denied();
    TEST_ASSERT_NOT_NULL(g_speed_cb);
}
static void test_boot_success_registers_callbacks_before_admission(void)
{
    app_main();
    TEST_ASSERT_TRUE(g_enabled);
    TEST_ASSERT_TRUE(g_admitted);
    TEST_ASSERT_EQUAL_UINT32(0, g_mask);
    TEST_ASSERT_EQUAL_INT(1, g_confirm);
    TEST_ASSERT_EQUAL_INT(1, g_listener);
    TEST_ASSERT_EQUAL_PTR(on_dcc_speed, g_speed_cb);
    TEST_ASSERT_EQUAL_PTR(on_dcc_function, g_function_cb);
    TEST_ASSERT_EQUAL_PTR(on_dcc_cv_write, g_write_cb);
    TEST_ASSERT_EQUAL_PTR(on_dcc_cv_read, g_read_cb);
    TEST_ASSERT_EQUAL_PTR(on_dcc_reset, g_reset_cb);
    TEST_ASSERT_EQUAL_PTR(on_dcc_emergency_stop, g_estop_cb);
    TEST_ASSERT_EQUAL_PTR(on_dcc_hard_reset, g_hard_cb);
}
static void test_safety_start_wdt_registration_failure(void)
{
    mock_task_create_hook = NULL;
    s_safety_ready = xQueueCreate(1, sizeof(esp_err_t));
    mock_task_wdt_add_err = ESP_FAIL;
    safety_task(NULL);
    esp_err_t ready = ESP_OK;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(s_safety_ready, &ready, 0));
    TEST_ASSERT_EQUAL(ESP_FAIL, ready);
    TEST_ASSERT_BITS_HIGH(MOTOR_INHIBIT_SAFETY, g_mask);
}
static void test_safety_start_zero_and_stale_heartbeat_rejected(void)
{
    s_safety_ready = xQueueCreate(1, sizeof(esp_err_t));
    g_tick = 0;
    safety_task(NULL);
    esp_err_t ready = ESP_OK;
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(s_safety_ready, &ready, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, ready);
    TEST_ASSERT_TRUE(mock_task_wdt_reset_calls > 0);
    g_tick = mock_timer_now_us - 300000;
    safety_task(NULL);
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(s_safety_ready, &ready, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, ready);
}
static void test_safety_step_first_tick_latches_without_storage(void)
{
    bool timeout = false;
    g_tick = 0; g_enabled = true;
    safety_step(&timeout);
    TEST_ASSERT_TRUE(s_motor_stall_latched);
    TEST_ASSERT_TRUE(s_function_cleanup_requested);
    TEST_ASSERT_FALSE(g_enabled);
    TEST_ASSERT_BITS_HIGH(MOTOR_INHIBIT_SAFETY, g_mask);
    TEST_ASSERT_EQUAL_INT(0, g_flush);
    TEST_ASSERT_EQUAL_INT(0, g_deferred);
    TEST_ASSERT_EQUAL_INT(0, g_functions);
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_set_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_commit_calls);
    g_tick = mock_timer_now_us;
    safety_step(&timeout);
    TEST_ASSERT_BITS_HIGH(MOTOR_INHIBIT_SAFETY, g_mask);
    TEST_ASSERT_FALSE(g_enabled);
}
static void test_safety_step_stale_heartbeat_latches(void)
{
    bool timeout = false;
    g_tick = mock_timer_now_us - 200001;
    safety_step(&timeout);
    TEST_ASSERT_TRUE(s_motor_stall_latched);
    TEST_ASSERT_BITS_HIGH(MOTOR_INHIBIT_SAFETY, g_mask);
}

static void test_calibration_guard_runs_independently_of_motor_heartbeat(void)
{
    bool timeout = false;
    for (unsigned i = 0; i < 100; ++i) {
        mock_timer_now_us += 10000;
        g_tick = mock_timer_now_us;
        safety_step(&timeout);
    }
    TEST_ASSERT_EQUAL_UINT32(100, g_cal_watchdog_calls);
    TEST_ASSERT_FALSE(s_motor_stall_latched);
    TEST_ASSERT_FALSE(timeout);
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_set_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_commit_calls);
    g_tick = 0;
    safety_step(&timeout);
    TEST_ASSERT_EQUAL_UINT32(101, g_cal_watchdog_calls);
    TEST_ASSERT_TRUE(s_motor_stall_latched);
}

static void test_actual_safety_task_checks_calibration_every_ten_ms(void)
{
    s_safety_ready = xQueueCreate(1, sizeof(esp_err_t));
    TEST_ASSERT_NOT_NULL(s_safety_ready);
    g_bound_safety = true;
    if (setjmp(g_safety_exit) == 0) {
        safety_task(NULL);
        TEST_FAIL_MESSAGE("Safety loop unexpectedly returned");
    }
    g_bound_safety = false;
    TEST_ASSERT_EQUAL_UINT32(3, g_cal_watchdog_calls);
    TEST_ASSERT_EQUAL_INT(3, mock_task_wdt_reset_calls);
    TEST_ASSERT_EQUAL_INT64(1030000, esp_timer_get_time());
    TEST_ASSERT_FALSE(s_motor_stall_latched);
    TEST_ASSERT_EQUAL_INT(0, mock_nvs_set_calls);
}
static void test_cv11_dcc_timeout_recovery_preserves_other_owners(void)
{
    bool timeout = false;
    g_cv[11] = 1;
    g_packet = mock_timer_now_us - 30000;
    g_mask |= MOTOR_INHIBIT_SAFETY;
    safety_step(&timeout);
    TEST_ASSERT_TRUE(timeout);
    TEST_ASSERT_BITS_HIGH(MOTOR_INHIBIT_DCC_TIMEOUT, g_mask);
    g_packet = mock_timer_now_us;
    safety_step(&timeout);
    TEST_ASSERT_FALSE(timeout);
    TEST_ASSERT_BITS_LOW(MOTOR_INHIBIT_DCC_TIMEOUT, g_mask);
    TEST_ASSERT_BITS_HIGH(MOTOR_INHIBIT_CONTROL | MOTOR_INHIBIT_SAFETY, g_mask);
}
static void test_cv11_ignored_in_dc_and_web_modes(void)
{
    bool timeout = false;
    g_cv[11] = 1; g_packet = 0;
    g_dc = true;
    safety_step(&timeout);
    TEST_ASSERT_FALSE(timeout);
    g_dc = false; g_rails = false;
    safety_step(&timeout);
    TEST_ASSERT_FALSE(timeout);
    TEST_ASSERT_BITS_LOW(MOTOR_INHIBIT_DCC_TIMEOUT, g_mask);
}
static void test_callbacks_source_leases_and_accepted_motion_only(void)
{
    on_dcc_speed(30, true);
    TEST_ASSERT_EQUAL_INT(1, g_notes);
    TEST_ASSERT_EQUAL_INT(1, g_motion);
    g_motor_err = ESP_ERR_INVALID_STATE;
    on_dcc_speed(40, false);
    TEST_ASSERT_EQUAL_INT(1, g_notes);
    on_dcc_function(2, true);
    TEST_ASSERT_EQUAL_INT(1, g_functions);
    TEST_ASSERT_EQUAL_INT(1, g_notes);
    on_dcc_emergency_stop();
    TEST_ASSERT_EQUAL_INT(2, g_notes);
    on_dcc_reset();
    TEST_ASSERT_EQUAL_INT(3, g_notes);
    TEST_ASSERT_EQUAL_INT(SETTINGS_FUNC_MAP_COUNT + 1, g_functions);
    TEST_ASSERT_EQUAL_INT(1, g_sound_stops);
    TEST_ASSERT_EQUAL_INT(5, g_lease_ends);
    g_rails = false;
    on_dcc_speed(50, true); on_dcc_function(1, true);
    on_dcc_reset(); on_dcc_emergency_stop();
    TEST_ASSERT_EQUAL_INT(4, g_motor_calls);
    TEST_ASSERT_EQUAL_INT(3, g_notes);
    g_rails = true; g_dc = true;
    on_dcc_speed(50, true); on_dcc_function(1, true);
    TEST_ASSERT_EQUAL_INT(4, g_motor_calls);
}
static void test_cv_write_error_prevents_ack_side_effects(void)
{
    g_cv_err = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, on_dcc_cv_write(8, 8, true));
    TEST_ASSERT_EQUAL_INT(0, g_deferred);
    TEST_ASSERT_EQUAL_INT(0, g_reload);
    TEST_ASSERT_EQUAL_INT(0, g_volume);
}
static void test_cv_write_reset_reload_and_master_alias(void)
{
    const uint16_t reload[] = {1, 8, 17, 18, 19, 21, 22, 29};
    for (size_t i = 0; i < sizeof(reload) / sizeof(reload[0]); ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, on_dcc_cv_write(reload[i], 8, false));
        TEST_ASSERT_EQUAL_INT(i + 1, g_reload);
    }
    TEST_ASSERT_EQUAL_INT(50, g_volume);
    TEST_ASSERT_EQUAL(ESP_OK, on_dcc_cv_write(63, 255, false));
    TEST_ASSERT_EQUAL_INT(100, g_volume);
    TEST_ASSERT_EQUAL_INT(8, g_reload);
    TEST_ASSERT_EQUAL_INT(9, g_deferred);
    TEST_ASSERT_EQUAL_INT(0, g_flush);
}
static void test_hard_reset_delegates_subset_and_error_reload(void)
{
    memset(g_cv, 55, sizeof(g_cv));
    on_dcc_hard_reset();
    TEST_ASSERT_EQUAL_INT(1, g_hard_reset_calls);
    TEST_ASSERT_EQUAL_INT(1, g_reload);
    for (uint16_t cv = 1; cv <= SETTINGS_CV_COUNT; ++cv) {
        uint8_t expected = cv == 29 ? 2 :
                           (cv == 19 || cv == 31 || cv == 32) ? 0 : 55;
        TEST_ASSERT_EQUAL_UINT8(expected, g_cv[cv]);
    }
    g_hard_reset_err = ESP_FAIL;
    on_dcc_hard_reset();
    TEST_ASSERT_EQUAL_INT(1, g_reload);
    TEST_ASSERT_EQUAL_INT(0, g_deferred);
}

static void test_hard_reset_denied_source_cannot_change_cvs(void)
{
    g_rails = false;
    on_dcc_hard_reset();
    TEST_ASSERT_EQUAL_INT(0, g_hard_reset_calls);
    TEST_ASSERT_EQUAL_INT(0, g_reload);
}

static void run_one_persistence_iteration(void)
{
    g_worker_delays = 0;
    g_bound_worker = true;
    if (setjmp(g_worker_exit) == 0) { persistence_task(NULL); }
    g_bound_worker = false;
}

static void test_persistence_worker_owns_flush_and_deferred_cleanup(void)
{
    s_motor_stall_latched = true;
    s_function_cleanup_requested = true;
    run_one_persistence_iteration();
    TEST_ASSERT_EQUAL_INT(1, g_flush);
    TEST_ASSERT_EQUAL_INT(SETTINGS_FUNC_MAP_COUNT, g_functions);
    TEST_ASSERT_EQUAL_INT(1, g_sound_stops);
    TEST_ASSERT_FALSE(s_function_cleanup_requested);
    TEST_ASSERT_TRUE(s_motor_stall_latched);
}

static void test_persistence_worker_skips_recovered_timeout_cleanup(void)
{
    s_function_cleanup_requested = true;
    g_cv[11] = 1;
    /* Keep packet fresh at the worker's first 100ms wakeup. */
    g_packet = mock_timer_now_us + 100000;
    run_one_persistence_iteration();
    TEST_ASSERT_EQUAL_INT(1, g_flush);
    TEST_ASSERT_EQUAL_INT(0, g_functions);
    TEST_ASSERT_EQUAL_INT(0, g_sound_stops);
    TEST_ASSERT_FALSE(s_function_cleanup_requested);
}

static void test_selected_graph_restores_without_legacy_fallback(void)
{
    restore_sound_graph();
    TEST_ASSERT_EQUAL(0, g_graph_installs);
    g_graph_selected = true;
    restore_sound_graph();
    TEST_ASSERT_EQUAL(1, g_graph_installs);
    TEST_ASSERT_EQUAL(0, g_graph_faults);
    g_graph_read_err = ESP_ERR_NOT_FOUND;
    restore_sound_graph();
    TEST_ASSERT_EQUAL(1, g_graph_installs);
    TEST_ASSERT_EQUAL(1, g_graph_faults);
    g_graph_read_err = ESP_OK;
    g_graph_assets_err = ESP_FAIL;
    restore_sound_graph();
    TEST_ASSERT_EQUAL(2, g_graph_faults);
}

static void test_corrupt_selection_and_missing_storage_fail_safe(void)
{
    g_selection_err = ESP_FAIL;
    restore_sound_graph();
    TEST_ASSERT_EQUAL(1, g_graph_faults);
    g_storage_mounted = false;
    restore_sound_graph();
    TEST_ASSERT_EQUAL(1, g_graph_faults);
    TEST_ASSERT_EQUAL(0, g_graph_installs);
}

static void test_failed_sound_initialization_never_restores_legacy_audio(void)
{
    g_sound_init_err = ESP_FAIL;
    g_graph_selected = true;
    app_main();
    TEST_ASSERT_EQUAL(1, g_graph_faults);
    TEST_ASSERT_EQUAL(0, g_graph_installs);
}

static void test_emergency_stop_requests_audio_stop_immediately(void)
{
    on_dcc_emergency_stop();
    TEST_ASSERT_EQUAL(1, g_sound_requests);
    bool timed_out = false;
    g_cv[11] = 1;
    g_packet = mock_timer_now_us - 30000;
    safety_step(&timed_out);
    TEST_ASSERT_TRUE(timed_out);
    TEST_ASSERT_TRUE(g_sound_requests >= 2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_selected_graph_restores_without_legacy_fallback);
    RUN_TEST(test_corrupt_selection_and_missing_storage_fail_safe);
    RUN_TEST(test_failed_sound_initialization_never_restores_legacy_audio);
    RUN_TEST(test_emergency_stop_requests_audio_stop_immediately);
    RUN_TEST(test_boot_required_consumers_fail_closed);
    RUN_TEST(test_boot_persistence_allocation_fails_closed);
    RUN_TEST(test_boot_safety_allocation_fails_closed);
    RUN_TEST(test_boot_ready_queue_allocation_fails_closed);
    RUN_TEST(test_boot_missing_ack_fails_closed_queue_survives);
    RUN_TEST(test_boot_error_ack_fails_closed);
    RUN_TEST(test_boot_web_admission_fails_closed);
    RUN_TEST(test_boot_success_registers_callbacks_before_admission);
    RUN_TEST(test_safety_start_wdt_registration_failure);
    RUN_TEST(test_safety_start_zero_and_stale_heartbeat_rejected);
    RUN_TEST(test_safety_step_first_tick_latches_without_storage);
    RUN_TEST(test_safety_step_stale_heartbeat_latches);
    RUN_TEST(test_calibration_guard_runs_independently_of_motor_heartbeat);
    RUN_TEST(test_actual_safety_task_checks_calibration_every_ten_ms);
    RUN_TEST(test_cv11_dcc_timeout_recovery_preserves_other_owners);
    RUN_TEST(test_cv11_ignored_in_dc_and_web_modes);
    RUN_TEST(test_callbacks_source_leases_and_accepted_motion_only);
    RUN_TEST(test_cv_write_error_prevents_ack_side_effects);
    RUN_TEST(test_cv_write_reset_reload_and_master_alias);
    RUN_TEST(test_hard_reset_delegates_subset_and_error_reload);
    RUN_TEST(test_hard_reset_denied_source_cannot_change_cvs);
    RUN_TEST(test_persistence_worker_owns_flush_and_deferred_cleanup);
    RUN_TEST(test_persistence_worker_skips_recovered_timeout_cleanup);
    return UNITY_END();
}
