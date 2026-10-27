#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "hal/ledc_ll.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pinmap.h"
#include "settings.h"
#include "bemf_cal_base.h"

static void (*g_adc_hook)(int channel);
static void (*g_delay_hook)(TickType_t ticks);
static void (*g_create_hook)(void (*task)(void *), void *arg);

static esp_err_t test_adc_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                               int *raw);
static void test_coast_delay(uint32_t us);
static void test_task_delay(TickType_t ticks);
static void test_delay_until(TickType_t *last, TickType_t period);
static TickType_t test_tick_count(void);
static BaseType_t test_task_create(void (*task)(void *), const char *name, uint32_t stack,
                                   void *arg, UBaseType_t priority, TaskHandle_t *handle);

#define static
#define adc_oneshot_read test_adc_read
#define esp_rom_delay_us test_coast_delay
#define vTaskDelay test_task_delay
#define vTaskDelayUntil test_delay_until
#define xTaskGetTickCount test_tick_count
#define xTaskCreate test_task_create
#include "../../components/motor/src/motor.c"
#undef static
#undef adc_oneshot_read
#undef esp_rom_delay_us
#undef vTaskDelay
#undef vTaskDelayUntil
#undef xTaskGetTickCount
#undef xTaskCreate

#include "../../test_libs/teststubs/stubs.c"

static uint32_t g_coast_calls;

static esp_err_t test_adc_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                               int *raw)
{
    if (g_adc_hook != NULL) g_adc_hook((int)channel);
    return adc_oneshot_read(unit, channel, raw);
}

static void test_coast_delay(uint32_t us)
{
    ++g_coast_calls;
    esp_rom_delay_us(us);
}

static void test_task_delay(TickType_t ticks)
{
    vTaskDelay(ticks);
    if (g_delay_hook != NULL) g_delay_hook(ticks);
}

static TickType_t test_tick_count(void)
{
    return (TickType_t)(mock_timer_now_us / 1000);
}

static void test_delay_until(TickType_t *last, TickType_t period)
{
    *last += period;
    mock_timer_now_us += (int64_t)period * 1000;
    if (g_delay_hook != NULL) g_delay_hook(period);
}

static BaseType_t test_task_create(void (*task)(void *), const char *name, uint32_t stack,
                                   void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    BaseType_t result = xTaskCreate(task, name, stack, arg, priority, handle);
    if (result == pdPASS && g_create_hook != NULL) g_create_hook(task, arg);
    return result;
}

/* ---- settings substitute ------------------------------------------- */
static uint8_t g_cv[SETTINGS_CV_COUNT + 1];
static settings_bemf_cal_t g_cal;
static bool g_cal_valid;
static bool g_bemf_use = true;
static uint16_t g_bemf_window = SETTINGS_BEMF_WINDOW_DEFAULT_US;
static esp_err_t g_save_error;

esp_err_t settings_cv_write(uint16_t index, uint8_t value)
{
    if (index == 0U || index > SETTINGS_CV_COUNT) return ESP_ERR_INVALID_ARG;
    g_cv[index] = value;
    return ESP_OK;
}

esp_err_t settings_cv_commit(void)
{
    return ESP_OK;
}

esp_err_t settings_cv_snapshot(uint8_t out[SETTINGS_CV_COUNT + 1])
{
    memcpy(out, g_cv, sizeof(g_cv));
    return ESP_OK;
}

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (out == NULL || idx < 1 || idx > SETTINGS_CV_COUNT) return ESP_ERR_INVALID_ARG;
    *out = g_cv[idx];
    return ESP_OK;
}

bool settings_bemf_cal_validate(const settings_bemf_cal_t *cal)
{
    if (cal == NULL || cal->count < 2U || cal->count > SETTINGS_BEMF_CAL_MAX_POINTS) {
        return false;
    }
    if (cal->speed[0] == 0U && cal->frac[0] != 0U) return false;
    for (uint8_t i = 0; i < cal->count; ++i) {
        if (cal->speed[i] > 126U || cal->frac[i] > 1024U ||
            (i > 0U && (cal->speed[i] <= cal->speed[i - 1U] ||
                        cal->frac[i] < cal->frac[i - 1U]))) {
            return false;
        }
    }
    return cal->frac[cal->count - 1U] >= SETTINGS_BEMF_CAL_MIN_END_FRAC;
}

esp_err_t settings_bemf_cal_load(settings_bemf_cal_t *cal)
{
    if (cal == NULL) return ESP_ERR_INVALID_ARG;
    if (!g_cal_valid) return ESP_ERR_NOT_FOUND;
    *cal = g_cal;
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save(const settings_bemf_cal_t *cal)
{
    if (cal == NULL) return ESP_ERR_INVALID_ARG;
    if (g_save_error != ESP_OK) return g_save_error;
    g_cal = *cal;
    g_cal_valid = true;
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save_guarded(const settings_bemf_cal_t *cal,
                                         settings_bemf_cal_authorize_t authorize,
                                         void *context)
{
    if (!settings_bemf_cal_validate(cal)) return ESP_ERR_INVALID_ARG;
    if (authorize != NULL && !authorize(context)) return ESP_ERR_INVALID_STATE;
    return settings_bemf_cal_save(cal);
}

esp_err_t settings_bemf_cal_clear(void)
{
    g_cal_valid = false;
    memset(&g_cal, 0, sizeof(g_cal));
    return ESP_OK;
}

esp_err_t settings_bemf_use_load(bool *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = g_bemf_use;
    return ESP_OK;
}

esp_err_t settings_bemf_use_save(bool enabled)
{
    g_bemf_use = enabled;
    return ESP_OK;
}

esp_err_t settings_bemf_window_load(uint16_t *out_us)
{
    if (out_us == NULL) return ESP_ERR_INVALID_ARG;
    *out_us = g_bemf_window;
    return ESP_OK;
}

esp_err_t settings_bemf_window_save(uint16_t us)
{
    if (us < SETTINGS_BEMF_WINDOW_MIN_US || us > SETTINGS_BEMF_WINDOW_MAX_US) {
        return ESP_ERR_INVALID_ARG;
    }
    g_bemf_window = us;
    return ESP_OK;
}

/* ---- helpers -------------------------------------------------------- */

static int mv_to_raw(int mv)
{
    if (mv < 0) mv = 0;
    long raw = (long)mv * 4095L / 3100L;
    if (raw > 4094L) raw = 4094L;
    return (int)raw;
}

static void refresh_rail(TickType_t ticks)
{
    (void)ticks;
    motor_set_rail_voltage_mv(1000);
}

static void zero_bemf_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1) || channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static void duty_bemf_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        uint32_t mv = s_last_duty * 800U / 1023U;
        mock_adc_raw[channel] = mv_to_raw((int)mv);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static void constant_bemf_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        mock_adc_raw[channel] = mv_to_raw(60);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static void flat_when_spinning_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        mock_adc_raw[channel] = s_last_duty > 0U ? mv_to_raw(400) : 0;
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static unsigned g_check_reads;

static void check_bemf_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        ++g_check_reads;
        if (g_check_reads <= 3U) {
            mock_adc_raw[channel] = 0;
        } else if (g_check_reads == 4U) {
            mock_adc_raw[channel] = mv_to_raw(300);
        } else {
            mock_adc_raw[channel] = 0;
        }
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = g_check_reads == 5U ? mv_to_raw(300) : 0;
    }
}

static void cv_defaults(void)
{
    memset(g_cv, 0, sizeof(g_cv));
    g_cv[1] = 3;
    g_cv[2] = 0;
    g_cv[5] = 255;
    g_cv[6] = 128;
    g_cv[7] = 12;
    g_cv[29] = 0x02;
    g_cv[54] = 128;
    g_cv[55] = 60;
    g_cv[56] = 32;
    g_cv[66] = 128;
    g_cv[95] = 128;
    for (int i = 0; i < 28; ++i) {
        g_cv[67 + i] = (uint8_t)((i * 255) / 27);
    }
}

static void set_stored_curve(void)
{
    memset(&g_cal, 0, sizeof(g_cal));
    g_cal.count = 2;
    g_cal.speed[0] = 12;
    g_cal.speed[1] = 126;
    g_cal.frac[0] = 100;
    g_cal.frac[1] = 800;
    g_cal_valid = true;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_reload());
    TEST_ASSERT_TRUE(s_cal_valid);
}

static void run_task_now(void (*task)(void *), void *arg)
{
    task(arg);
}

static void capture_task(void (*task)(void *), void *arg)
{
    (void)task;
    (void)arg;
}

static void tick_speed(uint8_t speed, bool forward, unsigned ticks)
{
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(speed, forward));
    for (unsigned i = 0; i < ticks; ++i) motor_tick();
}

static void cleanup_abandoned_calibration(void)
{
    s_cal_active = false;
    s_cal_cancelled = false;
    s_cal_task = NULL;
    s_cal_drive_active = false;
    s_cal_drive_deadline_us = 0;
    s_stop_requested = false;
}

void setUp(void)
{
    g_adc_hook = NULL;
    g_delay_hook = refresh_rail;
    g_create_hook = NULL;
    g_coast_calls = 0;
    g_check_reads = 0;
    g_save_error = ESP_OK;
    g_cal_valid = false;
    g_bemf_use = true;
    g_bemf_window = SETTINGS_BEMF_WINDOW_DEFAULT_US;
    memset(&g_cal, 0, sizeof(g_cal));
    mock_timer_now_us = 0;
    mock_adc_ok = 1;
    mock_task_create_ok = 1;
    mock_task_create_fail_after = -1;
    mock_task_create_calls = 0;
    mock_mutex_create_fail = 0;
    mock_sem_take_fail = 0;
    mock_ledc_update_count = 0;
    memset(mock_ledc_duty, 0, sizeof(mock_ledc_duty));
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    mock_rom_delay_us_total = 0;
    cv_defaults();

    TEST_ASSERT_EQUAL(ESP_OK, motor_init());
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(false));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(1000));
}

void tearDown(void)
{
    if (s_control_mutex != NULL) {
        vSemaphoreDelete(s_control_mutex);
        s_control_mutex = NULL;
    }
    if (s_adc_mutex != NULL) {
        vSemaphoreDelete(s_adc_mutex);
        s_adc_mutex = NULL;
    }
}

/* ---- speed curve ---------------------------------------------------- */

static void test_speed_duty_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, speed_duty(0));
}

static void test_speed_duty_low_end_is_monotonic(void)
{
    uint32_t previous = speed_duty(0);
    for (uint8_t speed = 1; speed <= 126U; ++speed) {
        uint32_t duty = speed_duty(speed);
        TEST_ASSERT_TRUE(duty >= previous);
        previous = duty;
    }
    TEST_ASSERT_EQUAL_UINT32(MOTOR_CREEP_MIN_DUTY, speed_duty(1));
    TEST_ASSERT_EQUAL_UINT32(MOTOR_CREEP_MIN_DUTY, speed_duty(3));
    TEST_ASSERT_EQUAL_UINT32(MOTOR_CREEP_MIN_DUTY, speed_duty(5));
    TEST_ASSERT_TRUE(speed_duty(6) >= MOTOR_CREEP_MIN_DUTY);
}

static void test_speed_duty_linear_curve(void)
{
    TEST_ASSERT_EQUAL_UINT32(74, speed_duty(10));
    TEST_ASSERT_EQUAL_UINT32(512, speed_duty(63));
    TEST_ASSERT_EQUAL_UINT32(1020, speed_duty(126));
}

static void test_speed_duty_table_branch(void)
{
    g_cv[29] = 0x12U;
    TEST_ASSERT_EQUAL_UINT32(curve_duty(1), speed_duty(1));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)g_cv[94] * 4U, speed_duty(126));
}

static void test_speed_duty_table_non_monotonic_no_wrap(void)
{
    g_cv[29] = 0x12U;
    g_cv[67] = 200;
    g_cv[68] = 10;
    uint32_t duty = speed_duty(3);
    TEST_ASSERT_TRUE(duty <= LEDC_MAX);
}

static void test_speed_duty_vhigh_zero(void)
{
    g_cv[5] = 0;
    TEST_ASSERT_EQUAL_UINT32(LEDC_MAX, speed_duty(126));
}

/* ---- init, inhibit, safe stop --------------------------------------- */

static void test_boot_safe_drives_both_inputs_low(void)
{
    mock_gpio_set_level_count = 0;
    motor_boot_safe();
    TEST_ASSERT_TRUE(mock_gpio_set_level_count >= 2);
}

static void test_inhibit_owners_are_independent(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true));
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(false));
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(20, true));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, false));
    TEST_ASSERT_FALSE(motor_is_inhibited());
}

static void test_emergency_stop_coasts_and_rejects_stale_output(void)
{
    tick_speed(60, true, 1);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
    motor_emergency_stop();
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
    uint8_t applied = 255;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, applied);
    uint8_t target = 255;
    motor_get_status(&target, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, target);
}

static void test_inhibit_coasts_immediately(void)
{
    tick_speed(60, true, 1);
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(true));
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_new_command_after_estop_restarts_motion(void)
{
    tick_speed(50, true, 1);
    motor_emergency_stop();
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(30, true));
    motor_tick();
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(30, applied);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
}

static void test_new_command_after_inhibit_release_restarts_motion(void)
{
    tick_speed(50, true, 1);
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(true));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(false));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(30, true));
    motor_tick();
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(30, applied);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
}

/* ---- ramp, direction, kickstart ------------------------------------- */

static void test_ramp_no_inertia(void)
{
    tick_speed(80, true, 1);
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(80, applied);
}

static void test_ramp_uses_cv3_and_cv4(void)
{
    g_cv[3] = 1;
    g_cv[4] = 1;
    tick_speed(20, true, 5);
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_TRUE(applied > 0U && applied < 20U);
    tick_speed(0, true, 5);
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_TRUE(applied < 20U);
}

static void test_reverse_uses_other_channel(void)
{
    g_cv[4] = 0;
    tick_speed(50, true, 1);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
    tick_speed(50, false, 2);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_1] > 0U);
}

static void test_kickstart_boosts_and_releases(void)
{
    g_cv[65] = 255;
    tick_speed(50, true, 1);
    TEST_ASSERT_EQUAL_UINT32(1020U, mock_ledc_duty[LEDC_CHANNEL_0]);
    tick_speed(50, true, KICK_TICKS + 1U);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(50), mock_ledc_duty[LEDC_CHANNEL_0]);
}

/* ---- BEMF validity and open-loop fallback --------------------------- */

static void test_zero_bemf_is_invalid_but_motor_keeps_running(void)
{
    g_adc_hook = zero_bemf_adc;
    tick_speed(50, true, 40);
    TEST_ASSERT_FALSE(s_bemf_valid);
    TEST_ASSERT_FALSE(s_bemf_acquired);
    TEST_ASSERT_TRUE(s_open_loop_fallback);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(50), mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_FALSE(motor_is_inhibited());
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(50, applied);
}

static void test_no_curve_skips_bemf_sampling(void)
{
    g_adc_hook = duty_bemf_adc;
    tick_speed(50, true, 5);
    TEST_ASSERT_EQUAL_UINT32(0, g_coast_calls);
}

static void test_valid_bemf_acquires_and_regulates(void)
{
    set_stored_curve();
    g_adc_hook = duty_bemf_adc;
    tick_speed(63, true, 6);
    TEST_ASSERT_TRUE(s_bemf_valid);
    TEST_ASSERT_TRUE(s_bemf_acquired);
    TEST_ASSERT_TRUE(s_last_pid_ok);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] >= speed_duty(63));
    TEST_ASSERT_FALSE(motor_is_inhibited());
}

static void test_feedback_loss_falls_back_without_stopping(void)
{
    set_stored_curve();
    g_adc_hook = duty_bemf_adc;
    tick_speed(63, true, 6);
    TEST_ASSERT_TRUE(s_bemf_acquired);
    g_adc_hook = zero_bemf_adc;
    for (unsigned i = 0; i < BEMF_LOSS_TICKS + 5U; ++i) motor_tick();
    TEST_ASSERT_FALSE(s_bemf_acquired);
    TEST_ASSERT_TRUE(s_open_loop_fallback);
    TEST_ASSERT_FALSE(motor_is_inhibited());
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(63, applied);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(63), mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_bemf_above_ceiling_is_rejected(void)
{
    set_stored_curve();
    g_adc_hook = zero_bemf_adc;
    tick_speed(63, true, 5);
    g_adc_hook = NULL;
    mock_adc_raw[PIN_BEMF1 - 1] = 4090;
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    motor_tick();
    TEST_ASSERT_FALSE(s_bemf_valid);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(63), mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_saturated_rail_disables_correction_but_motor_runs(void)
{
    set_stored_curve();
    g_adc_hook = duty_bemf_adc;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(3001));
    tick_speed(63, true, 6);
    TEST_ASSERT_FALSE(s_bemf_valid);
    TEST_ASSERT_TRUE(s_open_loop_fallback);
    TEST_ASSERT_FALSE(motor_is_inhibited());
    TEST_ASSERT_EQUAL_UINT32(speed_duty(63), mock_ledc_duty[LEDC_CHANNEL_0]);
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(63, applied);
}

static void test_diag_reports_terminal_voltages(void)
{
    set_stored_curve();
    g_adc_hook = NULL;
    mock_adc_raw[PIN_BEMF1 - 1] = mv_to_raw(100);
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    tick_speed(40, true, 2);
    motor_bemf_diag_t d;
    motor_bemf_diag(&d);
    TEST_ASSERT_EQUAL_UINT32(1000, d.rail_mv);
    TEST_ASSERT_EQUAL_UINT32(7296, d.rail_motor_mv);
    TEST_ASSERT_EQUAL_UINT32(d.bemf1_mv * 11820U / 1620U, d.bemf1_motor_mv);
    TEST_ASSERT_EQUAL_UINT32(0, d.bemf2_motor_mv);
}

/* ---- calibration ---------------------------------------------------- */

static void start_calibration_and_run(motor_bemf_cal_mode_t mode, uint8_t points)
{
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    cfg.mode = mode;
    cfg.points = points;
    cfg.dwell_ms = 60;
    cfg.settle_ms = 40;
    cfg.interval_ms = 10;
    cfg.samples = 3;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
}

static void test_calibration_success_stores_curve(void)
{
    g_adc_hook = duty_bemf_adc;
    g_create_hook = run_task_now;
    start_calibration_and_run(MOTOR_BEMF_MODE_CUSTOM, 4);
    g_create_hook = NULL;

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_FALSE(info.active);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SUCCEEDED, info.result);
    TEST_ASSERT_TRUE(info.valid);
    TEST_ASSERT_TRUE(info.stored);
    TEST_ASSERT_TRUE(info.count >= CAL_MIN_POINTS);
    for (uint8_t i = 1; i < info.count; ++i) {
        TEST_ASSERT_TRUE(info.speed[i] > info.speed[i - 1U]);
        TEST_ASSERT_TRUE(info.frac[i] >= info.frac[i - 1U]);
    }
    TEST_ASSERT_TRUE(g_cal_valid);
}

static void test_zero_bemf_calibration_fails_but_keeps_old_curve(void)
{
    set_stored_curve();
    settings_bemf_cal_t before = g_cal;
    g_adc_hook = zero_bemf_adc;
    g_create_hook = run_task_now;
    start_calibration_and_run(MOTOR_BEMF_MODE_CUSTOM, 4);
    g_create_hook = NULL;

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_FAILED, info.result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_NO_RISE, info.reason);
    TEST_ASSERT_EQUAL_UINT8(before.count, g_cal.count);
    TEST_ASSERT_TRUE(s_cal_valid);

    g_adc_hook = zero_bemf_adc;
    tick_speed(50, true, 5);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(50), mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_FALSE(motor_is_inhibited());
}

static void test_calibration_rejects_coasting_motor(void)
{
    g_adc_hook = constant_bemf_adc;
    g_create_hook = run_task_now;
    start_calibration_and_run(MOTOR_BEMF_MODE_CUSTOM, 4);
    g_create_hook = NULL;

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_FAILED, info.result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_CURVE, info.reason);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_calibration_admission_requires_rest(void)
{
    tick_speed(40, true, 1);
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start_ex(&cfg));
    motor_emergency_stop();
    motor_tick();
    g_create_hook = capture_task;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    TEST_ASSERT_TRUE(motor_bemf_cal_active());
    cleanup_abandoned_calibration();
}

static void test_stop_cancels_calibration_and_coasts(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    TEST_ASSERT_TRUE(motor_bemf_cal_active());
    motor_set_speed(0, true);
    TEST_ASSERT_TRUE(s_cal_cancelled);
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, info.result);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    cleanup_abandoned_calibration();
}

static void test_custom_calibration_plan(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    cfg.mode = MOTOR_BEMF_MODE_CUSTOM;
    cfg.speed_count = 2;
    cfg.speed[0] = 20;
    cfg.speed[1] = 80;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    TEST_ASSERT_EQUAL_UINT8(2, s_cal_plan_count);
    TEST_ASSERT_EQUAL_UINT8(20, s_cal_plan[0]);
    TEST_ASSERT_EQUAL_UINT8(80, s_cal_plan[1]);
    cleanup_abandoned_calibration();
}

static void test_calibration_mode_selects_point_count(void)
{
    struct {
        motor_bemf_cal_mode_t mode;
        uint8_t points;
    } cases[] = {
        { MOTOR_BEMF_MODE_AUTO, 12 },
        { MOTOR_BEMF_MODE_FAST, 10 },
        { MOTOR_BEMF_MODE_BALANCED, 12 },
        { MOTOR_BEMF_MODE_THOROUGH, 14 },
    };
    g_create_hook = capture_task;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        motor_bemf_cal_config_t cfg;
        motor_bemf_cal_default_config(&cfg);
        cfg.mode = cases[i].mode;
        cfg.points = 0; /* web UI sends only mode=... */
        TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
        TEST_ASSERT_EQUAL_UINT8(cases[i].points, s_cal_plan_count);
        cleanup_abandoned_calibration();
    }
    g_create_hook = NULL;

    /* An explicit points= value still overrides the preset. */
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    cfg.mode = MOTOR_BEMF_MODE_FAST;
    cfg.points = 14;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    TEST_ASSERT_EQUAL_UINT8(14, s_cal_plan_count);
    cleanup_abandoned_calibration();
}

static void test_calibration_clear_removes_curve(void)
{
    set_stored_curve();
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_clear());
    TEST_ASSERT_FALSE(s_cal_valid);
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_FALSE(info.valid);
    TEST_ASSERT_EQUAL_UINT8(0, info.count);
}

static void test_flat_stored_curve_does_not_enable_closed_loop(void)
{
    memset(&g_cal, 0, sizeof(g_cal));
    g_cal.count = 2;
    g_cal.speed[0] = 12;
    g_cal.speed[1] = 126;
    g_cal.frac[0] = 400;
    g_cal.frac[1] = 410;
    g_cal_valid = true;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_reload());
    TEST_ASSERT_FALSE(s_cal_valid);
}

static void test_flat_sensor_calibration_fails_without_stopping_motor(void)
{
    g_adc_hook = flat_when_spinning_adc;
    g_create_hook = run_task_now;
    start_calibration_and_run(MOTOR_BEMF_MODE_CUSTOM, 4);
    g_create_hook = NULL;

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_FAILED, info.result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_CURVE, info.reason);
    TEST_ASSERT_FALSE(s_cal_valid);

    tick_speed(50, true, 5);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(50), mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_FALSE(motor_is_inhibited());
}

/* ---- BEMF check, diagnostics ---------------------------------------- */

static void test_bemf_check_static_only(void)
{
    g_adc_hook = zero_bemf_adc;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_check(&out, false));
    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_TRUE(out.static_ok);
    TEST_ASSERT_FALSE(out.spin_ok);
}

static void test_bemf_check_detects_stuck_high(void)
{
    g_adc_hook = NULL;
    mock_adc_raw[PIN_BEMF1 - 1] = mv_to_raw(900);
    mock_adc_raw[PIN_BEMF2 - 1] = mv_to_raw(900);
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_STUCK_HIGH, out.reason);
}

static void test_bemf_check_spin_polarity(void)
{
    g_check_reads = 0;
    g_adc_hook = check_bemf_adc;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_check(&out, true));
    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_TRUE(out.static_ok);
    TEST_ASSERT_TRUE(out.spin_ok);
    TEST_ASSERT_TRUE(out.polarity_ok);
}

static void test_bemf_check_inhibited(void)
{
    motor_set_inhibited(true);
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_INHIBITED, out.reason);
}

static void test_bemf_diagnostics_and_base_info(void)
{
    g_adc_hook = zero_bemf_adc;
    tick_speed(40, true, 2);
    motor_bemf_diag_t d;
    motor_bemf_diag(&d);
    TEST_ASSERT_EQUAL_UINT32(1000, d.rail_mv);
    TEST_ASSERT_TRUE(d.open_loop_fallback);

    motor_bemf_base_info_t base;
    motor_bemf_base_info(&base);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_BASE.count, base.count);

    uint16_t b1 = 0, b2 = 0, rail = 0;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_adc_dump_checked(&b1, &b2, &rail));
}

static void test_adc_dump_failure_is_reported(void)
{
    mock_adc_ok = 0;
    uint16_t b1 = 1, b2 = 1, rail = 1;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_adc_dump_checked(&b1, &b2, &rail));
    TEST_ASSERT_EQUAL_UINT16(0, b1);
}

static void test_heartbeat_updates_before_control_mutex(void)
{
    int64_t before = motor_last_tick_us();
    mock_timer_now_us += 50000;
    mock_sem_take_fail = 1;
    motor_tick();
    mock_sem_take_fail = 0;
    TEST_ASSERT_TRUE(motor_last_tick_us() > before);
}

static void test_bemf_toggle_resets_tracking(void)
{
    set_stored_curve();
    g_adc_hook = duty_bemf_adc;
    tick_speed(63, true, 6);
    TEST_ASSERT_TRUE(s_bemf_acquired);
    motor_set_bemf_enabled(false);
    motor_tick();
    TEST_ASSERT_FALSE(s_bemf_acquired);
    TEST_ASSERT_FALSE(motor_get_bemf_enabled());
    motor_set_bemf_enabled(true);
    TEST_ASSERT_TRUE(motor_get_bemf_enabled());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_speed_duty_zero);
    RUN_TEST(test_speed_duty_low_end_is_monotonic);
    RUN_TEST(test_speed_duty_linear_curve);
    RUN_TEST(test_speed_duty_table_branch);
    RUN_TEST(test_speed_duty_table_non_monotonic_no_wrap);
    RUN_TEST(test_speed_duty_vhigh_zero);
    RUN_TEST(test_boot_safe_drives_both_inputs_low);
    RUN_TEST(test_inhibit_owners_are_independent);
    RUN_TEST(test_emergency_stop_coasts_and_rejects_stale_output);
    RUN_TEST(test_inhibit_coasts_immediately);
    RUN_TEST(test_new_command_after_estop_restarts_motion);
    RUN_TEST(test_new_command_after_inhibit_release_restarts_motion);
    RUN_TEST(test_ramp_no_inertia);
    RUN_TEST(test_ramp_uses_cv3_and_cv4);
    RUN_TEST(test_reverse_uses_other_channel);
    RUN_TEST(test_kickstart_boosts_and_releases);
    RUN_TEST(test_zero_bemf_is_invalid_but_motor_keeps_running);
    RUN_TEST(test_no_curve_skips_bemf_sampling);
    RUN_TEST(test_valid_bemf_acquires_and_regulates);
    RUN_TEST(test_feedback_loss_falls_back_without_stopping);
    RUN_TEST(test_bemf_above_ceiling_is_rejected);
    RUN_TEST(test_saturated_rail_disables_correction_but_motor_runs);
    RUN_TEST(test_diag_reports_terminal_voltages);
    RUN_TEST(test_calibration_success_stores_curve);
    RUN_TEST(test_zero_bemf_calibration_fails_but_keeps_old_curve);
    RUN_TEST(test_calibration_rejects_coasting_motor);
    RUN_TEST(test_calibration_admission_requires_rest);
    RUN_TEST(test_stop_cancels_calibration_and_coasts);
    RUN_TEST(test_custom_calibration_plan);
    RUN_TEST(test_calibration_mode_selects_point_count);
    RUN_TEST(test_calibration_clear_removes_curve);
    RUN_TEST(test_flat_stored_curve_does_not_enable_closed_loop);
    RUN_TEST(test_flat_sensor_calibration_fails_without_stopping_motor);
    RUN_TEST(test_bemf_check_static_only);
    RUN_TEST(test_bemf_check_detects_stuck_high);
    RUN_TEST(test_bemf_check_spin_polarity);
    RUN_TEST(test_bemf_check_inhibited);
    RUN_TEST(test_bemf_diagnostics_and_base_info);
    RUN_TEST(test_adc_dump_failure_is_reported);
    RUN_TEST(test_heartbeat_updates_before_control_mutex);
    RUN_TEST(test_bemf_toggle_resets_tracking);
    return UNITY_END();
}
