#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Pre-include every header motor.c pulls in (directly or transitively) so the
 * `static` strip below cannot change their linkage rules. */
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
static void (*g_coast_hook)(void);
static void (*g_delay_hook)(TickType_t delay);
static void (*g_create_hook)(void (*task)(void *), void *arg);
static void (*g_validate_hook)(void);
static void (*g_save_admission_hook)(void);
static void (*g_save_commit_hook)(void);
static void (*g_sem_take_hook)(SemaphoreHandle_t semaphore, TickType_t wait);
static esp_err_t g_save_error;
static uint32_t g_mutex_create_calls;
static uint32_t g_mutex_fail_call;
static TickType_t g_now_ticks;
static esp_err_t test_adc_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel, int *raw);
static void test_coast_delay(uint32_t us);
static void test_task_delay(TickType_t ticks);
static void test_delay_until(TickType_t *last, TickType_t period);
static TickType_t test_tick_count(void);
static SemaphoreHandle_t test_mutex_create(void);
static BaseType_t test_sem_take(SemaphoreHandle_t semaphore, TickType_t wait);
static BaseType_t test_task_create(void (*task)(void *), const char *name, uint32_t stack,
                                  void *arg, UBaseType_t priority, TaskHandle_t *handle);

/* White-box: expose the motor's static helpers (speed_duty, bemf_update,
 * bemf_cal_build_table, motor_tick, load_pid, ...). */
#define static
#define adc_oneshot_read test_adc_read
#define esp_rom_delay_us test_coast_delay
#define vTaskDelay test_task_delay
#define vTaskDelayUntil test_delay_until
#define xTaskGetTickCount test_tick_count
#define xSemaphoreCreateMutex test_mutex_create
#define xSemaphoreTake test_sem_take
#define xTaskCreate test_task_create
#include "../../components/motor/src/motor.c"
#undef static
#undef adc_oneshot_read
#undef esp_rom_delay_us
#undef vTaskDelay
#undef vTaskDelayUntil
#undef xTaskGetTickCount
#undef xSemaphoreCreateMutex
#undef xSemaphoreTake
#undef xTaskCreate

#include "../../test_libs/teststubs/stubs.c"

static esp_err_t test_adc_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel, int *raw)
{
    if (g_adc_hook != NULL) {
        g_adc_hook((int)channel);
    }
    return adc_oneshot_read(unit, channel, raw);
}

static void test_coast_delay(uint32_t us)
{
    esp_rom_delay_us(us);
    if (g_coast_hook != NULL) {
        g_coast_hook();
    }
}

static void test_task_delay(TickType_t ticks)
{
    vTaskDelay(ticks);
    g_now_ticks += ticks;
    if (g_delay_hook != NULL) {
        g_delay_hook(ticks);
    }
}

static TickType_t test_tick_count(void)
{
    return g_now_ticks;
}

static void test_delay_until(TickType_t *last, TickType_t period)
{
    TickType_t deadline = *last + period;
    TEST_ASSERT_TRUE((int32_t)(deadline - g_now_ticks) > 0);
    g_now_ticks = deadline;
    *last = deadline;
    if (g_delay_hook != NULL) {
        g_delay_hook(period);
    }
}

static SemaphoreHandle_t test_mutex_create(void)
{
    ++g_mutex_create_calls;
    return g_mutex_create_calls == g_mutex_fail_call ? NULL : xSemaphoreCreateMutex();
}

static BaseType_t test_sem_take(SemaphoreHandle_t semaphore, TickType_t wait)
{
    if (g_sem_take_hook != NULL) g_sem_take_hook(semaphore, wait);
    return xSemaphoreTake(semaphore, wait);
}

static BaseType_t test_task_create(void (*task)(void *), const char *name, uint32_t stack,
                                  void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    BaseType_t result = xTaskCreate(task, name, stack, arg, priority, handle);
    if (result == pdPASS && g_create_hook != NULL) {
        g_create_hook(task, arg);
    }
    return result;
}

/* ---- settings.c substitute ------------------------------------------- */
/* motor.c talks to the CV store and the stored BEMF calibration. A tiny
 * in-memory model keeps this suite free of the NVS/FreeRTOS machinery. */
static uint8_t g_cv[SETTINGS_CV_COUNT + 1];
static settings_bemf_cal_t g_cal;
static bool g_cal_valid;

esp_err_t settings_cv_snapshot(uint8_t out[SETTINGS_CV_COUNT + 1])
{
    memcpy(out, g_cv, sizeof(g_cv));
    return ESP_OK;
}

bool settings_bemf_cal_validate(const settings_bemf_cal_t *cal)
{
    if (g_validate_hook != NULL) {
        g_validate_hook();
    }
    if (cal == NULL || cal->count < 2U || cal->count > SETTINGS_BEMF_CAL_MAX_POINTS) {
        return false;
    }
    for (uint8_t i = 0; i < cal->count; ++i) {
        if (cal->speed[i] > 126U || cal->frac[i] > 1024U ||
            (i > 0U && (cal->speed[i] <= cal->speed[i - 1U] || cal->frac[i] < cal->frac[i - 1U]))) {
            return false;
        }
    }
    return cal->frac[cal->count - 1U] >= SETTINGS_BEMF_CAL_MIN_END_FRAC;
}

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (out == NULL || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = g_cv[idx];
    return ESP_OK;
}

esp_err_t settings_bemf_cal_load(settings_bemf_cal_t *cal)
{
    if (cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_cal_valid) {
        return ESP_ERR_NOT_FOUND;
    }
    *cal = g_cal;
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save(const settings_bemf_cal_t *cal)
{
    if (cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_save_commit_hook != NULL) g_save_commit_hook();
    if (g_save_error != ESP_OK) return g_save_error;
    g_cal = *cal;
    g_cal_valid = true;
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save_guarded(const settings_bemf_cal_t *cal,
                                       settings_bemf_cal_authorize_t authorize, void *context)
{
    if (!settings_bemf_cal_validate(cal)) return ESP_ERR_INVALID_ARG;
    if (g_save_admission_hook != NULL) g_save_admission_hook();
    if (authorize != NULL && !authorize(context)) return ESP_ERR_INVALID_STATE;
    return settings_bemf_cal_save(cal);
}

esp_err_t settings_bemf_cal_clear(void)
{
    g_cal_valid = false;
    return ESP_OK;
}

/* motor_init() loads the closed-loop flag from settings. */
static bool g_bemf_use = true;

esp_err_t settings_bemf_use_load(bool *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = g_bemf_use;
    return ESP_OK;
}

esp_err_t settings_bemf_use_save(bool enabled)
{
    g_bemf_use = enabled;
    return ESP_OK;
}

/* ---- helpers ---- */

static void cv_defaults(void)
{
    memset(g_cv, 0, sizeof(g_cv));
    g_cv[1] = 3;
    g_cv[2] = 0;   /* Vstart */
    g_cv[5] = 255; /* Vhigh: full */
    g_cv[6] = 128; /* Vmid: half */
    g_cv[7] = 4;
    g_cv[29] = 0x02;
    g_cv[54] = 128;
    g_cv[55] = 60;
    g_cv[56] = 32;
    for (int i = 0; i < 28; ++i) {
        g_cv[67 + i] = (uint8_t)((i * 255) / 27);
    }
}

/* Preserve white-box table-only test coverage without a production helper. */
static void bemf_cal_build_table(const settings_bemf_cal_t *cal)
{
    s_cal_valid = bemf_cal_prepare_table(cal, s_cal_frac_table);
}

void setUp(void)
{
    g_adc_hook = NULL;
    g_coast_hook = NULL;
    g_delay_hook = NULL;
    g_create_hook = NULL;
    g_validate_hook = NULL;
    g_save_admission_hook = NULL;
    g_save_commit_hook = NULL;
    g_sem_take_hook = NULL;
    g_save_error = ESP_OK;
    g_mutex_create_calls = 0;
    g_mutex_fail_call = 0;
    g_now_ticks = 0;
    mock_timer_now_us = 0;
    mock_mutex_create_fail = 0;
    mock_sem_take_fail = 0;
    mock_task_create_ok = 1;
    mock_nvs_reset();
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    mock_adc_ok = 1;
    mock_ledc_update_count = 0;
    memset(mock_ledc_duty, 0, sizeof(mock_ledc_duty));
    mock_rom_delay_us_total = 0;

    g_cal_valid = false;
    memset(&g_cal, 0, sizeof(g_cal));
    g_bemf_use = true;
    cv_defaults();

    s_bemf_filtered = 0.0f;
    s_bemf_valid = false;
    s_pid_integral = 0.0f;
    s_pid_prev_error = 0.0f;
    s_last_duty = 0;
    s_last_pid_ok = false;
    s_cal_active = false;
    s_cal_task = NULL;
    s_pid_reload = 0;
    s_motor_iter_cap = 0;
    s_stop_requested = false;
    s_bemf_max_fraction = 0.85f;

    TEST_ASSERT_EQUAL(ESP_OK, motor_init());
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(false));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(0));
}

void tearDown(void)
{
}

/* ---- CV2/CV5/CV6 speed curve ---- */

static void test_speed_duty_zero(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, speed_duty(0));
}

static void test_speed_duty_linear_curve(void)
{
    /* Factory curve: CV2=0 (Vstart=0), CV5=255 (Vhigh=1020), CV6=128 (Vmid=512).
     * The lowest steps must stay small so setting a small speed really creeps. */
    TEST_ASSERT_EQUAL_UINT32(0, speed_duty(1));
    TEST_ASSERT_EQUAL_UINT32(74, speed_duty(10)); /* 512 * 9 / 62 */
    TEST_ASSERT_EQUAL_UINT32(512, speed_duty(63));
    TEST_ASSERT_EQUAL_UINT32(1020, speed_duty(126));
}

/* Regression: non-monotonic CV2/CV5/CV6 used to underflow the unsigned
 * interpolation and slam the PWM to a huge value. */
static void test_speed_duty_non_monotonic_no_wrap(void)
{
    g_cv[2] = 200; /* Vstart = 800 */
    g_cv[5] = 50;  /* Vhigh  = 200 (below Vstart) */
    g_cv[6] = 10;  /* Vmid   = 40  (below Vstart) */

    uint32_t d126 = speed_duty(126);
    uint32_t d100 = speed_duty(100);
    TEST_ASSERT_TRUE(d126 <= 1023);
    TEST_ASSERT_TRUE(d100 <= 1023);
    /* The curve is clamped to Vstart <= Vmid <= Vhigh, so everything saturates
     * at the enforced Vhigh = 800. */
    TEST_ASSERT_EQUAL_UINT32(800, d126);
    TEST_ASSERT_EQUAL_UINT32(800, d100);
}

static void test_speed_duty_28_point_table(void)
{
    g_cv[29] = 0x12; /* bit4: use CV67..CV94 table; bit1: 28 steps */

    /* Highest step uses CV94 directly. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)g_cv[94] * 4U, speed_duty(126));
    /* i >= 27 branch also covers the top steps. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)g_cv[94] * 4U, speed_duty(124));
    /* First segment interpolates between CV67 and CV68. */
    uint32_t expected = ((uint32_t)g_cv[67] * 4U) +
                        (((uint32_t)g_cv[68] * 4U) - (uint32_t)g_cv[67] * 4U) * 28U / 128U;
    TEST_ASSERT_EQUAL_UINT32(expected, speed_duty(1));
}

/* ---- BEMF calibration curve ---- */

static void test_bemf_cal_table_interpolation(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 2;
    cal.speed[0] = 12;
    cal.frac[0] = 62;
    cal.speed[1] = 126;
    cal.frac[1] = 725;
    bemf_cal_apply(&cal);

    TEST_ASSERT_TRUE(s_cal_valid);
    TEST_ASSERT_EQUAL_UINT8(2, s_cal_count);
    TEST_ASSERT_EQUAL_UINT16(62, s_cal_frac_table[12]);
    TEST_ASSERT_EQUAL_UINT16(725, s_cal_frac_table[126]);
    /* Linear between (12,62) and (126,725). */
    TEST_ASSERT_EQUAL_UINT16(393, s_cal_frac_table[69]);
    TEST_ASSERT_EQUAL_UINT16(31, s_cal_frac_table[6]);
}

static void test_bemf_cal_table_invalid_when_low(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 2;
    cal.speed[0] = 12;
    cal.frac[0] = 10;
    cal.speed[1] = 126;
    cal.frac[1] = 40; /* <= BEMF_CAL_MIN_VALID */
    bemf_cal_apply(&cal);
    TEST_ASSERT_FALSE(s_cal_valid);
}

static void test_bemf_cal_table_rejects_non_monotonic(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 3;
    cal.speed[0] = 12;
    cal.frac[0] = 200;
    cal.speed[1] = 64;
    cal.frac[1] = 100; /* lower than the previous point -> clamped up */
    cal.speed[2] = 126;
    cal.frac[2] = 725;
    bemf_cal_apply(&cal);

    TEST_ASSERT_FALSE(s_cal_valid);
    TEST_ASSERT_EQUAL_UINT8(0, s_cal_count);
    TEST_ASSERT_EQUAL_UINT16(0, s_cal_frac_table[126]);
}

static void test_bemf_cal_table_needs_two_points(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 1;
    cal.speed[0] = 126;
    cal.frac[0] = 725;
    bemf_cal_apply(&cal);
    TEST_ASSERT_FALSE(s_cal_valid);
    TEST_ASSERT_EQUAL_UINT16(0, s_cal_frac_table[126]);
}

/* ---- PID coefficients ---- */

static void test_load_pid_maps_cv_range(void)
{
    g_cv[54] = 0;
    g_cv[55] = 0;
    g_cv[56] = 0;
    load_pid();
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.10f, s_pid_kp);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.00f, s_pid_ki);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.00f, s_pid_kd);

    g_cv[54] = 255;
    g_cv[55] = 255;
    g_cv[56] = 255;
    load_pid();
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.00f, s_pid_kp);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.30f, s_pid_ki);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.80f, s_pid_kd);
}

/* ---- speed setter / status ---- */

static void test_motor_set_speed_clamps(void)
{
    uint8_t sp = 0;
    bool fwd = false;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(200, true));
    motor_get_status(&sp, &fwd);
    TEST_ASSERT_EQUAL_UINT8(126, sp);
    TEST_ASSERT_TRUE(fwd);

    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(50, false));
    motor_get_status(&sp, &fwd);
    TEST_ASSERT_EQUAL_UINT8(50, sp);
    TEST_ASSERT_FALSE(fwd);

    motor_stop();
    motor_get_status(&sp, &fwd);
    TEST_ASSERT_EQUAL_UINT8(0, sp);

    motor_get_status(NULL, NULL);
}

static void test_motor_get_applied_speed(void)
{
    uint8_t sp = 0;
    bool fwd = false;
    applied_publish(42, false);
    motor_get_applied_speed(&sp, &fwd);
    TEST_ASSERT_EQUAL_UINT8(42, sp);
    TEST_ASSERT_FALSE(fwd);
    applied_publish(7, true);
    motor_get_applied_speed(&sp, &fwd);
    TEST_ASSERT_EQUAL_UINT8(7, sp);
    TEST_ASSERT_TRUE(fwd);
    motor_get_applied_speed(NULL, NULL);
}

/* ---- motor_tick: ramp / kickstart ---- */

static void test_motor_tick_ramp_no_inertia(void)
{
    motor_set_bemf_enabled(false);
    g_cv[3] = 0;
    g_cv[4] = 0;
    s_target_speed = 126;
    s_target_forward = true;
    s_applied_speed = 0;

    motor_tick();

    TEST_ASSERT_EQUAL_UINT8(126, s_applied_speed);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(126), mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_motor_tick_ramp_uses_cv3(void)
{
    g_cv[3] = 1; /* 1 s per full range -> ~100 ticks of ramping */
    s_target_speed = 126;
    s_applied_speed = 0;
    s_ramp_acc = 0;

    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(1, s_applied_speed);
    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(2, s_applied_speed);
}

static void test_motor_tick_decel_uses_cv4(void)
{
    g_cv[4] = 1;
    s_target_speed = 0;
    s_applied_speed = 126;
    s_ramp_acc = 0;
    s_was_stopped = false;

    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(125, s_applied_speed);
}

static void test_motor_tick_kickstart(void)
{
    motor_set_rail_voltage_mv(1000); /* Fresh pair permits the startup kick. */
    g_cv[65] = 255; /* full kick */
    g_cv[5] = 100;  /* Vhigh = 400 */
    g_cv[6] = 0;    /* Vmid = (Vstart+Vhigh)/2 so the kick clearly dominates */
    g_cv[3] = 0;
    s_target_speed = 50;
    s_applied_speed = 0;
    s_was_stopped = true;
    s_ramp_acc = 0;

    motor_tick();

    TEST_ASSERT_EQUAL_UINT8(50, s_applied_speed);
    /* Kick duty = Vhigh * CV65 / 255 = 400 dominates the open-loop duty. */
    TEST_ASSERT_EQUAL_UINT32(400, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(KICK_TICKS - 1), s_kick_left);
}

static void test_motor_tick_reverse_drives_other_channel(void)
{
    motor_set_bemf_enabled(false);
    g_cv[3] = 0;
    s_target_speed = 100;
    s_target_forward = false;
    s_applied_speed = 0;

    motor_tick();

    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(100), mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_motor_tick_bemf_disabled_open_loop(void)
{
    motor_set_bemf_enabled(false);
    TEST_ASSERT_FALSE(motor_get_bemf_enabled());

    /* A measurable back-EMF is present, but with the loop disabled the PID
     * must not touch the duty and the coast sample window must not run. */
    motor_set_rail_voltage_mv(2000);
    s_bemf1_mv = 500;
    s_bemf2_mv = 0;
    s_bemf_filtered = 10.0f;
    s_bemf_valid = true;
    mock_rom_delay_us_total = 0;

    g_cv[3] = 0;
    s_target_speed = 10;
    s_applied_speed = 0;

    motor_tick();

    TEST_ASSERT_EQUAL_UINT32(0, mock_rom_delay_us_total); /* no coast window */
    TEST_ASSERT_EQUAL_UINT32(speed_duty(10), mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_FALSE(s_last_pid_ok);
}

static void test_motor_bemf_enable_resets_state(void)
{
    s_pid_integral = 1234.0f;
    s_pid_prev_error = 42.0f;
    s_bemf_filtered = 99.0f;
    s_bemf_valid = true;

    motor_set_bemf_enabled(false);
    motor_tick(); /* owner consumes feedback reset */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, s_pid_integral);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, s_pid_prev_error);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, s_bemf_filtered);
    TEST_ASSERT_FALSE(s_bemf_valid);

    motor_set_bemf_enabled(true);
    TEST_ASSERT_TRUE(motor_get_bemf_enabled());
}

/* ---- BEMF update ---- */

static void test_bemf_update_rejects_rail_clamp(void)
{
    motor_set_rail_voltage_mv(1000);
    s_bemf1_mv = 900;
    s_bemf2_mv = 0;
    s_bemf_filtered = 0.0f;

    bemf_update();
    TEST_ASSERT_FALSE(s_bemf_valid); /* 900 > 0.85 * 1000: freewheel clamp */

    s_bemf1_mv = 400;
    bemf_update();
    TEST_ASSERT_TRUE(s_bemf_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 60.0f, s_bemf_filtered); /* 0.15 * 400 */
}

static void test_bemf_update_without_adc(void)
{
    s_bemf_adc_ready = false;
    s_bemf_filtered = 123.0f;
    bemf_update();
    TEST_ASSERT_FALSE(s_bemf_valid);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, s_bemf_filtered);
}

/* ---- diagnostics / calibration API ---- */

static void test_motor_bemf_adc_dump(void)
{
    mock_adc_raw[PIN_BEMF1 - 1] = 111;
    mock_adc_raw[PIN_BEMF2 - 1] = 222;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 333;
    uint16_t b1 = 0, b2 = 0, rail = 0;
    motor_bemf_adc_dump(&b1, &b2, &rail);
    TEST_ASSERT_EQUAL_UINT16(111, b1);
    TEST_ASSERT_EQUAL_UINT16(222, b2);
    TEST_ASSERT_EQUAL_UINT16(333, rail);
    motor_bemf_adc_dump(NULL, NULL, NULL); /* must not crash */
}

static void test_motor_bemf_coast_read(void)
{
    mock_adc_raw[PIN_BEMF1 - 1] = 4094;
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    uint16_t b1 = 0, b2 = 0;
    motor_bemf_coast_read(&b1, &b2);
    TEST_ASSERT_EQUAL_UINT16(3099, b1);
    TEST_ASSERT_EQUAL_UINT16(0, b2);
    TEST_ASSERT_EQUAL_UINT32(1000, mock_rom_delay_us_total); /* BEMF_SETTLE_US */
}

static void test_motor_bemf_diag(void)
{
    motor_set_rail_voltage_mv(1234);
    motor_bemf_diag_t d;
    memset(&d, 0xFF, sizeof(d));
    motor_bemf_diag(&d);
    TEST_ASSERT_EQUAL_UINT32(1234, d.rail_mv);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, (float)s_bemf_filtered, (float)d.bemf_filtered_mv);
    motor_bemf_diag(NULL); /* must not crash */
}

static void test_motor_bemf_base_info(void)
{
    motor_bemf_base_info_t info;
    memset(&info, 0, sizeof(info));
    motor_bemf_base_info(&info);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_BASE.count, info.count);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_BASE.speed[0], info.speed[0]);
    TEST_ASSERT_EQUAL_UINT16(BEMF_CAL_BASE.frac[info.count - 1], info.full_frac);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(0.08f * 1024.0f), info.start_frac);
    motor_bemf_base_info(NULL); /* must not crash */
}

static void test_motor_bemf_cal_info_base(void)
{
    motor_bemf_cal_info_t info;
    memset(&info, 0, sizeof(info));
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_BASE.count, info.count);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_POINTS, info.total);
    TEST_ASSERT_TRUE(info.valid);
    TEST_ASSERT_FALSE(info.stored); /* nothing saved */
    TEST_ASSERT_FALSE(info.active);
    motor_bemf_cal_info(NULL); /* must not crash */
}

static void test_motor_bemf_cal_info_stored(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 3;
    cal.speed[0] = 12;
    cal.frac[0] = 100;
    cal.speed[1] = 60;
    cal.frac[1] = 400;
    cal.speed[2] = 126;
    cal.frac[2] = 700;
    TEST_ASSERT_EQUAL(ESP_OK, settings_bemf_cal_save(&cal));
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_reload());

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL_UINT8(3, info.count);
    TEST_ASSERT_TRUE(info.stored);
    TEST_ASSERT_TRUE(info.valid);
    TEST_ASSERT_EQUAL_UINT16(700, info.frac[2]);
}

static void test_motor_bemf_cal_clear_restores_base(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 2;
    cal.speed[0] = 12;
    cal.frac[0] = 100;
    cal.speed[1] = 126;
    cal.frac[1] = 700;
    (void)settings_bemf_cal_save(&cal);
    (void)motor_bemf_cal_reload();

    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_clear());

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_FALSE(info.stored);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_BASE.count, info.count);
}

/* ---- coverage: remaining branches ---- */

static void test_speed_duty_vhigh_zero(void)
{
    g_cv[5] = 0; /* Vhigh=0 -> LEDC_MAX */
    g_cv[6] = 0; /* Vmid=0 -> midpoint */
    TEST_ASSERT_EQUAL_UINT32(1023, speed_duty(126));

    /* Non-monotonic CV2/CV6/CV5 must be clamped (no unsigned wrap). */
    g_cv[2] = 200;
    g_cv[6] = 50;
    g_cv[5] = 10;
    TEST_ASSERT_TRUE(speed_duty(64) <= 1023);
}

static void test_speed_duty_table_branch(void)
{
    g_cv[29] = 0x12; /* CV29 bit 4 -> 28-point table */
    g_cv[94] = 200;
    g_cv[67] = 10;
    g_cv[68] = 40;
    TEST_ASSERT_EQUAL_UINT32(0, speed_duty(0));
    TEST_ASSERT_TRUE(speed_duty(1) >= 40);         /* interpolated */
    TEST_ASSERT_EQUAL_UINT32(800, speed_duty(126)); /* i>=27 -> CV94*4 */
}

static void test_bemf_sample_window_lock_fails(void)
{
    mock_sem_take_fail = 1;
    bemf_sample_window(); /* motor_bemf_lock() false -> early return */
    mock_sem_take_fail = 0;
}

static void test_bemf_cal_build_table_edges(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 3;
    cal.speed[0] = 10;
    cal.frac[0] = 100;
    cal.speed[1] = 5; /* decreasing -> skipped */
    cal.frac[1] = 50;
    cal.speed[2] = 126;
    cal.frac[2] = 700;
    bemf_cal_build_table(&cal);
    TEST_ASSERT_FALSE(s_cal_valid);

    /* Last point below 126 -> the tail is filled. */
    memset(&cal, 0, sizeof(cal));
    cal.count = 2;
    cal.speed[0] = 10;
    cal.frac[0] = 100;
    cal.speed[1] = 60;
    cal.frac[1] = 300;
    bemf_cal_build_table(&cal);
    TEST_ASSERT_EQUAL_UINT16(300, s_cal_frac_table[126]);

    /* Invalid: full-speed fraction below the trust threshold. */
    memset(&cal, 0, sizeof(cal));
    cal.count = 2;
    cal.speed[0] = 10;
    cal.frac[0] = 10;
    cal.speed[1] = 60;
    cal.frac[1] = 20;
    bemf_cal_build_table(&cal);
    TEST_ASSERT_FALSE(s_cal_valid);

    bemf_cal_build_table(NULL);
    TEST_ASSERT_FALSE(s_cal_valid);
    memset(&cal, 0, sizeof(cal));
    cal.count = 1;
    bemf_cal_build_table(&cal);
    TEST_ASSERT_FALSE(s_cal_valid);
}

static void test_bemf_cal_apply_preserves_all_points(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = MOTOR_BEMF_CAL_MAX_POINTS;
    for (int i = 0; i < cal.count; ++i) {
        cal.speed[i] = (uint8_t)(10 + i * 5);
        cal.frac[i] = (uint16_t)(100 + i * 50);
    }
    bemf_cal_apply(&cal);
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CAL_MAX_POINTS, s_cal_count);
}

static void test_motor_tick_cal_active(void)
{
    s_cal_active = true;
    s_target_speed = 10;
    s_applied_speed = 0;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(10, s_applied_speed); /* applied = target */
    TEST_ASSERT_FALSE(s_was_stopped);
    TEST_ASSERT_EQUAL_UINT8(0, s_kick_left);
    s_cal_active = false;
}

static void test_motor_tick_ramp_completes_and_kickstart(void)
{
    g_cv[3] = 1;    /* fast ramp */
    g_cv[65] = 200; /* kickstart enabled */
    g_cv[5] = 255;
    s_target_speed = 1;
    s_applied_speed = 0;
    s_was_stopped = true;
    s_cal_active = false;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(1, s_applied_speed);
    TEST_ASSERT_EQUAL_UINT32(0, s_ramp_acc);
    TEST_ASSERT_TRUE(s_kick_duty > 0);
}

static void test_motor_tick_target_equals_applied_and_stop(void)
{
    s_target_speed = 5;
    s_applied_speed = 5;
    s_cal_active = false;
    s_was_stopped = false;
    motor_tick(); /* target == applied -> ramp_acc reset */
    TEST_ASSERT_EQUAL_UINT32(0, s_ramp_acc);

    s_target_speed = 0;
    s_applied_speed = 0;
    g_cv[3] = 1;
    motor_tick(); /* applied == 0 -> stopped, pid reset */
    TEST_ASSERT_TRUE(s_was_stopped);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_integral);
}

static void test_motor_tick_pid(void)
{
    s_bemf_enabled = true;
    s_bemf_adc_ready = true;
    s_cal_active = false;
    s_target_speed = 50;
    s_applied_speed = 50;
    s_target_forward = true;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(1000));
    s_bemf_filtered = 0.0f;
    s_bemf_valid = true;
    s_pid_reload = 99;
    s_cal_valid = false;

    motor_tick();
    TEST_ASSERT_TRUE(s_last_pid_ok); /* PID ran (linear fallback) */
    TEST_ASSERT_EQUAL_UINT8(0, s_pid_reload); /* load_pid() ran */

    /* Saturated low: huge measured BEMF -> error very negative -> clamp up. */
    s_bemf_filtered = 100000.0f;
    motor_tick();
    TEST_ASSERT_TRUE(s_last_pid_ok);

    /* Valid calibration table branch. */
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = 2;
    cal.speed[0] = 12;
    cal.frac[0] = 100;
    cal.speed[1] = 126;
    cal.frac[1] = 700;
    bemf_cal_apply(&cal);
    TEST_ASSERT_TRUE(s_cal_valid);
    s_bemf_filtered = 100.0f;
    motor_tick();
    TEST_ASSERT_TRUE(s_last_pid_ok);
}

static void test_motor_boot_safe_and_task(void)
{
    motor_boot_safe();
    s_motor_iter_cap = 1;
    motor_task(NULL); /* one tick */
    s_motor_iter_cap = 0;
}

static void test_motor_init_and_set_speed_errors(void)
{
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, motor_init());
    mock_task_create_ok = 1;
    TEST_ASSERT_EQUAL(ESP_OK, motor_init());

    s_init = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(1, true));
    s_init = true;
}

static void refresh_calibration_rail(TickType_t ticks)
{
    (void)ticks;
    motor_set_rail_voltage_mv(1000); /* Model the independent track publisher. */
}

static void test_bemf_cal_task_runs(void)
{
    g_delay_hook = refresh_calibration_rail;
    mock_adc_raw[PIN_BEMF1 - 1] = 1000; /* ~757 mV */
    mock_adc_raw[PIN_BEMF2 - 1] = 0;    /* measurable valid unloaded curve */
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(1000));
    s_bemf_valid = false;
    s_bemf_filtered = 0.0f;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_TRUE(g_cal_valid);
    /* Calibration must leave the motor truly stopped, not at step 126. */
    TEST_ASSERT_EQUAL_UINT8(0, s_applied_speed);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_integral);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_prev_error);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);

    /* Rail too low -> n stays 0 -> frac cleared. */
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    TEST_ASSERT_FALSE(s_cal_active);
}

static void test_motor_bemf_cal_start_states(void)
{
    s_cal_task = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(1000));
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());

    s_cal_task = (TaskHandle_t)1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    s_cal_task = NULL;
    s_cal_active = false;

    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, motor_bemf_cal_start());
    mock_task_create_ok = 1;
}

static void test_motor_bemf_cal_info_paths(void)
{
    motor_bemf_cal_info(NULL); /* early return */

    motor_bemf_cal_info_t info;
    s_cal_active = true;
    s_cal_step = 3;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_TRUE(info.active);
    TEST_ASSERT_EQUAL_UINT8(3, info.count);

    s_cal_active = false;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_FALSE(info.active);
}

static void test_motor_bemf_cal_clear_reload_active(void)
{
    s_cal_active = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_clear());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_reload());
    s_cal_active = false;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_reload());
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_clear());
}

static void test_motor_bemf_adc_dump_and_coast_read(void)
{
    uint16_t b1 = 0, b2 = 0, rail = 0;
    s_bemf_adc_ready = false;
    motor_bemf_adc_dump(&b1, &b2, &rail); /* early return */
    motor_bemf_coast_read(&b1, &b2);      /* early return */

    s_bemf_adc_ready = true;
    mock_adc_raw[PIN_BEMF1 - 1] = 111;
    mock_adc_raw[PIN_BEMF2 - 1] = 222;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 333;
    motor_bemf_adc_dump(&b1, &b2, &rail);
    TEST_ASSERT_EQUAL_UINT16(111, b1);
    TEST_ASSERT_EQUAL_UINT16(222, b2);
    TEST_ASSERT_EQUAL_UINT16(333, rail);
    motor_bemf_adc_dump(NULL, NULL, NULL); /* null args, no crash */

    motor_bemf_coast_read(&b1, &b2);
    motor_bemf_coast_read(NULL, NULL);
}

static void test_motor_bemf_lock_null_mutex(void)
{
    SemaphoreHandle_t saved = s_bemf_mutex;
    s_bemf_mutex = NULL;
    TEST_ASSERT_FALSE(motor_bemf_lock());
    motor_bemf_unlock();
    s_bemf_mutex = saved;
}

static void test_motor_tick_pid_kick_and_integral_clamp(void)
{
    s_bemf_enabled = true;
    s_bemf_adc_ready = true;
    s_cal_active = false;
    s_target_speed = 50;
    s_applied_speed = 50;
    s_target_forward = true;
    g_cv[55] = 0;   /* Ki = 0 so the preloaded integral only feeds the clamp */
    g_cv[65] = 200; /* kickstart */
    g_cv[5] = 255;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(1000));
    s_bemf_valid = true;
    s_bemf_filtered = 455.0f;   /* below target -> negative error */
    s_pid_integral = -30000.0f; /* integral + error < -20000 */
    s_pid_prev_error = 0.0f;
    s_pid_reload = 99; /* load_pid() runs on this tick */

    /* Pre-arm a kick window so `kicking` is true and duty_final is raised. */
    s_kick_left = 10;
    s_kick_duty = 1023;
    s_was_stopped = false;

    motor_tick();
    TEST_ASSERT_TRUE(s_last_pid_ok);
    TEST_ASSERT_EQUAL_FLOAT(-20000.0f, s_pid_integral); /* clamped low */
}

/* Regression: a non-monotonic CV67..CV94 must not wrap the table interpolation
 * and slam the PWM to an arbitrary value. */
static void test_speed_duty_table_non_monotonic_no_wrap(void)
{
    g_cv[29] = 0x12; /* table mode */
    g_cv[67] = 255;
    g_cv[68] = 0;

    uint32_t d = speed_duty(1);
    TEST_ASSERT_TRUE(d <= 1023);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)g_cv[67] * 4U, d);
}

static void test_motor_emergency_stop(void)
{
    s_target_speed = 50;
    s_applied_speed = 50;
    s_applied_forward = false;
    s_pid_integral = 10.0f;
    s_pid_prev_error = 5.0f;
    s_kick_left = 3;
    s_kick_duty = 100;
    s_ramp_acc = 7;
    s_last_duty = 500;

    motor_emergency_stop();

    TEST_ASSERT_EQUAL_UINT8(0, s_target_speed);
    uint8_t published = 255;
    motor_get_applied_speed(&published, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, published);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
    motor_tick(); /* owner consumes its deferred ramp/PID reset */
    TEST_ASSERT_EQUAL_UINT8(0, s_applied_speed);
    TEST_ASSERT_TRUE(s_applied_forward);
    TEST_ASSERT_EQUAL_UINT32(0, s_ramp_acc);
    TEST_ASSERT_TRUE(s_was_stopped);
    TEST_ASSERT_EQUAL_UINT8(0, s_kick_left);
    TEST_ASSERT_EQUAL_UINT32(0, s_kick_duty);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_integral);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_prev_error);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_motor_last_tick_timestamp(void)
{
    s_cal_active = false;
    s_target_speed = 0;
    s_applied_speed = 0;
    mock_timer_now_us = 4242;
    motor_tick();
    TEST_ASSERT_EQUAL_INT64(4242, motor_last_tick_us());
}

/* Reversing while moving must decelerate to a stop before the polarity flips
 * (no full-duty reversal). */
static void test_motor_tick_reverse_while_moving_ramps_down(void)
{
    motor_set_bemf_enabled(false);
    g_cv[4] = 1; /* deceleration 1 s */
    s_target_speed = 100;
    s_applied_speed = 100;
    s_applied_forward = true;
    s_target_forward = false;
    s_ramp_acc = 0;
    s_was_stopped = false;

    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(99, s_applied_speed);
    TEST_ASSERT_TRUE(s_applied_forward); /* still forward until stopped */
    TEST_ASSERT_EQUAL_UINT32(speed_duty(99), mock_ledc_duty[LEDC_CHANNEL_0]);

    for (int i = 0; i < 200 && s_applied_forward; ++i) {
        motor_tick();
    }
    TEST_ASSERT_FALSE(s_applied_forward);
    TEST_ASSERT_EQUAL_UINT8(0, s_applied_speed);
}

/* Rejected feedback must preserve PID/filter history, not invent speed decay. */
static void test_motor_tick_pid_unavailable_preserves_history(void)
{
    motor_set_bemf_enabled(true);
    motor_tick(); /* consume the enable transition before seeding PID history */
    s_bemf_adc_ready = true;
    s_cal_active = false;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(1000));
    mock_adc_raw[PIN_BEMF1 - 1] = 4095; /* rail-hitting sample -> not valid */
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    s_target_speed = 10;
    s_applied_speed = 10;
    s_target_forward = true;
    s_pid_integral = 100.0f;
    s_pid_prev_error = 100.0f;

    motor_tick();

    TEST_ASSERT_FALSE(s_last_pid_ok);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, s_pid_integral);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, s_pid_prev_error);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

/* Reversing while moving with no configured deceleration must stop at once. */
static void test_motor_tick_reverse_while_moving_no_decel(void)
{
    motor_set_bemf_enabled(false);
    g_cv[4] = 0; /* no deceleration ramp */
    s_target_speed = 50;
    s_applied_speed = 50;
    s_applied_forward = true;
    s_target_forward = false;
    s_ramp_acc = 0;
    s_was_stopped = false;

    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(0, s_applied_speed);
    TEST_ASSERT_FALSE(s_applied_forward);
    TEST_ASSERT_EQUAL_UINT32(0, s_ramp_acc);
}

/* In table mode the kickstart must be based on the table top (CV94), not CV5. */
static void test_motor_tick_kickstart_table_mode(void)
{
    motor_set_rail_voltage_mv(1000);
    g_cv[29] = 0x12;  /* table mode, 28 steps */
    g_cv[65] = 255;   /* full kick */
    g_cv[94] = 100;   /* table top = 400 */
    g_cv[5] = 0;      /* CV5 unused in table mode; must not force LEDC_MAX */
    g_cv[3] = 0;
    s_target_speed = 1; /* low duty_base so the kick clearly dominates */
    s_applied_speed = 0;
    s_was_stopped = true;
    s_ramp_acc = 0;
    s_cal_active = false;

    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(1, s_applied_speed);
    TEST_ASSERT_EQUAL_UINT32(400, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(KICK_TICKS - 1), s_kick_left);

    /* CV94 == 0 falls back to full duty. */
    g_cv[94] = 0;
    s_kick_left = 0;
    s_was_stopped = true;
    s_applied_speed = 0;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(LEDC_MAX, s_kick_duty);
}

static void cancel_coast(void)
{
    motor_emergency_stop();
}

static void reverse_during_coast(void)
{
    g_coast_hook = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(100, false));
}

static void fail_second_adc(int channel)
{
    if (channel == PIN_BEMF2 - 1) {
        mock_adc_ok = 0;
    }
}

static void lose_rail_during_adc(int channel)
{
    (void)channel;
    motor_set_rail_voltage_mv(0);
}

static void cancel_cal_delay(TickType_t ticks)
{
    (void)ticks;
    g_delay_hook = NULL;
    motor_stop();
    TEST_ASSERT_TRUE(s_cal_active); /* reservation survives cancellation */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
}

static void test_persistent_inhibit_discards_commands(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(80, false));
    motor_tick();
    uint32_t stale = s_output_generation;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(true));
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(80, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    TEST_ASSERT_FALSE(apply_pwm(800, false, stale));
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
    motor_set_inhibited(false);
    motor_tick();
    motor_tick();
    uint8_t speed = 255;
    motor_get_status(&speed, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, speed);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
}

static void test_stop_nonblocking_and_stale_output_rejected(void)
{
    motor_set_bemf_enabled(false);
    uint32_t stale = s_output_generation;
    s_last_duty = 700;
    mock_sem_take_fail = 1; /* simulate a hung control/ADC owner */
    motor_emergency_stop();
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
    TEST_ASSERT_FALSE(apply_pwm(700, true, stale));
    mock_sem_take_fail = 0;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(30, false));
    TEST_ASSERT_FALSE(apply_pwm(700, true, stale));
    motor_tick();
    motor_tick();
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_1] > 0U);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_coast_cancel_never_restores_motion(void)
{
    s_last_duty = 600;
    applied_publish(60, false);
    g_coast_hook = cancel_coast;
    uint16_t b1 = 0, b2 = 0;
    motor_bemf_coast_read(&b1, &b2);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_direction_snapshot_survives_mid_tick_command(void)
{
    motor_set_rail_voltage_mv(1000);
    g_cv[4] = 1;
    s_target_speed = 100;
    s_target_forward = true;
    s_applied_speed = 100;
    s_applied_forward = true;
    s_was_stopped = false;
    applied_publish(100, true);
    g_coast_hook = reverse_during_coast;
    motor_tick();
    TEST_ASSERT_TRUE(s_applied_forward);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(99, s_applied_speed);
    TEST_ASSERT_TRUE(s_applied_forward);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_adc_split_pair_is_never_feedback(void)
{
    s_bemf1_mv = 400;
    s_bemf2_mv = 100;
    s_bemf_valid = true;
    mock_adc_raw[PIN_BEMF1 - 1] = 900;
    g_adc_hook = fail_second_adc;
    TEST_ASSERT_FALSE(bemf_sample_window());
    TEST_ASSERT_FALSE(s_bemf_valid);
    TEST_ASSERT_EQUAL_UINT32(400, s_bemf1_mv);
    TEST_ASSERT_EQUAL_UINT32(100, s_bemf2_mv);
    g_adc_hook = NULL;
    mock_adc_ok = 1;
    mock_sem_take_fail = 1;
    s_bemf_valid = true;
    TEST_ASSERT_FALSE(bemf_sample_window());
    TEST_ASSERT_FALSE(s_bemf_valid);
}

static void test_calibration_admission_and_reservation(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start()); /* no rail */
    motor_set_rail_voltage_mv(1000);
    motor_set_speed(20, false);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    motor_emergency_stop();
    motor_tick();
    s_bemf_adc_ready = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    s_bemf_adc_ready = true;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    TEST_ASSERT_TRUE(s_cal_active); /* reserved before task execution */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(40, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_reload());
}

static void test_regular_stop_cancels_calibration(void)
{
    motor_set_rail_voltage_mv(1000);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    uint32_t generation = s_output_generation;
    g_delay_hook = cancel_cal_delay;
    bemf_cal_task((void *)(uintptr_t)generation);
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_FALSE(g_cal_valid);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
    TEST_ASSERT_FALSE(apply_pwm(600, true, generation));
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_calibration_raw_average_has_no_iir_bias(void)
{
    g_delay_hook = refresh_calibration_rail;
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 661; /* floor(661*3100/4095) = 500 */
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    s_bemf_filtered = 900.0f;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_TRUE(g_cal_valid);
    for (uint8_t i = 0; i < g_cal.count; ++i) {
        TEST_ASSERT_EQUAL_UINT16(512, g_cal.frac[i]);
    }
}

static void test_failed_calibration_preserves_stored_curve(void)
{
    g_cal = BEMF_CAL_BASE;
    g_cal_valid = true;
    settings_bemf_cal_t before = g_cal;
    motor_set_rail_voltage_mv(1000);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    mock_adc_ok = 0;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_cal, sizeof(g_cal));
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
}

static void test_calibration_rail_snapshot_no_division_race(void)
{
    g_cal = BEMF_CAL_BASE;
    g_cal_valid = true;
    settings_bemf_cal_t before = g_cal;
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 661;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    g_adc_hook = lose_rail_during_adc;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_cal, sizeof(g_cal));
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
}

static void test_pid_can_reduce_below_half_base_preserves_kick(void)
{
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 1057; /* 800 mV, accepted below 85% rail */
    s_bemf_filtered = 800.0f;
    s_target_speed = 20;
    s_applied_speed = 20;
    s_was_stopped = false;
    s_reset_requested = false;
    motor_tick();
    TEST_ASSERT_TRUE(s_last_pid_ok);
    TEST_ASSERT_TRUE(s_last_duty < speed_duty(20) / 2U);
    s_kick_left = 2;
    s_kick_duty = 800;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(800, s_last_duty);
}

static void test_bemf_ceiling_bounds_fallback_and_accepts_measured_high(void)
{
    motor_set_rail_voltage_mv(1000);
    s_cal_valid = false;
    s_target_speed = 126;
    s_applied_speed = 126;
    s_was_stopped = false;
    motor_tick();
    TEST_ASSERT_TRUE(s_last_target <= 850.0f);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, motor_set_bemf_max_fraction(1025));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, motor_set_bemf_max_fraction(81));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_bemf_max_fraction(973));
    mock_adc_raw[PIN_BEMF1 - 1] = 1189; /* 900mV > old85%, < measured95% */
    motor_tick();
    TEST_ASSERT_TRUE(s_bemf_valid);
    TEST_ASSERT_TRUE(s_last_pid_ok);
}

static void test_calibration_export_all_sixteen_points(void)
{
    settings_bemf_cal_t cal = {0};
    cal.count = 16;
    for (uint8_t i = 0; i < cal.count; ++i) {
        cal.speed[i] = (uint8_t)(i * 8U);
        cal.frac[i] = (uint16_t)(i * 50U);
    }
    settings_bemf_cal_save(&cal);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_reload());
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL_UINT8(16, info.count);
    TEST_ASSERT_EQUAL_MEMORY(cal.speed, info.speed, sizeof(cal.speed));
    TEST_ASSERT_EQUAL_MEMORY(cal.frac, info.frac, sizeof(cal.frac));
}

static void test_corrupt_curve_rejected_before_pid(void)
{
    settings_bemf_cal_t cal = BEMF_CAL_BASE;
    cal.frac[cal.count - 1U] = 65535;
    bemf_cal_apply(&cal);
    TEST_ASSERT_FALSE(s_cal_valid);
    TEST_ASSERT_EQUAL_UINT16(0, s_cal_frac_table[126]);
    cal.count = 17;
    bemf_cal_apply(&cal);
    TEST_ASSERT_FALSE(s_cal_valid);
}

static void test_feedback_reset_is_owner_applied(void)
{
    s_pid_integral = 123;
    motor_set_bemf_enabled(false);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 123.0f, s_pid_integral);
    motor_tick();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_integral);
    s_pid_integral = 345;
    motor_bemf_cal_reload();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 345.0f, s_pid_integral);
    motor_tick();
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_integral);
}

static void test_heartbeat_full_width(void)
{
    mock_timer_now_us = ((int64_t)1 << 32) + 1234;
    motor_tick();
    TEST_ASSERT_EQUAL_INT64(mock_timer_now_us, motor_last_tick_us());
}

static uint32_t g_task_waits;
static void simulate_overrun(TickType_t ticks)
{
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(MOTOR_TICK_MS), ticks);
    ++g_task_waits;
    mock_timer_now_us += 150000; /* late task never runs catch-up iterations */
    g_now_ticks += pdMS_TO_TICKS(150);
}

static void test_overrun_does_not_replay_control_ticks(void)
{
    g_task_waits = 0;
    g_delay_hook = simulate_overrun;
    s_motor_iter_cap = 3;
    motor_task(NULL);
    TEST_ASSERT_EQUAL_UINT32(3, g_task_waits);
}

static void test_adc_mutex_missing_fails_closed(void)
{
    SemaphoreHandle_t saved = s_bemf_mutex;
    s_bemf_mutex = NULL;
    TEST_ASSERT_FALSE(motor_bemf_lock());
    TEST_ASSERT_EQUAL_INT(-1, motor_adc_read_raw(PIN_BEMF1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    s_bemf_mutex = saved;
}

static void finish_task_before_create_returns(void (*task)(void *), void *arg)
{
    g_create_hook = NULL;
    task(arg);
}

static void test_cal_worker_can_finish_before_create_returns(void)
{
    g_delay_hook = refresh_calibration_rail;
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 661;
    g_create_hook = finish_task_before_create_returns;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_NULL(s_cal_task);
    TEST_ASSERT_TRUE(g_cal_valid);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
}

static void test_adc_mutex_allocation_failure_disables_adc_features(void)
{
    g_mutex_fail_call = g_mutex_create_calls + 2U; /* control succeeds, ADC fails */
    TEST_ASSERT_EQUAL(ESP_OK, motor_init());
    TEST_ASSERT_FALSE(s_bemf_adc_ready);
    TEST_ASSERT_FALSE(motor_bemf_lock());
    TEST_ASSERT_TRUE(motor_is_inhibited());
    motor_set_inhibited(false);
    motor_set_rail_voltage_mv(1000);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(20, true));
    uint32_t before = mock_rom_delay_us_total;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(before, mock_rom_delay_us_total);
    TEST_ASSERT_FALSE(s_last_pid_ok);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_control_mutex_allocation_failure_stops_motor(void)
{
    mock_mutex_create_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, motor_init());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(20, true));
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
    motor_emergency_stop();
}

static void inspect_old_curve_during_prepare(void)
{
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_BASE.count, info.count);
    TEST_ASSERT_EQUAL_UINT16(BEMF_CAL_BASE.frac[0], info.frac[0]);
}

static void test_curve_publication_is_complete_snapshot(void)
{
    settings_bemf_cal_t cal = {0};
    cal.count = 2;
    cal.speed[0] = 12;
    cal.frac[0] = 123;
    cal.speed[1] = 126;
    cal.frac[1] = 700;
    g_validate_hook = inspect_old_curve_during_prepare;
    bemf_cal_apply(&cal);
    g_validate_hook = NULL;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL_UINT8(2, info.count);
    TEST_ASSERT_EQUAL_UINT16(123, info.frac[0]);
    TEST_ASSERT_EQUAL_UINT16(700, s_cal_frac_table[126]);
}

static void test_inhibit_owners_cannot_clear_other_safety_gates(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(40, true));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(false));
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(40, true));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_DCC_TIMEOUT, true));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, false));
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_DCC_TIMEOUT, false));
    TEST_ASSERT_FALSE(motor_is_inhibited());
    uint8_t speed = 255;
    motor_get_status(&speed, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, speed);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      motor_set_inhibit_reason((motor_inhibit_reason_t)8U, false));
}

static void test_adc_dump_status_distinguishes_zero_and_failures(void)
{
    uint16_t b1 = 9, b2 = 9, rail = 9;
    s_bemf_adc_ready = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_adc_dump_checked(&b1, &b2, &rail));
    s_bemf_adc_ready = true;
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, motor_bemf_adc_dump_checked(&b1, &b2, &rail));
    mock_sem_take_fail = 0;
    mock_adc_ok = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_adc_dump_checked(&b1, &b2, &rail));
    TEST_ASSERT_EQUAL_UINT16(0, b1);
    TEST_ASSERT_EQUAL_UINT16(0, b2);
    TEST_ASSERT_EQUAL_UINT16(0, rail);
    mock_adc_ok = 1;
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_adc_dump_checked(&b1, &b2, &rail));
    TEST_ASSERT_EQUAL_UINT16(0, b1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, motor_bemf_adc_dump_checked(NULL, NULL, NULL));
}

static void feedback_reduced_drive(void)
{
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 1057; /* 800 mV accepted. */
    s_bemf_filtered = 800.0f;
    s_reset_requested = false;
    s_was_stopped = false;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(63, true));
    motor_tick();
    TEST_ASSERT_TRUE(s_last_pid_ok);
    TEST_ASSERT_TRUE(s_last_duty > 0U);
    TEST_ASSERT_TRUE(s_last_duty < speed_duty(63));
}

static void test_feedback_loss_holds_reduced_actual_duty(void)
{
    feedback_reduced_drive();
    uint32_t duty = mock_ledc_duty[LEDC_CHANNEL_0];
    float filtered = s_bemf_filtered;
    float integral = s_pid_integral;
    mock_adc_raw[PIN_BEMF1 - 1] = 4095;
    mock_timer_now_us = 99999;
    motor_set_rail_voltage_mv(1000);
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(duty, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_FLOAT(filtered, s_bemf_filtered);
    TEST_ASSERT_EQUAL_FLOAT(integral, s_pid_integral);
    TEST_ASSERT_FALSE(motor_is_inhibited());
    mock_timer_now_us = 100000;
    motor_tick();
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT8(0, s_target_speed);
}

static void test_feedback_alternating_rejections_never_jump_to_base(void)
{
    feedback_reduced_drive();
    for (unsigned i = 0; i < 20U; ++i) {
        uint32_t before = mock_ledc_duty[LEDC_CHANNEL_0];
        mock_timer_now_us += 50000;
        motor_set_rail_voltage_mv(1000);
        mock_adc_raw[PIN_BEMF1 - 1] = 4095;
        motor_tick();
        TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] <= before);
        TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] < speed_duty(63));
        mock_timer_now_us += 10000;
        motor_set_rail_voltage_mv(1000);
        mock_adc_raw[PIN_BEMF1 - 1] = 1057;
        motor_tick();
        TEST_ASSERT_TRUE(s_last_pid_ok);
        TEST_ASSERT_FALSE(motor_is_inhibited());
    }
}

static void test_feedback_invalid_startup_and_repeat_commands_latch(void)
{
    g_cv[65] = 255;
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 4095;
    for (unsigned i = 0; i <= 10U; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(80, true));
        mock_timer_now_us = i * 10000;
        motor_set_rail_voltage_mv(1000);
        motor_tick();
        TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    }
    TEST_ASSERT_TRUE(motor_is_inhibited());
    for (unsigned i = 0; i < 20U; ++i) {
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(80, true));
        motor_tick();
        TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    }
    TEST_ASSERT_EQUAL_UINT8(0, s_target_speed);
}

static void test_feedback_zero_rail_and_unavailable_adc_never_kick(void)
{
    g_cv[65] = 255;
    motor_set_speed(80, true);
    motor_tick(); /* Zero rail is not usable feedback. */
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    motor_set_rail_voltage_mv(1000);
    s_bemf_adc_ready = false;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    motor_set_bemf_enabled(false);
    motor_tick();
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
}

static void test_feedback_stop_and_disable_clear_only_motor_fault(void)
{
    feedback_reduced_drive();
    mock_adc_ok = 0;
    mock_timer_now_us = 100000;
    motor_tick();
    TEST_ASSERT_TRUE(s_feedback_fault);
    motor_set_inhibit_reason(MOTOR_INHIBIT_CONTROL, true);
    motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
    motor_set_inhibit_reason(MOTOR_INHIBIT_DCC_TIMEOUT, true);
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(0, true));
    TEST_ASSERT_FALSE(s_feedback_fault);
    TEST_ASSERT_EQUAL_UINT32(7, s_inhibit_reasons);
    TEST_ASSERT_TRUE(motor_is_inhibited());
    s_feedback_fault = true;
    motor_set_bemf_enabled(true);
    TEST_ASSERT_TRUE(s_feedback_fault);
    motor_emergency_stop();
    TEST_ASSERT_TRUE(s_feedback_fault);
    motor_set_bemf_enabled(false);
    TEST_ASSERT_FALSE(s_feedback_fault);
    TEST_ASSERT_EQUAL_UINT32(7, s_inhibit_reasons);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(30, true));
}

static void test_feedback_hold_obeys_slowdown_and_direction(void)
{
    feedback_reduced_drive();
    mock_adc_ok = 0;
    g_cv[4] = 255; /* Hold must respect the lower command, not only slow ramp. */
    motor_set_speed(10, true);
    motor_tick();
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] <= speed_duty(10));
    uint32_t lowered = mock_ledc_duty[LEDC_CHANNEL_0];
    motor_set_speed(100, true);
    motor_tick();
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] <= lowered);
    motor_set_speed(100, false);
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_feedback_stale_rail_does_not_renew_deadline(void)
{
    feedback_reduced_drive();
    mock_timer_now_us = 100000;
    motor_tick(); /* Valid ADC pair, but rail publication expired. */
    TEST_ASSERT_FALSE(s_last_pid_ok);
    TEST_ASSERT_TRUE(s_feedback_fault);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void cancel_before_save_admission(void)
{
    motor_stop();
    TEST_ASSERT_TRUE(s_cal_active);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
}

static void test_cal_save_cancel_before_admission_preserves_curve(void)
{
    g_delay_hook = refresh_calibration_rail;
    g_cal = BEMF_CAL_BASE;
    g_cal_valid = true;
    settings_bemf_cal_t before = g_cal;
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 661;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    g_save_admission_hook = cancel_before_save_admission;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_cal, sizeof(before));
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_TRUE(s_cal_cancelled); /* Reached the injected save admission. */
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_cal_missing_adc_aborts_before_first_drive(void)
{
    motor_set_rail_voltage_mv(1000);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    mock_adc_ok = 0;
    mock_ledc_update_count = 0;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_FALSE(g_cal_valid);
    TEST_ASSERT_EQUAL_UINT8(0, s_cal_step);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, g_now_ticks); /* No 400 ms blind settle run. */
}

static void test_feedback_fault_requires_stop_or_explicit_disable_to_rearm(void)
{
    feedback_reduced_drive();
    mock_adc_ok = 0;
    mock_timer_now_us = 100000;
    motor_tick();
    TEST_ASSERT_TRUE(s_feedback_fault);
    motor_set_inhibited(false);
    motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, false);
    motor_set_inhibit_reason(MOTOR_INHIBIT_DCC_TIMEOUT, false);
    TEST_ASSERT_TRUE(s_feedback_fault);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(63, true));
    motor_stop();
    TEST_ASSERT_FALSE(motor_is_inhibited());
    motor_tick();
    mock_adc_ok = 1;
    motor_set_rail_voltage_mv(1000);
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(63, true));
    motor_tick();
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
    mock_adc_ok = 0;
    mock_timer_now_us += 100000;
    motor_tick();
    TEST_ASSERT_TRUE(s_feedback_fault);
    motor_set_bemf_enabled(false);
    TEST_ASSERT_FALSE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(63, false));
    motor_tick();
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(speed_duty(63), mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void test_feedback_split_adc_pair_holds_without_renewal(void)
{
    feedback_reduced_drive();
    uint32_t before = mock_ledc_duty[LEDC_CHANNEL_0];
    g_adc_hook = fail_second_adc;
    mock_timer_now_us = 50000;
    motor_set_rail_voltage_mv(1000);
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(before, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_INT64(0, s_feedback_since_us);
    mock_timer_now_us = 100000;
    motor_set_rail_voltage_mv(1000);
    motor_tick();
    TEST_ASSERT_TRUE(s_feedback_fault);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

/* A BEMF sense stuck at an exact zero pair is indistinguishable from a
 * standstill reading until the PID has driven the duty to saturation. That
 * sustained condition must latch the feedback fault instead of winding up. */
static void test_feedback_stuck_zero_sense_latches_at_saturated_drive(void)
{
    g_cv[3] = 0;   /* instant ramp */
    g_cv[5] = 255; /* Vhigh: duty_base near the top -> PID saturates */
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 0;
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    s_was_stopped = false;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(126, true));

    mock_timer_now_us = 0;
    motor_set_rail_voltage_mv(1000);
    motor_tick();
    TEST_ASSERT_TRUE(s_last_pid_ok); /* the zero pair is still treated as valid */
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] >= (LEDC_MAX * 9U / 10U));
    TEST_ASSERT_FALSE(s_feedback_fault); /* but only within the fault window */

    mock_timer_now_us = 100000; /* sense stays zero while the duty stays saturated */
    motor_set_rail_voltage_mv(1000);
    motor_tick();
    TEST_ASSERT_TRUE(s_feedback_fault);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT8(0, s_target_speed);
    TEST_ASSERT_TRUE(motor_is_inhibited());
}

static void test_cal_zero_measurements_abort_at_first_step(void)
{
    g_delay_hook = refresh_calibration_rail;
    motor_set_rail_voltage_mv(1000);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_FALSE(g_cal_valid);
    TEST_ASSERT_EQUAL_UINT8(0, s_cal_step);
    TEST_ASSERT_EQUAL_UINT32(BEMF_CAL_SETTLE_MS, g_now_ticks);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_cal_stale_rail_aborts_first_step_without_full_run(void)
{
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 661;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_FALSE(g_cal_valid);
    TEST_ASSERT_EQUAL_UINT8(0, s_cal_step);
    TEST_ASSERT_EQUAL_UINT32(120, g_now_ticks); /* Worker-only fallback; independent guard expires at 100 ms. */
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void repeat_command_during_adc(int channel)
{
    (void)channel;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(50, true));
}

static void test_repeated_dcc_command_does_not_cancel_sampling(void)
{
    motor_set_rail_voltage_mv(1000);
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(50, true));
    uint32_t generation = s_output_generation;
    g_adc_hook = repeat_command_during_adc;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT32(generation, s_output_generation);
    TEST_ASSERT_TRUE(s_bemf_valid);
    TEST_ASSERT_TRUE(s_last_pid_ok);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
}

static void test_normal_stop_keeps_feedback_history_for_cv4_ramp(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(50, true));
    s_bemf_filtered = 250.0f;
    s_feedback_reset_requested = false;
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(0, true));
    uint32_t generation = s_output_generation;
    TEST_ASSERT_FALSE(s_feedback_reset_requested);
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_speed(0, true));
    TEST_ASSERT_EQUAL_UINT32(generation, s_output_generation);
    TEST_ASSERT_FALSE(s_feedback_reset_requested);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, s_bemf_filtered);
}

static void test_zero_bemf_calibration_preserves_working_curve(void)
{
    g_cal = BEMF_CAL_BASE;
    g_cal_valid = true;
    settings_bemf_cal_t before = g_cal;
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    motor_set_rail_voltage_mv(1000);
    g_delay_hook = refresh_calibration_rail;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_cal, sizeof(before));
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
}

static void start_valid_calibration(void)
{
    motor_set_rail_voltage_mv(1000);
    mock_adc_raw[PIN_BEMF1 - 1] = 661;
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_RUNNING, s_cal_result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_NONE, s_cal_reason);
}

static void assert_cal_failure(motor_bemf_cal_error_t reason)
{
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_FAILED, info.result);
    TEST_ASSERT_EQUAL(reason, info.reason);
    TEST_ASSERT_TRUE(info.run_id != 0U);
    TEST_ASSERT_TRUE(info.error_code != ESP_OK);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
}

static void suspend_worker_with_healthy_ticks(TickType_t ticks)
{
    TEST_ASSERT_EQUAL_UINT32(40, ticks);
    g_delay_hook = NULL;
    int64_t lease = s_cal_drive_deadline_us;
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
    for (int i = 0; i < 15; ++i) {
        mock_timer_now_us += 10000;
        motor_set_rail_voltage_mv(1000);
        motor_bemf_cal_watchdog(); /* Independent safety caller, before normal tick. */
        motor_tick();
        TEST_ASSERT_EQUAL_INT64(mock_timer_now_us, motor_last_tick_us());
        if (mock_timer_now_us < lease) {
            TEST_ASSERT_EQUAL_INT64(lease, s_cal_drive_deadline_us);
            TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_0] > 0U);
        } else {
            assert_cal_failure(MOTOR_BEMF_CAL_ERR_TIMEOUT);
        }
    }
    TEST_ASSERT_TRUE(s_cal_active);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_set_speed(30, true));
    uint32_t generation = s_output_generation;
    motor_bemf_cal_watchdog();
    motor_bemf_cal_cancel();
    motor_bemf_cal_cancel();
    TEST_ASSERT_EQUAL_UINT32(generation, s_output_generation);
    motor_stop();
    motor_emergency_stop();
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_TIMEOUT);
}

static void test_cal_worker_starvation_not_hidden_by_healthy_motor_heartbeat(void)
{
    start_valid_calibration();
    uint32_t generation = s_output_generation;
    g_delay_hook = suspend_worker_with_healthy_ticks;
    bemf_cal_task((void *)(uintptr_t)generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_TIMEOUT);
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_FALSE(g_cal_valid);
    TEST_ASSERT_FALSE(bemf_cal_sample_apply(generation, 800, NULL));
    TEST_ASSERT_FALSE(apply_pwm(800, true, generation));
}

static void blocked_control_guard(TickType_t ticks)
{
    (void)ticks;
    g_delay_hook = NULL;
    mock_timer_now_us = s_cal_drive_deadline_us;
    motor_set_rail_voltage_mv(1000);
    mock_sem_take_fail = 1; /* Guard must not attempt either mutex/ADC/storage. */
    uint32_t adc_delays = mock_rom_delay_us_total;
    motor_bemf_cal_watchdog();
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT32(adc_delays, mock_rom_delay_us_total);
    TEST_ASSERT_TRUE(s_cal_active);
    mock_sem_take_fail = 0;
}

static void test_cal_independent_guard_coasts_with_control_owner_blocked(void)
{
    start_valid_calibration();
    g_delay_hook = blocked_control_guard;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_TIMEOUT);
}

static void lose_rail_inside_settle(TickType_t ticks)
{
    TEST_ASSERT_EQUAL_UINT32(40, ticks);
    g_delay_hook = NULL;
    motor_set_rail_voltage_mv(0);
    motor_bemf_cal_watchdog();
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_RAIL);
}

static void test_cal_zero_rail_inside_settle_coasts_immediately(void)
{
    start_valid_calibration();
    g_delay_hook = lose_rail_inside_settle;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_RAIL);
    TEST_ASSERT_EQUAL_UINT32(40, g_now_ticks);
}

static void stale_rail_guard_inside_settle(TickType_t ticks)
{
    (void)ticks;
    g_delay_hook = NULL;
    while (mock_timer_now_us < 100000) {
        mock_timer_now_us += 10000;
        motor_bemf_cal_watchdog();
    }
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_RAIL);
}

static void test_cal_stale_rail_guard_aborts_at_freshness_deadline(void)
{
    start_valid_calibration();
    g_delay_hook = stale_rail_guard_inside_settle;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL_INT64(100000, mock_timer_now_us);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_RAIL);
}

static void fail_adc_inside_settle(TickType_t ticks)
{
    TEST_ASSERT_EQUAL_UINT32(40, ticks);
    g_delay_hook = NULL;
    motor_set_rail_voltage_mv(1000);
    mock_adc_ok = 0;
}

static void test_cal_adc_loss_inside_settle_aborts_first_slice(void)
{
    start_valid_calibration();
    g_delay_hook = fail_adc_inside_settle;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_ADC);
    TEST_ASSERT_EQUAL_UINT32(40, g_now_ticks);
}

static void saturate_adc_inside_settle(TickType_t ticks)
{
    TEST_ASSERT_EQUAL_UINT32(40, ticks);
    g_delay_hook = NULL;
    motor_set_rail_voltage_mv(3000);
    mock_adc_raw[PIN_BEMF1 - 1] = 4095;
    mock_adc_raw[PIN_BEMF2 - 1] = 1000; /* Differential alone would pass 85% rail. */
}

static void test_cal_saturated_individual_adc_inside_settle_aborts(void)
{
    start_valid_calibration();
    g_delay_hook = saturate_adc_inside_settle;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_ADC);
    TEST_ASSERT_EQUAL_UINT32(40, g_now_ticks);
}

static void test_feedback_rejects_each_saturated_or_negative_node_accepts_low_zero(void)
{
    mock_adc_raw[PIN_BEMF1 - 1] = 4095;
    mock_adc_raw[PIN_BEMF2 - 1] = 1000;
    TEST_ASSERT_FALSE(bemf_sample_window());
    mock_adc_raw[PIN_BEMF1 - 1] = 1000;
    mock_adc_raw[PIN_BEMF2 - 1] = 4095;
    TEST_ASSERT_FALSE(bemf_sample_window());
    mock_adc_raw[PIN_BEMF2 - 1] = -1;
    TEST_ASSERT_FALSE(bemf_sample_window());
    mock_adc_raw[PIN_BEMF1 - 1] = -1;
    mock_adc_raw[PIN_BEMF2 - 1] = 1000;
    TEST_ASSERT_FALSE(bemf_sample_window());
    mock_adc_raw[PIN_BEMF1 - 1] = 0;
    TEST_ASSERT_TRUE(bemf_sample_window());
    TEST_ASSERT_EQUAL_UINT32(0, s_bemf1_mv);
    mock_adc_raw[PIN_BEMF1 - 1] = 1000;
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    TEST_ASSERT_TRUE(bemf_sample_window());
}

static uint32_t g_cal_slices;
static void successful_guarded_slice(TickType_t ticks)
{
    TEST_ASSERT_EQUAL_UINT32(40, ticks);
    ++g_cal_slices;
    motor_set_rail_voltage_mv(1000);
    motor_bemf_cal_watchdog();
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_RUNNING, s_cal_result);
    motor_tick();
}

static void test_full_cal_worker_succeeds_all_steps_under_independent_guard(void)
{
    start_valid_calibration();
    g_cal_slices = 0;
    g_delay_hook = successful_guarded_slice;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL_UINT32(10U * (10U + 8U), g_cal_slices);
    TEST_ASSERT_EQUAL_UINT32(7200, g_now_ticks);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SUCCEEDED, s_cal_result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_NONE, s_cal_reason);
    TEST_ASSERT_EQUAL_UINT8(10, s_cal_step);
    TEST_ASSERT_EQUAL_UINT8(10, s_cal_count);
    TEST_ASSERT_TRUE(g_cal_valid);
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_FALSE(s_cal_drive_active);
    TEST_ASSERT_EQUAL_INT64(0, s_cal_drive_deadline_us);
}

static void test_cal_storage_failure_reports_failure_preserves_runtime(void)
{
    g_cal = BEMF_CAL_BASE;
    g_cal_valid = true;
    settings_bemf_cal_t before = g_cal;
    uint16_t table[127];
    memcpy(table, s_cal_frac_table, sizeof(table));
    start_valid_calibration();
    g_delay_hook = refresh_calibration_rail;
    g_save_error = ESP_FAIL;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_STORAGE);
    TEST_ASSERT_EQUAL_MEMORY(table, s_cal_frac_table, sizeof(table));
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_cal, sizeof(before));
    TEST_ASSERT_FALSE(s_cal_active);
}

static void test_new_run_resets_error_progress_and_succeeds(void)
{
    start_valid_calibration();
    uint32_t first = s_cal_run_id;
    mock_adc_ok = 0;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_ADC);
    mock_adc_ok = 1;
    s_cal_run_speed[0] = 126;
    s_cal_run_frac[0] = 800;
    start_valid_calibration();
    TEST_ASSERT_EQUAL_UINT32(first + 1U, s_cal_run_id);
    TEST_ASSERT_EQUAL_UINT8(0, s_cal_step);
    TEST_ASSERT_EQUAL_UINT8(0, s_cal_run_speed[0]);
    TEST_ASSERT_EQUAL_UINT16(0, s_cal_run_frac[0]);
    g_delay_hook = refresh_calibration_rail;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SUCCEEDED, s_cal_result);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_clear());
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_IDLE, s_cal_result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_NONE, s_cal_reason);
    TEST_ASSERT_EQUAL_UINT32(first + 1U, s_cal_run_id);
}

static void explicit_cancel_inside_settle(TickType_t ticks)
{
    (void)ticks;
    g_delay_hook = NULL;
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_cancel());
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_cancel());
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, s_cal_result);
    TEST_ASSERT_TRUE(s_cal_active);
    TEST_ASSERT_EQUAL_INT64(0, s_cal_drive_deadline_us);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    mock_sem_take_fail = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
}

static void test_cal_cancel_nonblocking_reservation_and_inactive_motion(void)
{
    motor_set_bemf_enabled(false);
    motor_set_speed(30, true);
    motor_tick();
    uint32_t duty = s_last_duty;
    uint32_t generation = s_output_generation;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_cancel());
    TEST_ASSERT_EQUAL_UINT32(duty, s_last_duty);
    TEST_ASSERT_EQUAL_UINT32(generation, s_output_generation);
    motor_emergency_stop();
    motor_tick();
    start_valid_calibration();
    generation = s_output_generation;
    g_delay_hook = explicit_cancel_inside_settle;
    bemf_cal_task((void *)(uintptr_t)generation);
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, s_cal_result);
    TEST_ASSERT_FALSE(bemf_cal_sample_apply(generation, 900, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
}

static void stop_after_save_admission(void)
{
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SAVING, info.result);
    TEST_ASSERT_TRUE(info.active);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
    mock_timer_now_us += 1000000;
    motor_set_rail_voltage_mv(0);
    motor_bemf_cal_watchdog(); /* No drive expiry or rail failure during NVS. */
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SAVING, s_cal_result);
    motor_bemf_cal_cancel();
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, s_cal_result);
}

static void test_cal_save_stop_after_admission_keeps_commit_but_not_runtime_success(void)
{
    uint16_t table[127];
    memcpy(table, s_cal_frac_table, sizeof(table));
    start_valid_calibration();
    g_delay_hook = refresh_calibration_rail;
    g_save_commit_hook = stop_after_save_admission;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_TRUE(g_cal_valid); /* Documented admission: commit already authorized. */
    TEST_ASSERT_EQUAL_MEMORY(table, s_cal_frac_table, sizeof(table));
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, s_cal_result);
    TEST_ASSERT_FALSE(s_cal_active);
}

static void test_cal_creation_failure_explicit_start_result(void)
{
    motor_set_rail_voltage_mv(1000);
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, motor_bemf_cal_start());
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_START);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, s_cal_error_code);
    TEST_ASSERT_FALSE(s_cal_active);
}

static void test_cal_late_sample_cannot_restore_expired_lease_while_coasted(void)
{
    start_valid_calibration();
    uint32_t generation = s_output_generation;
    TEST_ASSERT_TRUE(bemf_cal_sample_apply(generation, 500, NULL));
    s_cal_drive_active = false;
    pwm_write_locked(0, true);
    mock_timer_now_us = s_cal_drive_deadline_us;
    motor_set_rail_voltage_mv(1000);
    motor_bemf_cal_watchdog();
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_RUNNING, s_cal_result); /* Safely off: guard skips lease. */
    TEST_ASSERT_FALSE(bemf_cal_sample_apply(generation, 500, NULL));
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_RUNNING, s_cal_result); /* Worker failure publishes after cleanup. */
    bemf_cal_task((void *)(uintptr_t)generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_TIMEOUT);
}

static void test_cal_control_mutex_failure_reports_control_and_releases_reservation(void)
{
    start_valid_calibration();
    mock_sem_take_fail = 1;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_CONTROL);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_cal_error_code);
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_EQUAL_UINT32(0, g_now_ticks);
}

static void low_curve_guarded_slice(TickType_t ticks)
{
    (void)ticks;
    motor_set_rail_voltage_mv(1000);
    motor_bemf_cal_watchdog();
}

static void test_cal_full_invalid_curve_reports_curve_preserves_old_runtime(void)
{
    start_valid_calibration();
    mock_adc_raw[PIN_BEMF1 - 1] = 20; /* Measurable, but end fraction below trust threshold. */
    g_delay_hook = low_curve_guarded_slice;
    uint16_t table[127];
    memcpy(table, s_cal_frac_table, sizeof(table));
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_CURVE);
    TEST_ASSERT_EQUAL_UINT8(10, s_cal_step);
    TEST_ASSERT_EQUAL_MEMORY(table, s_cal_frac_table, sizeof(table));
    TEST_ASSERT_FALSE(g_cal_valid);
}

static void expire_during_adc_coast(void)
{
    g_coast_hook = NULL;
    mock_timer_now_us = s_cal_drive_deadline_us;
    motor_set_rail_voltage_mv(1000);
    motor_bemf_cal_watchdog(); /* Bridge is off, so no guard failure yet. */
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_RUNNING, s_cal_result);
}

static void test_cal_lease_expiring_during_sample_never_renews_or_restores_pwm(void)
{
    start_valid_calibration();
    uint32_t generation = s_output_generation;
    TEST_ASSERT_TRUE(bemf_cal_sample_apply(generation, 600, NULL));
    g_coast_hook = expire_during_adc_coast;
    TEST_ASSERT_FALSE(bemf_cal_sample_apply(generation, 600, NULL));
    TEST_ASSERT_EQUAL_INT64(0, s_cal_drive_deadline_us);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    bemf_cal_task((void *)(uintptr_t)generation);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_TIMEOUT);
}

static void cancel_during_worker_adc(int channel)
{
    (void)channel;
    g_adc_hook = NULL;
    motor_bemf_cal_cancel();
}

static void test_cal_cancel_between_adc_and_atomic_apply_never_renews_lease(void)
{
    start_valid_calibration();
    uint32_t generation = s_output_generation;
    g_adc_hook = cancel_during_worker_adc;
    bemf_cal_task((void *)(uintptr_t)generation);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, s_cal_result);
    TEST_ASSERT_EQUAL_INT64(0, s_cal_drive_deadline_us);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_FALSE(g_cal_valid);
}

static motor_bemf_cal_error_t g_expected_cleanup_reason;
static esp_err_t g_expected_cleanup_code;
static bool g_cleanup_cancelled;

static void cancel_on_cleanup_acquire(SemaphoreHandle_t semaphore, TickType_t wait)
{
    TEST_ASSERT_TRUE(semaphore == s_control_mutex);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(MOTOR_TICK_MS), wait);
    g_sem_take_hook = NULL;
    g_cleanup_cancelled = true;
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SAVING, s_cal_result);
    TEST_ASSERT_EQUAL(g_expected_cleanup_reason, s_cal_reason);
    TEST_ASSERT_EQUAL(g_expected_cleanup_code, s_cal_error_code);
    TEST_ASSERT_TRUE(s_cal_active);
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_cancel());
    motor_stop();
    motor_emergency_stop();
    TEST_ASSERT_EQUAL(g_expected_cleanup_reason, s_cal_reason);
    TEST_ASSERT_EQUAL(g_expected_cleanup_code, s_cal_error_code);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
    mock_sem_take_fail = 0;
}

static void arm_cleanup_cancel(void)
{
    g_sem_take_hook = cancel_on_cleanup_acquire;
}

static void test_cal_save_failure_latched_before_cleanup_cancel_preserves_runtime(void)
{
    g_cal = BEMF_CAL_BASE;
    g_cal_valid = true;
    settings_bemf_cal_t before = g_cal;
    uint16_t table[127];
    memcpy(table, s_cal_frac_table, sizeof(table));
    start_valid_calibration();
    g_delay_hook = refresh_calibration_rail;
    g_expected_cleanup_reason = MOTOR_BEMF_CAL_ERR_STORAGE;
    g_expected_cleanup_code = ESP_ERR_NOT_FOUND;
    g_cleanup_cancelled = false;
    g_save_error = g_expected_cleanup_code;
    g_save_commit_hook = arm_cleanup_cancel;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_TRUE(g_cleanup_cancelled);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_STORAGE);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, s_cal_error_code);
    TEST_ASSERT_EQUAL_MEMORY(table, s_cal_frac_table, sizeof(table));
    TEST_ASSERT_EQUAL_MEMORY(&before, &g_cal, sizeof(before));
    TEST_ASSERT_FALSE(s_cal_active);
}

static void test_cal_curve_failure_latched_before_cleanup_cancel(void)
{
    start_valid_calibration();
    mock_adc_raw[PIN_BEMF1 - 1] = 20;
    g_delay_hook = refresh_calibration_rail;
    g_expected_cleanup_reason = MOTOR_BEMF_CAL_ERR_CURVE;
    g_expected_cleanup_code = ESP_ERR_INVALID_ARG;
    g_cleanup_cancelled = false;
    g_validate_hook = arm_cleanup_cancel;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    g_validate_hook = NULL;
    TEST_ASSERT_TRUE(g_cleanup_cancelled);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_CURVE);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, s_cal_error_code);
    TEST_ASSERT_FALSE(g_cal_valid);
}

static void fail_runtime_apply_acquire(SemaphoreHandle_t semaphore, TickType_t wait)
{
    (void)wait;
    TEST_ASSERT_TRUE(semaphore == s_control_mutex);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SAVING, s_cal_result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_NONE, s_cal_reason);
    mock_sem_take_fail = 1;
    g_sem_take_hook = cancel_on_cleanup_acquire;
}

static void arm_runtime_apply_failure(void)
{
    g_sem_take_hook = fail_runtime_apply_acquire;
}

static void test_cal_apply_control_failure_latched_before_cleanup_cancel(void)
{
    uint16_t table[127];
    memcpy(table, s_cal_frac_table, sizeof(table));
    start_valid_calibration();
    g_delay_hook = refresh_calibration_rail;
    g_expected_cleanup_reason = MOTOR_BEMF_CAL_ERR_CONTROL;
    g_expected_cleanup_code = ESP_ERR_TIMEOUT;
    g_cleanup_cancelled = false;
    g_save_commit_hook = arm_runtime_apply_failure;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_TRUE(g_cleanup_cancelled);
    assert_cal_failure(MOTOR_BEMF_CAL_ERR_CONTROL);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, s_cal_error_code);
    TEST_ASSERT_EQUAL_MEMORY(table, s_cal_frac_table, sizeof(table));
    TEST_ASSERT_TRUE(g_cal_valid); /* Save completed before runtime admission failed. */
}

static void test_cal_cancel_before_storage_failure_detection_keeps_cancelled(void)
{
    start_valid_calibration();
    g_delay_hook = refresh_calibration_rail;
    g_save_error = ESP_ERR_NOT_FOUND;
    g_save_commit_hook = stop_after_save_admission;
    bemf_cal_task((void *)(uintptr_t)s_output_generation);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, s_cal_result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_NONE, s_cal_reason);
    TEST_ASSERT_EQUAL(ESP_OK, s_cal_error_code);
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_FALSE(g_cal_valid);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cal_save_failure_latched_before_cleanup_cancel_preserves_runtime);
    RUN_TEST(test_cal_curve_failure_latched_before_cleanup_cancel);
    RUN_TEST(test_cal_apply_control_failure_latched_before_cleanup_cancel);
    RUN_TEST(test_cal_cancel_before_storage_failure_detection_keeps_cancelled);
    RUN_TEST(test_cal_control_mutex_failure_reports_control_and_releases_reservation);
    RUN_TEST(test_cal_full_invalid_curve_reports_curve_preserves_old_runtime);
    RUN_TEST(test_cal_lease_expiring_during_sample_never_renews_or_restores_pwm);
    RUN_TEST(test_cal_cancel_between_adc_and_atomic_apply_never_renews_lease);
    RUN_TEST(test_cal_worker_starvation_not_hidden_by_healthy_motor_heartbeat);
    RUN_TEST(test_cal_independent_guard_coasts_with_control_owner_blocked);
    RUN_TEST(test_cal_zero_rail_inside_settle_coasts_immediately);
    RUN_TEST(test_cal_stale_rail_guard_aborts_at_freshness_deadline);
    RUN_TEST(test_cal_adc_loss_inside_settle_aborts_first_slice);
    RUN_TEST(test_cal_saturated_individual_adc_inside_settle_aborts);
    RUN_TEST(test_feedback_rejects_each_saturated_or_negative_node_accepts_low_zero);
    RUN_TEST(test_full_cal_worker_succeeds_all_steps_under_independent_guard);
    RUN_TEST(test_cal_storage_failure_reports_failure_preserves_runtime);
    RUN_TEST(test_new_run_resets_error_progress_and_succeeds);
    RUN_TEST(test_cal_cancel_nonblocking_reservation_and_inactive_motion);
    RUN_TEST(test_cal_save_stop_after_admission_keeps_commit_but_not_runtime_success);
    RUN_TEST(test_cal_creation_failure_explicit_start_result);
    RUN_TEST(test_cal_late_sample_cannot_restore_expired_lease_while_coasted);
    RUN_TEST(test_repeated_dcc_command_does_not_cancel_sampling);
    RUN_TEST(test_normal_stop_keeps_feedback_history_for_cv4_ramp);
    RUN_TEST(test_zero_bemf_calibration_preserves_working_curve);
    RUN_TEST(test_feedback_loss_holds_reduced_actual_duty);
    RUN_TEST(test_feedback_alternating_rejections_never_jump_to_base);
    RUN_TEST(test_feedback_invalid_startup_and_repeat_commands_latch);
    RUN_TEST(test_feedback_zero_rail_and_unavailable_adc_never_kick);
    RUN_TEST(test_feedback_stop_and_disable_clear_only_motor_fault);
    RUN_TEST(test_feedback_hold_obeys_slowdown_and_direction);
    RUN_TEST(test_feedback_stale_rail_does_not_renew_deadline);
    RUN_TEST(test_cal_save_cancel_before_admission_preserves_curve);
    RUN_TEST(test_cal_missing_adc_aborts_before_first_drive);
    RUN_TEST(test_feedback_fault_requires_stop_or_explicit_disable_to_rearm);
    RUN_TEST(test_feedback_split_adc_pair_holds_without_renewal);
    RUN_TEST(test_feedback_stuck_zero_sense_latches_at_saturated_drive);
    RUN_TEST(test_cal_zero_measurements_abort_at_first_step);
    RUN_TEST(test_cal_stale_rail_aborts_first_step_without_full_run);
    RUN_TEST(test_speed_duty_zero);
    RUN_TEST(test_speed_duty_linear_curve);
    RUN_TEST(test_speed_duty_non_monotonic_no_wrap);
    RUN_TEST(test_speed_duty_28_point_table);
    RUN_TEST(test_bemf_cal_table_interpolation);
    RUN_TEST(test_bemf_cal_table_invalid_when_low);
    RUN_TEST(test_bemf_cal_table_rejects_non_monotonic);
    RUN_TEST(test_bemf_cal_table_needs_two_points);
    RUN_TEST(test_load_pid_maps_cv_range);
    RUN_TEST(test_motor_set_speed_clamps);
    RUN_TEST(test_motor_get_applied_speed);
    RUN_TEST(test_motor_tick_ramp_no_inertia);
    RUN_TEST(test_motor_tick_ramp_uses_cv3);
    RUN_TEST(test_motor_tick_decel_uses_cv4);
    RUN_TEST(test_motor_tick_kickstart);
    RUN_TEST(test_motor_tick_reverse_drives_other_channel);
    RUN_TEST(test_motor_tick_bemf_disabled_open_loop);
    RUN_TEST(test_motor_bemf_enable_resets_state);
    RUN_TEST(test_bemf_update_rejects_rail_clamp);
    RUN_TEST(test_bemf_update_without_adc);
    RUN_TEST(test_motor_bemf_adc_dump);
    RUN_TEST(test_motor_bemf_coast_read);
    RUN_TEST(test_motor_bemf_diag);
    RUN_TEST(test_motor_bemf_base_info);
    RUN_TEST(test_motor_bemf_cal_info_base);
    RUN_TEST(test_motor_bemf_cal_info_stored);
    RUN_TEST(test_motor_bemf_cal_clear_restores_base);
    RUN_TEST(test_speed_duty_vhigh_zero);
    RUN_TEST(test_speed_duty_table_branch);
    RUN_TEST(test_bemf_sample_window_lock_fails);
    RUN_TEST(test_bemf_cal_build_table_edges);
    RUN_TEST(test_bemf_cal_apply_preserves_all_points);
    RUN_TEST(test_motor_tick_cal_active);
    RUN_TEST(test_motor_tick_ramp_completes_and_kickstart);
    RUN_TEST(test_motor_tick_target_equals_applied_and_stop);
    RUN_TEST(test_motor_tick_pid);
    RUN_TEST(test_motor_boot_safe_and_task);
    RUN_TEST(test_motor_init_and_set_speed_errors);
    RUN_TEST(test_bemf_cal_task_runs);
    RUN_TEST(test_motor_bemf_cal_start_states);
    RUN_TEST(test_motor_bemf_cal_info_paths);
    RUN_TEST(test_motor_bemf_cal_clear_reload_active);
    RUN_TEST(test_motor_bemf_adc_dump_and_coast_read);
    RUN_TEST(test_motor_bemf_lock_null_mutex);
    RUN_TEST(test_motor_tick_pid_kick_and_integral_clamp);
    RUN_TEST(test_speed_duty_table_non_monotonic_no_wrap);
    RUN_TEST(test_motor_emergency_stop);
    RUN_TEST(test_motor_last_tick_timestamp);
    RUN_TEST(test_motor_tick_reverse_while_moving_ramps_down);
    RUN_TEST(test_motor_tick_reverse_while_moving_no_decel);
    RUN_TEST(test_motor_tick_kickstart_table_mode);
    RUN_TEST(test_motor_tick_pid_unavailable_preserves_history);
    RUN_TEST(test_persistent_inhibit_discards_commands);
    RUN_TEST(test_stop_nonblocking_and_stale_output_rejected);
    RUN_TEST(test_coast_cancel_never_restores_motion);
    RUN_TEST(test_direction_snapshot_survives_mid_tick_command);
    RUN_TEST(test_adc_split_pair_is_never_feedback);
    RUN_TEST(test_calibration_admission_and_reservation);
    RUN_TEST(test_regular_stop_cancels_calibration);
    RUN_TEST(test_calibration_raw_average_has_no_iir_bias);
    RUN_TEST(test_failed_calibration_preserves_stored_curve);
    RUN_TEST(test_calibration_rail_snapshot_no_division_race);
    RUN_TEST(test_pid_can_reduce_below_half_base_preserves_kick);
    RUN_TEST(test_bemf_ceiling_bounds_fallback_and_accepts_measured_high);
    RUN_TEST(test_calibration_export_all_sixteen_points);
    RUN_TEST(test_corrupt_curve_rejected_before_pid);
    RUN_TEST(test_feedback_reset_is_owner_applied);
    RUN_TEST(test_heartbeat_full_width);
    RUN_TEST(test_overrun_does_not_replay_control_ticks);
    RUN_TEST(test_adc_mutex_missing_fails_closed);
    RUN_TEST(test_cal_worker_can_finish_before_create_returns);
    RUN_TEST(test_adc_mutex_allocation_failure_disables_adc_features);
    RUN_TEST(test_control_mutex_allocation_failure_stops_motor);
    RUN_TEST(test_curve_publication_is_complete_snapshot);
    RUN_TEST(test_inhibit_owners_cannot_clear_other_safety_gates);
    RUN_TEST(test_adc_dump_status_distinguishes_zero_and_failures);
    return UNITY_END();
}
