#include "motor.h"

#include <math.h>
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

/* 20 kHz: at 8% the DRV8870 sees a 4.0 us pulse, right at its input filter;
 * 8.1% (4.05 us) just passes, which is the observed 0/on threshold. Lowering
 * to 10 kHz makes the pulses 8 us (passes the filter) but the longer off-time
 * makes the motor current discontinuous, so the usable crawl starts only at
 * ~15%. 20 kHz keeps the current continuous and is the better compromise;
 * below ~8% the driver ignores the pulses, above it the average is linear. */
#define LEDC_FREQ_HZ 20000
#define LEDC_MODE    LEDC_LOW_SPEED_MODE
#define LEDC_TIMER   LEDC_TIMER_0
#define LEDC_MAX     1023U
/* The DRV8870 ignores input pulses shorter than ~4 us. At 20 kHz that is 8 %
 * (82/1023); below it the 10 ms tick emits packets of MOTOR_PACKET_DUTY whose
 * density matches the requested average, so 5.9 % still turns the motor
 * without changing the carrier frequency. */
#define MOTOR_PACKET_DUTY 84U
#define MOTOR_TICK_MS 10
/* BEMF is sampled once every few ticks. A long PWM-off settle (up to 5 ms)
 * costs the same average duty loss as a 1 ms window every tick, but samples
 * the rotor voltage after the inductive flyback has decayed. The PID keeps
 * running every tick on the last filtered measurement. */
#define MOTOR_SAMPLE_DIV_MIN 1U
#define MOTOR_SAMPLE_DIV_MAX 32U
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
/* Correction slew per 10 ms tick: keeps PID noise from becoming audible torque
 * steps while a real load still gets its correction within a few hundred ms. */
#define BEMF_CORR_SLEW         2.0f
#define BEMF_TARGET_START_FRAC 0.09f
#define BEMF_TARGET_FULL_FRAC  0.90f

#define CAL_MAX_POINTS         MOTOR_BEMF_CAL_MAX_POINTS
#define CAL_MIN_POINTS         2U
#define CAL_MIN_END_FRAC       SETTINGS_BEMF_CAL_MIN_END_FRAC
#define CAL_MIN_RISE_FRAC      32U
#define CAL_REST_DIV           32U
#define CAL_DRIVE_CHUNK_MS     20U

/* AUTO (web "Стандартный") uses a raw PWM sweep instead of the speed ladder:
 * +5% duty every 2 s, first visible BEMF is the closed-loop threshold, 95%
 * duty is full speed. The low anchor is the measured BEMF at the first
 * movement (no synthetic backoff/clamp), so the PID can run from the moment
 * the motor starts. The stored curve is a line between those two anchors. */
#define CAL_SWEEP_START_PCT    5U
#define CAL_SWEEP_STEP_PCT     5U
#define CAL_SWEEP_MAX_PCT      95U
#define CAL_SWEEP_DWELL_MS     2000U
#define CAL_SWEEP_SAMPLE_GAP_MS 100U
#define CAL_SWEEP_SAMPLES      3U
#define CAL_SWEEP_STEPS \
    (((CAL_SWEEP_MAX_PCT - CAL_SWEEP_START_PCT) / CAL_SWEEP_STEP_PCT) + 1U)

/* Adaptive settle-window probe: drive at a mid duty, then sample the same BEMF
 * with several PWM-off delays. The smallest delay whose level is within
 * CAL_WINDOW_STABLE_PCT of the longest delay is selected and persisted; it is
 * used for the whole sweep and by the runtime PID. */
#define CAL_WINDOW_PROBE_PCT      20U
#define CAL_WINDOW_PROBE_DRIVE_MS 300U
#define CAL_WINDOW_STABLE_PCT     5U
/* The probe may wait longer than the runtime setting: at stall current the
 * inductive decay dominates a 1-2 ms window, so the true plateau is only
 * visible after ~10-20 ms. The runtime window still comes from the first four
 * candidates (<= SETTINGS_BEMF_WINDOW_MAX_US). */
#define CAL_PROBE_MAX_US          20000U
#define CAL_RUNTIME_WINDOW_COUNT  4U
/* Never let a noisy sweep cap the top speed below a usable fraction of the
 * rail (the PID simply saturates at full duty if it cannot reach it). */
#define CAL_MIN_FULL_FRAC         870U  /* 85% of rail */

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

/* Bounded raw-PWM bench mode (web /api/motor/duty). */
static bool s_test_active;
static uint16_t s_test_duty;
static bool s_test_forward;
static int64_t s_test_deadline_us;
static uint32_t s_kick_duty;
static uint8_t s_kick_left;
static uint32_t s_last_duty;
static uint32_t s_last_duty_avg;
static uint16_t s_low_packet_acc;
static bool s_low_packet_active;

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
static bool s_bemf_sample_ok;
static uint8_t s_bemf_sample_div;
static uint8_t s_bemf_diag_div;
static bool s_bemf_filter_primed;
static bool s_bemf_acquired;
static bool s_bemf_deadband;
static uint16_t s_low_speed_warmup_ticks;
static uint8_t s_bemf_valid_ticks;
static uint8_t s_bemf_loss_ticks;
static volatile bool s_bemf_reset_requested;
static bool s_open_loop_fallback = true;
static uint16_t s_bemf_settle_us = BEMF_SETTLE_US;
static uint16_t s_cal_prev_window_us = BEMF_SETTLE_US;
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
static settings_bemf_duty_t s_bemf_duty_stored;
static bool s_bemf_duty_stored_valid;
static uint16_t s_bemf_duty_table[127];
static bool s_bemf_duty_valid;
static settings_bemf_duty_t s_cal_duty_last;
static bool s_cal_duty_last_valid;

static TaskHandle_t s_cal_task;
static volatile bool s_cal_active;
static volatile bool s_cal_cancelled;
static volatile uint8_t s_cal_step;
static volatile uint8_t s_cal_total;
static motor_bemf_cal_state_t s_cal_result;
static motor_bemf_cal_error_t s_cal_reason;
static esp_err_t s_cal_error_code;
static uint32_t s_cal_run_id;
static motor_bemf_profile_t s_cal_profile;
static uint8_t s_cal_phase;
static motor_bemf_cal_config_t s_cal_cfg;
static int64_t s_cal_drive_deadline_us;
static bool s_cal_drive_active;
static motor_bemf_cal_step_log_t s_cal_log[MOTOR_BEMF_CAL_LOG_MAX];
static uint8_t s_cal_log_count;
static bool s_cal_config_busy;

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
    s_bemf_sample_div = 0;
    s_bemf_filter_primed = false;
    s_bemf_acquired = false;
    s_bemf_deadband = false;
    s_low_speed_warmup_ticks = 0;
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

/* One "window" = coast + settle + a burst of ADC pairs. The runtime uses 3
 * pairs (short coast) and its IIR averages successive windows. Calibration
 * needs a real average: the raw BEMF has a ~2 ms commutation ripple and
 * isolated ADC spikes, so it samples 41 pairs over ~8 ms and keeps the
 * median — repeatable to ~1-3 % across runs instead of 8-11 %. */
#define BEMF_OVERSAMPLE_MAX 41U
#define BEMF_CAL_PAIR_GAP_US 200U

static bool bemf_sample_window(void)
{
    s_bemf_sample_error = ESP_FAIL;
    s_bemf_sample_ok = false;
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
    uint32_t settle_max = s_cal_active ? CAL_PROBE_MAX_US : SETTINGS_BEMF_WINDOW_MAX_US;
    if (settle_us < SETTINGS_BEMF_WINDOW_MIN_US || settle_us > settle_max) {
        settle_us = BEMF_SETTLE_US;
    }
    esp_rom_delay_us(settle_us);

    int r1[BEMF_OVERSAMPLE_MAX] = { 0 };
    int r2[BEMF_OVERSAMPLE_MAX] = { 0 };
    int diff[BEMF_OVERSAMPLE_MAX] = { 0 };
    uint8_t pairs = s_cal_active ? BEMF_OVERSAMPLE_MAX : 3U;
    uint8_t n = 0;
    for (uint8_t i = 0; i < pairs; ++i) {
        int a, b;
        if ((i & 1U) != 0U) {
            b = motor_adc_read_raw(PIN_BEMF2);
            a = motor_adc_read_raw(PIN_BEMF1);
        } else {
            a = motor_adc_read_raw(PIN_BEMF1);
            b = motor_adc_read_raw(PIN_BEMF2);
        }
        if (a < 0 || a >= 4095 || b < 0 || b >= 4095) continue;
        r1[n] = a;
        r2[n] = b;
        diff[n] = a > b ? a - b : b - a;
        ++n;
        if (s_cal_active && i + 1U < pairs) {
            esp_rom_delay_us(BEMF_CAL_PAIR_GAP_US);
        }
    }
    motor_bemf_unlock();

    portENTER_CRITICAL(&s_output_mux);
    s_sample_active = false;
    portEXIT_CRITICAL(&s_output_mux);

    if (n < 3U) return false;

    /* Insertion sort the three parallel arrays by differential, then keep the
     * middle pair (median). */
    for (uint8_t i = 1; i < n; ++i) {
        int d = diff[i], a = r1[i], b = r2[i];
        int j = (int)i - 1;
        while (j >= 0 && diff[j] > d) {
            diff[j + 1] = diff[j];
            r1[j + 1] = r1[j];
            r2[j + 1] = r2[j];
            --j;
        }
        diff[j + 1] = d;
        r1[j + 1] = a;
        r2[j + 1] = b;
    }
    uint8_t mid = (uint8_t)(n / 2U);
    int raw1 = r1[mid];
    int raw2 = r2[mid];

    s_bemf1_mv = (uint32_t)raw1 * MOTOR_ADC_FULL_SCALE_MV / MOTOR_ADC_MAX_RAW;
    s_bemf2_mv = (uint32_t)raw2 * MOTOR_ADC_FULL_SCALE_MV / MOTOR_ADC_MAX_RAW;
    s_bemf_mag_mv = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv
                                            : s_bemf2_mv - s_bemf1_mv;
    s_bemf1_motor_mv = pin_mv_to_motor_mv(s_bemf1_mv);
    s_bemf2_motor_mv = pin_mv_to_motor_mv(s_bemf2_mv);
    s_bemf_mag_motor_mv = pin_mv_to_motor_mv(s_bemf_mag_mv);
    s_bemf_sample_error = ESP_OK;
    s_bemf_sample_ok = true;
    return true;
}

static uint32_t bemf_floor_mv(uint32_t rail)
{
    uint32_t floor = rail / BEMF_MIN_DIV;
    return floor < BEMF_MIN_MV ? BEMF_MIN_MV : floor;
}

/* This front-end reads well above the applied average at low duty (inductive
 * flyback/divider coupling), and that reading is still a repeatable speed/
 * load signal. Allow a wide 6x envelope so the low-speed PID can use it;
 * only gross spikes and the 90% rail ceiling are rejected. */
static bool bemf_plausible_for_duty(uint32_t mag, uint32_t rail, uint32_t duty)
{
    uint32_t vmax = (uint32_t)((uint64_t)duty * rail / LEDC_MAX);
    uint32_t allowed = vmax * 6U + 32U;
    return mag <= allowed;
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
                     (float)mag <= (float)rail * BEMF_CEILING &&
                     bemf_plausible_for_duty(mag, rail, s_last_duty);
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
        /* At low duty the raw reading is flyback/commutation dominated; a
         * slower IIR removes the visible jitter while the mechanical time
         * constant is still long. Above 25% use the faster 0.15 filter. */
        float alpha = (s_last_duty < 400U) ? 0.05f : 0.08f;
        s_bemf_filtered = (1.0f - alpha) * s_bemf_filtered + alpha * (float)mag;
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
    /* Factory defaults: the compiled base curve is used whenever no valid
     * calibration is stored (fresh board, after Reset). Falling back to an
     * empty curve disabled BEMF until the next reboot. */
    cal_apply(&BEMF_CAL_BASE);
}

static bool cal_duty_table_build(const settings_bemf_duty_t *cal, uint16_t table[127])
{
    memset(table, 0, 127U * sizeof(*table));
    if (!settings_bemf_duty_validate(cal)) return false;
    uint8_t prev_speed = 0U;
    uint16_t prev_duty = 0U;
    for (uint8_t i = 0U; i < cal->count; ++i) {
        uint8_t spd = cal->speed[i];
        uint16_t duty = cal->duty[i];
        if (spd > 126U || spd <= prev_speed) continue;
        if (duty < prev_duty) duty = prev_duty;
        for (uint8_t s = prev_speed; s <= spd; ++s) {
            uint32_t v = prev_duty + (uint32_t)(duty - prev_duty) *
                                       (uint32_t)(s - prev_speed) / (spd - prev_speed);
            table[s] = (uint16_t)v;
        }
        prev_speed = spd;
        prev_duty = duty;
    }
    for (uint8_t s = (uint8_t)(prev_speed + 1U); s <= 126U; ++s) table[s] = prev_duty;
    return table[1] > 0U;
}

static void cal_duty_table_apply(void)
{
    uint16_t table[127];
    bool valid = s_bemf_duty_stored_valid &&
                 cal_duty_table_build(&s_bemf_duty_stored, table);
    portENTER_CRITICAL(&s_output_mux);
    memcpy(s_bemf_duty_table, table, sizeof(table));
    s_bemf_duty_valid = valid;
    portEXIT_CRITICAL(&s_output_mux);
}

static void cal_duty_store(const settings_bemf_duty_t *cal, bool valid)
{
    if (valid && cal != NULL) {
        s_bemf_duty_stored = *cal;
        s_bemf_duty_stored_valid = true;
    } else {
        memset(&s_bemf_duty_stored, 0, sizeof(s_bemf_duty_stored));
        s_bemf_duty_stored_valid = false;
    }
    cal_duty_table_apply();
}

static void cal_duty_reload(void)
{
    settings_bemf_duty_t cal;
    if (settings_bemf_duty_load(&cal) == ESP_OK) {
        cal_duty_store(&cal, true);
    } else {
        cal_duty_store(NULL, false);
    }
}

static uint32_t bemf_regulate(uint32_t base, uint8_t applied, uint32_t rail, bool *closed)
{
    *closed = false;
    if (!s_bemf_enabled || !s_cal_valid || !s_bemf_acquired || rail <= 300U ||
        applied <= 1U || base == 0U) {
        return base;
    }
    if (!s_bemf_valid) {
        /* A single missed sample must not drop a running motor to the base
         * duty: hold the last correction for the short loss window (the
         * acquisition state still exists). A real loss clears s_bemf_acquired
         * in bemf_update and the next call falls back to base. */
        float limit = clampf((float)base * 0.50f, BEMF_CORR_LIMIT_MIN, BEMF_CORR_LIMIT_MAX);
        float hold = clampf(s_last_corr, -limit, limit);
        float duty = (float)base + hold;
        if (duty < 0.0f) duty = 0.0f;
        if (duty > (float)LEDC_MAX) duty = (float)LEDC_MAX;
        s_last_corr = hold;
        s_last_error = 0.0f;
        *closed = true;
        return (uint32_t)duty;
    }
    portENTER_CRITICAL(&s_output_mux);
    float frac = (float)s_cal_frac_table[applied] / 1024.0f;
    portEXIT_CRITICAL(&s_output_mux);
    if (frac <= 0.0f) return base;

    float target = frac * (float)rail;
    float error = target - s_bemf_filtered;
    /* At low duty the BEMF reading is flyback/commutation dominated and its
     * noise is comparable to the target error. The slow 0.05 IIR also needs
     * ~3 s to settle from a cold start, so hold the calibrated base duty until
     * it is ready. */
    if (base < 250U && s_low_speed_warmup_ticks < 400U) {
        ++s_low_speed_warmup_ticks;
        return base;
    }
    /* Deadband with hysteresis: hold the integral across the noise band, but
     * enter/leave at different thresholds so a sample on the edge does not
     * toggle the P/D every tick. */
    float enter = (float)rail * (base < 250U ? 0.035f : 0.030f);
    float leave = (float)rail * (base < 250U ? 0.025f : 0.020f);
    bool inside = s_bemf_deadband ? (fabsf(error) < enter) : (fabsf(error) < leave);
    s_bemf_deadband = inside;
    if (inside) {
        float limit = clampf((float)base * 0.50f, BEMF_CORR_LIMIT_MIN, BEMF_CORR_LIMIT_MAX);
        float hold = clampf(s_pid_ki * s_pid_integral * 0.5f, -limit, limit);
        hold = clampf(hold, s_last_corr - BEMF_CORR_SLEW, s_last_corr + BEMF_CORR_SLEW);
        float duty = (float)base + hold;
        if (duty < 0.0f) duty = 0.0f;
        if (duty > (float)LEDC_MAX) duty = (float)LEDC_MAX;
        s_pid_prev_error = 0.0f;
        s_last_error = 0.0f;
        s_last_integral = s_pid_integral;
        s_last_corr = hold;
        s_last_target = target;
        *closed = true;
        return (uint32_t)duty;
    }
    int64_t now = esp_timer_get_time();
    float dt = 1.0f;
    if (s_last_pid_us > 0 && now > s_last_pid_us) {
        dt = clampf((float)(now - s_last_pid_us) / 10000.0f, 0.5f, 2.0f);
    }
    s_last_pid_us = now;
    float derivative = (error - s_pid_prev_error) / dt;
    s_pid_prev_error = error;

    float corr = s_pid_kp * error + s_pid_ki * s_pid_integral + s_pid_kd * derivative;
    float limit = (float)base * 0.50f;
    limit = clampf(limit, BEMF_CORR_LIMIT_MIN, BEMF_CORR_LIMIT_MAX);
    corr = clampf(corr * 0.5f, -limit, limit);
    corr = clampf(corr, s_last_corr - BEMF_CORR_SLEW, s_last_corr + BEMF_CORR_SLEW);
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

    if (s_test_active) {
        uint16_t duty = 0;
        bool tforward = true;
        bool test_stop = false;
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_output_mux);
        if (s_inhibited || s_stop_requested ||
            (s_test_deadline_us != 0 && now >= s_test_deadline_us)) {
            s_test_active = false;
            s_test_duty = 0;
            test_stop = true;
        } else {
            duty = s_test_duty;
            tforward = s_test_forward;
        }
        portEXIT_CRITICAL(&s_output_mux);

        if (test_stop) {
            portENTER_CRITICAL(&s_output_mux);
            pwm_write_locked(0, true);
            s_last_duty = 0;
            s_target_speed = 0;
            s_target_forward = true;
            s_applied_speed = 0;
            s_applied_state = 0x100U;
            s_stop_requested = true;
            portEXIT_CRITICAL(&s_output_mux);
            control_reset();
            xSemaphoreGive(s_control_mutex);
            return;
        }

        /* The raw-duty test is deliberately open-loop (no BEMF regulation).
         * Sample only every ~200 ms for the UI; the 2 ms coast then costs 1%
         * of the duty instead of biasing the crawl threshold. */
        uint32_t sample_div = 20U;
        bool due = (++s_bemf_sample_div % sample_div) == 0U || !s_bemf_valid;
        /* The bench test uses the same packet fallback, otherwise a requested
         * duty below the 20 kHz floor would look like a dead button. */
        uint32_t test_out = duty;
        bool test_packet = false;
        if (duty > 0U && duty < MOTOR_PACKET_DUTY) {
            s_low_packet_acc = (uint16_t)(s_low_packet_acc + duty);
            if (s_low_packet_acc >= MOTOR_PACKET_DUTY) {
                s_low_packet_acc = (uint16_t)(s_low_packet_acc - MOTOR_PACKET_DUTY);
                test_out = MOTOR_PACKET_DUTY;
            } else {
                test_out = 0U;
            }
            test_packet = true;
        } else {
            s_low_packet_acc = 0U;
        }
        s_low_packet_active = test_packet;
        s_last_duty_avg = duty;
        s_last_duty = test_out;
        if (s_bemf_adc_ready && due) {
            if (bemf_sample_window()) {
                bemf_update();
            } else {
                s_bemf_valid = false;
                s_bemf_valid_ticks = 0;
            }
            s_last_duty = test_out;
        }
        (void)apply_pwm(test_out, tforward, generation);
        /* Keep the sound/UI in idle: the test is a bench drive, not motion. */
        s_applied_speed = 0;
        applied_publish(0, true);
        xSemaphoreGive(s_control_mutex);
        return;
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
        duty_base = (duty_base * (uint32_t)trim + 64U) / 128U;
    }
    /* Calibrated feed-forward: the duty the sweep actually used for this
     * notch. The PID then holds the same BEMF instead of correcting the
     * speed-curve duty after the warmup. */
    portENTER_CRITICAL(&s_output_mux);
    if (applied > 0U && s_bemf_duty_valid) {
        duty_base = s_bemf_duty_table[applied];
    }
    portEXIT_CRITICAL(&s_output_mux);
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

    uint32_t sample_div = ((uint32_t)s_bemf_settle_us + 999U) / 1000U;
    if (sample_div < MOTOR_SAMPLE_DIV_MIN) sample_div = MOTOR_SAMPLE_DIV_MIN;
    if (sample_div > MOTOR_SAMPLE_DIV_MAX) sample_div = MOTOR_SAMPLE_DIV_MAX;
    bool control_sense = enabled && s_cal_valid && applied > 0U && !trim_zero;
    bool sample_due = (++s_bemf_sample_div % sample_div) == 0U || !s_bemf_valid;
    /* Even when closed-loop control is off (BEMF disabled or not calibrated),
     * keep the sensor values live for the UI at a slow cadence. The coast is
     * short and infrequent, so the open-loop drive barely notices. */
    bool diag_due = false;
    if (!control_sense && !calibration && s_bemf_adc_ready) {
        uint8_t diag_div = applied > 0U ? 20U : 50U;
        if (++s_bemf_diag_div >= diag_div) {
            s_bemf_diag_div = 0;
            diag_due = true;
        }
    }
    if (!calibration && s_bemf_adc_ready && ((control_sense && sample_due) || diag_due)) {
        if (bemf_sample_window()) {
            bemf_update();
        } else {
            s_bemf_valid = false;
            s_bemf_valid_ticks = 0;
        }
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

    /* Below the 20 kHz driver floor the tick emits 10 ms packets of 8.2 %;
     * the average equals the requested calibrated duty (e.g. 5.9 %). */
    uint32_t duty_requested = duty_final;
    bool low_packet = false;
    if (!calibration && !trim_zero && applied > 0U && duty_final > 0U &&
        duty_final < MOTOR_PACKET_DUTY) {
        s_low_packet_acc = (uint16_t)(s_low_packet_acc + duty_final);
        if (s_low_packet_acc >= MOTOR_PACKET_DUTY) {
            s_low_packet_acc = (uint16_t)(s_low_packet_acc - MOTOR_PACKET_DUTY);
            duty_final = MOTOR_PACKET_DUTY;
        } else {
            duty_final = 0U;
        }
        low_packet = true;
    } else {
        s_low_packet_acc = 0U;
    }
    s_low_packet_active = low_packet;
    s_last_duty_avg = duty_requested;

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
    cal_duty_reload();
    /* Honour the stored BEMF-loop setting. A successful calibration leaves
     * it enabled; the panel can still disable the PID. */
    {
        bool bemf_use = true;
        if (settings_bemf_use_load(&bemf_use) != ESP_OK) bemf_use = true;
        s_bemf_enabled = bemf_use;
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
    if (s_test_active) {
        s_test_active = false;
        s_test_duty = 0;
    }
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
    s_test_active = false;
    s_test_duty = 0;
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
        s_test_active = false;
        s_test_duty = 0;
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

esp_err_t motor_test_duty(uint16_t duty, bool forward, uint32_t duration_ms)
{
    if (!s_init || duty > LEDC_MAX || duration_ms == 0U) return ESP_ERR_INVALID_ARG;
    if (duration_ms > MOTOR_TEST_MAX_MS) duration_ms = MOTOR_TEST_MAX_MS;
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&s_output_mux);
    if (s_inhibited || s_cal_active || s_bemf_check_active || s_sample_active) {
        err = ESP_ERR_INVALID_STATE;
    } else if (duty == 0U) {
        s_test_active = false;
        s_test_duty = 0;
        s_stop_requested = true;
        ++s_output_generation;
    } else {
        ++s_output_generation;
        s_test_active = true;
        s_test_duty = duty;
        s_test_forward = forward;
        s_test_deadline_us = esp_timer_get_time() + (int64_t)duration_ms * 1000;
        s_target_speed = 0;
        s_target_forward = forward;
        s_stop_requested = false;
        s_cal_cancelled = false;
        s_bemf_reset_requested = true;
    }
    portEXIT_CRITICAL(&s_output_mux);
    xSemaphoreGive(s_control_mutex);
    return err;
}

bool motor_test_info(uint16_t *out_duty, bool *out_forward, uint32_t *out_left_ms)
{
    portENTER_CRITICAL(&s_output_mux);
    bool active = s_test_active;
    uint16_t duty = s_test_duty;
    bool forward = s_test_forward;
    int64_t deadline = s_test_deadline_us;
    portEXIT_CRITICAL(&s_output_mux);
    if (out_duty != NULL) *out_duty = active ? duty : 0U;
    if (out_forward != NULL) *out_forward = forward;
    if (out_left_ms != NULL) {
        int64_t left = deadline - esp_timer_get_time();
        *out_left_ms = active && left > 0 ? (uint32_t)(left / 1000) : 0U;
    }
    return active;
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

typedef enum {
    CAL_SAMPLE_OK = 0,
    CAL_SAMPLE_NO_SIGNAL,
    CAL_SAMPLE_CEILING,
    CAL_SAMPLE_FAIL,
} cal_sample_result_t;

static cal_sample_result_t cal_sample_limit(uint32_t generation, uint32_t duty, bool forward,
                                            float ceiling, uint16_t *frac)
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
    } else if ((float)mag > (float)rail * ceiling) {
        /* 90% BEMF can only be real when the bridge actually applies a high
         * duty. At low duty this is the inductive flyback/freewheel transient
         * (same divider, different node/current), so skip it instead of ending
         * the sweep early. */
        uint32_t ceiling_duty = (LEDC_MAX * 90U) / 100U;
        result = (duty >= ceiling_duty) ? CAL_SAMPLE_CEILING : CAL_SAMPLE_NO_SIGNAL;
    } else if (mag < bemf_floor_mv(rail)) {
        result = CAL_SAMPLE_NO_SIGNAL;
    } else if (frac != NULL) {
        *frac = (uint16_t)((uint32_t)mag * 1024U / rail);
    }
    if (!cal_drive(generation, duty, forward)) return CAL_SAMPLE_FAIL;
    return result;
}

static void cal_profile_recommend(motor_bemf_profile_t *p, const settings_bemf_cal_t *cal);

static uint32_t cal_pct_duty(uint32_t pct)
{
    return (LEDC_MAX * pct + 50U) / 100U;
}

/* Map a raw duty back to the speed notch whose CV curve uses it (for the
 * profile's "ceiling" display only). */
static uint8_t cal_speed_for_duty(uint32_t duty)
{
    uint8_t lo = 1U, hi = 126U;
    while (lo < hi) {
        uint8_t mid = (uint8_t)((lo + hi + 1U) / 2U);
        if (speed_duty(mid) <= duty) {
            lo = mid;
        } else {
            hi = (uint8_t)(mid - 1U);
        }
    }
    return lo;
}

/* Drive one 5% PWM step for 2 s, then take the mean BEMF level. Zero frac means
 * no usable signal; a ceiling ends the sweep because the sensor saturates. */
static bool cal_sweep_step(uint32_t generation, uint32_t duty, uint16_t *frac,
                           uint8_t *quality, bool *ceiling)
{
    *frac = 0U;
    *quality = 0U;
    *ceiling = false;
    if (!cal_drive_for(generation, duty, true, CAL_SWEEP_DWELL_MS)) return false;
    uint32_t sum = 0U, minimum = 0U, maximum = 0U, n = 0U;
    for (uint8_t i = 0; i < CAL_SWEEP_SAMPLES; ++i) {
        /* Keep the 100 ms drive lease alive across the sample gap. A plain
         * vTaskDelay() here let the watchdog coast the bridge exactly at the
         * lease boundary, which surfaced as a random "timeout" after two or
         * three steps (and at the final step) even though the sweep was
         * healthy. */
        if (!cal_drive_for(generation, duty, true, CAL_SWEEP_SAMPLE_GAP_MS)) return false;
        uint16_t sample = 0U;
        cal_sample_result_t res = cal_sample_limit(generation, duty, true, BEMF_CEILING,
                                                   &sample);
        if (res == CAL_SAMPLE_FAIL) return false;
        if (res == CAL_SAMPLE_CEILING) {
            *ceiling = true;
            return true;
        }
        if (res == CAL_SAMPLE_NO_SIGNAL) continue;
        if (n == 0U) {
            minimum = sample;
            maximum = sample;
        } else {
            if (sample < minimum) minimum = sample;
            if (sample > maximum) maximum = sample;
        }
        sum += sample;
        ++n;
    }
    if (n == 0U) return true;
    uint32_t avg = sum / n;
    uint32_t spread = maximum - minimum;
    int32_t q = 100 - (int32_t)(spread * 100U / (avg + 1U));
    *quality = (uint8_t)(q < 0 ? 0 : q);
    *frac = (uint16_t)avg;
    return true;
}

/* Snapshot the measured values of one sweep step for the UI log. Called after
 * cal_sweep_step(), so the globals hold the last sample of that step. */
static void cal_log_step(uint16_t duty, uint16_t frac, uint8_t quality, bool ceiling)
{
    portENTER_CRITICAL(&s_output_mux);
    if (s_cal_log_count < MOTOR_BEMF_CAL_LOG_MAX) {
        motor_bemf_cal_step_log_t *e = &s_cal_log[s_cal_log_count++];
        e->step = s_cal_step;
        e->speed = cal_speed_for_duty(duty);
        e->quality = quality;
        e->state = ceiling ? MOTOR_BEMF_CAL_STEP_CEILING
                           : (frac == 0U ? MOTOR_BEMF_CAL_STEP_NO_SIGNAL
                                         : MOTOR_BEMF_CAL_STEP_OK);
        e->duty = duty;
        e->rail_mv = (uint16_t)pin_mv_to_motor_mv(s_rail_mv);
        e->b1_mv = (uint16_t)s_bemf1_mv;
        e->b2_mv = (uint16_t)s_bemf2_mv;
        e->diff_mv = (uint16_t)s_bemf_mag_mv;
        e->frac = frac;
    }
    portEXIT_CRITICAL(&s_output_mux);
}

static void cal_sweep_progress(uint32_t generation, uint8_t step, uint8_t total)
{
    portENTER_CRITICAL(&s_output_mux);
    if (generation == s_output_generation && !s_cal_cancelled) {
        s_cal_step = step;
        s_cal_total = total;
        s_cal_profile.planned_points = total;
        s_cal_profile.phase = MOTOR_BEMF_CAL_PHASE_CURVE;
    }
    portEXIT_CRITICAL(&s_output_mux);
}

/* Measure the same BEMF with several PWM-off delays and pick the shortest one
 * that has settled. A too-short window samples the inductive flyback instead
 * of the rotor voltage, which inflates the low end and trips the ceiling at
 * mid duty. The chosen delay is persisted with the curve. */
static bool cal_window_probe(uint32_t generation, motor_bemf_profile_t *profile)
{
    static const uint16_t candidates[MOTOR_BEMF_CAL_WINDOW_STEPS] = {
        200U, 500U, 1000U, 2000U, 5000U, 10000U, 20000U,
    };
    uint32_t duty = cal_pct_duty(CAL_WINDOW_PROBE_PCT);
    uint16_t frac[MOTOR_BEMF_CAL_WINDOW_STEPS] = { 0 };
    profile->window_count = 0;

    for (uint8_t i = 0; i < MOTOR_BEMF_CAL_WINDOW_STEPS; ++i) {
        if (!cal_job_ok(generation)) return false;
        s_bemf_settle_us = candidates[i];
        uint32_t sum = 0U, n = 0U;
        for (uint8_t s = 0; s < 2U; ++s) {
            /* Re-establish the same steady speed before every sample: a long
             * coast shortens the effective duty and would otherwise make the
             * longer windows look slower by construction. */
            if (!cal_drive_for(generation, duty, true, CAL_WINDOW_PROBE_DRIVE_MS)) {
                return false;
            }
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
            if (rail <= 300U || saturated || mag < bemf_floor_mv(rail) ||
                (float)mag > (float)rail * BEMF_CEILING) {
                continue;
            }
            sum += (uint32_t)mag * 1024U / rail;
            ++n;
        }
        if (n > 0U) frac[i] = (uint16_t)(sum / n);
        profile->window_us[i] = candidates[i];
        profile->window_frac[i] = frac[i];
        ++profile->window_count;
    }

    /* Prefer the shortest window after which every longer candidate agrees:
     * a single accidental match is not a plateau. With a noisy sensor this
     * falls back to the longest (most settled) window. */
    uint16_t ref = frac[CAL_RUNTIME_WINDOW_COUNT - 1U];
    uint8_t chosen = CAL_RUNTIME_WINDOW_COUNT - 1U;
    if (ref > 0U) {
        uint16_t tolerance = (uint16_t)((uint32_t)ref * CAL_WINDOW_STABLE_PCT / 100U);
        for (uint8_t i = 0; i + 1U < CAL_RUNTIME_WINDOW_COUNT; ++i) {
            if (frac[i] == 0U) continue;
            bool stable = true;
            for (uint8_t j = i + 1U; j < CAL_RUNTIME_WINDOW_COUNT; ++j) {
                if (frac[j] == 0U) continue;
                uint16_t d = frac[i] > frac[j] ? (uint16_t)(frac[i] - frac[j])
                                               : (uint16_t)(frac[j] - frac[i]);
                if (d > tolerance) {
                    stable = false;
                    break;
                }
            }
            if (stable) {
                chosen = i;
                break;
            }
        }
    }
    if (ref == 0U) {
        s_bemf_settle_us = s_cal_prev_window_us;
        profile->optimal_us = s_bemf_settle_us;
    } else {
        s_bemf_settle_us = candidates[chosen];
        profile->optimal_us = s_bemf_settle_us;
    }
    if (!cal_drive(generation, duty, true)) return false;
    return cal_job_ok(generation);
}

/* AUTO: start at the user's measured crawl (exact CV2 duty), find the BEMF at
 * that point, sweep to 95% for the maximum, and store a straight line between
 * speed 1 and speed 126. The low anchor is the measured BEMF so the PID can
 * run from the first movement. */
static bool cal_run_sweep(uint32_t generation, settings_bemf_cal_t *cal,
                          motor_bemf_profile_t *profile)
{
    uint32_t pwm_min = 0U, pwm_max = 0U;
    uint16_t frac_min = 0U, frac_max = 0U;
    uint32_t quality_sum = 0U;
    uint8_t quality_n = 0U, step = 0U;
    bool ceiling = false;
    uint8_t raw_n = 0U;
    uint8_t raw_speed[CAL_SWEEP_STEPS];
    uint16_t raw_frac[CAL_SWEEP_STEPS];
    uint16_t raw_duty[CAL_SWEEP_STEPS];

    /* The first step is the exact CV2 crawl duty, not a rounded percentage:
     * the low anchor must match the duty the runtime uses at speed 1, so the
     * PID holds that same BEMF from the first movement. */
    uint8_t start_pct = CAL_SWEEP_START_PCT;
    uint32_t start_duty = 0U;
    uint8_t cv2 = 0;
    if (motor_cv_read(2, &cv2) == ESP_OK && cv2 > 0U) {
        start_duty = (uint32_t)cv2 * 4U;
        uint8_t hint_pct = (uint8_t)((start_duty * 100U) / LEDC_MAX);
        if (hint_pct > start_pct) start_pct = hint_pct;
        if (start_pct > CAL_SWEEP_MAX_PCT - 10U) start_pct = CAL_SWEEP_MAX_PCT - 10U;
    }
    if (start_duty == 0U) start_duty = cal_pct_duty(start_pct);
    uint8_t total = (uint8_t)(((CAL_SWEEP_MAX_PCT - start_pct) / CAL_SWEEP_STEP_PCT) + 1U);

    cal_set_phase(generation, MOTOR_BEMF_CAL_PHASE_CURVE);
    bool first_step = true;
    for (uint32_t pct = start_pct; pct <= CAL_SWEEP_MAX_PCT;
         pct += CAL_SWEEP_STEP_PCT) {
        if (!cal_job_ok(generation)) return false;
        uint32_t duty = first_step ? start_duty : cal_pct_duty(pct);
        first_step = false;
        uint16_t frac = 0U;
        uint8_t quality = 0U;
        bool ceil_step = false;
        cal_sweep_progress(generation, (uint8_t)(++step), total);
        if (!cal_sweep_step(generation, duty, &frac, &quality, &ceil_step)) return false;
        cal_log_step((uint16_t)duty, frac, quality, ceil_step);
        if (ceil_step) {
            ceiling = true;
            break;
        }
        if (frac == 0U) continue;
        if (pwm_min == 0U) {
            pwm_min = duty;
            frac_min = frac;
        }
        uint8_t raw_spd = cal_speed_for_duty(duty);
        if (raw_n < CAL_SWEEP_STEPS && raw_spd > 0U) {
            raw_speed[raw_n] = raw_spd;
            raw_frac[raw_n] = frac;
            raw_duty[raw_n] = (uint16_t)duty;
            ++raw_n;
        }
        pwm_max = duty;
        frac_max = frac;
        quality_sum += quality;
        ++quality_n;
    }
    if (pwm_min == 0U || pwm_max <= pwm_min || frac_min == 0U) {
        cal_fail(generation, MOTOR_BEMF_CAL_ERR_NO_RISE, ESP_ERR_INVALID_STATE);
        return false;
    }

    /* The low anchor is the BEMF actually measured at the first movement, not
     * a synthetic backoff: the PID can then use that same value from the
     * moment the motor starts. Validate on the measured rise so a flat/dead
     * sensor still fails instead of producing a curve. */
    uint32_t start = pwm_min;
    int32_t start_frac = (int32_t)frac_min;
    if (start_frac < 0) start_frac = 0;
    if (frac_max < CAL_MIN_END_FRAC ||
        frac_max < (uint16_t)start_frac + CAL_MIN_RISE_FRAC) {
        cal_fail(generation, MOTOR_BEMF_CAL_ERR_CURVE, ESP_ERR_INVALID_STATE);
        return false;
    }
    /* The single-ended BEMF proxy is noisy and can dip at high duty. Build a
     * smoothed, monotonic multi-point curve instead of a straight line between
     * the first and the single maximum; the runtime interpolation already
     * supports up to SETTINGS_BEMF_CAL_MAX_POINTS points. */
    uint16_t sm[CAL_SWEEP_STEPS];
    for (uint8_t i = 0U; i < raw_n; ++i) {
        uint32_t v = raw_frac[i];
        if (i > 0U && (uint32_t)(i + 1U) < raw_n) {
            v = ((uint32_t)raw_frac[i - 1U] + 2U * raw_frac[i] +
                 raw_frac[i + 1U] + 2U) / 4U;
        }
        if (i > 0U && v < sm[i - 1U]) v = sm[i - 1U];
        sm[i] = (uint16_t)v;
    }
    uint16_t top = sm[raw_n - 1U];
    if (top < CAL_MIN_FULL_FRAC) top = CAL_MIN_FULL_FRAC;

    uint8_t pts_speed[CAL_SWEEP_STEPS + 1U];
    uint16_t pts_frac[CAL_SWEEP_STEPS + 1U];
    uint16_t pts_duty[CAL_SWEEP_STEPS + 1U];
    uint8_t pts_n = 1U;
    pts_speed[0] = 1U;
    pts_frac[0] = sm[0];
    pts_duty[0] = raw_duty[0];
    for (uint8_t i = 1U; i < raw_n; ++i) {
        if (raw_speed[i] <= pts_speed[pts_n - 1U]) continue;
        pts_speed[pts_n] = raw_speed[i];
        pts_frac[pts_n] = sm[i];
        pts_duty[pts_n] = raw_duty[i];
        ++pts_n;
    }
    if (pts_speed[pts_n - 1U] < 126U) {
        pts_speed[pts_n] = 126U;
        pts_frac[pts_n] = top;
        pts_duty[pts_n] = raw_duty[raw_n - 1U];
        ++pts_n;
    } else {
        pts_frac[pts_n - 1U] = top;
        pts_duty[pts_n - 1U] = raw_duty[raw_n - 1U];
    }
    settings_bemf_duty_t duty_cal;
    memset(&duty_cal, 0, sizeof(duty_cal));
    duty_cal.version = SETTINGS_BEMF_DUTY_VERSION;
    if (pts_n > SETTINGS_BEMF_CAL_MAX_POINTS) {
        uint8_t keep = SETTINGS_BEMF_CAL_MAX_POINTS;
        uint8_t out = 0U;
        for (uint8_t k = 0U; k < keep; ++k) {
            uint8_t idx = (uint8_t)((uint32_t)k * (pts_n - 1U) / (keep - 1U));
            cal->speed[out] = pts_speed[idx];
            cal->frac[out] = pts_frac[idx];
            duty_cal.speed[out] = pts_speed[idx];
            duty_cal.duty[out] = pts_duty[idx];
            ++out;
        }
        cal->count = out;
        duty_cal.count = out;
    } else {
        for (uint8_t i = 0U; i < pts_n; ++i) {
            cal->speed[i] = pts_speed[i];
            cal->frac[i] = pts_frac[i];
            duty_cal.speed[i] = pts_speed[i];
            duty_cal.duty[i] = pts_duty[i];
        }
        cal->count = pts_n;
        duty_cal.count = pts_n;
    }
    s_cal_duty_last = duty_cal;
    s_cal_duty_last_valid = true;
    if (!cal_curve_rising(cal)) {
        cal_fail(generation, MOTOR_BEMF_CAL_ERR_CURVE, ESP_ERR_INVALID_STATE);
        return false;
    }
    frac_max = top;

    profile->mode = MOTOR_BEMF_MODE_AUTO;
    profile->phase = MOTOR_BEMF_CAL_PHASE_CURVE;
    profile->planned_points = total;
    profile->measured_points = cal->count;
    profile->first_move_speed = 1U;
    profile->first_frac = cal->frac[0];
    profile->dwell_ms = CAL_SWEEP_DWELL_MS;
    profile->optimal_us = s_bemf_settle_us;
    profile->sensor_ok = true;
    profile->quality = quality_n > 0U ? (uint8_t)(quality_sum / quality_n) : 0U;
    profile->pwm_min_pct = (uint8_t)((pwm_min * 100U) / LEDC_MAX);
    profile->pwm_start_pct = (uint8_t)((start * 100U) / LEDC_MAX);
    profile->pwm_max_pct = (uint8_t)((pwm_max * 100U) / LEDC_MAX);
    profile->ceiling_speed = ceiling ? cal_speed_for_duty(pwm_max) : 0U;
    cal_profile_recommend(profile, cal);
    return true;
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
            if (prof.optimal_us >= SETTINGS_BEMF_WINDOW_MIN_US &&
                prof.optimal_us <= SETTINGS_BEMF_WINDOW_MAX_US) {
                s_bemf_settle_us = prof.optimal_us;
                if (settings_bemf_window_save != NULL) {
                    (void)settings_bemf_window_save(prof.optimal_us);
                }
            }
            s_cal_valid = true;
            succeeded = true;
            portEXIT_CRITICAL(&s_output_mux);
        }
    }
    if (!succeeded) {
        /* A failed/cancelled run must not leave the probing window active. */
        s_bemf_settle_us = s_cal_prev_window_us;
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
        /* Keep the loop enabled and use the calibrated duty as its base: the
         * PID then holds the BEMF measured during the sweep instead of
         * correcting a different speed-curve duty after the warmup. */
        if (s_cal_duty_last_valid &&
            settings_bemf_duty_save(&s_cal_duty_last) == ESP_OK) {
            cal_duty_store(&s_cal_duty_last, true);
        } else {
            ESP_LOGW(TAG, "calibration: BEMF duty curve not saved");
        }
        if (settings_bemf_use_save(true) != ESP_OK) {
            ESP_LOGW(TAG, "calibration: could not persist BEMF loop on");
        }
        motor_set_bemf_enabled(true);
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
    settings_bemf_cal_t cal = {0};
    motor_bemf_profile_t profile = {0};
    bool complete = true;

    cal_set_phase(generation, MOTOR_BEMF_CAL_PHASE_PREFLIGHT);
    if (!cal_rest_check(generation)) complete = false;
    if (complete) {
        cal_set_phase(generation, MOTOR_BEMF_CAL_PHASE_WINDOW);
        complete = cal_window_probe(generation, &profile);
    }
    if (complete) complete = cal_run_sweep(generation, &cal, &profile);
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
                s_test_active || s_cal_config_busy || s_bemf_check_active ||
                s_cal_task != NULL ||
                !s_bemf_adc_ready || s_adc_mutex == NULL || s_target_speed != 0U ||
                s_applied_speed != 0U || (s_applied_state & 0xFFU) != 0U ||
                s_last_duty != 0U;
    if (!deny) {
        s_cal_cfg = *cfg;
    }
    if (deny) {
        portEXIT_CRITICAL(&s_output_mux);
        xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = ++s_output_generation;
    s_cal_prev_window_us = s_bemf_settle_us;
    memset(&s_cal_profile, 0, sizeof(s_cal_profile));
    s_cal_profile.mode = MOTOR_BEMF_MODE_AUTO;
    s_cal_profile.dwell_ms = CAL_SWEEP_DWELL_MS;
    s_cal_profile.planned_points = CAL_SWEEP_STEPS;
    s_cal_active = true;
    s_cal_cancelled = false;
    s_stop_requested = false;
    s_cal_phase = MOTOR_BEMF_CAL_PHASE_IDLE;
    s_cal_step = 0;
    s_cal_log_count = 0;
    s_cal_total = CAL_SWEEP_STEPS;
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
    cfg->apply_pid = false;
}

esp_err_t motor_bemf_cal_start_ex(const motor_bemf_cal_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;
    return cal_admit(cfg);
}

esp_err_t motor_bemf_cal_start(void)
{
    motor_bemf_cal_config_t cfg;
    motor_bemf_cal_default_config(&cfg);
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
    info->count = s_cal_count;
    for (uint8_t i = 0; i < info->count && i < CAL_MAX_POINTS; ++i) {
        info->speed[i] = s_cal_speed[i];
        info->frac[i] = s_cal_frac[i];
    }
    portEXIT_CRITICAL(&s_output_mux);
    if (!info->active) {
        settings_bemf_cal_t cal;
        info->stored = (settings_bemf_cal_load(&cal) == ESP_OK);
    }
}

size_t motor_bemf_cal_log_copy(motor_bemf_cal_step_log_t *out, size_t max)
{
    if (out == NULL || max == 0U) return 0U;
    portENTER_CRITICAL(&s_output_mux);
    size_t n = s_cal_log_count < max ? s_cal_log_count : max;
    memcpy(out, s_cal_log, n * sizeof(*out));
    portEXIT_CRITICAL(&s_output_mux);
    return n;
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
    esp_err_t duty_err = settings_bemf_duty_clear();
    if (duty_err != ESP_OK) {
        ESP_LOGW(TAG, "BEMF duty curve clear failed (%s)", esp_err_to_name(duty_err));
    }
    if (err == ESP_OK) {
        cal_reload();
        cal_duty_reload();
    }
    portENTER_CRITICAL(&s_output_mux);
    s_cal_config_busy = false;
    if (err == ESP_OK) {
        s_cal_result = MOTOR_BEMF_CAL_IDLE;
        s_cal_reason = MOTOR_BEMF_CAL_ERR_NONE;
        s_cal_error_code = ESP_OK;
        s_cal_step = 0;
        s_cal_total = 0;
        s_cal_log_count = 0;
        memset(&s_cal_profile, 0, sizeof(s_cal_profile));
    }
    portEXIT_CRITICAL(&s_output_mux);
    return err;
}

esp_err_t motor_bemf_cal_reload(void)
{
    if (!cal_config_begin()) return ESP_ERR_INVALID_STATE;
    cal_reload();
    cal_duty_reload();
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
    d->bemf_mag_mv = s_bemf_mag_mv;
    d->bemf_filtered_mv = (uint32_t)s_bemf_filtered;
    d->rail_motor_mv = pin_mv_to_motor_mv(s_rail_mv);
    d->bemf1_motor_mv = s_bemf1_motor_mv;
    d->bemf2_motor_mv = s_bemf2_motor_mv;
    d->bemf_mag_motor_mv = s_bemf_mag_motor_mv;
    d->bemf_filtered_motor_mv = pin_mv_to_motor_mv((uint32_t)s_bemf_filtered);
    d->sample_ok = s_bemf_sample_ok;
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
    d->low_packet = s_low_packet_active;
    d->duty_avg = s_last_duty_avg;
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

esp_err_t motor_bemf_wave_capture(uint16_t step_us, uint16_t duration_ms,
                                  motor_bemf_wave_point_t *points,
                                  uint16_t max_points, uint16_t *out_count,
                                  uint32_t *duty, bool *forward)
{
    if (points == NULL || out_count == NULL || max_points == 0U ||
        step_us < 20U || duration_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0U;
    if (!s_bemf_adc_ready || s_control_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint32_t gen;
    uint32_t cur_duty;
    bool fwd;
    bool busy;
    portENTER_CRITICAL(&s_output_mux);
    gen = s_output_generation;
    cur_duty = s_last_duty;
    fwd = (s_applied_state & 0x100U) != 0U;
    busy = s_cal_active || s_bemf_check_active;
    portEXIT_CRITICAL(&s_output_mux);
    if (busy) {
        xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_output_mux);
    s_sample_active = true;
    pwm_write_locked(0, true);
    portEXIT_CRITICAL(&s_output_mux);
    int64_t t0 = esp_timer_get_time();
    uint32_t duration_us = (uint32_t)duration_ms * 1000U;
    uint16_t n = 0U;
    while (n < max_points) {
        int64_t now = esp_timer_get_time();
        int64_t elapsed = now - t0;
        if (elapsed > (int64_t)duration_us) break;
        int a = motor_adc_read_raw(PIN_BEMF1);
        int b = motor_adc_read_raw(PIN_BEMF2);
        if (a >= 0 && b >= 0) {
            points[n].us = (uint16_t)(elapsed > 65535 ? 65535 : elapsed);
            points[n].b1_mv = (uint16_t)((uint32_t)a * MOTOR_ADC_FULL_SCALE_MV /
                                        MOTOR_ADC_MAX_RAW);
            points[n].b2_mv = (uint16_t)((uint32_t)b * MOTOR_ADC_FULL_SCALE_MV /
                                        MOTOR_ADC_MAX_RAW);
            ++n;
            int64_t next = t0 + (int64_t)n * (int64_t)step_us;
            int64_t wait = next - esp_timer_get_time();
            if (wait > 0) esp_rom_delay_us((uint32_t)wait);
        } else {
            esp_rom_delay_us(step_us);
        }
    }
    (void)apply_pwm(cur_duty, fwd, gen);
    portENTER_CRITICAL(&s_output_mux);
    s_sample_active = false;
    portEXIT_CRITICAL(&s_output_mux);
    xSemaphoreGive(s_control_mutex);
    *out_count = n;
    if (duty != NULL) *duty = cur_duty;
    if (forward != NULL) *forward = fwd;
    return n > 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
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
    } else if (s_cal_active || s_bemf_check_active || s_sample_active || s_test_active) {
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
