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
static esp_err_t g_cv_write_error;

esp_err_t settings_cv_write(uint16_t index, uint8_t value)
{
    if (index == 0U || index > SETTINGS_CV_COUNT) return ESP_ERR_INVALID_ARG;
    if (g_cv_write_error != ESP_OK) return g_cv_write_error;
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

static uint32_t g_sweep_max_duty;

static void sweep_rail_refresh(TickType_t ticks)
{
    (void)ticks;
    motor_set_rail_voltage_mv(2500);
}

static void sweep_bemf_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        uint32_t duty = s_last_duty;
        if (duty > g_sweep_max_duty) g_sweep_max_duty = duty;
        uint32_t mv = duty > 200U ? (duty - 200U) * 2300U / 823U : 0U;
        mock_adc_raw[channel] = mv_to_raw((int)mv);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static void zero_sweep_bemf_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        if (s_last_duty > g_sweep_max_duty) g_sweep_max_duty = s_last_duty;
        mock_adc_raw[channel] = 0;
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

static void static_error_bemf_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        mock_adc_raw[channel] = mv_to_raw(100);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static unsigned g_check_reads;

static void spin_ceiling_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        ++g_check_reads;
        mock_adc_raw[channel] = g_check_reads <= 3U ? 0 : mv_to_raw(950);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static void spin_same_polarity_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        ++g_check_reads;
        mock_adc_raw[channel] = g_check_reads <= 3U ? 0 : mv_to_raw(300);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static void sweep_ceiling_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        uint32_t mv = s_last_duty > 300U ? 950U : s_last_duty * 800U / 1023U;
        mock_adc_raw[channel] = mv_to_raw((int)mv);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static void ceiling_after_two_adc(int channel)
{
    if (channel == (int)(PIN_BEMF1 - 1)) {
        uint32_t mv = s_target_speed < 50U ? s_last_duty * 800U / 1023U : 950U;
        mock_adc_raw[channel] = mv_to_raw((int)mv);
    } else if (channel == (int)(PIN_BEMF2 - 1)) {
        mock_adc_raw[channel] = 0;
    }
}

static unsigned g_delay_count;

static void rail_zero_after_first_delays(TickType_t ticks)
{
    (void)ticks;
    ++g_delay_count;
    motor_set_rail_voltage_mv(g_delay_count >= 2U ? 0U : 1000U);
}

static void adc_fail_after_first_delays(TickType_t ticks)
{
    (void)ticks;
    ++g_delay_count;
    if (g_delay_count >= 2U) mock_adc_ok = 0;
    motor_set_rail_voltage_mv(1000);
}

static void adc_fail_after_five_delays(TickType_t ticks)
{
    (void)ticks;
    ++g_delay_count;
    if (g_delay_count >= 5U) mock_adc_ok = 0;
    motor_set_rail_voltage_mv(1000);
}

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
    g_delay_count = 0;
    g_save_error = ESP_OK;
    g_cv_write_error = ESP_OK;
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

static void start_calibration_and_run(void)
{
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
}

static void test_auto_calibration_pwm_sweep(void)
{
    motor_set_rail_voltage_mv(2500);
    g_sweep_max_duty = 0;
    g_adc_hook = sweep_bemf_adc;
    g_delay_hook = sweep_rail_refresh;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = NULL;

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_FALSE(info.active);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SUCCEEDED, info.result);
    TEST_ASSERT_EQUAL_UINT8(2, info.count);
    TEST_ASSERT_EQUAL_UINT8(1, info.speed[0]);
    TEST_ASSERT_EQUAL_UINT8(126, info.speed[1]);
    TEST_ASSERT_TRUE(info.frac[1] >= 51U);
    TEST_ASSERT_TRUE(info.frac[1] > info.frac[0]);
    TEST_ASSERT_EQUAL_UINT8(25, info.profile.pwm_min_pct);
    TEST_ASSERT_EQUAL_UINT8(15, info.profile.pwm_start_pct);
    TEST_ASSERT_EQUAL_UINT8(95, info.profile.pwm_max_pct);
    TEST_ASSERT_EQUAL_UINT8(0, info.profile.ceiling_speed);
    TEST_ASSERT_TRUE(g_sweep_max_duty >= 970U);

    TEST_ASSERT_EQUAL_UINT8(2, g_cal.count);
    TEST_ASSERT_EQUAL_UINT8(1, g_cal.speed[0]);
    TEST_ASSERT_EQUAL_UINT8(126, g_cal.speed[1]);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_reload());
    motor_bemf_cal_info(&info);
    TEST_ASSERT_TRUE(info.valid);
    TEST_ASSERT_EQUAL_UINT8(2, info.count);
}

static void test_auto_sweep_without_signal_fails(void)
{
    motor_set_rail_voltage_mv(2500);
    g_sweep_max_duty = 0;
    g_adc_hook = zero_sweep_bemf_adc;
    g_delay_hook = sweep_rail_refresh;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = NULL;

    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_FAILED, info.result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_NO_RISE, info.reason);
    TEST_ASSERT_FALSE(info.stored);
    /* The sweep always runs to 95%, even with no BEMF. */
    TEST_ASSERT_TRUE(g_sweep_max_duty >= 970U);
}

static void test_zero_bemf_calibration_fails_but_keeps_old_curve(void)
{
    set_stored_curve();
    settings_bemf_cal_t before = g_cal;
    motor_set_rail_voltage_mv(2500);
    g_adc_hook = zero_sweep_bemf_adc;
    g_delay_hook = sweep_rail_refresh;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = NULL;

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
    start_calibration_and_run();
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
    motor_set_rail_voltage_mv(2500);
    g_adc_hook = flat_when_spinning_adc;
    g_delay_hook = sweep_rail_refresh;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = NULL;

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

static void test_motor_task_runs_one_iteration(void)
{
    s_motor_iter_cap = 1;
    motor_task(NULL);
    s_motor_iter_cap = 0;
    TEST_ASSERT_TRUE(motor_last_tick_us() > 0);
}

static void test_motor_stop_helper(void)
{
    tick_speed(20, true, 1);
    motor_stop();
    motor_tick();
    uint8_t applied = 255;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, applied);
}

static void test_consist_accel_decel_offsets(void)
{
    g_cv[19] = 1;
    g_cv[3] = 0;
    g_cv[4] = 0;
    g_cv[23] = 3;
    g_cv[24] = 4;
    tick_speed(30, true, 2);
    uint8_t applied = 0;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_TRUE(applied < 30U);
}

static void test_reverse_ramp_finishes_on_other_channel(void)
{
    g_cv[4] = 1;
    tick_speed(20, true, 1);
    tick_speed(20, false, 30);
    uint8_t applied = 0;
    bool forward = true;
    motor_get_applied_speed(&applied, &forward);
    TEST_ASSERT_EQUAL_UINT8(20, applied);
    TEST_ASSERT_FALSE(forward);
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_1] > 0U);
}

static void test_trim_cv66_and_zero(void)
{
    g_cv[66] = 64;
    tick_speed(50, true, 1);
    TEST_ASSERT_EQUAL_UINT32(speed_duty(50) * 64U / 128U,
                             mock_ledc_duty[LEDC_CHANNEL_0]);
    g_cv[66] = 0;
    tick_speed(50, true, 2);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_kickstart_table_base(void)
{
    g_cv[29] = 0x12U;
    g_cv[65] = 255;
    g_cv[94] = 200;
    tick_speed(50, true, 1);
    TEST_ASSERT_EQUAL_UINT32(800, mock_ledc_duty[LEDC_CHANNEL_0]);
}

static void test_coast_read(void)
{
    g_adc_hook = zero_bemf_adc;
    uint16_t b1 = 99, b2 = 99;
    motor_bemf_coast_read(&b1, &b2);
    TEST_ASSERT_EQUAL_UINT16(0, b1);
    TEST_ASSERT_EQUAL_UINT16(0, b2);
}

static void test_coast_read_busy_is_ignored(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    uint16_t b1 = 99, b2 = 99;
    motor_bemf_coast_read(&b1, &b2);
    TEST_ASSERT_EQUAL_UINT16(0, b1);
    TEST_ASSERT_EQUAL_UINT16(0, b2);
    cleanup_abandoned_calibration();
}

static void test_curve_with_zero_and_flat_fill(void)
{
    memset(&g_cal, 0, sizeof(g_cal));
    g_cal.count = 3;
    g_cal.speed[0] = 0;
    g_cal.speed[1] = 12;
    g_cal.speed[2] = 100;
    g_cal.frac[0] = 0;
    g_cal.frac[1] = 100;
    g_cal.frac[2] = 700;
    g_cal_valid = true;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_reload());
    TEST_ASSERT_TRUE(s_cal_valid);
    TEST_ASSERT_EQUAL_UINT16(700, s_cal_frac_table[126]);
}

static void test_legacy_cal_start_uses_sweep(void)
{
    g_create_hook = capture_task;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    g_create_hook = NULL;
    TEST_ASSERT_EQUAL_UINT8(19, s_cal_total);
    cleanup_abandoned_calibration();
}

static void test_cal_cancel_api(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_cancel());
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_CANCELLED, info.result);
    cleanup_abandoned_calibration();
}

static void test_cal_watchdog_timeout(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    s_cal_drive_active = true;
    s_cal_drive_deadline_us = esp_timer_get_time() - 1;
    motor_bemf_cal_watchdog();
    TEST_ASSERT_TRUE(s_cal_cancelled);
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_TIMEOUT, info.reason);
    cleanup_abandoned_calibration();
}

static void test_cal_clear_is_rejected_while_active(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_clear());
    cleanup_abandoned_calibration();
}

static void test_cal_task_create_failure(void)
{
    mock_task_create_fail_after = mock_task_create_calls;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, motor_bemf_cal_start_ex(&cfg));
    mock_task_create_fail_after = -1;
    TEST_ASSERT_FALSE(motor_bemf_cal_active());
}

static void test_cal_sample_rail_failure(void)
{
    g_adc_hook = zero_bemf_adc;
    g_delay_hook = rail_zero_after_first_delays;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = refresh_rail;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_FAILED, info.result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_RAIL, info.reason);
}

static void test_cal_rest_check_low_rail(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(0));
    g_adc_hook = zero_bemf_adc;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_RAIL, info.reason);
}

static void test_cal_rest_check_adc_failure(void)
{
    mock_adc_ok = 0;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_ADC, info.reason);
}

static void test_cal_sample_adc_failure(void)
{
    g_adc_hook = zero_bemf_adc;
    g_delay_hook = adc_fail_after_first_delays;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = refresh_rail;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_ADC, info.reason);
}

static void test_cal_ceiling_saves_partial_curve(void)
{
    motor_set_rail_voltage_mv(1000);
    g_adc_hook = sweep_ceiling_adc;
    g_delay_hook = refresh_rail;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = NULL;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SUCCEEDED, info.result);
    TEST_ASSERT_EQUAL_UINT8(2, info.count);
    TEST_ASSERT_EQUAL_UINT8(25, info.profile.pwm_max_pct);
    TEST_ASSERT_TRUE(info.profile.ceiling_speed > 0U);
}

static void test_cal_storage_failure(void)
{
    g_save_error = ESP_FAIL;
    motor_set_rail_voltage_mv(2500);
    g_adc_hook = sweep_bemf_adc;
    g_delay_hook = sweep_rail_refresh;
    g_create_hook = run_task_now;
    start_calibration_and_run();
    g_create_hook = NULL;
    g_delay_hook = NULL;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_FAILED, info.result);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_ERR_STORAGE, info.reason);
}

static void test_cal_apply_pid_writes_cv54_56(void)
{
    motor_set_rail_voltage_mv(2500);
    g_adc_hook = sweep_bemf_adc;
    g_delay_hook = sweep_rail_refresh;
    g_create_hook = run_task_now;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    cfg.apply_pid = true;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    g_delay_hook = NULL;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SUCCEEDED, info.result);
    TEST_ASSERT_TRUE(info.profile.pid_applied);
    TEST_ASSERT_TRUE(g_cv[54] != 128 || g_cv[55] != 60 || g_cv[56] != 32);
}

static void test_bemf_check_not_initialized(void)
{
    bool saved = s_init;
    s_init = false;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_BUSY, out.reason);
    s_init = saved;
}

static void test_bemf_check_control_timeout(void)
{
    mock_sem_take_fail = 1;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_CONTROL, out.reason);
    mock_sem_take_fail = 0;
}

static void test_bemf_check_low_rail_and_moving(void)
{
    motor_set_rail_voltage_mv(0);
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_RAIL, out.reason);
    motor_set_rail_voltage_mv(1000);
    tick_speed(40, true, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_BUSY, out.reason);
}

static void test_bemf_check_static_differential(void)
{
    g_adc_hook = static_error_bemf_adc;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_STATIC, out.reason);
}

static void test_bemf_check_spin_stuck_low(void)
{
    g_adc_hook = zero_bemf_adc;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_check(&out, true));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_STUCK_LOW, out.reason);
}

static void test_bemf_check_spin_ceiling(void)
{
    g_check_reads = 0;
    g_adc_hook = spin_ceiling_adc;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_check(&out, true));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_CEILING, out.reason);
}

static void test_bemf_check_spin_same_polarity(void)
{
    g_check_reads = 0;
    g_adc_hook = spin_same_polarity_adc;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_check(&out, true));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_POLARITY, out.reason);
}

static void test_bemf_check_adc_failure(void)
{
    mock_adc_ok = 0;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_ADC, out.reason);
}

static void test_motor_tick_during_calibration_does_not_drive(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    s_target_speed = 20;
    s_applied_speed = 10;
    s_kick_left = 3;
    s_kick_duty = 100;
    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(20, s_applied_speed);
    TEST_ASSERT_EQUAL_UINT8(0, s_kick_left);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    cleanup_abandoned_calibration();
}

static void test_kickstart_cancelled_by_stop(void)
{
    g_cv[65] = 255;
    tick_speed(50, true, 1);
    TEST_ASSERT_TRUE(s_kick_left > 0U);
    motor_set_speed(0, true);
    motor_tick();
    TEST_ASSERT_EQUAL_UINT8(0, s_kick_left);
}

static void test_motor_task_two_iterations(void)
{
    s_motor_iter_cap = 2;
    motor_task(NULL);
    s_motor_iter_cap = 0;
    TEST_ASSERT_TRUE(motor_last_tick_us() > 0);
}

static void test_invalid_inhibit_reason(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      motor_set_inhibit_reason((motor_inhibit_reason_t)99, true));
}

static void test_cal_info_and_diag_accept_null(void)
{
    motor_bemf_cal_info(NULL);
    motor_bemf_diag(NULL);
}

static void test_coast_read_without_adc(void)
{
    bool saved = s_bemf_adc_ready;
    s_bemf_adc_ready = false;
    uint16_t b1 = 9, b2 = 9;
    motor_bemf_coast_read(&b1, &b2);
    TEST_ASSERT_EQUAL_UINT16(0, b1);
    TEST_ASSERT_EQUAL_UINT16(0, b2);
    s_bemf_adc_ready = saved;
}

static void test_bemf_check_requires_adc(void)
{
    bool saved = s_bemf_adc_ready;
    s_bemf_adc_ready = false;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_ADC, out.reason);
    s_bemf_adc_ready = saved;
}

static void test_bemf_check_busy_during_calibration(void)
{
    g_create_hook = capture_task;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_check(&out, false));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_BUSY, out.reason);
    cleanup_abandoned_calibration();
}

static void test_bemf_check_spin_adc_failure(void)
{
    g_delay_hook = adc_fail_after_five_delays;
    g_adc_hook = zero_bemf_adc;
    motor_bemf_check_t out;
    TEST_ASSERT_EQUAL(ESP_FAIL, motor_bemf_check(&out, true));
    TEST_ASSERT_EQUAL_UINT8(MOTOR_BEMF_CHECK_ERR_ADC, out.reason);
    g_delay_hook = refresh_rail;
}

static void test_cal_apply_pid_write_failure_is_reported(void)
{
    g_cv_write_error = ESP_FAIL;
    motor_set_rail_voltage_mv(2500);
    g_adc_hook = sweep_bemf_adc;
    g_delay_hook = sweep_rail_refresh;
    g_create_hook = run_task_now;
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    cfg.apply_pid = true;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    g_create_hook = NULL;
    g_delay_hook = NULL;
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_CAL_SUCCEEDED, info.result);
    TEST_ASSERT_FALSE(info.profile.pid_applied);
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
    RUN_TEST(test_auto_calibration_pwm_sweep);
    RUN_TEST(test_auto_sweep_without_signal_fails);
    RUN_TEST(test_zero_bemf_calibration_fails_but_keeps_old_curve);
    RUN_TEST(test_calibration_rejects_coasting_motor);
    RUN_TEST(test_calibration_admission_requires_rest);
    RUN_TEST(test_stop_cancels_calibration_and_coasts);
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
    RUN_TEST(test_motor_task_runs_one_iteration);
    RUN_TEST(test_motor_stop_helper);
    RUN_TEST(test_consist_accel_decel_offsets);
    RUN_TEST(test_reverse_ramp_finishes_on_other_channel);
    RUN_TEST(test_trim_cv66_and_zero);
    RUN_TEST(test_kickstart_table_base);
    RUN_TEST(test_coast_read);
    RUN_TEST(test_coast_read_busy_is_ignored);
    RUN_TEST(test_curve_with_zero_and_flat_fill);
    RUN_TEST(test_legacy_cal_start_uses_sweep);
    RUN_TEST(test_cal_cancel_api);
    RUN_TEST(test_cal_watchdog_timeout);
    RUN_TEST(test_cal_clear_is_rejected_while_active);
    RUN_TEST(test_cal_task_create_failure);
    RUN_TEST(test_cal_sample_rail_failure);
    RUN_TEST(test_cal_rest_check_low_rail);
    RUN_TEST(test_cal_rest_check_adc_failure);
    RUN_TEST(test_cal_sample_adc_failure);
    RUN_TEST(test_cal_ceiling_saves_partial_curve);
    RUN_TEST(test_cal_storage_failure);
    RUN_TEST(test_cal_apply_pid_writes_cv54_56);
    RUN_TEST(test_bemf_check_not_initialized);
    RUN_TEST(test_bemf_check_control_timeout);
    RUN_TEST(test_bemf_check_low_rail_and_moving);
    RUN_TEST(test_bemf_check_static_differential);
    RUN_TEST(test_bemf_check_spin_stuck_low);
    RUN_TEST(test_bemf_check_spin_ceiling);
    RUN_TEST(test_bemf_check_spin_same_polarity);
    RUN_TEST(test_bemf_check_adc_failure);
    RUN_TEST(test_motor_tick_during_calibration_does_not_drive);
    RUN_TEST(test_kickstart_cancelled_by_stop);
    RUN_TEST(test_motor_task_two_iterations);
    RUN_TEST(test_invalid_inhibit_reason);
    RUN_TEST(test_cal_info_and_diag_accept_null);
    RUN_TEST(test_coast_read_without_adc);
    RUN_TEST(test_bemf_check_requires_adc);
    RUN_TEST(test_bemf_check_busy_during_calibration);
    RUN_TEST(test_bemf_check_spin_adc_failure);
    RUN_TEST(test_cal_apply_pid_write_failure_is_reported);
    return UNITY_END();
}
