#include "motor.h"

#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#ifndef LEDC_LL_GET_HW
#include "hal/ledc_ll.h"
#endif
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pinmap.h"
#include "settings.h"
#include "bemf_cal_base.h"

#pragma weak settings_bemf_window_load
#pragma weak settings_bemf_window_save

static const char *TAG = "motor";

#define LEDC_FREQ_HZ 20000
#define LEDC_MODE    LEDC_LOW_SPEED_MODE
#define LEDC_TIMER   LEDC_TIMER_0
#define LEDC_MAX     1023U
#define MOTOR_TICK_MS 10
#define KICK_TICKS   (120 / MOTOR_TICK_MS)
#define FEEDBACK_FRESH_US 100000

/* The CV2/CV5/CV6 curve is zero at step 1 when Vstart is zero. A small floor
 * on the first notches keeps the locomotive creeping instead of reporting an
 * applied speed of 1-3 while the bridge outputs almost nothing. */
#define MOTOR_CREEP_MIN_DUTY 40U

/* BEMF is an optional correction layer. The base PWM always comes from the
 * speed curve; a missing, noisy or stuck sensor only disables the correction,
 * it must never coast the motor. */
#define BEMF_SETTLE_US         1000U
#define BEMF_CEILING           0.90f
#define BEMF_MIN_DIV           64U
#define BEMF_MIN_MV            8U

/* AURA-X v1 analog front-end (see the Altium schematic / BOM):
 *   RAIL_SENSE: R10 10.2k series, R13 1.62k to GND, C11 1nF
 *   BEMF1:      R11 10.2k series, R14 1.62k to GND, C12 1nF
 *   BEMF2:      R12 10.2k series, R15 1.62k to GND, C13 1nF
 * All three dividers are identical, so a fraction of rail measured on a BEMF
 * pin is the same fraction of the motor terminal voltage. The ratio is still
 * used explicitly for terminal-voltage diagnostics and for the ADC saturation
 * guard (3.1 V pin full scale -> about 22.6 V at the motor terminals). */
#define MOTOR_BEMF_DIV_R_TOP_OHM    10200U
#define MOTOR_BEMF_DIV_R_BOTTOM_OHM 1620U
#define MOTOR_BEMF_DIV_NUM (MOTOR_BEMF_DIV_R_TOP_OHM + MOTOR_BEMF_DIV_R_BOTTOM_OHM)
#define MOTOR_BEMF_DIV_DEN MOTOR_BEMF_DIV_R_BOTTOM_OHM
#define MOTOR_ADC_FULL_SCALE_MV 3100U
#define MOTOR_ADC_MAX_RAW       4095U
/* Reject the top ~3% of the ADC range: the rail/BEMF sense is saturating and
 * the differential is no longer trustworthy. 3000 mV at the pin is ~21.9 V of
 * actual rail with the 10.2k/1.62k divider. */
#define MOTOR_BEMF_PIN_MAX_MV   3000U
#define BEMF_ACQUIRE_TICKS     3U
#define BEMF_LOSS_TICKS        20U
#define BEMF_INTEGRAL_LIMIT    5000.0f
#define BEMF_CORR_LIMIT_MIN    32.0f
#define BEMF_CORR_LIMIT_MAX    260.0f
#define BEMF_TARGET_START_FRAC 0.08f
#define BEMF_TARGET_FULL_FRAC  0.90f

#define CAL_MAX_POINTS         MOTOR_BEMF_CAL_MAX_POINTS
#define CAL_MIN_POINTS         2U
#define CAL_MIN_END_FRAC       SETTINGS_BEMF_CAL_MIN_END_FRAC
#define CAL_MIN_RISE_FRAC      32U
#define CAL_REST_DIV           32U
#define CAL_DEFAULT_DWELL_MS   300U
#define CAL_DEFAULT_SETTLE_MS  220U
#define CAL_DEFAULT_INTERVAL   40U
#define CAL_DEFAULT_SAMPLES    6U
#define CAL_MAX_SAMPLES        16U
#define CAL_MAX_DWELL_MS       5000U
#define CAL_MAX_INTERVAL_MS    80U
#define CAL_DRIVE_CHUNK_MS     20U

static bool s_init;
static bool s_pwm_ready;
static bool s_inhibited = true;
static uint32_t s_inhibit_reasons = MOTOR_INHIBIT_CONTROL;
static uint32_t s_output_generation;
static bool s_stop_requested;
static bool s_sample_active;
static bool s_bemf_check_active;
static int64_t s_last_tick_us;

static portMUX_TYPE s_output_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_control_mutex;
static SemaphoreHandle_t s_adc_mutex;

static uint8_t s_target_speed;
static bool s_target_forward;
static uint8_t s_applied_speed;
static bool s_applied_forward;
static volatile uint16_t s_applied_state;

static uint32_t s_ramp_acc;
static bool s_was_stopped = true;
static uint32_t s_kick_duty;
static uint8_t s_kick_left;
static uint32_t s_last_duty;

static uint8_t s_tick_cv[SETTINGS_CV_COUNT + 1];
static bool s_tick_cv_active;

/* Rail sense (divider mV) published by the track task. It scales the BEMF
 * target only; motion never depends on it. */
static volatile uint32_t s_rail_mv;
static int64_t s_rail_update_us;

/* Shared ADC1. */
static adc_oneshot_unit_handle_t s_adc;
static bool s_adc_ready;
static bool s_bemf_adc_ready;

/* BEMF measurement state. */
static uint32_t s_bemf1_mv;
static uint32_t s_bemf2_mv;
static uint32_t s_bemf_mag_mv;
static uint32_t s_bemf1_motor_mv;
static uint32_t s_bemf2_motor_mv;
static uint32_t s_bemf_mag_motor_mv;
static float s_bemf_filtered;
static bool s_bemf_valid;
static bool s_bemf_filter_primed;
static bool s_bemf_acquired;
static uint8_t s_bemf_valid_ticks;
static uint8_t s_bemf_loss_ticks;
static volatile bool s_bemf_reset_requested;
static bool s_open_loop_fallback = true;
static uint16_t s_bemf_settle_us = BEMF_SETTLE_US;
static bool s_bemf_enabled = true;
static esp_err_t s_bemf_sample_error;

static float s_pid_kp;
static float s_pid_ki;
static float s_pid_kd;
static float s_pid_integral;
static float s_pid_prev_error;
static int64_t s_last_pid_us;
static bool s_last_pid_ok;
static float s_last_error;
static float s_last_integral;
static float s_last_corr;
static float s_last_target;

/* BEMF calibration curve: measured back-EMF as a fraction of rail * 1024 at
 * open-loop speed steps. Closed-loop regulation is enabled only when a stored
 * curve exists; there is no "foreign" base curve silently driving the PID. */
static uint8_t s_cal_count;
static uint8_t s_cal_speed[CAL_MAX_POINTS];
static uint16_t s_cal_frac[CAL_MAX_POINTS];
static uint16_t s_cal_frac_table[127];
static bool s_cal_valid;

static TaskHandle_t s_cal_task;
static volatile bool s_cal_active;
static volatile bool s_cal_cancelled;
static volatile uint8_t s_cal_step;
static volatile uint8_t s_cal_total;
static volatile uint8_t s_cal_measured;
static motor_bemf_cal_state_t s_cal_result;
static motor_bemf_cal_error_t s_cal_reason;
static esp_err_t s_cal_error_code;
static uint32_t s_cal_run_id;
static motor_bemf_profile_t s_cal_profile;
static uint8_t s_cal_phase;
static motor_bemf_cal_config_t s_cal_cfg;
static uint8_t s_cal_plan[CAL_MAX_POINTS];
static uint8_t s_cal_plan_count;
static uint8_t s_cal_run_speed[CAL_MAX_POINTS];
static uint16_t s_cal_run_frac[CAL_MAX_POINTS];
static int64_t s_cal_drive_deadline_us;
static bool s_cal_drive_active;
static bool s_cal_config_busy;

static const uint8_t CAL_DEFAULT_PLAN[] = {
    6, 8, 12, 16, 22, 30, 40, 52, 66, 80, 94, 108, 120, 126,
};

static void applied_publish(uint8_t speed128, bool forward)
{
    s_applied_state = (uint16_t)((uint16_t)speed128 | (forward ? 0x100U : 0U));
}

static uint32_t rail_snapshot(void)
{
    portENTER_CRITICAL(&s_output_mux);
    uint32_t rail = esp_timer_get_time() - s_rail_update_us < FEEDBACK_FRESH_US
                        ? s_rail_mv
                        : 0U;
    portEXIT_CRITICAL(&s_output_mux);
    return rail;
}

static esp_err_t motor_cv_read(uint16_t index, uint8_t *out)
{
    if (s_tick_cv_active) {
        *out = s_tick_cv[index];
        return ESP_OK;
    }
    return settings_cv_read(index, out);
}

static float clampf(float x, float lo, float hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static uint32_t pin_mv_to_motor_mv(uint32_t pin_mv)
{
    return pin_mv * MOTOR_BEMF_DIV_NUM / MOTOR_BEMF_DIV_DEN;
}

static void load_pid(void)
{
    uint8_t kp = 0, ki = 0, kd = 0;
    (void)motor_cv_read(54, &kp);
    (void)motor_cv_read(55, &ki);
    (void)motor_cv_read(56, &kd);
    s_pid_kp = 0.10f + ((float)kp / 255.0f) * 1.90f;
    s_pid_ki = ((float)ki / 255.0f) * 0.20f;
    s_pid_kd = ((float)kd / 255.0f) * 0.50f;
}

static uint32_t curve_duty(uint8_t speed)
{
    if (speed == 0U) return 0U;

    uint8_t cv2 = 0, cv5 = 0, cv6 = 0, cv29 = 0;
    (void)motor_cv_read(2, &cv2);
    (void)motor_cv_read(5, &cv5);
    (void)motor_cv_read(6, &cv6);
    (void)motor_cv_read(29, &cv29);

    if ((cv29 & 0x10U) != 0U) {
        uint32_t pos = (uint32_t)speed * 28U;
        uint8_t i = (uint8_t)(pos / 128U);
        uint8_t f = (uint8_t)(pos % 128U);
        if (i >= 27U) {
            uint8_t cv94 = 0;
            (void)motor_cv_read(94, &cv94);
            return (uint32_t)cv94 * 4U;
        }
        uint8_t a = 0, b = 0;
        (void)motor_cv_read(67 + i, &a);
        (void)motor_cv_read(68 + i, &b);
        uint32_t lo = (uint32_t)a * 4U;
        uint32_t hi = (uint32_t)b * 4U;
        if (hi < lo) hi = lo;
        uint32_t d = lo + (hi - lo) * f / 128U;
        return d > LEDC_MAX ? LEDC_MAX : d;
    }

    uint32_t vs = (uint32_t)cv2 * 4U;
    uint32_t vm = (uint32_t)cv6 * 4U;
    uint32_t vh = (uint32_t)cv5 * 4U;
    if (vh == 0U) vh = LEDC_MAX;
    if (vm == 0U) vm = (vs + vh) / 2U;
    if (vm < vs) vm = vs;
    if (vh < vm) vh = vm;
    if (speed >= 126U) return vh;
    if (speed <= 63U) return vs + (vm - vs) * (speed - 1U) / 62U;
    return vm + (vh - vm) * (speed - 63U) / 63U;
}

static uint32_t speed_duty(uint8_t speed)
{
    uint32_t duty = curve_duty(speed);
    if (speed == 0U) return 0U;
    uint8_t cv29 = 0;
    (void)motor_cv_read(29, &cv29);
    /* The floor only applies to the CV2/CV5/CV6 curve. A 28-point table is an
     * explicit per-notch request and must be used verbatim. */
    if ((cv29 & 0x10U) == 0U && speed <= 5U && duty < MOTOR_CREEP_MIN_DUTY) {
        duty = MOTOR_CREEP_MIN_DUTY;
    }
    return duty > LEDC_MAX ? LEDC_MAX : duty;
}

static void IRAM_ATTR pwm_channel(ledc_channel_t channel, uint32_t duty)
{
    ledc_ll_set_duty_int_part(LEDC_LL_GET_HW(), LEDC_MODE, channel, duty);
    ledc_ll_set_idle_level(LEDC_LL_GET_HW(), LEDC_MODE, channel, 0);
    ledc_ll_set_sig_out_en(LEDC_LL_GET_HW(), LEDC_MODE, channel, duty != 0U);
    if (duty != 0U) {
        ledc_ll_set_duty_start(LEDC_LL_GET_HW(), LEDC_MODE, channel);
    }
    ledc_ll_ls_channel_update(LEDC_LL_GET_HW(), LEDC_MODE, channel);
}

static void IRAM_ATTR pwm_write_locked(uint32_t duty, bool forward)
{
    if (!s_pwm_ready) return;
    if (forward) {
        pwm_channel(LEDC_CHANNEL_1, 0);
        pwm_channel(LEDC_CHANNEL_0, duty);
    } else {
        pwm_channel(LEDC_CHANNEL_0, 0);
        pwm_channel(LEDC_CHANNEL_1, duty);
    }
}

static void bemf_reset_tracking(void)
{
    s_bemf_valid = false;
    s_bemf_filter_primed = false;
    s_bemf_acquired = false;
    s_bemf_valid_ticks = 0;
    s_bemf_loss_ticks = 0;
    s_bemf_filtered = 0.0f;
    s_pid_integral = 0.0f;
    s_pid_prev_error = 0.0f;
    s_last_pid_us = 0;
    s_last_pid_ok = false;
    s_last_error = 0.0f;
    s_last_integral = 0.0f;
    s_last_corr = 0.0f;
    s_last_target = 0.0f;
}

static void control_reset(void)
{
    s_applied_speed = 0;
    s_applied_forward = true;
    s_ramp_acc = 0;
    s_was_stopped = true;
    s_kick_left = 0;
    s_kick_duty = 0;
    bemf_reset_tracking();
}

static void IRAM_ATTR bemf_cal_stop_locked(motor_bemf_cal_error_t reason, esp_err_t code)
{
    if (!s_cal_active) return;
    if (s_cal_reason == MOTOR_BEMF_CAL_ERR_NONE) {
        s_cal_result = reason == MOTOR_BEMF_CAL_ERR_NONE ? MOTOR_BEMF_CAL_CANCELLED
                                                         : MOTOR_BEMF_CAL_FAILED;
        s_cal_reason = reason;
        s_cal_error_code = code;
    }
    ++s_output_generation;
    s_cal_cancelled = true;
    s_cal_drive_active = false;
    s_cal_drive_deadline_us = 0;
    s_stop_requested = true;
    s_target_speed = 0;
    s_target_forward = true;
    s_applied_state = 0x100U;
    s_last_duty = 0;
    pwm_write_locked(0, true);
}

void motor_bemf_cal_watchdog(void)
{
    portENTER_CRITICAL(&s_output_mux);
    if (s_cal_active && !s_cal_cancelled && s_cal_drive_active &&
        s_cal_drive_deadline_us != 0 && esp_timer_get_time() >= s_cal_drive_deadline_us) {
        bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_TIMEOUT, ESP_ERR_TIMEOUT);
    }
    portEXIT_CRITICAL(&s_output_mux);
}

esp_err_t motor_bemf_cal_cancel(void)
{
    portENTER_CRITICAL(&s_output_mux);
    if (s_cal_active && !s_cal_cancelled) {
        bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_NONE, ESP_OK);
    }
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

static bool apply_pwm(uint32_t duty, bool forward, uint32_t generation)
{
    portENTER_CRITICAL(&s_output_mux);
    bool accepted = generation == s_output_generation && !s_inhibited &&
                    !s_stop_requested && !s_sample_active && !s_cal_active &&
                    !s_bemf_check_active;
    if (accepted) {
        pwm_write_locked(duty, forward);
        s_last_duty = duty;
    }
    portEXIT_CRITICAL(&s_output_mux);
    return accepted;
}

/* Shared ADC1 (adc_oneshot). One unit serves the BEMF sample window, the rail
 * sense and diagnostics. ESP32-S3 ADC1 channels 0..9 map to GPIO1..10. */
#define ADC1_GPIO_MIN 1
#define ADC1_GPIO_MAX 10

static esp_err_t adc_unit_ensure(void)
{
    if (s_adc_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (s_adc_ready) return ESP_OK;
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&cfg, &s_adc);
    if (err != ESP_OK) return err;
    s_adc_ready = true;
    return ESP_OK;
}

esp_err_t motor_adc_config_channel(int gpio_num)
{
    if (gpio_num < ADC1_GPIO_MIN || gpio_num > ADC1_GPIO_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = adc_unit_ensure();
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    return adc_oneshot_config_channel(s_adc, (adc_channel_t)(gpio_num - 1), &chan);
}

int motor_adc_read_raw(int gpio_num)
{
    if (gpio_num < ADC1_GPIO_MIN || gpio_num > ADC1_GPIO_MAX) return -1;
    if (adc_unit_ensure() != ESP_OK) return -1;
    int raw = -1;
    if (adc_oneshot_read(s_adc, (adc_channel_t)(gpio_num - 1), &raw) != ESP_OK) {
        return -1;
    }
    return raw;
}

bool motor_bemf_lock(void)
{
    if (s_adc_mutex != NULL) {
        return xSemaphoreTake(s_adc_mutex, pdMS_TO_TICKS(10)) == pdTRUE;
    }
    return false;
}

void motor_bemf_unlock(void)
{
    if (s_adc_mutex != NULL) xSemaphoreGive(s_adc_mutex);
}

static bool bemf_sample_window(void)
{
    s_bemf_sample_error = ESP_FAIL;
    if (!s_bemf_adc_ready) return false;
    if (!motor_bemf_lock()) {
        s_bemf_sample_error = ESP_ERR_TIMEOUT;
        return false;
    }
    portENTER_CRITICAL(&s_output_mux);
    s_sample_active = true;
    pwm_write_locked(0, true);
    portEXIT_CRITICAL(&s_output_mux);

    uint32_t settle_us = s_bemf_settle_us;
    if (settle_us < SETTINGS_BEMF_WINDOW_MIN_US || settle_us > SETTINGS_BEMF_WINDOW_MAX_US) {
        settle_us = BEMF_SETTLE_US;
    }
    esp_rom_delay_us(settle_us);
    int raw1 = motor_adc_read_raw(PIN_BEMF1);
    int raw2 = motor_adc_read_raw(PIN_BEMF2);
    motor_bemf_unlock();

    portENTER_CRITICAL(&s_output_mux);
    s_sample_active = false;
    portEXIT_CRITICAL(&s_output_mux);

    if (raw1 < 0 || raw1 >= 4095 || raw2 < 0 || raw2 >= 4095) {
        return false;
    }
    s_bemf1_mv = (uint32_t)raw1 * MOTOR_ADC_FULL_SCALE_MV / MOTOR_ADC_MAX_RAW;
    s_bemf2_mv = (uint32_t)raw2 * MOTOR_ADC_FULL_SCALE_MV / MOTOR_ADC_MAX_RAW;
    s_bemf_mag_mv = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv
                                            : s_bemf2_mv - s_bemf1_mv;
    s_bemf1_motor_mv = pin_mv_to_motor_mv(s_bemf1_mv);
    s_bemf2_motor_mv = pin_mv_to_motor_mv(s_bemf2_mv);
    s_bemf_mag_motor_mv = pin_mv_to_motor_mv(s_bemf_mag_mv);
    s_bemf_sample_error = ESP_OK;
    return true;
}

static uint32_t bemf_floor_mv(uint32_t rail)
{
    uint32_t floor = rail / BEMF_MIN_DIV;
    return floor < BEMF_MIN_MV ? BEMF_MIN_MV : floor;
}

/* Only a signal above the noise floor and below the rail ceiling is usable.
 * A zero/equal differential means "no measurable back-EMF", not "valid zero
 * speed": treating it as valid used to wind the old PID up and fault-stop a
 * perfectly healthy motor. */
static void bemf_update(void)
{
    uint32_t rail = rail_snapshot();
    uint32_t mag = s_bemf_mag_mv;
    bool saturated = rail > MOTOR_BEMF_PIN_MAX_MV ||
                     s_bemf1_mv > MOTOR_BEMF_PIN_MAX_MV ||
                     s_bemf2_mv > MOTOR_BEMF_PIN_MAX_MV;
    bool plausible = !saturated && rail > 300U && mag >= bemf_floor_mv(rail) &&
                     (float)mag <= (float)rail * BEMF_CEILING;
    if (!plausible) {
        s_bemf_valid = false;
        s_bemf_valid_ticks = 0;
        if (s_bemf_acquired && ++s_bemf_loss_ticks >= BEMF_LOSS_TICKS) {
            s_bemf_acquired = false;
            s_pid_integral = 0.0f;
            s_pid_prev_error = 0.0f;
        }
        return;
    }
    if (!s_bemf_filter_primed) {
        s_bemf_filtered = (float)mag;
        s_bemf_filter_primed = true;
    } else {
        s_bemf_filtered = 0.85f * s_bemf_filtered + 0.15f * (float)mag;
    }
    s_bemf_valid = true;
    s_bemf_loss_ticks = 0;
    if (!s_bemf_acquired && ++s_bemf_valid_ticks >= BEMF_ACQUIRE_TICKS) {
        s_bemf_acquired = true;
    }
}

static bool cal_curve_rising(const settings_bemf_cal_t *cal)
{
    return cal != NULL && cal->count >= 2U &&
           cal->frac[cal->count - 1U] >= cal->frac[0] + CAL_MIN_RISE_FRAC;
}

static bool cal_table_build(const settings_bemf_cal_t *cal, uint16_t table[127])
{
    memset(table, 0, 127U * sizeof(*table));
    if (!settings_bemf_cal_validate(cal) || !cal_curve_rising(cal)) return false;
    uint8_t prev_speed = 0;
    uint16_t prev_frac = 0;
    for (uint8_t i = 0; i < cal->count; ++i) {
        uint8_t spd = cal->speed[i];
        uint16_t frac = cal->frac[i];
        if (spd == 0U) {
            table[0] = frac;
            prev_frac = frac;
            continue;
        }
        if (spd > 126U || spd <= prev_speed) continue;
        if (frac < prev_frac) frac = prev_frac;
        for (uint8_t s = prev_speed; s <= spd; ++s) {
            uint32_t f = prev_frac + (uint32_t)(frac - prev_frac) * (s - prev_speed) /
                                       (spd - prev_speed);
            table[s] = (uint16_t)f;
        }
        prev_speed = spd;
        prev_frac = frac;
    }
    for (uint8_t s = (uint8_t)(prev_speed + 1U); s <= 126U; ++s) {
        table[s] = prev_frac;
    }
    return table[126] >= CAL_MIN_END_FRAC;
}

static void cal_apply(const settings_bemf_cal_t *cal)
{
    uint16_t table[127];
    bool valid = cal_table_build(cal, table);
    portENTER_CRITICAL(&s_output_mux);
    s_cal_valid = valid;
    s_cal_count = valid ? cal->count : 0U;
    for (uint8_t i = 0; i < s_cal_count; ++i) {
        s_cal_speed[i] = cal->speed[i];
        s_cal_frac[i] = cal->frac[i];
    }
    memcpy(s_cal_frac_table, table, sizeof(table));
    s_bemf_reset_requested = true;
    portEXIT_CRITICAL(&s_output_mux);
}

static void cal_reload(void)
{
    settings_bemf_cal_t cal;
    if (settings_bemf_cal_load(&cal) == ESP_OK && settings_bemf_cal_validate(&cal)) {
        cal_apply(&cal);
        return;
    }
    settings_bemf_cal_t empty = {0};
    cal_apply(&empty);
}

static uint32_t bemf_regulate(uint32_t base, uint8_t applied, uint32_t rail, bool *closed)
{
    *closed = false;
    if (!s_bemf_enabled || !s_cal_valid || !s_bemf_acquired || !s_bemf_valid ||
        rail <= 300U || applied == 0U || base == 0U) {
        return base;
    }
    portENTER_CRITICAL(&s_output_mux);
    float frac = (float)s_cal_frac_table[applied] / 1024.0f;
    portEXIT_CRITICAL(&s_output_mux);
    if (frac <= 0.0f) return base;

    float target = frac * (float)rail;
    float error = target - s_bemf_filtered;
    int64_t now = esp_timer_get_time();
    float dt = 1.0f;
    if (s_last_pid_us > 0 && now > s_last_pid_us) {
        dt = clampf((float)(now - s_last_pid_us) / 10000.0f, 0.5f, 2.0f);
    }
    s_last_pid_us = now;
    float derivative = (error - s_pid_prev_error) / dt;
    s_pid_prev_error = error;

    float corr = s_pid_kp * error + s_pid_ki * s_pid_integral + s_pid_kd * derivative;
    float limit = (float)base * 0.30f;
    limit = clampf(limit, BEMF_CORR_LIMIT_MIN, BEMF_CORR_LIMIT_MAX);
    corr = clampf(corr * 0.5f, -limit, limit);
    s_pid_integral = clampf(s_pid_integral + error * dt,
                            -BEMF_INTEGRAL_LIMIT, BEMF_INTEGRAL_LIMIT);

    float duty = (float)base + corr;
    if (duty < 0.0f) duty = 0.0f;
    if (duty > (float)LEDC_MAX) duty = (float)LEDC_MAX;

    s_last_error = error;
    s_last_integral = s_pid_integral;
    s_last_corr = corr;
    s_last_target = target;
    *closed = true;
    return (uint32_t)duty;
}

static void motor_tick(void)
{
    portENTER_CRITICAL(&s_output_mux);
    s_last_tick_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_output_mux);
    motor_bemf_cal_watchdog();

    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return;
    }

    portENTER_CRITICAL(&s_output_mux);
    uint32_t generation = s_output_generation;
    uint8_t target = s_target_speed;
    bool target_forward = s_target_forward;
    bool calibration = s_cal_active;
    bool enabled = s_bemf_enabled;
    bool reset = s_stop_requested;
    bool reset_bemf = s_bemf_reset_requested;
    bool stopped = s_stop_requested || s_inhibited || s_cal_cancelled;
    s_stop_requested = false;
    s_bemf_reset_requested = false;
    portEXIT_CRITICAL(&s_output_mux);

    if (reset) {
        control_reset();
    } else if (reset_bemf) {
        bemf_reset_tracking();
    }
    if (stopped) {
        target = 0;
    }

    if (settings_cv_snapshot(s_tick_cv) != ESP_OK) {
        motor_emergency_stop();
        xSemaphoreGive(s_control_mutex);
        return;
    }
    s_tick_cv_active = true;

    uint8_t cv3 = 0, cv4 = 0;
    (void)motor_cv_read(3, &cv3);
    (void)motor_cv_read(4, &cv4);
    uint8_t cv19 = 0;
    if (motor_cv_read(19, &cv19) == ESP_OK && (cv19 & 0x7FU) != 0U) {
        uint8_t cv23 = 0, cv24 = 0;
        (void)motor_cv_read(23, &cv23);
        (void)motor_cv_read(24, &cv24);
        uint16_t accel = (uint16_t)cv3 + cv23;
        uint16_t decel = (uint16_t)cv4 + cv24;
        cv3 = (uint8_t)(accel > 255U ? 255U : accel);
        cv4 = (uint8_t)(decel > 255U ? 255U : decel);
    }

    uint8_t applied = s_applied_speed;
    uint8_t applied_prev = applied;
    bool forward_prev = s_applied_forward;
    bool reversing = (s_applied_forward != target_forward) && (applied > 0U);

    if (reversing) {
        if (cv4 == 0U) {
            applied = 0U;
            s_ramp_acc = 0;
        } else {
            uint32_t ticks = (uint32_t)cv4 * 100U;
            s_ramp_acc += 126U;
            while (s_ramp_acc >= ticks && applied > 0U) {
                s_ramp_acc -= ticks;
                applied--;
            }
            if (applied == 0U) s_ramp_acc = 0;
        }
    } else if (calibration) {
        applied = target;
        s_ramp_acc = 0;
    } else if (target != applied) {
        uint8_t cv = (target > applied) ? cv3 : cv4;
        if (cv == 0U) {
            applied = target;
            s_ramp_acc = 0;
        } else {
            uint32_t ticks = (uint32_t)cv * 100U;
            s_ramp_acc += 126U;
            while (s_ramp_acc >= ticks && applied != target) {
                s_ramp_acc -= ticks;
                if (target > applied) applied++; else applied--;
            }
            if (applied == target) s_ramp_acc = 0;
        }
    } else {
        s_ramp_acc = 0;
    }

    s_applied_speed = applied;
    if (!reversing || applied == 0U) {
        s_applied_forward = target_forward;
    }
    if (s_applied_forward != forward_prev) {
        bemf_reset_tracking();
    }

    if (s_kick_left > 0U &&
        (target == 0U || target < applied || target < applied_prev ||
         s_applied_forward != target_forward)) {
        s_kick_left = 0;
        s_kick_duty = 0;
    }

    uint32_t duty_base = speed_duty(applied);
    uint8_t trim = 128;
    if (motor_cv_read(s_applied_forward ? 66U : 95U, &trim) == ESP_OK && trim != 128U) {
        duty_base = (duty_base * trim) / 128U;
        if (duty_base > LEDC_MAX) duty_base = LEDC_MAX;
    }
    bool trim_zero = applied > 0U && duty_base == 0U;

    bool kicking = false;
    if (calibration) {
        s_was_stopped = false;
        s_kick_left = 0;
        s_kick_duty = 0;
    } else if (applied == 0U) {
        s_was_stopped = true;
        s_kick_left = 0;
        s_kick_duty = 0;
    } else if (s_was_stopped) {
        s_was_stopped = false;
        uint8_t cv65 = 0;
        (void)motor_cv_read(65, &cv65);
        if (cv65 > 0U) {
            uint8_t cv29 = 0;
            (void)motor_cv_read(29, &cv29);
            uint32_t base;
            if ((cv29 & 0x10U) != 0U) {
                uint8_t cv94 = 0;
                (void)motor_cv_read(94, &cv94);
                base = (uint32_t)cv94 * 4U;
                if (base == 0U) base = LEDC_MAX;
            } else {
                uint8_t cv5 = 0;
                (void)motor_cv_read(5, &cv5);
                base = (cv5 != 0U) ? (uint32_t)cv5 * 4U : LEDC_MAX;
            }
            s_kick_duty = (base * cv65 / 255U > LEDC_MAX) ? LEDC_MAX : base * cv65 / 255U;
            s_kick_left = KICK_TICKS;
        }
    }
    uint32_t duty_final = duty_base;
    if (s_kick_left > 0U && applied > 0U) {
        if (duty_final < s_kick_duty) duty_final = s_kick_duty;
        kicking = true;
        s_kick_left--;
    }

    bool sampled = false;
    if (enabled && s_cal_valid && !calibration && s_bemf_adc_ready && applied > 0U &&
        !trim_zero) {
        sampled = bemf_sample_window();
    }
    if (sampled) {
        bemf_update();
    } else {
        s_bemf_valid = false;
    }

    uint32_t rail = rail_snapshot();
    bool closed = false;
    if (!calibration && !trim_zero && applied > 0U) {
        duty_final = bemf_regulate(duty_base, applied, rail, &closed);
        if (kicking && duty_final < s_kick_duty) duty_final = s_kick_duty;
    }
    s_open_loop_fallback = !closed;
    s_last_pid_ok = closed;

    if (applied == 0U || trim_zero) {
        bemf_reset_tracking();
        duty_final = 0U;
    }

    if (!calibration) {
        (void)apply_pwm(duty_final, s_applied_forward, generation);
    }

    if (applied == 0U) {
        bemf_reset_tracking();
    }

    portENTER_CRITICAL(&s_output_mux);
    if (generation == s_output_generation && !s_inhibited && !s_stop_requested &&
        !s_cal_cancelled) {
        applied_publish(s_applied_speed, s_applied_forward);
    }
    portEXIT_CRITICAL(&s_output_mux);

    s_tick_cv_active = false;
    xSemaphoreGive(s_control_mutex);
}

static uint32_t s_motor_iter_cap;

static void motor_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(MOTOR_TICK_MS);
    uint32_t iters = 0;
    while (s_motor_iter_cap == 0U || iters < s_motor_iter_cap) {
        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last) >= period) {
            last = now;
        }
        vTaskDelayUntil(&last, period);
        motor_tick();
        iters++;
    }
}

void motor_boot_safe(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_MOTOR_IN1) | (1ULL << PIN_MOTOR_IN2),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&cfg);
    (void)gpio_set_level((gpio_num_t)PIN_MOTOR_IN1, 0);
    (void)gpio_set_level((gpio_num_t)PIN_MOTOR_IN2, 0);
}

esp_err_t motor_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch1 = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_MOTOR_IN1,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch1));

    ledc_channel_config_t ch2 = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_MOTOR_IN2,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch2));

    s_pwm_ready = true;
    s_control_mutex = xSemaphoreCreateMutex();
    if (s_control_mutex == NULL) {
        s_init = false;
        motor_emergency_stop();
        return ESP_ERR_NO_MEM;
    }
    s_adc_mutex = xSemaphoreCreateMutex();
    s_bemf_adc_ready = false;
    esp_err_t adc_err = s_adc_mutex != NULL
                            ? motor_adc_config_channel(PIN_BEMF1)
                            : ESP_ERR_NO_MEM;
    if (adc_err == ESP_OK) adc_err = motor_adc_config_channel(PIN_BEMF2);
    if (adc_err == ESP_OK) adc_err = motor_adc_config_channel(PIN_RAIL_SENSE);
    if (adc_err == ESP_OK) {
        s_bemf_adc_ready = true;
    } else {
        ESP_LOGW(TAG, "ADC init failed (%s): BEMF correction unavailable",
                 esp_err_to_name(adc_err));
    }

    load_pid();
    {
        uint16_t window = SETTINGS_BEMF_WINDOW_DEFAULT_US;
        if (settings_bemf_window_load != NULL &&
            settings_bemf_window_load(&window) == ESP_OK &&
            window >= SETTINGS_BEMF_WINDOW_MIN_US &&
            window <= SETTINGS_BEMF_WINDOW_MAX_US) {
            s_bemf_settle_us = window;
        } else {
            s_bemf_settle_us = BEMF_SETTLE_US;
        }
    }
    cal_reload();
    {
        bool use_bemf = true;
        (void)settings_bemf_use_load(&use_bemf);
        s_bemf_enabled = use_bemf;
    }

    s_target_speed = 0;
    s_target_forward = true;
    s_applied_speed = 0;
    s_applied_forward = true;
    applied_publish(0, true);
    s_ramp_acc = 0;
    s_was_stopped = true;
    s_kick_duty = 0;
    s_kick_left = 0;
    s_last_duty = 0;
    s_last_tick_us = 0;
    s_stop_requested = false;
    s_sample_active = false;
    s_bemf_check_active = false;
    s_cal_active = false;
    s_cal_cancelled = false;
    s_cal_task = NULL;
    s_cal_config_busy = false;
    s_cal_drive_active = false;
    s_cal_drive_deadline_us = 0;
    s_cal_result = MOTOR_BEMF_CAL_IDLE;
    s_cal_reason = MOTOR_BEMF_CAL_ERR_NONE;
    s_cal_error_code = ESP_OK;
    s_cal_step = 0;
    s_cal_total = 0;
    s_cal_measured = 0;
    memset(&s_cal_profile, 0, sizeof(s_cal_profile));
    bemf_reset_tracking();
    s_inhibited = true;
    s_inhibit_reasons = MOTOR_INHIBIT_CONTROL;
    ++s_output_generation;
    s_init = true;

    if (xTaskCreate(motor_task, "motor", 3072, NULL, 7, NULL) != pdPASS) {
        s_init = false;
        motor_emergency_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Motor PWM initialized (LEDC 20 kHz, BEMF correction %s)",
             s_bemf_enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    if (!s_init) return ESP_ERR_INVALID_STATE;
    if (speed128 > 126U) speed128 = 126U;

    portENTER_CRITICAL(&s_output_mux);
    if (speed128 == 0U && s_cal_active) {
        bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_NONE, ESP_OK);
        portEXIT_CRITICAL(&s_output_mux);
        motor_emergency_stop();
        return ESP_OK;
    }
    if (s_inhibited || s_cal_active || s_bemf_check_active) {
        portEXIT_CRITICAL(&s_output_mux);
        return speed128 == 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (!s_stop_requested && s_target_speed == speed128 && s_target_forward == forward) {
        portEXIT_CRITICAL(&s_output_mux);
        return ESP_OK;
    }
    ++s_output_generation;
    s_target_speed = speed128;
    s_target_forward = forward;
    s_cal_cancelled = false;
    /* A new accepted command supersedes a previous e-stop/inhibit coast. The
     * inhibit owners were checked above; clearing this flag lets the ramp
     * restart instead of being reset to zero by the next motor tick. */
    s_stop_requested = false;
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

void motor_stop(void)
{
    (void)motor_set_speed(0, true);
}

void IRAM_ATTR motor_emergency_stop(void)
{
    portENTER_CRITICAL(&s_output_mux);
    bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_NONE, ESP_OK);
    ++s_output_generation;
    s_stop_requested = true;
    s_cal_cancelled = true;
    pwm_write_locked(0, true);
    s_target_speed = 0;
    s_target_forward = true;
    s_applied_state = 0x100U;
    s_last_duty = 0;
    portEXIT_CRITICAL(&s_output_mux);
}

esp_err_t motor_set_inhibited(bool inhibited)
{
    return motor_set_inhibit_reason(MOTOR_INHIBIT_CONTROL, inhibited);
}

esp_err_t motor_set_inhibit_reason(motor_inhibit_reason_t reason, bool inhibited)
{
    if (reason != MOTOR_INHIBIT_CONTROL && reason != MOTOR_INHIBIT_SAFETY &&
        reason != MOTOR_INHIBIT_DCC_TIMEOUT) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_output_mux);
    if (inhibited) {
        s_inhibit_reasons |= (uint32_t)reason;
    } else {
        s_inhibit_reasons &= ~(uint32_t)reason;
    }
    s_inhibited = s_inhibit_reasons != 0U;
    if (inhibited) {
        bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_NONE, ESP_OK);
        ++s_output_generation;
        s_stop_requested = true;
        s_cal_cancelled = true;
        s_target_speed = 0;
        s_target_forward = true;
        s_applied_state = 0x100U;
        s_last_duty = 0;
        pwm_write_locked(0, true);
    }
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

bool motor_is_inhibited(void)
{
    portENTER_CRITICAL(&s_output_mux);
    bool inhibited = s_inhibited;
    portEXIT_CRITICAL(&s_output_mux);
    return inhibited;
}

int64_t motor_last_tick_us(void)
{
    portENTER_CRITICAL(&s_output_mux);
    int64_t timestamp = s_last_tick_us;
    portEXIT_CRITICAL(&s_output_mux);
    return timestamp;
}

void motor_get_status(uint8_t *out_speed128, bool *out_forward)
{
    portENTER_CRITICAL(&s_output_mux);
    uint8_t speed = s_target_speed;
    bool forward = s_target_forward;
    portEXIT_CRITICAL(&s_output_mux);
    if (out_speed128 != NULL) *out_speed128 = speed;
    if (out_forward != NULL) *out_forward = forward;
}

void motor_get_applied_speed(uint8_t *out_speed128, bool *out_forward)
{
    portENTER_CRITICAL(&s_output_mux);
    uint16_t st = s_applied_state;
    portEXIT_CRITICAL(&s_output_mux);
    if (out_speed128 != NULL) *out_speed128 = (uint8_t)(st & 0xFFU);
    if (out_forward != NULL) *out_forward = (st & 0x100U) != 0U;
}

esp_err_t motor_set_rail_voltage_mv(uint32_t rail_mv)
{
    portENTER_CRITICAL(&s_output_mux);
    s_rail_mv = rail_mv;
    s_rail_update_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

/* ---- calibration ------------------------------------------------------- */

static void cal_set_phase(uint32_t generation, uint8_t phase)
{
    portENTER_CRITICAL(&s_output_mux);
    if (generation == s_output_generation && !s_cal_cancelled && !s_inhibited) {
        s_cal_phase = phase;
        s_cal_profile.phase = phase;
    }
    portEXIT_CRITICAL(&s_output_mux);
}

static bool cal_job_ok(uint32_t generation)
{
    portENTER_CRITICAL(&s_output_mux);
    bool ok = generation == s_output_generation && s_cal_active &&
              !s_cal_cancelled && !s_inhibited;
    portEXIT_CRITICAL(&s_output_mux);
    return ok;
}

static bool cal_drive(uint32_t generation, uint32_t duty, bool forward)
{
    bool current = false;
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return false;
    }
    portENTER_CRITICAL(&s_output_mux);
    current = generation == s_output_generation && s_cal_active &&
              !s_cal_cancelled && !s_inhibited && !s_stop_requested;
    if (current) {
        s_cal_drive_deadline_us = esp_timer_get_time() + FEEDBACK_FRESH_US;
        s_cal_drive_active = duty != 0U;
        pwm_write_locked(duty, forward);
        s_last_duty = duty;
        applied_publish(s_target_speed, forward);
    }
    portEXIT_CRITICAL(&s_output_mux);
    xSemaphoreGive(s_control_mutex);
    return current;
}

static bool cal_drive_for(uint32_t generation, uint32_t duty, bool forward, uint32_t ms)
{
    for (uint32_t elapsed = 0; elapsed < ms; elapsed += CAL_DRIVE_CHUNK_MS) {
        if (!cal_drive(generation, duty, forward)) return false;
        vTaskDelay(pdMS_TO_TICKS(CAL_DRIVE_CHUNK_MS));
    }
    return cal_drive(generation, duty, forward);
}

static void cal_fail(uint32_t generation, motor_bemf_cal_error_t reason, esp_err_t code)
{
    portENTER_CRITICAL(&s_output_mux);
    if (generation == s_output_generation) {
        bemf_cal_stop_locked(reason, code);
    }
    portEXIT_CRITICAL(&s_output_mux);
}

static bool cal_set_target(uint32_t generation, uint8_t speed)
{
    bool ok;
    portENTER_CRITICAL(&s_output_mux);
    ok = generation == s_output_generation && s_cal_active && !s_cal_cancelled &&
         !s_inhibited;
    if (ok) {
        s_target_speed = speed;
        s_target_forward = true;
    }
    portEXIT_CRITICAL(&s_output_mux);
    return ok;
}

static esp_err_t cal_point_duty(uint8_t speed, uint32_t *duty)
{
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = settings_cv_snapshot(s_tick_cv);
    if (err == ESP_OK) {
        s_tick_cv_active = true;
        *duty = speed_duty(speed);
        s_tick_cv_active = false;
    }
    xSemaphoreGive(s_control_mutex);
    return err;
}

typedef enum {
    CAL_SAMPLE_OK = 0,
    CAL_SAMPLE_NO_SIGNAL,
    CAL_SAMPLE_CEILING,
    CAL_SAMPLE_FAIL,
} cal_sample_result_t;

static cal_sample_result_t cal_sample(uint32_t generation, uint32_t duty, bool forward,
                                      uint16_t *frac)
{
    if (!bemf_sample_window()) {
        cal_fail(generation, s_bemf_sample_error == ESP_ERR_TIMEOUT
                                  ? MOTOR_BEMF_CAL_ERR_CONTROL
                                  : MOTOR_BEMF_CAL_ERR_ADC,
                 s_bemf_sample_error);
        return CAL_SAMPLE_FAIL;
    }
    uint32_t rail = rail_snapshot();
    uint32_t mag = s_bemf_mag_mv;
    cal_sample_result_t result = CAL_SAMPLE_OK;
    bool saturated = rail > MOTOR_BEMF_PIN_MAX_MV ||
                     s_bemf1_mv > MOTOR_BEMF_PIN_MAX_MV ||
                     s_bemf2_mv > MOTOR_BEMF_PIN_MAX_MV;
    if (rail <= 300U || saturated) {
        result = CAL_SAMPLE_FAIL;
        cal_fail(generation, MOTOR_BEMF_CAL_ERR_RAIL, ESP_ERR_INVALID_STATE);
    } else if ((float)mag > (float)rail * BEMF_CEILING) {
        result = CAL_SAMPLE_CEILING;
    } else if (mag < bemf_floor_mv(rail)) {
        result = CAL_SAMPLE_NO_SIGNAL;
    } else if (frac != NULL) {
        *frac = (uint16_t)((uint32_t)mag * 1024U / rail);
    }
    if (!cal_drive(generation, duty, forward)) return CAL_SAMPLE_FAIL;
    return result;
}

static bool cal_rest_check(uint32_t generation)
{
    if (!bemf_sample_window()) {
        cal_fail(generation, s_bemf_sample_error == ESP_ERR_TIMEOUT
                                  ? MOTOR_BEMF_CAL_ERR_CONTROL
                                  : MOTOR_BEMF_CAL_ERR_ADC,
                 s_bemf_sample_error);
        return false;
    }
    uint32_t rail = rail_snapshot();
    uint32_t mag = s_bemf_mag_mv;
    bool saturated = rail > MOTOR_BEMF_PIN_MAX_MV ||
                     s_bemf1_mv > MOTOR_BEMF_PIN_MAX_MV ||
                     s_bemf2_mv > MOTOR_BEMF_PIN_MAX_MV;
    if (rail <= 300U || saturated) {
        cal_fail(generation, MOTOR_BEMF_CAL_ERR_RAIL, ESP_ERR_INVALID_STATE);
        return false;
    }
    if (mag > rail / CAL_REST_DIV) {
        ESP_LOGW(TAG, "calibration refused: motor not at rest (bemf=%lu rail=%lu mV)",
                 (unsigned long)mag, (unsigned long)rail);
        cal_fail(generation, MOTOR_BEMF_CAL_ERR_CURVE, ESP_ERR_INVALID_STATE);
        return false;
    }
    return cal_job_ok(generation);
}

static uint8_t cal_plan_build(const motor_bemf_cal_config_t *cfg, uint8_t *plan)
{
    if (cfg->mode == MOTOR_BEMF_MODE_CUSTOM && cfg->speed_count >= 2U) {
        uint8_t n = cfg->speed_count > CAL_MAX_POINTS ? CAL_MAX_POINTS : cfg->speed_count;
        for (uint8_t i = 0; i < n; ++i) {
            if (cfg->speed[i] == 0U || cfg->speed[i] > 126U ||
                (i > 0U && cfg->speed[i] <= cfg->speed[i - 1U])) {
                return 0U;
            }
            plan[i] = cfg->speed[i];
        }
        return n;
    }
    const uint8_t base_n = (uint8_t)(sizeof(CAL_DEFAULT_PLAN) / sizeof(CAL_DEFAULT_PLAN[0]));
    uint8_t want = cfg->points;
    if (want < 2U) want = base_n;
    if (want > base_n) want = base_n;
    if (want >= base_n) {
        memcpy(plan, CAL_DEFAULT_PLAN, base_n);
        return base_n;
    }
    for (uint8_t i = 0; i < want; ++i) {
        uint32_t idx = ((uint32_t)i * (base_n - 1U) + (want - 1U) / 2U) / (want - 1U);
        plan[i] = CAL_DEFAULT_PLAN[idx];
    }
    return want;
}

static void cal_profile_recommend(motor_bemf_profile_t *p, const settings_bemf_cal_t *cal)
{
    uint32_t rail = rail_snapshot();
    if (rail < 300U) rail = 0U;
    float gain_sum = 0.0f;
    uint8_t gain_n = 0;
    uint32_t prev_duty = 0;
    bool have_prev = false;
    for (uint8_t i = 0; i < cal->count; ++i) {
        uint32_t duty = speed_duty(cal->speed[i]);
        if (have_prev && duty > prev_duty && cal->frac[i] >= cal->frac[i - 1U]) {
            int32_t dbemf = ((int32_t)cal->frac[i] - (int32_t)cal->frac[i - 1U]) *
                            (int32_t)(rail ? rail : 1000U) / 1024;
            int32_t dduty = (int32_t)(duty - prev_duty);
            if (dbemf > 0 && dduty > 0) {
                gain_sum += (float)dbemf / (float)dduty;
                ++gain_n;
            }
        }
        prev_duty = duty;
        have_prev = true;
    }
    if (gain_n == 0U || gain_sum <= 0.0f) return;
    float gain = gain_sum / (float)gain_n;
    float kp = clampf(1.0f / gain, 0.20f, 1.50f);
    float ki = kp * 0.05f;
    float kd = kp * 0.10f;
    p->plant_gain_x1000 = (uint16_t)clampf(gain * 1000.0f, 0.0f, 65535.0f);
    p->kp_x100 = (uint16_t)(kp * 100.0f);
    p->ki_x1000 = (uint16_t)(ki * 1000.0f);
    p->kd_x100 = (uint16_t)(kd * 100.0f);
}

static bool cal_apply_pid(const motor_bemf_profile_t *p)
{
    float kp = (float)p->kp_x100 / 100.0f;
    float ki = (float)p->ki_x1000 / 1000.0f;
    float kd = (float)p->kd_x100 / 100.0f;
    if (kp <= 0.0f) return false;
    uint8_t cv54 = (uint8_t)clampf((kp - 0.10f) / 1.90f * 255.0f, 0.0f, 255.0f);
    uint8_t cv55 = (uint8_t)clampf(ki / 0.20f * 255.0f, 0.0f, 255.0f);
    uint8_t cv56 = (uint8_t)clampf(kd / 0.50f * 255.0f, 0.0f, 255.0f);
    if (settings_cv_write(54U, cv54) != ESP_OK ||
        settings_cv_write(55U, cv55) != ESP_OK ||
        settings_cv_write(56U, cv56) != ESP_OK ||
        settings_cv_commit() != ESP_OK) {
        ESP_LOGW(TAG, "BEMF recommended PID could not be stored");
        return false;
    }
    load_pid();
    return true;
}

static bool cal_save_authorized(void *context)
{
    uint32_t generation = *(const uint32_t *)context;
    portENTER_CRITICAL(&s_output_mux);
    bool current = generation == s_output_generation && s_cal_active &&
                   !s_cal_cancelled && !s_inhibited;
    portEXIT_CRITICAL(&s_output_mux);
    return current;
}

static void cal_finish(uint32_t generation, settings_bemf_cal_t *cal, bool complete,
                       motor_bemf_profile_t *profile, bool apply_pid)
{
    motor_bemf_profile_t prof = {0};
    if (profile != NULL) prof = *profile;

    portENTER_CRITICAL(&s_output_mux);
    pwm_write_locked(0, true);
    s_target_speed = 0;
    s_target_forward = true;
    s_applied_state = 0x100U;
    s_last_duty = 0;
    s_cal_drive_active = false;
    s_cal_drive_deadline_us = 0;
    s_stop_requested = true;
    bool current = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
    if (complete && current) {
        s_cal_result = MOTOR_BEMF_CAL_SAVING;
        prof.phase = MOTOR_BEMF_CAL_PHASE_SAVING;
        s_cal_phase = MOTOR_BEMF_CAL_PHASE_SAVING;
    }
    portEXIT_CRITICAL(&s_output_mux);

    bool succeeded = false;
    motor_bemf_cal_error_t reason = MOTOR_BEMF_CAL_ERR_NONE;
    esp_err_t code = ESP_OK;
    uint16_t table[127];
    if (complete && current) {
        if (!cal_table_build(cal, table)) {
            reason = MOTOR_BEMF_CAL_ERR_CURVE;
            code = ESP_ERR_INVALID_ARG;
        } else if ((code = settings_bemf_cal_save_guarded(cal, cal_save_authorized,
                                                          &generation)) != ESP_OK) {
            reason = MOTOR_BEMF_CAL_ERR_STORAGE;
        } else {
            portENTER_CRITICAL(&s_output_mux);
            s_cal_count = cal->count;
            for (uint8_t i = 0; i < cal->count; ++i) {
                s_cal_speed[i] = cal->speed[i];
                s_cal_frac[i] = cal->frac[i];
            }
            memcpy(s_cal_frac_table, table, sizeof(table));
            s_cal_valid = true;
            succeeded = true;
            portEXIT_CRITICAL(&s_output_mux);
        }
    }
    if (reason == MOTOR_BEMF_CAL_ERR_NONE && !complete && current && !s_cal_cancelled) {
        reason = cal->count > 0U ? MOTOR_BEMF_CAL_ERR_CURVE : MOTOR_BEMF_CAL_ERR_NO_RISE;
        code = ESP_ERR_INVALID_STATE;
    }
    if (succeeded && apply_pid && prof.kp_x100 != 0U) {
        prof.pid_applied = cal_apply_pid(&prof);
    }
    if (reason != MOTOR_BEMF_CAL_ERR_NONE) {
        cal_fail(generation, reason, code);
    }

    if (s_control_mutex != NULL &&
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) == pdTRUE) {
        control_reset();
        xSemaphoreGive(s_control_mutex);
    }

    if (succeeded) {
        prof.phase = MOTOR_BEMF_CAL_PHASE_IDLE;
        if (prof.optimal_us == 0U) prof.optimal_us = s_bemf_settle_us;
    }
    portENTER_CRITICAL(&s_output_mux);
    s_cal_profile = prof;
    s_cal_phase = prof.phase;
    if (generation == s_output_generation && !s_cal_cancelled) {
        if (reason != MOTOR_BEMF_CAL_ERR_NONE) {
            bemf_cal_stop_locked(reason, code);
        } else if (succeeded) {
            s_cal_result = MOTOR_BEMF_CAL_SUCCEEDED;
            s_cal_reason = MOTOR_BEMF_CAL_ERR_NONE;
            s_cal_error_code = ESP_OK;
        }
    }
    if (s_cal_reason != MOTOR_BEMF_CAL_ERR_NONE) s_cal_result = MOTOR_BEMF_CAL_FAILED;
    if (succeeded) s_cal_total = cal->count;
    s_cal_active = false;
    s_cal_task = NULL;
    portEXIT_CRITICAL(&s_output_mux);
    vTaskDelete(NULL);
}

static void bemf_cal_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    motor_bemf_cal_config_t cfg = s_cal_cfg;
    uint8_t plan[CAL_MAX_POINTS];
    uint8_t plan_count = s_cal_plan_count;
    memcpy(plan, s_cal_plan, plan_count);

    settings_bemf_cal_t cal = {0};
    uint32_t quality_sum = 0;
    uint8_t stalled = 0;
    uint8_t first_speed = 0;
    uint16_t first_frac = 0;
    uint16_t last_frac = 0;
    uint8_t ceiling_speed = 0;
    bool complete = true;

    cal_set_phase(generation, MOTOR_BEMF_CAL_PHASE_PREFLIGHT);
    if (!cal_rest_check(generation)) {
        complete = false;
    }

    for (uint8_t i = 0; complete && i < plan_count; ++i) {
        uint8_t spd = plan[i];
        uint32_t duty = 0;
        if (!cal_job_ok(generation)) {
            complete = false;
            break;
        }
        esp_err_t derr = cal_point_duty(spd, &duty);
        if (derr != ESP_OK) {
            cal_fail(generation, MOTOR_BEMF_CAL_ERR_CONTROL, derr);
            complete = false;
            break;
        }
        if (!cal_set_target(generation, spd)) {
            complete = false;
            break;
        }
        cal_set_phase(generation, MOTOR_BEMF_CAL_PHASE_CURVE);

        if (!cal_drive_for(generation, duty, true, cfg.dwell_ms)) {
            complete = false;
            break;
        }
        if (!cal_drive_for(generation, duty, true, cfg.settle_ms)) {
            complete = false;
            break;
        }

        uint32_t sum = 0;
        uint32_t n = 0;
        uint32_t minimum = 0;
        uint32_t maximum = 0;
        bool ceiling = false;
        for (uint8_t s = 0; s < cfg.samples; ++s) {
            vTaskDelay(pdMS_TO_TICKS(cfg.interval_ms));
            if (!cal_drive(generation, duty, true)) {
                complete = false;
                break;
            }
            uint16_t frac = 0;
            cal_sample_result_t res = cal_sample(generation, duty, true, &frac);
            if (res == CAL_SAMPLE_FAIL) {
                complete = false;
                break;
            }
            if (res == CAL_SAMPLE_CEILING) {
                ceiling = true;
                break;
            }
            if (res == CAL_SAMPLE_NO_SIGNAL) continue;
            if (n == 0U) {
                minimum = frac;
                maximum = frac;
            } else {
                if (frac < minimum) minimum = frac;
                if (frac > maximum) maximum = frac;
            }
            sum += frac;
            ++n;
        }
        if (!complete) break;
        if (ceiling) {
            ceiling_speed = spd;
            if (cal.count >= CAL_MIN_POINTS) {
                ESP_LOGW(TAG, "BEMF calibration ceiling at speed %u; saving %u points",
                         (unsigned)spd, (unsigned)cal.count);
            }
            break;
        }
        if (n == 0U) {
            ++stalled;
            portENTER_CRITICAL(&s_output_mux);
            if (generation == s_output_generation && !s_cal_cancelled) {
                s_cal_step = (uint8_t)(i + 1U);
                s_cal_profile.stalled_points = stalled;
            }
            portEXIT_CRITICAL(&s_output_mux);
            continue;
        }

        uint16_t avg = (uint16_t)(sum / n);
        if (cal.count > 0U && avg < cal.frac[cal.count - 1U]) {
            avg = cal.frac[cal.count - 1U];
        }
        uint32_t spread = maximum - minimum;
        int32_t quality = 100 - (int32_t)(spread * 100U / (avg + 1U));
        if (quality < 0) quality = 0;
        cal.speed[cal.count] = spd;
        cal.frac[cal.count] = avg;
        cal.count = (uint8_t)(cal.count + 1U);
        last_frac = avg;
        if (first_speed == 0U) {
            first_speed = spd;
            first_frac = avg;
        }
        quality_sum += (uint32_t)quality;

        portENTER_CRITICAL(&s_output_mux);
        if (generation == s_output_generation && !s_cal_cancelled) {
            s_cal_step = (uint8_t)(i + 1U);
            s_cal_measured = cal.count;
            s_cal_run_speed[cal.count - 1U] = spd;
            s_cal_run_frac[cal.count - 1U] = avg;
            s_cal_profile.measured_points = cal.count;
            s_cal_profile.stalled_points = stalled;
            s_cal_profile.first_move_speed = first_speed;
            s_cal_profile.first_frac = first_frac;
        } else {
            complete = false;
        }
        portEXIT_CRITICAL(&s_output_mux);
    }

    motor_bemf_profile_t profile = {0};
    profile.mode = (uint8_t)cfg.mode;
    profile.phase = MOTOR_BEMF_CAL_PHASE_CURVE;
    profile.planned_points = plan_count;
    profile.measured_points = cal.count;
    profile.stalled_points = stalled;
    profile.first_move_speed = first_speed;
    profile.first_frac = first_frac;
    profile.dwell_ms = cfg.dwell_ms;
    profile.optimal_us = s_bemf_settle_us;
    profile.sensor_ok = cal.count > 0U;
    profile.quality = cal.count > 0U ? (uint8_t)(quality_sum / cal.count) : 0U;
    profile.settle_ms_avg = cfg.settle_ms;
    cal_profile_recommend(&profile, &cal);

    bool save = cal.count >= CAL_MIN_POINTS && last_frac >= CAL_MIN_END_FRAC &&
                cal_curve_rising(&cal);
    if (ceiling_speed != 0U) {
        profile.ceiling_speed = ceiling_speed;
    } else if (!save && cal.count > 0U && last_frac < CAL_MIN_END_FRAC) {
        profile.ceiling_speed = cal.speed[cal.count - 1U];
    }
    if (!save) {
        complete = false;
    }
    cal_finish(generation, &cal, complete, &profile, cfg.apply_pid);
}

static esp_err_t cal_admit(const motor_bemf_cal_config_t *cfg)
{
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_output_mux);
    bool deny = !s_init || s_inhibited || s_stop_requested || s_cal_active ||
                s_cal_config_busy || s_bemf_check_active || s_cal_task != NULL ||
                !s_bemf_adc_ready || s_adc_mutex == NULL || s_target_speed != 0U ||
                s_applied_speed != 0U || (s_applied_state & 0xFFU) != 0U ||
                s_last_duty != 0U;
    if (!deny) {
        s_cal_cfg = *cfg;
        s_cal_plan_count = cal_plan_build(cfg, s_cal_plan);
        if (s_cal_plan_count < CAL_MIN_POINTS) deny = true;
    }
    if (deny) {
        portEXIT_CRITICAL(&s_output_mux);
        xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = ++s_output_generation;
    memset(&s_cal_profile, 0, sizeof(s_cal_profile));
    s_cal_profile.mode = (uint8_t)cfg->mode;
    s_cal_profile.dwell_ms = cfg->dwell_ms;
    s_cal_profile.planned_points = s_cal_plan_count;
    s_cal_active = true;
    s_cal_cancelled = false;
    s_stop_requested = false;
    s_cal_phase = MOTOR_BEMF_CAL_PHASE_IDLE;
    s_cal_step = 0;
    s_cal_total = s_cal_plan_count;
    s_cal_measured = 0;
    memset(s_cal_run_speed, 0, sizeof(s_cal_run_speed));
    memset(s_cal_run_frac, 0, sizeof(s_cal_run_frac));
    if (++s_cal_run_id == 0U) ++s_cal_run_id;
    s_cal_result = MOTOR_BEMF_CAL_RUNNING;
    s_cal_reason = MOTOR_BEMF_CAL_ERR_NONE;
    s_cal_error_code = ESP_OK;
    s_cal_drive_active = false;
    s_cal_drive_deadline_us = 0;
    portEXIT_CRITICAL(&s_output_mux);
    xSemaphoreGive(s_control_mutex);

    if (xTaskCreate(bemf_cal_task, "bemf_cal", 4096, (void *)(uintptr_t)generation,
                    6, &s_cal_task) != pdPASS) {
        portENTER_CRITICAL(&s_output_mux);
        bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_START, ESP_ERR_NO_MEM);
        s_cal_active = false;
        s_cal_task = NULL;
        portEXIT_CRITICAL(&s_output_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void motor_bemf_cal_default_config(motor_bemf_cal_config_t *cfg)
{
    if (cfg == NULL) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MOTOR_BEMF_MODE_AUTO;
    cfg->points = 12;
    cfg->settle_ms = CAL_DEFAULT_SETTLE_MS;
    cfg->settle_max_ms = CAL_DEFAULT_SETTLE_MS;
    cfg->dwell_ms = CAL_DEFAULT_DWELL_MS;
    cfg->interval_ms = CAL_DEFAULT_INTERVAL;
    cfg->samples = CAL_DEFAULT_SAMPLES;
    cfg->retries = 0;
    cfg->boost_ms = 0;
    cfg->adaptive = false;
    cfg->apply_pid = false;
}

static void cal_normalize_config(motor_bemf_cal_config_t *cfg)
{
    if (cfg->mode == MOTOR_BEMF_MODE_CUSTOM) {
        if (cfg->speed_count < 2U) {
            cfg->mode = MOTOR_BEMF_MODE_AUTO;
        } else if (cfg->points < cfg->speed_count) {
            cfg->points = cfg->speed_count;
        }
    }
    if (cfg->points < CAL_MIN_POINTS) {
        switch (cfg->mode) {
        case MOTOR_BEMF_MODE_FAST:
            cfg->points = 10U;
            break;
        case MOTOR_BEMF_MODE_BALANCED:
            cfg->points = 12U;
            break;
        case MOTOR_BEMF_MODE_THOROUGH:
            cfg->points = 14U;
            break;
        default:
            cfg->points = 12U;
            break;
        }
    }
    if (cfg->points > CAL_MAX_POINTS) cfg->points = CAL_MAX_POINTS;
    if (cfg->dwell_ms == 0U) cfg->dwell_ms = CAL_DEFAULT_DWELL_MS;
    if (cfg->dwell_ms > CAL_MAX_DWELL_MS) cfg->dwell_ms = CAL_MAX_DWELL_MS;
    if (cfg->settle_ms == 0U) cfg->settle_ms = CAL_DEFAULT_SETTLE_MS;
    if (cfg->settle_ms > 2000U) cfg->settle_ms = 2000U;
    if (cfg->interval_ms < 10U) cfg->interval_ms = CAL_DEFAULT_INTERVAL;
    if (cfg->interval_ms > CAL_MAX_INTERVAL_MS) cfg->interval_ms = CAL_MAX_INTERVAL_MS;
    if (cfg->samples < 2U) cfg->samples = CAL_DEFAULT_SAMPLES;
    if (cfg->samples > CAL_MAX_SAMPLES) cfg->samples = CAL_MAX_SAMPLES;
}

esp_err_t motor_bemf_cal_start_ex(const motor_bemf_cal_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    motor_bemf_cal_config_t next = *cfg;
    cal_normalize_config(&next);
    return cal_admit(&next);
}

esp_err_t motor_bemf_cal_start(void)
{
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
    cfg.mode = MOTOR_BEMF_MODE_FAST;
    cfg.points = 10;
    return motor_bemf_cal_start_ex(&cfg);
}

bool motor_bemf_cal_active(void)
{
    portENTER_CRITICAL(&s_output_mux);
    bool active = s_cal_active;
    portEXIT_CRITICAL(&s_output_mux);
    return active;
}

void motor_bemf_cal_info(motor_bemf_cal_info_t *info)
{
    if (info == NULL) return;
    memset(info, 0, sizeof(*info));
    portENTER_CRITICAL(&s_output_mux);
    info->active = s_cal_active;
    info->phase = s_cal_phase;
    info->step = s_cal_step;
    info->total = s_cal_total;
    info->valid = s_cal_valid;
    info->run_id = s_cal_run_id;
    info->result = s_cal_result;
    info->reason = s_cal_reason;
    info->error_code = s_cal_error_code;
    info->profile = s_cal_profile;
    if (s_cal_active) {
        info->count = s_cal_measured;
        for (uint8_t i = 0; i < info->count && i < CAL_MAX_POINTS; ++i) {
            info->speed[i] = s_cal_run_speed[i];
            info->frac[i] = s_cal_run_frac[i];
        }
    } else {
        info->count = s_cal_count;
        for (uint8_t i = 0; i < info->count && i < CAL_MAX_POINTS; ++i) {
            info->speed[i] = s_cal_speed[i];
            info->frac[i] = s_cal_frac[i];
        }
    }
    portEXIT_CRITICAL(&s_output_mux);
    if (!info->active) {
        settings_bemf_cal_t cal;
        info->stored = (settings_bemf_cal_load(&cal) == ESP_OK);
    }
}

static bool cal_config_begin(void)
{
    if (s_control_mutex == NULL) return false;
    portENTER_CRITICAL(&s_output_mux);
    bool admitted = !s_cal_active && !s_cal_config_busy;
    if (admitted) s_cal_config_busy = true;
    portEXIT_CRITICAL(&s_output_mux);
    return admitted;
}

esp_err_t motor_bemf_cal_clear(void)
{
    if (!cal_config_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = settings_bemf_cal_clear();
    if (err == ESP_OK) cal_reload();
    portENTER_CRITICAL(&s_output_mux);
    s_cal_config_busy = false;
    if (err == ESP_OK) {
        s_cal_result = MOTOR_BEMF_CAL_IDLE;
        s_cal_reason = MOTOR_BEMF_CAL_ERR_NONE;
        s_cal_error_code = ESP_OK;
        s_cal_step = 0;
        s_cal_total = 0;
        s_cal_measured = 0;
        memset(&s_cal_profile, 0, sizeof(s_cal_profile));
    }
    portEXIT_CRITICAL(&s_output_mux);
    return err;
}

esp_err_t motor_bemf_cal_reload(void)
{
    if (!cal_config_begin()) return ESP_ERR_INVALID_STATE;
    cal_reload();
    portENTER_CRITICAL(&s_output_mux);
    s_cal_config_busy = false;
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

void motor_set_bemf_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_output_mux);
    s_bemf_enabled = enabled;
    s_bemf_reset_requested = true;
    portEXIT_CRITICAL(&s_output_mux);
}

bool motor_get_bemf_enabled(void)
{
    portENTER_CRITICAL(&s_output_mux);
    bool enabled = s_bemf_enabled;
    portEXIT_CRITICAL(&s_output_mux);
    return enabled;
}

void motor_bemf_diag(motor_bemf_diag_t *d)
{
    if (d == NULL) return;
    memset(d, 0, sizeof(*d));
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_output_mux);
    d->rail_mv = s_rail_mv;
    d->bemf1_mv = s_bemf1_mv;
    d->bemf2_mv = s_bemf2_mv;
    d->bemf_filtered_mv = (uint32_t)s_bemf_filtered;
    d->rail_motor_mv = pin_mv_to_motor_mv(s_rail_mv);
    d->bemf1_motor_mv = s_bemf1_motor_mv;
    d->bemf2_motor_mv = s_bemf2_motor_mv;
    d->bemf_filtered_motor_mv = pin_mv_to_motor_mv((uint32_t)s_bemf_filtered);
    d->bemf_valid = s_bemf_valid;
    d->cal_valid = s_cal_valid;
    d->cal_active = s_cal_active;
    d->cal_step = s_cal_step;
    d->applied_speed = (uint8_t)s_applied_state;
    d->duty = s_last_duty;
    d->error = (int32_t)s_last_error;
    d->integral = (int32_t)s_last_integral;
    d->corr = (int32_t)s_last_corr;
    d->pid_ok = s_last_pid_ok;
    d->bemf_acquired = s_bemf_acquired;
    d->open_loop_fallback = s_open_loop_fallback;
    d->window_us = s_bemf_settle_us;
    d->target = (int32_t)s_last_target;
    portEXIT_CRITICAL(&s_output_mux);
    xSemaphoreGive(s_control_mutex);
}

esp_err_t motor_bemf_adc_dump_checked(uint16_t *b1_raw, uint16_t *b2_raw, uint16_t *rail_raw)
{
    if (b1_raw != NULL) *b1_raw = 0;
    if (b2_raw != NULL) *b2_raw = 0;
    if (rail_raw != NULL) *rail_raw = 0;
    if (b1_raw == NULL && b2_raw == NULL && rail_raw == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_bemf_adc_ready) return ESP_ERR_INVALID_STATE;
    if (!motor_bemf_lock()) return ESP_ERR_TIMEOUT;
    int b1 = b1_raw != NULL ? motor_adc_read_raw(PIN_BEMF1) : 0;
    int b2 = b2_raw != NULL ? motor_adc_read_raw(PIN_BEMF2) : 0;
    int rail = rail_raw != NULL ? motor_adc_read_raw(PIN_RAIL_SENSE) : 0;
    motor_bemf_unlock();
    if (b1 < 0 || b2 < 0 || rail < 0) return ESP_FAIL;
    if (b1_raw != NULL) *b1_raw = (uint16_t)b1;
    if (b2_raw != NULL) *b2_raw = (uint16_t)b2;
    if (rail_raw != NULL) *rail_raw = (uint16_t)rail;
    return ESP_OK;
}

void motor_bemf_coast_read(uint16_t *b1_mv, uint16_t *b2_mv)
{
    if (b1_mv != NULL) *b1_mv = 0;
    if (b2_mv != NULL) *b2_mv = 0;
    if (!s_bemf_adc_ready || s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_output_mux);
    uint32_t generation = s_output_generation;
    uint32_t duty = s_last_duty;
    bool forward = (s_applied_state & 0x100U) != 0U;
    bool busy = s_cal_active || s_bemf_check_active;
    portEXIT_CRITICAL(&s_output_mux);
    if (busy) {
        xSemaphoreGive(s_control_mutex);
        return;
    }
    bool fresh = bemf_sample_window();
    if (fresh && b1_mv != NULL) *b1_mv = (uint16_t)s_bemf1_mv;
    if (fresh && b2_mv != NULL) *b2_mv = (uint16_t)s_bemf2_mv;
    (void)apply_pwm(duty, forward, generation);
    xSemaphoreGive(s_control_mutex);
}

void motor_bemf_base_info(motor_bemf_base_info_t *info)
{
    if (info == NULL) return;
    memset(info, 0, sizeof(*info));
    uint8_t n = BEMF_CAL_BASE.count > MOTOR_BEMF_BASE_MAX_POINTS
                    ? MOTOR_BEMF_BASE_MAX_POINTS
                    : BEMF_CAL_BASE.count;
    info->count = n;
    for (uint8_t i = 0; i < n; ++i) {
        info->speed[i] = BEMF_CAL_BASE.speed[i];
        info->frac[i] = BEMF_CAL_BASE.frac[i];
    }
    info->start_frac = (uint16_t)(BEMF_TARGET_START_FRAC * 1024.0f);
    info->full_frac = n > 0 ? BEMF_CAL_BASE.frac[n - 1U]
                            : (uint16_t)(BEMF_TARGET_FULL_FRAC * 1024.0f);
}

/* ---- sensor check ------------------------------------------------------ */

static void bemf_check_release(void)
{
    portENTER_CRITICAL(&s_output_mux);
    pwm_write_locked(0, true);
    s_last_duty = 0;
    s_bemf_check_active = false;
    ++s_output_generation;
    s_stop_requested = true;
    s_target_speed = 0;
    s_target_forward = true;
    s_applied_state = 0x100U;
    portEXIT_CRITICAL(&s_output_mux);
}

static bool bemf_check_drive(uint32_t generation, uint32_t duty, bool forward)
{
    bool ok = false;
    portENTER_CRITICAL(&s_output_mux);
    if (generation == s_output_generation && s_bemf_check_active && !s_inhibited &&
        !s_cal_active && !s_stop_requested) {
        pwm_write_locked(duty, forward);
        s_last_duty = duty;
        ok = true;
    }
    portEXIT_CRITICAL(&s_output_mux);
    return ok;
}

esp_err_t motor_bemf_check(motor_bemf_check_t *out, bool spin)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->reason = MOTOR_BEMF_CHECK_OK;

    if (s_control_mutex == NULL || !s_init) {
        out->reason = MOTOR_BEMF_CHECK_ERR_BUSY;
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        out->reason = MOTOR_BEMF_CHECK_ERR_CONTROL;
        return ESP_ERR_TIMEOUT;
    }

    uint32_t generation = 0;
    motor_bemf_check_reason_t deny = MOTOR_BEMF_CHECK_OK;
    portENTER_CRITICAL(&s_output_mux);
    uint32_t rail = (esp_timer_get_time() - s_rail_update_us < FEEDBACK_FRESH_US)
                        ? s_rail_mv : 0U;
    bool at_rest = s_target_speed == 0U && s_applied_speed == 0U &&
                   (s_applied_state & 0xFFU) == 0U && s_last_duty == 0U;
    if (!s_bemf_adc_ready || s_adc_mutex == NULL) {
        deny = MOTOR_BEMF_CHECK_ERR_ADC;
    } else if (s_cal_active || s_bemf_check_active || s_sample_active) {
        deny = MOTOR_BEMF_CHECK_ERR_BUSY;
    } else if (s_inhibited) {
        deny = MOTOR_BEMF_CHECK_ERR_INHIBITED;
    } else if (rail <= 300U || rail > MOTOR_BEMF_PIN_MAX_MV) {
        deny = MOTOR_BEMF_CHECK_ERR_RAIL;
    } else if (!at_rest || s_stop_requested) {
        deny = MOTOR_BEMF_CHECK_ERR_BUSY;
    }
    if (deny == MOTOR_BEMF_CHECK_OK) {
        generation = ++s_output_generation;
        s_bemf_check_active = true;
        s_target_speed = 0U;
        s_target_forward = true;
        s_applied_state = 0x100U;
        s_last_duty = 0U;
    }
    portEXIT_CRITICAL(&s_output_mux);
    xSemaphoreGive(s_control_mutex);
    if (deny != MOTOR_BEMF_CHECK_OK) {
        out->reason = (uint8_t)deny;
        return ESP_ERR_INVALID_STATE;
    }

    out->rail_mv = (uint16_t)rail;
    out->optimal_us = s_bemf_settle_us;
    bool ok = true;

    uint32_t max_diff = 0;
    uint32_t max_noise = 0;
    uint32_t max_node = 0;
    for (uint8_t i = 0; i < 3U && ok; ++i) {
        if (!bemf_sample_window()) {
            out->reason = MOTOR_BEMF_CHECK_ERR_ADC;
            ok = false;
            break;
        }
        uint32_t mag = s_bemf_mag_mv;
        if (mag > max_diff) max_diff = mag;
        if (s_bemf1_mv > max_node) max_node = s_bemf1_mv;
        if (s_bemf2_mv > max_node) max_node = s_bemf2_mv;
        if (i > 0U) {
            uint32_t noise = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv
                                                    : s_bemf2_mv - s_bemf1_mv;
            if (noise > max_noise) max_noise = noise;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ok) {
        out->offset_mv = (uint16_t)((s_bemf1_mv + s_bemf2_mv) / 2U);
        out->noise_mv = (uint16_t)max_noise;
        if (max_node >= rail * 7U / 8U) {
            out->reason = MOTOR_BEMF_CHECK_ERR_STUCK_HIGH;
            ok = false;
        } else if (max_diff > rail / 16U) {
            out->reason = MOTOR_BEMF_CHECK_ERR_STATIC;
            ok = false;
        } else {
            out->static_ok = true;
        }
    }

    if (ok && spin) {
        uint32_t duty = speed_duty(40);
        if (duty < 250U) duty = 250U;
        if (duty > 450U) duty = 450U;
        int32_t fwd_diff = 0;
        int32_t rev_diff = 0;
        for (uint8_t phase = 0; phase < 2U && ok; ++phase) {
            bool forward = phase == 0U;
            for (uint32_t elapsed = 0; elapsed < 200U; elapsed += 20U) {
                if (!bemf_check_drive(generation, duty, forward)) {
                    out->reason = MOTOR_BEMF_CHECK_ERR_STOPPED;
                    ok = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (!ok) break;
            portENTER_CRITICAL(&s_output_mux);
            pwm_write_locked(0, true);
            s_last_duty = 0;
            portEXIT_CRITICAL(&s_output_mux);
            vTaskDelay(pdMS_TO_TICKS(20));
            if (!bemf_sample_window()) {
                out->reason = MOTOR_BEMF_CHECK_ERR_ADC;
                ok = false;
                break;
            }
            int32_t diff = (int32_t)s_bemf1_mv - (int32_t)s_bemf2_mv;
            uint32_t mag = s_bemf_mag_mv;
            if (mag < bemf_floor_mv(rail)) {
                out->reason = MOTOR_BEMF_CHECK_ERR_STUCK_LOW;
                ok = false;
                break;
            }
            if ((float)mag > (float)rail * BEMF_CEILING) {
                out->reason = MOTOR_BEMF_CHECK_ERR_CEILING;
                ok = false;
                break;
            }
            out->bemf_mv = (uint16_t)mag;
            if (forward) fwd_diff = diff; else rev_diff = diff;
        }
        if (ok) {
            out->spin_ok = true;
            if (fwd_diff == 0 || rev_diff == 0 ||
                ((fwd_diff > 0) == (rev_diff > 0))) {
                out->reason = MOTOR_BEMF_CHECK_ERR_POLARITY;
                ok = false;
            } else {
                out->polarity_ok = true;
            }
        }
    }

    out->duty = (uint16_t)0;
    out->ok = ok;
    if (!ok) out->optimal_us = 0;
    bemf_check_release();
    return ok ? ESP_OK : ESP_FAIL;
}
