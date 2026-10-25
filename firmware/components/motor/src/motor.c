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

static const char *TAG = "motor";

#define LEDC_FREQ_HZ 20000
#define LEDC_MODE    LEDC_LOW_SPEED_MODE
#define LEDC_TIMER   LEDC_TIMER_0
#define LEDC_MAX     1023U
#define MOTOR_TICK_MS 10
#define FEEDBACK_FRESH_US 100000 /* Existing track freshness software deadline. */
#define KICK_TICKS   (120 / MOTOR_TICK_MS)
/* Hi-Z coast window for the back-EMF sample. IN1=IN2=0 coasts the DRV8870 and
 * the free-wheel current dissipates through the body diodes in ~10-50 us.
 * 1 ms gives a generous window after the coast applies; the driver keeps the
 * outputs High-Z even after entering low-power sleep (~1 ms). */
#define BEMF_SETTLE_US 1000
/* A differential below 1/64 of the rail while the PID drives at >=90 % duty is
 * a failed/stuck BEMF sense, not a valid zero-speed reading: a healthy motor
 * at that duty produces far more back-EMF. This also catches a sense stuck at
 * the same non-zero voltage on both nodes, which an exact-zero test missed. */
#define BEMF_SENSE_ZERO_DIV 64U
/* While the differential is below the noise floor, cap positive integral
 * windup so a failed/stuck sense cannot hold medium duty at a low command. */
#define BEMF_SENSE_INTEGRAL_MAX 2000.0f
/* A valid motor at this target fraction (1/16 rail) must produce a measurable
 * differential; below it only the integral cap applies, so legitimate slow
 * creep is not faulted. */
#define BEMF_SENSE_TARGET_DIV 16U
/* A measurement below this fraction of the PID target while the duty is already
 * high is not a plausible speed reading. The near-zero detector above only
 * covers <= rail/64; this covers the much larger stuck/under-reading band
 * (e.g. a divider fault at 2-80% rail) and a stalled motor. A healthy motor
 * under heavy load can still reach >35% of the target at 50% duty; the long
 * window keeps legitimate starts from tripping. */
#define BEMF_SENSE_LOW_RATIO 0.35f
#define BEMF_SENSE_PLAUSIBILITY_US 500000
/* Calibration refuses to start while the motor is still coasting: residual
 * back-EMF biases the first (low-speed) points upward, and the resulting curve
 * then commands too high a target at low throttle. */
#define BEMF_CAL_REST_DIV 32U
/* A short open-loop boost only for the first calibration step. Without it a
 * high-friction motor can stall at the lowest step and abort the whole run. */
#define BEMF_CAL_BOOST_SPEED 24U
#define BEMF_CAL_BOOST_MS 120U
/* If the two halves of a calibration sample window differ by more than this
 * (rail fraction x1024), the rotor is not settled (accelerating or still
 * coasting down) and the point is re-measured after another settle window. */
#define BEMF_CAL_STABLE_DELTA 16U
/* A target cannot exceed what the open-loop duty can physically produce (plus
 * a margin for the PID drive, which may be higher than the feed-forward at low
 * speed). This bounds a flat/offset calibration curve before it commands a
 * high target at a low speed step. */
#define BEMF_TARGET_DUTY_RATIO 1.25f
#define BEMF_TARGET_DUTY_MARGIN 0.03f
/* Keep the PID target below the measurement acceptance ceiling so normal
 * noise/overshoot does not turn a healthy top-speed run into a false fault. */
#define BEMF_TARGET_CEILING_MARGIN 0.97f

static bool s_init;

/* Timestamp of the last completed motor tick, for the safety task's aliveness
 * check: if the motor task hangs (e.g. inside a flash operation), the bridge is
 * coasted from safety_task because motor_stop() would never be applied. */
static int64_t s_last_tick_us;
static portMUX_TYPE s_output_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_control_mutex;
static bool s_pwm_ready;
static bool s_inhibited = true;
static uint32_t s_inhibit_reasons = MOTOR_INHIBIT_CONTROL;
static uint32_t s_output_generation;
static bool s_reset_requested;
static bool s_feedback_fault;
static bool s_feedback_reset_requested;
static bool s_cal_cancelled;
static bool s_sample_active;
static float s_bemf_max_fraction = 0.85f;
static uint8_t s_tick_cv[SETTINGS_CV_COUNT + 1];
static bool s_tick_cv_active;

static uint8_t s_target_speed;
static bool s_target_forward;

static uint8_t s_applied_speed;
static bool s_applied_forward;

/* Published applied pair, read and written under s_output_mux.
 * bit8 = forward, bits 0..7 = speed (0..126). */
static volatile uint16_t s_applied_state;

static void applied_publish(uint8_t speed128, bool forward)
{
    s_applied_state = (uint16_t)((uint16_t)speed128 | (forward ? 0x100U : 0U));
}

static uint32_t s_ramp_acc;
static bool s_was_stopped = true;
/* Set by motor_emergency_stop() and consumed by motor_tick() so a fail-safe
 * stop from another task is not undone by the next duty write (REV-M3). */
static volatile bool s_stop_requested;
static uint32_t s_kick_duty;
static uint8_t s_kick_left;

/* Rail sense (divider mV) fed by the track task; BEMF feedback + PID. */
static volatile uint32_t s_rail_mv;
static int64_t s_rail_update_us;
/* Control-owner history; only an accepted output with fresh feedback renews it. */
static bool s_feedback_clock_active;
static int64_t s_feedback_since_us;
/* Stuck-sense watchdog: an exact-zero BEMF pair held while the PID drives the
 * duty to saturation is a failed sense, not a valid slow-speed reading. */
static bool s_zero_clock_active;
static int64_t s_zero_since_us;
static bool s_feedback_hold_valid;
static uint32_t s_feedback_hold_duty;
static bool s_feedback_hold_forward;
static uint32_t s_bemf1_mv;
static uint32_t s_bemf2_mv;
static float s_bemf_filtered;
static bool s_bemf_valid;
static bool s_bemf_filter_primed;
static bool s_bemf_reject_high;
static bool s_plausibility_clock_active;
static int64_t s_plausibility_since_us;
static int64_t s_last_pid_us;
static bool s_target_clamp_warned;
static esp_err_t s_bemf_sample_error;
static float s_pid_integral;
static float s_pid_prev_error;
static float s_pid_kp;
static float s_pid_ki;
static float s_pid_kd;
static SemaphoreHandle_t s_bemf_mutex;
static bool s_bemf_adc_ready;
static bool s_bemf_enabled = true;
static adc_oneshot_unit_handle_t s_adc;
static bool s_adc_ready;
static uint16_t s_pid_reload;

/* BEMF calibration: applied speed steps measured with the motor unloaded.
 * The measured back-EMF at each step (fraction of the rail voltage, x1024) is
 * stored in NVS and used as the PID target afterwards, so the no-load RPM is
 * held when the motor is loaded. */
#define BEMF_CAL_POINTS      10
#define BEMF_CAL_SETTLE_MS   400
#define BEMF_CAL_SAMPLES     8
#define BEMF_CAL_SAMPLE_MS   40
#define BEMF_CAL_FRAC_SCALE  1024U
/* Adaptive planner limits. */
#define BEMF_CAL_MAX_PLAN    MOTOR_BEMF_CAL_MAX_POINTS
#define BEMF_CAL_MIN_PLAN    MOTOR_BEMF_CAL_MIN_POINTS
#define BEMF_CAL_MAX_SETTLE  4000U
#define BEMF_CAL_MAX_INTERVAL 80U
#define BEMF_CAL_MAX_SAMPLES 32U
#define BEMF_CAL_MAX_RETRIES 4U
#define BEMF_CAL_STALL_DIV   256U
#define BEMF_CAL_STALL_MV    4U
#define BEMF_CAL_MIN_QUALITY 50U

/* Linear speed target: back-EMF (as a fraction of the rail voltage) ramps
 * linearly from BEMF_TARGET_START_FRAC at speed 1 to the full-speed value at
 * speed 126, so 1% already gives a small movement and 100% is max speed. The
 * start fraction must clear the motor's friction threshold. */
#define BEMF_TARGET_START_FRAC 0.08f
#define BEMF_TARGET_FULL_FRAC  0.90f

static const uint8_t BEMF_CAL_SPEEDS[BEMF_CAL_POINTS] = { 12, 24, 36, 48, 60, 72, 84, 96, 108, 126 };

static TaskHandle_t s_cal_task;
static volatile bool s_cal_active;
static volatile uint8_t s_cal_step;
static volatile uint8_t s_cal_total = BEMF_CAL_POINTS;
static volatile uint8_t s_cal_measured;
static uint8_t s_cal_count;
static uint8_t s_cal_speed[MOTOR_BEMF_CAL_MAX_POINTS];
static uint16_t s_cal_frac[MOTOR_BEMF_CAL_MAX_POINTS];
static uint8_t s_cal_run_speed[MOTOR_BEMF_CAL_MAX_POINTS];
static uint16_t s_cal_run_frac[MOTOR_BEMF_CAL_MAX_POINTS];
static uint32_t s_cal_run_id;
static motor_bemf_cal_state_t s_cal_result;
static motor_bemf_cal_error_t s_cal_reason;
static esp_err_t s_cal_error_code;
static int64_t s_cal_drive_deadline_us;
static bool s_cal_drive_active;
static motor_bemf_profile_t s_cal_profile;

/* Adaptive job. Only one calibration can be admitted at a time, so a single
 * copy is enough; the worker reads it after the admission has published it. */
typedef struct {
    motor_bemf_cal_config_t cfg;
    uint8_t plan[BEMF_CAL_MAX_PLAN];
    uint8_t plan_count;
} bemf_cal_job_t;
static bemf_cal_job_t s_cal_job;

static void bemf_cal_adaptive_task(void *arg);
static void bemf_cal_finish(uint32_t generation, settings_bemf_cal_t *cal,
                            bool complete, const motor_bemf_profile_t *profile,
                            bool apply_pid);
static void bemf_cal_profile_fill(motor_bemf_profile_t *p,
                                  const settings_bemf_cal_t *cal);

/* Interpolated rail-fraction curve indexed by applied speed 0..126. */
static uint16_t s_cal_frac_table[127];
static bool s_cal_valid;
/* Serializes the NVS clear/reload paths with calibration admission without
 * holding the motor control mutex across flash operations. */
static bool s_cal_config_busy;
static uint32_t s_last_duty;
static float s_last_error;
static float s_last_integral;
static float s_last_corr;
static float s_last_target;
static bool s_last_pid_ok;

static uint32_t rail_snapshot(void)
{
    portENTER_CRITICAL(&s_output_mux);
    uint32_t rail = esp_timer_get_time() - s_rail_update_us < FEEDBACK_FRESH_US ? s_rail_mv : 0U;
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
    if (x < lo) {
        return lo;
    }
    if (x > hi) {
        return hi;
    }
    return x;
}

static void load_pid(void)
{
    uint8_t kp = 0, ki = 0, kd = 0;
    (void)motor_cv_read(54, &kp);
    (void)motor_cv_read(55, &ki);
    (void)motor_cv_read(56, &kd);
    s_pid_kp = 0.10f + ((float)kp / 255.0f) * 1.90f;
    s_pid_ki = ((float)ki / 255.0f) * 0.30f;
    s_pid_kd = ((float)kd / 255.0f) * 0.80f;
}

/* Map speed step 0..126 to PWM duty 0..1023 using either the CV67-CV94
 * 28-point table (CV29 bit 4) or the Vstart/Vmid/Vhigh curve (CV2/5/6). */
static uint32_t speed_duty(uint8_t speed)
{
    if (speed == 0U) {
        return 0U;
    }

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
        if (hi < lo) {
            hi = lo; /* non-monotonic CV67..94 must not wrap the difference */
        }
        uint32_t d = lo + (hi - lo) * f / 128U;
        return (d > LEDC_MAX) ? LEDC_MAX : d;
    }

    uint32_t vs = (uint32_t)cv2 * 4U;
    uint32_t vm = (uint32_t)cv6 * 4U;
    uint32_t vh = (uint32_t)cv5 * 4U;
    if (vh == 0U) {
        vh = LEDC_MAX;
    }
    if (vm == 0U) {
        vm = (vs + vh) / 2U;
    }
    /* Enforce Vstart <= Vmid <= Vhigh so the interpolation below never
     * underflows (a non-monotonic CV2/CV5/CV6 previously wrapped the unsigned
     * difference into a huge duty value). */
    if (vm < vs) {
        vm = vs;
    }
    if (vh < vm) {
        vh = vm;
    }
    if (speed >= 126U) {
        return vh;
    }
    if (speed <= 63U) {
        return vs + (vm - vs) * (speed - 1U) / 62U;
    }
    return vm + (vh - vm) * (speed - 63U) / 63U;
}

/* Channels 0/1 are exclusively motor-owned, with no fade operations. IDF 6's
 * ledc_set_duty may wait on a fade semaphore. Runtime uses the same LL duty,
 * enable/start/latch operations instead; never wait or call ADC under this mux.
 * Channel config already installed num=1, cycle=1, scale=0 (non-fading duty). */
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
    if (!s_pwm_ready) {
        return;
    }
    if (forward) {
        pwm_channel(LEDC_CHANNEL_1, 0);
        pwm_channel(LEDC_CHANNEL_0, duty);
    } else {
        pwm_channel(LEDC_CHANNEL_0, 0);
        pwm_channel(LEDC_CHANNEL_1, duty);
    }
}

/* Caller holds output mux. Keep a watchdog failure sticky through later STOPs. */
static void IRAM_ATTR bemf_cal_stop_locked(motor_bemf_cal_error_t reason, esp_err_t code)
{
    if (!s_cal_active) return;
    if (s_cal_reason == MOTOR_BEMF_CAL_ERR_NONE) {
        s_cal_result = reason == MOTOR_BEMF_CAL_ERR_NONE ? MOTOR_BEMF_CAL_CANCELLED : MOTOR_BEMF_CAL_FAILED;
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

/* Worker errors coast now but publish terminal state after bounded cleanup.
 * The saved cause is already sticky if an independent STOP arrives meanwhile. */
static void bemf_cal_worker_fail_locked(motor_bemf_cal_error_t reason, esp_err_t code)
{
    motor_bemf_cal_state_t previous = s_cal_result;
    bool first = s_cal_reason == MOTOR_BEMF_CAL_ERR_NONE;
    bemf_cal_stop_locked(reason, code);
    if (first && (previous == MOTOR_BEMF_CAL_RUNNING || previous == MOTOR_BEMF_CAL_SAVING)) {
        s_cal_result = previous;
    }
}

void motor_bemf_cal_watchdog(void)
{
    portENTER_CRITICAL(&s_output_mux);
    if (s_cal_active && !s_cal_cancelled && s_cal_drive_active) {
        int64_t now = esp_timer_get_time();
        if (s_rail_mv <= 300U || now - s_rail_update_us >= FEEDBACK_FRESH_US) {
            bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_RAIL, ESP_ERR_INVALID_STATE);
        } else if (now >= s_cal_drive_deadline_us) {
            bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_TIMEOUT, ESP_ERR_TIMEOUT);
        }
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
                    !s_feedback_fault && !s_stop_requested && !s_sample_active && !s_cal_cancelled &&
                    !s_cal_active;
    if (accepted) {
        pwm_write_locked(duty, forward);
        s_last_duty = duty;
    }
    portEXIT_CRITICAL(&s_output_mux);
    return accepted;
}

/* Only the control owner touches ramp/PID/filter state. An emergency stop
 * publishes zero immediately and asks that owner to discard its stale state. */
static void control_reset(void)
{
    s_applied_speed = 0;
    s_applied_forward = true;
    s_ramp_acc = 0;
    s_was_stopped = true;
    s_kick_left = 0;
    s_kick_duty = 0;
    s_pid_integral = 0.0f;
    s_pid_prev_error = 0.0f;
    s_bemf_filtered = 0.0f;
    s_bemf_valid = false;
    s_bemf_filter_primed = false;
    s_bemf_reject_high = false;
    s_plausibility_clock_active = false;
    s_plausibility_since_us = 0;
    s_last_pid_us = 0;
    s_target_clamp_warned = false;
    s_last_pid_ok = false;
    s_feedback_hold_valid = false;
    s_feedback_clock_active = false;
    s_feedback_since_us = 0;
}

/* Shared ADC1 (adc_oneshot). One unit serves the BEMF sample window, the rail
 * sense and the diagnostics dump; channels are 12-bit at 12 dB attenuation.
 * ESP32-S3 ADC1 channels 0..9 map to GPIO1..10. */
#define ADC1_GPIO_MIN 1
#define ADC1_GPIO_MAX 10

static esp_err_t adc_unit_ensure(void)
{
    if (s_bemf_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_adc_ready) {
        return ESP_OK;
    }
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&cfg, &s_adc);
    if (err != ESP_OK) {
        return err;
    }
    s_adc_ready = true;
    return ESP_OK;
}

esp_err_t motor_adc_config_channel(int gpio_num)
{
    if (gpio_num < ADC1_GPIO_MIN || gpio_num > ADC1_GPIO_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = adc_unit_ensure();
    if (err != ESP_OK) {
        return err;
    }
    adc_oneshot_chan_cfg_t chan = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    return adc_oneshot_config_channel(s_adc, (adc_channel_t)(gpio_num - 1), &chan);
}

int motor_adc_read_raw(int gpio_num)
{
    if (gpio_num < ADC1_GPIO_MIN || gpio_num > ADC1_GPIO_MAX) {
        return -1;
    }
    if (adc_unit_ensure() != ESP_OK) {
        return -1;
    }
    int raw = -1;
    if (adc_oneshot_read(s_adc, (adc_channel_t)(gpio_num - 1), &raw) != ESP_OK) {
        return -1;
    }
    return raw;
}

static bool bemf_sample_window(void)
{
    s_bemf_valid = false;
    s_bemf_sample_error = ESP_FAIL;
    if (!s_bemf_adc_ready) {
        return false;
    }
    if (!motor_bemf_lock()) {
        s_bemf_sample_error = ESP_ERR_TIMEOUT;
        return false;
    }
    /* Coast the DRV8870 for the sample window: IN1=IN2=LOW (0% duty on both
     * LEDC channels) = "Coast", both half-bridge outputs high-Z. The motor
     * free-wheels through the body diodes: one terminal is pulled to GND and
     * the other to VM. After the L/R freewheel decays, the GND-side terminal
     * stays near 0 V and the other terminal carries the back-EMF directly, so
     * it is read on the higher sense node without any 2x scaling.
     * IN1=IN2=HIGH would be BRAKE (both outputs shorted to GND) and would
     * zero the measurement. */
    portENTER_CRITICAL(&s_output_mux);
    s_sample_active = true;
    s_cal_drive_active = false;
    pwm_write_locked(0, true);
    portEXIT_CRITICAL(&s_output_mux);
    esp_rom_delay_us(BEMF_SETTLE_US);
    int raw1 = motor_adc_read_raw(PIN_BEMF1);
    int raw2 = motor_adc_read_raw(PIN_BEMF2);
    motor_bemf_unlock();
    portENTER_CRITICAL(&s_output_mux);
    s_sample_active = false;
    portEXIT_CRITICAL(&s_output_mux);
    if (raw1 >= 0 && raw1 < 4095 && raw2 >= 0 && raw2 < 4095) {
        s_bemf1_mv = (uint32_t)raw1 * 3100U / 4095U;
        s_bemf2_mv = (uint32_t)raw2 * 3100U / 4095U;
        s_bemf_sample_error = ESP_OK;
        return true;
    }
    return false;
}

static void bemf_update(void)
{
    if (!s_bemf_adc_ready) {
        s_bemf_filtered = 0.0f;
        s_bemf_valid = false;
        s_bemf_filter_primed = false;
        s_bemf_reject_high = false;
        return;
    }
    /* Back-EMF is the potential difference across the motor: bEMF =
     * V_OUT1 - V_OUT2. In the coast window one terminal rests near GND while
     * the other carries the back-EMF, so the magnitude of the ADC difference
     * is the pure back-EMF (divider-scaled), independent of direction. */
    int32_t diff = (int32_t)s_bemf1_mv - (int32_t)s_bemf2_mv;
    uint32_t mag = (diff < 0) ? (uint32_t)(-diff) : (uint32_t)diff;
    uint32_t rail = rail_snapshot();
    s_bemf_reject_high = rail > 300U && (float)mag > (float)rail * s_bemf_max_fraction;
    bool clamped = rail <= 300U || s_bemf_reject_high;
    if (!clamped) {
        if (!s_bemf_filter_primed) {
            /* Priming with the first accepted sample avoids the artificial
             * "zero speed" lag after every stop/reset, which used to add duty
             * on startup and contributed to overshoot on a light motor. */
            s_bemf_filtered = (float)mag;
            s_bemf_filter_primed = true;
        } else {
            s_bemf_filtered = 0.85f * s_bemf_filtered + 0.15f * (float)mag;
        }
        s_bemf_valid = true;
    } else {
        /* Rejected measurements do not invent a speed change. */
        s_bemf_valid = false;
    }
}

/* Build the per-speed rail-fraction lookup (speed 0..126) from the measured
 * calibration points by linear interpolation. A curve is only trusted if the
 * motor actually produced measurable back-EMF at full speed. */
static bool bemf_cal_prepare_table(const settings_bemf_cal_t *cal, uint16_t table[127])
{
    memset(table, 0, 127U * sizeof(*table));
    bool valid = settings_bemf_cal_validate(cal);
    uint8_t prev_speed = 0;
    uint16_t prev_frac = 0;
    for (uint8_t i = 0; valid && i < cal->count; ++i) {
        uint8_t spd = cal->speed[i];
        uint16_t frac = cal->frac[i];
        /* A documented curve may start at speed 0; use that fraction as the
         * low-speed anchor instead of silently interpolating from zero. */
        if (i == 0U && spd == 0U) {
            table[0] = frac;
            prev_frac = frac;
            continue;
        }
        if (spd > 126U || spd <= prev_speed) {
            continue;
        }
        /* Enforce monotonicity: a lower point would make (frac - prev_frac)
         * negative and wrap the unsigned multiply into garbage. */
        if (frac < prev_frac) {
            frac = prev_frac;
        }
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
    return valid && table[126] >= SETTINGS_BEMF_CAL_MIN_END_FRAC;
}

/* ---- adaptive calibration planner ------------------------------------- */

static const uint8_t BEMF_CAL_FAST_PLAN[] = { 12, 24, 36, 48, 60, 72, 84, 96, 108, 126 };
static const uint8_t BEMF_CAL_BALANCED_PLAN[] = { 6, 10, 14, 20, 28, 38, 50, 64, 80, 96, 112, 126 };
static const uint8_t BEMF_CAL_THOROUGH_PLAN[] = { 5, 8, 12, 16, 22, 30, 40, 52, 66, 80, 94, 108, 120, 126 };

static void bemf_cal_apply_mode_defaults(motor_bemf_cal_config_t *cfg)
{
    motor_bemf_cal_config_t d = {0};
    d.mode = cfg->mode;
    d.apply_pid = cfg->apply_pid;
    switch (cfg->mode) {
    case MOTOR_BEMF_MODE_FAST:
        d.points = 10; d.settle_ms = 400; d.settle_max_ms = 1200; d.interval_ms = 40;
        d.samples = 8; d.retries = 1; d.boost_ms = 120; d.adaptive = false;
        break;
    case MOTOR_BEMF_MODE_BALANCED:
        d.points = 12; d.settle_ms = 500; d.settle_max_ms = 2000; d.interval_ms = 50;
        d.samples = 10; d.retries = 1; d.boost_ms = 120; d.adaptive = true;
        break;
    case MOTOR_BEMF_MODE_CUSTOM:
        d.points = cfg->speed_count >= BEMF_CAL_MIN_PLAN ? cfg->speed_count : BEMF_CAL_MIN_PLAN;
        d.settle_ms = 500; d.settle_max_ms = 2000; d.interval_ms = 40;
        d.samples = 10; d.retries = cfg->retries; d.boost_ms = cfg->boost_ms;
        d.adaptive = cfg->adaptive;
        break;
    case MOTOR_BEMF_MODE_THOROUGH:
    case MOTOR_BEMF_MODE_AUTO:
    default:
        d.mode = cfg->mode;
        d.points = 16; d.settle_ms = 600; d.settle_max_ms = 2400; d.interval_ms = 60;
        d.samples = 12; d.retries = 2; d.boost_ms = 150; d.adaptive = true;
        break;
    }
    /* Nonzero caller values override the mode defaults. For CUSTOM, retries
     * and boost are explicit (0 means "no retry"/"no boost"). */
    if (cfg->points != 0U) d.points = cfg->points;
    if (cfg->settle_ms != 0U) d.settle_ms = cfg->settle_ms;
    if (cfg->settle_max_ms != 0U) d.settle_max_ms = cfg->settle_max_ms;
    if (cfg->interval_ms != 0U) d.interval_ms = cfg->interval_ms;
    if (cfg->samples != 0U) d.samples = cfg->samples;
    if (cfg->mode != MOTOR_BEMF_MODE_CUSTOM && cfg->retries != 0U) d.retries = cfg->retries;
    if (cfg->mode != MOTOR_BEMF_MODE_CUSTOM && cfg->boost_ms != 0U) d.boost_ms = cfg->boost_ms;
    if (cfg->mode == MOTOR_BEMF_MODE_CUSTOM) {
        d.speed_count = cfg->speed_count;
        memcpy(d.speed, cfg->speed, sizeof(d.speed));
    }
    if (d.points < BEMF_CAL_MIN_PLAN) d.points = BEMF_CAL_MIN_PLAN;
    if (d.points > BEMF_CAL_MAX_PLAN) d.points = BEMF_CAL_MAX_PLAN;
    if (d.settle_ms < 100U) d.settle_ms = 100U;
    if (d.settle_ms > 2000U) d.settle_ms = 2000U;
    if (d.settle_max_ms == 0U) d.settle_max_ms = (uint16_t)(d.settle_ms * 4U);
    if (d.settle_max_ms < d.settle_ms) d.settle_max_ms = d.settle_ms;
    if (d.settle_max_ms > BEMF_CAL_MAX_SETTLE) d.settle_max_ms = BEMF_CAL_MAX_SETTLE;
    if (d.interval_ms < 10U) d.interval_ms = 10U;
    if (d.interval_ms > BEMF_CAL_MAX_INTERVAL) d.interval_ms = BEMF_CAL_MAX_INTERVAL;
    if (d.samples < 4U) d.samples = 4U;
    if (d.samples > BEMF_CAL_MAX_SAMPLES) d.samples = BEMF_CAL_MAX_SAMPLES;
    if (d.retries > BEMF_CAL_MAX_RETRIES) d.retries = BEMF_CAL_MAX_RETRIES;
    if (d.boost_ms > 500U) d.boost_ms = 500U;
    *cfg = d;
}

static bool bemf_cal_plan_valid(const uint8_t *plan, uint8_t count)
{
    if (count < BEMF_CAL_MIN_PLAN || count > BEMF_CAL_MAX_PLAN) return false;
    for (uint8_t i = 0; i < count; ++i) {
        if (plan[i] == 0U || plan[i] > 126U) return false;
        if (i > 0U && plan[i] <= plan[i - 1U]) return false;
    }
    return true;
}

static uint8_t bemf_cal_plan_add_midpoint(uint8_t *plan, uint8_t count)
{
    if (count >= BEMF_CAL_MAX_PLAN) return count;
    uint8_t best = 0;
    uint8_t best_gap = 0;
    for (uint8_t i = 1; i < count; ++i) {
        uint8_t gap = (uint8_t)(plan[i] - plan[i - 1U]);
        if (gap > best_gap) {
            best_gap = gap;
            best = i;
        }
    }
    if (best == 0U || best_gap < 3U) return count;
    uint8_t mid = (uint8_t)(plan[best - 1U] + best_gap / 2U);
    if (mid <= plan[best - 1U] || mid >= plan[best]) return count;
    for (uint8_t i = count; i > best; --i) {
        plan[i] = plan[i - 1U];
    }
    plan[best] = mid;
    return (uint8_t)(count + 1U);
}

/* Select `want` points evenly from a sorted template (keeping both ends). */
static uint8_t bemf_cal_plan_select(const uint8_t *src, uint8_t n, uint8_t want, uint8_t *out)
{
    if (want >= n) {
        memcpy(out, src, n);
        return n;
    }
    for (uint8_t i = 0; i < want; ++i) {
        uint32_t idx = (n == 1U) ? 0U : ((uint32_t)i * (n - 1U) + (want - 1U) / 2U) / (want - 1U);
        out[i] = src[idx];
    }
    /* The rounding can repeat an index for tiny n; compact duplicates. */
    uint8_t k = 0;
    for (uint8_t i = 0; i < want; ++i) {
        if (k == 0U || out[i] != out[k - 1U]) out[k++] = out[i];
    }
    return k;
}

static bool bemf_cal_build_job(bemf_cal_job_t *job, const motor_bemf_cal_config_t *input)
{
    if (job == NULL || input == NULL) return false;
    memset(job, 0, sizeof(*job));
    job->cfg = *input;
    bemf_cal_apply_mode_defaults(&job->cfg);

    if (job->cfg.mode == MOTOR_BEMF_MODE_CUSTOM && job->cfg.speed_count != 0U) {
        if (job->cfg.speed_count < BEMF_CAL_MIN_PLAN ||
            job->cfg.speed_count > BEMF_CAL_MAX_PLAN) return false;
        memcpy(job->plan, job->cfg.speed, job->cfg.speed_count);
        job->plan_count = job->cfg.speed_count;
        if (!bemf_cal_plan_valid(job->plan, job->plan_count)) return false;
        if (job->cfg.points < job->plan_count) job->cfg.points = job->plan_count;
    } else {
        const uint8_t *base;
        uint8_t base_count;
        switch (job->cfg.mode) {
        case MOTOR_BEMF_MODE_FAST:
            base = BEMF_CAL_FAST_PLAN;
            base_count = (uint8_t)sizeof(BEMF_CAL_FAST_PLAN);
            break;
        case MOTOR_BEMF_MODE_BALANCED:
            base = BEMF_CAL_BALANCED_PLAN;
            base_count = (uint8_t)sizeof(BEMF_CAL_BALANCED_PLAN);
            break;
        case MOTOR_BEMF_MODE_THOROUGH:
        case MOTOR_BEMF_MODE_AUTO:
        default:
            base = BEMF_CAL_THOROUGH_PLAN;
            base_count = (uint8_t)sizeof(BEMF_CAL_THOROUGH_PLAN);
            break;
        }
        job->plan_count = bemf_cal_plan_select(base, base_count, job->cfg.points, job->plan);
        if (!bemf_cal_plan_valid(job->plan, job->plan_count)) return false;
    }
    /* A non-adaptive run cannot add points after the pass, so fill its plan
     * up front. An adaptive run keeps the base plan and inserts based on the
     * measured curve. */
    if (!job->cfg.adaptive) {
        while (job->plan_count < job->cfg.points) {
            uint8_t next = bemf_cal_plan_add_midpoint(job->plan, job->plan_count);
            if (next == job->plan_count) break;
            job->plan_count = next;
        }
    }
    return bemf_cal_plan_valid(job->plan, job->plan_count);
}

/* Remember the active calibration points (for the info/dump API) and rebuild
 * the per-speed lookup table from them. */
static void bemf_cal_apply(const settings_bemf_cal_t *cal)
{
    uint16_t table[127];
    bool valid = bemf_cal_prepare_table(cal, table);
    uint8_t n = settings_bemf_cal_validate(cal) ? cal->count : 0;
    portENTER_CRITICAL(&s_output_mux);
    s_cal_count = n;
    for (uint8_t i = 0; i < n; ++i) {
        s_cal_speed[i] = cal->speed[i];
        s_cal_frac[i] = cal->frac[i];
    }
    memcpy(s_cal_frac_table, table, sizeof(table));
    s_cal_valid = valid;
    s_reset_requested = true;
    portEXIT_CRITICAL(&s_output_mux);
    if (valid) {
        /* A curve loaded from NVS/base has no live-run quality/stall data,
         * but its slope and recommended PID are still useful in the UI. */
        motor_bemf_profile_t p = {0};
        p.mode = MOTOR_BEMF_MODE_STORED;
        p.planned_points = n;
        p.measured_points = n;
        bemf_cal_profile_fill(&p, cal);
        /* No live settle/quality and no reliable rail-mV gain at boot; keep
         * the stored-curve card honest and show only the curve geometry. */
        p.settle_ms_avg = 0;
        p.plant_gain_x1000 = 0;
        p.kp_x100 = 0;
        p.ki_x1000 = 0;
        p.kd_x100 = 0;
        p.quality = 0;
        portENTER_CRITICAL(&s_output_mux);
        s_cal_profile = p;
        portEXIT_CRITICAL(&s_output_mux);
    }
}

/* (Re)load the stored calibration, falling back to the firmware base curve
 * when none was saved. Used at boot and after a serial/API calibration set. */
static void bemf_cal_reload(void)
{
    settings_bemf_cal_t cal;
    if (settings_bemf_cal_load(&cal) != ESP_OK || !settings_bemf_cal_validate(&cal)) {
        cal = BEMF_CAL_BASE;
    }
    bemf_cal_apply(&cal);
}

static void motor_tick(void)
{
    motor_bemf_cal_watchdog();
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_output_mux);
    uint32_t generation = s_output_generation;
    uint8_t target = s_target_speed;
    bool target_forward = s_target_forward;
    bool calibration = s_cal_active;
    bool enabled = s_bemf_enabled;
    bool reset = s_stop_requested;
    bool reset_feedback = s_reset_requested;
    bool reset_feedback_clock = s_feedback_reset_requested;
    bool stopped = s_stop_requested || s_inhibited || s_feedback_fault || s_cal_cancelled;
    s_stop_requested = false;
    s_reset_requested = false;
    s_feedback_reset_requested = false;
    portEXIT_CRITICAL(&s_output_mux);
    if (reset) {
        control_reset();
    } else if (reset_feedback) {
        s_pid_integral = 0.0f;
        s_pid_prev_error = 0.0f;
        s_bemf_filtered = 0.0f;
        s_bemf_valid = false;
        s_bemf_filter_primed = false;
        s_bemf_reject_high = false;
        s_plausibility_clock_active = false;
        s_plausibility_since_us = 0;
        s_last_pid_us = 0;
        s_target_clamp_warned = false;
    }
    if (stopped) {
        target = 0;
    }
    if (reset_feedback_clock) {
        s_feedback_clock_active = false;
        s_feedback_since_us = 0;
        s_feedback_hold_valid = false;
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
    bool reversing = (s_applied_forward != target_forward) && (applied > 0U);

    if (reversing) {
        /* Reversing while moving: decelerate to a stop before flipping the
         * bridge, so the polarity never changes at full duty (current spike /
         * brownout / hardware overcurrent). */
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
            if (applied == 0U) {
                s_ramp_acc = 0;
            }
        }
    } else if (calibration) {
        /* Calibration drives the motor open-loop: no ramp, no kick, no PID. */
        applied = target;
        s_ramp_acc = 0;
    } else if (target != applied) {
        uint8_t cv = (target > applied) ? cv3 : cv4;
        if (cv == 0U) {
            applied = target;
            s_ramp_acc = 0;
        } else {
            /* cv != 0 here (checked above), so ticks >= 100. */
            uint32_t ticks = (uint32_t)cv * 100U;
            s_ramp_acc += 126U;
            while (s_ramp_acc >= ticks && applied != target) {
                s_ramp_acc -= ticks;
                if (target > applied) {
                    applied++;
                } else {
                    applied--;
                }
            }
            if (applied == target) {
                s_ramp_acc = 0;
            }
        }
    } else {
        s_ramp_acc = 0;
    }

    s_applied_speed = applied;
    if (!reversing || applied == 0U) {
        s_applied_forward = target_forward;
    }

    uint32_t duty_base = speed_duty(applied);
    uint8_t trim = 128;
    if (motor_cv_read(s_applied_forward ? 66U : 95U, &trim) == ESP_OK && trim != 128U) {
        duty_base = (duty_base * trim) / 128U;
        /* CV66/CV95 allow up to 255/128 = ~2x. The LEDC channel is 10-bit;
         * never hand an out-of-range duty to the LL/open-loop path. */
        if (duty_base > LEDC_MAX) {
            duty_base = LEDC_MAX;
        }
    }
    uint32_t duty_final = duty_base;

    /* Kickstart (CV65): boost the duty for a short burst when starting. */
    bool kicking = false;
    if (calibration) {
        s_was_stopped = false;
        s_kick_left = 0;
    } else if (applied == 0U) {
        s_was_stopped = true;
        s_kick_left = 0;
    } else if (s_was_stopped) {
        s_was_stopped = false;
        uint8_t cv65 = 0;
        (void)motor_cv_read(65, &cv65);
        if (cv65 > 0U) {
            uint8_t cv29 = 0;
            (void)motor_cv_read(29, &cv29);
            uint32_t base;
            if ((cv29 & 0x10U) != 0U) {
                /* Table mode: base the kick on the table's top point (CV94),
                 * not on the unused CV5. */
                uint8_t cv94 = 0;
                (void)motor_cv_read(94, &cv94);
                base = (uint32_t)cv94 * 4U;
                if (base == 0U) {
                    base = LEDC_MAX;
                }
            } else {
                uint8_t cv5 = 0;
                (void)motor_cv_read(5, &cv5);
                base = (cv5 != 0U) ? (uint32_t)cv5 * 4U : LEDC_MAX;
            }
            s_kick_duty = (base * cv65 / 255U > LEDC_MAX) ? LEDC_MAX : base * cv65 / 255U;
            s_kick_left = KICK_TICKS;
        }
    }
    if (s_kick_left > 0U && applied > 0U) {
        if (duty_final < s_kick_duty) {
            duty_final = s_kick_duty;
        }
        kicking = true;
        s_kick_left--;
    }

    /* BEMF sampling window + PID speed regulation. Skipped entirely when the
     * user disabled BEMF: the motor then runs open-loop and the bridge is
     * never coasted (the 1 ms sample window would otherwise cost 10 % of the
     * torque on every 10 ms tick). */
    bool sampled = false;
    if (enabled && !calibration && s_bemf_adc_ready && applied > 0U) {
        sampled = bemf_sample_window();
    }
    if (sampled) {
        bemf_update();
    } else {
        s_bemf_valid = false;
        s_bemf_reject_high = false;
    }

    s_pid_reload++;
    if (s_pid_reload >= 100U) {
        load_pid();
        s_pid_reload = 0;
    }

    uint32_t rail = rail_snapshot();
    uint32_t bemf_mag = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv : s_bemf2_mv - s_bemf1_mv;
    bool sense_mag_low = rail > 300U && bemf_mag <= rail / BEMF_SENSE_ZERO_DIV;
    bool pid_ok = enabled && !calibration && s_bemf_adc_ready &&
                  s_bemf_valid && rail > 300U && applied > 0U;
    s_last_pid_ok = pid_ok;
    float pid_target = 0.0f;
    int64_t now = esp_timer_get_time();
    if (pid_ok) {
        /* PID target back-EMF (as a fraction of the rail voltage): use the
         * stored/motor calibration curve when it is valid so the no-load RPM
         * is held under load, otherwise fall back to the fixed linear ramp. */
        float frac;
        bool target_clamped = false;
        float target_cap = 0.0f;
        portENTER_CRITICAL(&s_output_mux);
        if (s_cal_valid) {
            frac = (float)s_cal_frac_table[applied] / (float)BEMF_CAL_FRAC_SCALE;
        } else {
            frac = BEMF_TARGET_START_FRAC +
                   ((float)applied / 126.0f) *
                   (clampf(BEMF_TARGET_FULL_FRAC, BEMF_TARGET_START_FRAC, s_bemf_max_fraction) - BEMF_TARGET_START_FRAC);
        }
        frac = clampf(frac, 0.0f, s_bemf_max_fraction);
        /* A curve with a flat/offset point can ask for a target that the
         * open-loop duty for this step could never produce. Bound it by the
         * applied voltage, with a margin because the PID may add drive at low
         * speed. This keeps a bogus low-speed point from causing a jump. */
        target_cap = ((float)duty_base / (float)LEDC_MAX) * BEMF_TARGET_DUTY_RATIO +
                     BEMF_TARGET_DUTY_MARGIN;
        if (target_cap < BEMF_TARGET_START_FRAC) target_cap = BEMF_TARGET_START_FRAC;
        float ceiling = s_bemf_max_fraction * BEMF_TARGET_CEILING_MARGIN;
        if (target_cap > ceiling) target_cap = ceiling;
        if (frac > target_cap) {
            frac = target_cap;
            target_clamped = true;
        }
        portEXIT_CRITICAL(&s_output_mux);
        if (target_clamped && !s_target_clamp_warned) {
            s_target_clamp_warned = true;
            ESP_LOGW(TAG, "BEMF target clamped at speed %u: duty=%lu frac_1024=%lu",
                     (unsigned)applied, (unsigned long)duty_base,
                     (unsigned long)(target_cap * (float)BEMF_CAL_FRAC_SCALE));
        }
        float target_bemf = frac * (float)rail;
        pid_target = target_bemf;
        float error = target_bemf - s_bemf_filtered;
        /* Normalize the loop to the nominal 10 ms tick. The task can be late
         * (ADC mutex, flash, web work); using raw per-tick increments changed
         * the effective Ki/Kd under load. Clamp so a single long gap cannot
         * create a derivative/integral kick. */
        float dt_norm = 1.0f;
        if (s_last_pid_us > 0 && now > s_last_pid_us) {
            dt_norm = clampf((float)(now - s_last_pid_us) / 10000.0f, 0.5f, 2.0f);
        }
        s_last_pid_us = now;
        float derivative = (error - s_pid_prev_error) / dt_norm;
        s_pid_prev_error = error;

        /* A failed or under-reading sense makes the error stay positive while
         * the PID drives the duty up. Cap the positive integral for both the
         * near-zero case and a measurement far below the target; otherwise a
         * stuck 2-80 % sensor winds the loop to high duty at a low command. */
        bool sense_below = pid_target > 0.0f &&
                           s_bemf_filtered < pid_target * BEMF_SENSE_LOW_RATIO;
        if ((sense_mag_low || sense_below) && s_pid_integral > BEMF_SENSE_INTEGRAL_MAX) {
            s_pid_integral = BEMF_SENSE_INTEGRAL_MAX;
        }
        float corr = s_pid_kp * error + s_pid_ki * s_pid_integral + s_pid_kd * derivative;
        s_last_error = error;
        s_last_integral = s_pid_integral;
        s_last_corr = corr;
        s_last_target = target_bemf;
        float duty_f = (float)duty_base + corr * 0.4f;
        if (kicking && duty_f < (float)duty_final) {
            duty_f = (float)duty_final;
        }
        float clamped = clampf(duty_f, 0.0f, (float)LEDC_MAX);

        /* Conditional integration (anti-windup): only accumulate the integral
         * while the output is not saturated in the direction of the error. */
        bool saturating = (clamped >= (float)LEDC_MAX && error > 0.0f) ||
                          (clamped <= 0.0f && error < 0.0f);
        if (!saturating) {
            s_pid_integral = clampf(s_pid_integral + error * dt_norm,
                                    -20000.0f, 20000.0f);
        }
        if ((sense_mag_low || sense_below) && s_pid_integral > BEMF_SENSE_INTEGRAL_MAX) {
            s_pid_integral = BEMF_SENSE_INTEGRAL_MAX;
        }
        duty_final = (uint32_t)clamped;
    } else if (applied == 0U) {
        s_pid_integral = 0.0f;
        s_pid_prev_error = 0.0f;
        s_last_pid_us = now;
    } else {
        s_last_pid_us = now;
    }

    /* A BEMF sense stuck near zero stays "valid" while the PID drives the duty
     * up; that is a failed sense, not a valid slow-speed zero. A near-zero
     * differential (both nodes equal, including a common stuck non-zero
     * voltage) is a fault at high drive or at a target that a healthy motor
     * must turn into measurable back-EMF. Below that target only the integral
     * is capped, so legitimate slow creep is not faulted. */
    bool sense_zero = pid_ok && sense_mag_low &&
                      (duty_final >= (LEDC_MAX * 9U / 10U) ||
                       pid_target >= (float)rail / (float)BEMF_SENSE_TARGET_DIV);
    if (sense_zero) {
        if (!s_zero_clock_active) {
            s_zero_clock_active = true;
            s_zero_since_us = now;
        }
    } else {
        s_zero_clock_active = false;
    }

    /* Plausibility: a valid measurement far below the target while the PID is
     * already driving hard is either a stuck/under-reading sense or a stalled
     * motor. The near-zero detector above covers only <= rail/64; this covers
     * the 2-80 % band that used to wind the duty up with no fault at all. */
    bool plausibility_low = pid_ok && pid_target > 0.0f &&
                            s_bemf_filtered < pid_target * BEMF_SENSE_LOW_RATIO &&
                            duty_final >= (LEDC_MAX / 2U);
    if (plausibility_low) {
        if (!s_plausibility_clock_active) {
            s_plausibility_clock_active = true;
            s_plausibility_since_us = now;
        }
    } else {
        s_plausibility_clock_active = false;
    }

    if (enabled && !calibration && applied > 0U && !pid_ok) {
        if (!s_feedback_clock_active) {
            s_feedback_clock_active = true;
            s_feedback_since_us = now;
        }
        /* No blind startup/kick or base-duty fallback. A changed command may
         * only lower the hold, and may never carry it into another direction. */
        uint32_t command_duty = speed_duty(target);
        duty_final = s_feedback_hold_valid && s_feedback_hold_forward == s_applied_forward &&
                     target_forward == s_applied_forward ? s_feedback_hold_duty : 0U;
        if (duty_final > duty_base) duty_final = duty_base;
        if (duty_final > command_duty) duty_final = command_duty;
        /* A rejected measurement above the acceptance ceiling is most often a
         * genuine overspeed or a sense stuck at/near the rail. Holding the old
         * (high) duty for the whole 100 ms fault window kept the motor fast and
         * kept the next samples rejected. Decay the hold instead; a transient
         * bad sample recovers on the next valid one, a persistent one coasts. */
        if (s_bemf_reject_high && duty_final > 0U) {
            duty_final = duty_final * 3U / 4U;
            if (s_feedback_hold_valid && s_feedback_hold_forward == s_applied_forward) {
                s_feedback_hold_duty = duty_final;
            }
        }
    }

    bool fault_latched = false;
    if ((enabled && !calibration && applied > 0U && !pid_ok &&
         s_feedback_clock_active && now - s_feedback_since_us >= FEEDBACK_FRESH_US) ||
        (sense_zero && now - s_zero_since_us >= FEEDBACK_FRESH_US) ||
        (plausibility_low && s_plausibility_clock_active &&
         now - s_plausibility_since_us >= BEMF_SENSE_PLAUSIBILITY_US)) {
        portENTER_CRITICAL(&s_output_mux);
        /* A concurrent STOP/disable supersedes this tick's fault decision. */
        if (s_bemf_enabled && s_target_speed > 0U && !s_feedback_reset_requested) {
            s_feedback_fault = true;
            ++s_output_generation;
            s_target_speed = 0;
            s_stop_requested = true;
            s_applied_state = 0x100U;
            s_last_duty = 0;
            pwm_write_locked(0, true);
            fault_latched = true;
        }
        portEXIT_CRITICAL(&s_output_mux);
        if (fault_latched) {
            ESP_LOGW(TAG, "BEMF feedback fault: duty=%lu target_mv=%lu filtered_mv=%lu rail_mv=%lu",
                     (unsigned long)duty_final, (unsigned long)pid_target,
                     (unsigned long)s_bemf_filtered, (unsigned long)rail);
        }
        duty_final = 0;
    }

    /* During calibration the cal task owns the bridge directly, so the motor
     * task must not overwrite the duty between samples. */
    if (!calibration) {
        if (apply_pwm(duty_final, s_applied_forward, generation)) {
            if (pid_ok) {
                s_feedback_clock_active = true;
                s_feedback_since_us = now;
                s_feedback_hold_valid = true;
                s_feedback_hold_forward = s_applied_forward;
                s_feedback_hold_duty = duty_final;
            } else if (s_feedback_hold_valid) {
                /* Once reduced during a gap, the hold may not rebound. */
                s_feedback_hold_duty = duty_final;
            }
            if (applied == 0U) s_feedback_hold_valid = false;
        }
    }
    /* A new drive must get its own full feedback grace period. Without this the
     * timestamp from the previous run makes the first rejected sample latch a
     * fault immediately after a stop or calibration. */
    if (applied == 0U) {
        s_feedback_clock_active = false;
        s_feedback_since_us = 0;
        s_plausibility_clock_active = false;
        s_plausibility_since_us = 0;
    }
    portENTER_CRITICAL(&s_output_mux);
    if (generation == s_output_generation && !s_inhibited && !s_feedback_fault && !s_stop_requested && !s_cal_cancelled) {
        applied_publish(s_applied_speed, s_applied_forward);
    }
    s_last_tick_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_output_mux);
    s_tick_cv_active = false;
    xSemaphoreGive(s_control_mutex);
}

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_motor_iter_cap;

static void motor_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(MOTOR_TICK_MS);
    uint32_t iters = 0;
    while (s_motor_iter_cap == 0U || iters < s_motor_iter_cap) {
        /* Keep the nominal period normally, but discard missed deadlines after
         * a stall instead of replaying PID/ramp/sample ticks back-to-back. */
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
    s_bemf_mutex = xSemaphoreCreateMutex();
    /* PIN_BEMF1=GPIO4 -> ADC1_CH3, PIN_BEMF2=GPIO5 -> ADC1_CH4,
     * PIN_RAIL_SENSE=GPIO6 -> ADC1_CH5. Enable BEMF only once every channel is
     * configured: otherwise the sample window would coast the bridge for 1 ms
     * every tick while reading -1, silently losing ~10 % duty (REV-M2). */
    s_bemf_adc_ready = false;
    esp_err_t adc_err = s_bemf_mutex != NULL ? motor_adc_config_channel(PIN_BEMF1) : ESP_ERR_NO_MEM;
    if (adc_err == ESP_OK) {
        adc_err = motor_adc_config_channel(PIN_BEMF2);
    }
    if (adc_err == ESP_OK) {
        adc_err = motor_adc_config_channel(PIN_RAIL_SENSE);
    }
    if (adc_err == ESP_OK) {
        s_bemf_adc_ready = true;
    } else {
        ESP_LOGW(TAG, "ADC init failed (%s): closed-loop drive unavailable", esp_err_to_name(adc_err));
    }

    load_pid();
    bemf_cal_reload();
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
    s_pid_integral = 0.0f;
    s_pid_prev_error = 0.0f;
    s_init = true;
    s_last_tick_us = 0;
    s_stop_requested = false;
    s_reset_requested = false;
    s_feedback_fault = false;
    s_feedback_reset_requested = false;
    s_feedback_clock_active = false;
    s_feedback_since_us = 0;
    s_feedback_hold_valid = false;
    s_zero_clock_active = false;
    s_zero_since_us = 0;
    s_cal_cancelled = false;
    s_sample_active = false;
    s_cal_active = false;
    s_cal_task = NULL;
    s_cal_config_busy = false;
    s_cal_result = MOTOR_BEMF_CAL_IDLE;
    s_cal_reason = MOTOR_BEMF_CAL_ERR_NONE;
    s_cal_error_code = ESP_OK;
    s_cal_drive_active = false;
    s_cal_drive_deadline_us = 0;
    s_inhibited = true;
    s_inhibit_reasons = MOTOR_INHIBIT_CONTROL;
    ++s_output_generation;

    if (xTaskCreate(motor_task, "motor", 3072, NULL, 7, NULL) != pdPASS) {
        s_init = false;
        motor_emergency_stop();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Motor PWM + BEMF-PID initialized (LEDC 20 kHz)");
    return ESP_OK;
}

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    if (!s_init) {
        return ESP_ERR_INVALID_STATE;
    }
    if (speed128 > 126U) {
        speed128 = 126U;
    }
    portENTER_CRITICAL(&s_output_mux);
    if (speed128 == 0U) {
        if (s_feedback_fault) {
            s_stop_requested = true;
            s_feedback_reset_requested = true;
        }
        s_feedback_fault = false;
    }
    if (s_inhibited || s_feedback_fault || s_cal_active) {
        bool cancel = speed128 == 0U && s_cal_active;
        portEXIT_CRITICAL(&s_output_mux);
        if (cancel) {
            motor_emergency_stop();
        }
        return speed128 == 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    /* Repeated DCC refreshes are not new physical commands. Invalidating the
     * sampling generation here would turn an otherwise valid coast window
     * into a missing-feedback event on every duplicate packet. */
    if (!s_stop_requested && s_cal_task == NULL &&
        s_target_speed == speed128 && s_target_forward == forward) {
        portEXIT_CRITICAL(&s_output_mux);
        return ESP_OK;
    }
    ++s_output_generation;
    s_target_speed = speed128;
    s_target_forward = forward;
    s_cal_cancelled = false;
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

void motor_stop(void)
{
    (void)motor_set_speed(0, true);
}

/* Fail-safe stop: force the bridge off immediately, without waiting for the
 * motor task to apply the target. Safe to call from any task, including when
 * the motor task is suspected to be stuck. */
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
    bool inhibited = s_inhibited || s_feedback_fault;
    portEXIT_CRITICAL(&s_output_mux);
    return inhibited;
}

/* Timestamp (us) of the last completed motor tick; 0 before the first tick. */
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
    if (out_speed128 != NULL) {
        *out_speed128 = speed;
    }
    if (out_forward != NULL) {
        *out_forward = forward;
    }
}

void motor_get_applied_speed(uint8_t *out_speed128, bool *out_forward)
{
    /* The published pair is copied in one critical section. */
    portENTER_CRITICAL(&s_output_mux);
    uint16_t st = s_applied_state;
    portEXIT_CRITICAL(&s_output_mux);
    if (out_speed128 != NULL) {
        *out_speed128 = (uint8_t)(st & 0xFFU);
    }
    if (out_forward != NULL) {
        *out_forward = (st & 0x100U) != 0U;
    }
}

/* Run the unloaded motor through the calibration speed steps open-loop, wait
 * for each step to settle, average the measured back-EMF and record it as a
 * fraction of the rail voltage. The curve is saved to NVS and applied to the
 * PID target immediately. */
static bool bemf_cal_save_authorized(void *context)
{
    uint32_t generation = *(const uint32_t *)context;
    portENTER_CRITICAL(&s_output_mux);
    bool current = generation == s_output_generation && s_cal_active &&
                   !s_cal_cancelled && !s_inhibited && !s_feedback_fault;
    portEXIT_CRITICAL(&s_output_mux);
    return current;
}

/* Each worker sample and exact-duty restore is serialized with normal control.
 * A late sample cannot resurrect an expired lease even while the bridge coasts. */
static bool bemf_cal_sample_apply(uint32_t generation, uint32_t duty, uint32_t *fraction)
{
    motor_bemf_cal_error_t reason = MOTOR_BEMF_CAL_ERR_NONE;
    esp_err_t code = ESP_OK;
    if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        reason = MOTOR_BEMF_CAL_ERR_CONTROL;
        code = ESP_ERR_TIMEOUT;
    } else {
        portENTER_CRITICAL(&s_output_mux);
        bool current = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
        bool expired = s_cal_drive_deadline_us != 0 && esp_timer_get_time() >= s_cal_drive_deadline_us;
        portEXIT_CRITICAL(&s_output_mux);
        if (current && expired) {
            reason = MOTOR_BEMF_CAL_ERR_TIMEOUT;
            code = ESP_ERR_TIMEOUT;
        } else if (current) {
            bool fresh = bemf_sample_window();
            uint32_t rail = rail_snapshot();
            uint32_t mag = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv : s_bemf2_mv - s_bemf1_mv;
            if (!fresh) {
                reason = s_bemf_sample_error == ESP_ERR_TIMEOUT ? MOTOR_BEMF_CAL_ERR_CONTROL : MOTOR_BEMF_CAL_ERR_ADC;
                code = s_bemf_sample_error;
            } else if (rail <= 300U) {
                reason = MOTOR_BEMF_CAL_ERR_RAIL;
                code = ESP_ERR_INVALID_STATE;
            } else if (fraction != NULL && (mag == 0U || (float)mag > (float)rail * s_bemf_max_fraction)) {
                reason = MOTOR_BEMF_CAL_ERR_CURVE;
                code = ESP_ERR_INVALID_ARG;
            } else {
                if (fraction != NULL) *fraction = (uint32_t)((uint64_t)mag * BEMF_CAL_FRAC_SCALE / rail);
                portENTER_CRITICAL(&s_output_mux);
                int64_t now = esp_timer_get_time();
                current = generation == s_output_generation && !s_cal_cancelled && !s_inhibited &&
                          !s_feedback_fault && !s_sample_active;
                if (current && (s_rail_mv <= 300U || now - s_rail_update_us >= FEEDBACK_FRESH_US)) {
                    reason = MOTOR_BEMF_CAL_ERR_RAIL;
                    code = ESP_ERR_INVALID_STATE;
                } else if (current && s_cal_drive_deadline_us != 0 && now >= s_cal_drive_deadline_us) {
                    reason = MOTOR_BEMF_CAL_ERR_TIMEOUT;
                    code = ESP_ERR_TIMEOUT;
                } else if (current) {
                    s_cal_drive_deadline_us = now + FEEDBACK_FRESH_US;
                    s_cal_drive_active = duty != 0U;
                    pwm_write_locked(duty, true);
                    s_last_duty = duty;
                    applied_publish(s_target_speed, true);
                }
                portEXIT_CRITICAL(&s_output_mux);
            }
        }
        xSemaphoreGive(s_control_mutex);
        if (!current) return false;
    }
    if (reason != MOTOR_BEMF_CAL_ERR_NONE) {
        portENTER_CRITICAL(&s_output_mux);
        if (generation == s_output_generation) bemf_cal_worker_fail_locked(reason, code);
        portEXIT_CRITICAL(&s_output_mux);
        return false;
    }
    return true;
}

typedef enum {
    BEMF_CAL_PT_OK = 0,
    BEMF_CAL_PT_STALL,
    BEMF_CAL_PT_CEILING,
    BEMF_CAL_PT_FAIL,
} bemf_cal_pt_t;

static uint32_t bemf_cal_rail_mv(void)
{
    portENTER_CRITICAL(&s_output_mux);
    uint32_t rail = s_rail_mv;
    portEXIT_CRITICAL(&s_output_mux);
    return rail;
}

/* Apply `duty`, wait until the freewheel BEMF stops moving (bounded by
 * settle_max), then average `samples` observations. The caller decides whether
 * a stall is retried with a boost and whether a ceiling ends the sweep. */
static bemf_cal_pt_t bemf_cal_read_point(uint32_t generation, uint32_t duty,
                                         const motor_bemf_cal_config_t *cfg,
                                         uint16_t settle_min, uint16_t *frac_out,
                                         uint8_t *quality_out, uint16_t *settle_out)
{
    uint32_t elapsed = cfg->interval_ms;
    uint32_t last_frac = 0;
    bool have_last = false;
    uint8_t stable = 0;
    uint32_t frac = 0;
    uint32_t rail = 0;
    bool settled = false;

    /* First call leaves the bridge driving at `duty`; its sample is taken at
     * coast and is deliberately ignored. Give the rotor one interval to react
     * before the first real probe (the legacy worker did the same). */
    if (!bemf_cal_sample_apply(generation, duty, NULL)) {
        return BEMF_CAL_PT_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(cfg->interval_ms));

    for (;;) {
        if (!bemf_cal_sample_apply(generation, duty, NULL)) {
            return BEMF_CAL_PT_FAIL;
        }
        rail = bemf_cal_rail_mv();
        if (rail <= 300U) {
            return BEMF_CAL_PT_FAIL;
        }
        uint32_t mag = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv
                                               : s_bemf2_mv - s_bemf1_mv;
        uint32_t stall = rail / BEMF_CAL_STALL_DIV;
        if (stall < BEMF_CAL_STALL_MV) stall = BEMF_CAL_STALL_MV;
        if (mag <= stall) {
            return BEMF_CAL_PT_STALL;
        }
        if ((float)mag > (float)rail * s_bemf_max_fraction) {
            return BEMF_CAL_PT_CEILING;
        }
        frac = (uint32_t)mag * BEMF_CAL_FRAC_SCALE / rail;
        if (elapsed >= settle_min) {
            if (have_last) {
                uint32_t delta = frac > last_frac ? frac - last_frac : last_frac - frac;
                stable = delta <= BEMF_CAL_STABLE_DELTA ? (uint8_t)(stable + 1U) : 0U;
            }
            have_last = true;
            last_frac = frac;
            if (stable >= 2U) {
                settled = true;
                break;
            }
        }
        if (elapsed >= cfg->settle_max_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(cfg->interval_ms));
        elapsed = (uint32_t)(elapsed + cfg->interval_ms);
    }

    uint32_t sum = frac;
    uint32_t minimum = frac;
    uint32_t maximum = frac;
    uint32_t n = 1;
    for (; n < cfg->samples; ++n) {
        vTaskDelay(pdMS_TO_TICKS(cfg->interval_ms));
        elapsed = (uint32_t)(elapsed + cfg->interval_ms);
        if (!bemf_cal_sample_apply(generation, duty, NULL)) {
            return BEMF_CAL_PT_FAIL;
        }
        rail = bemf_cal_rail_mv();
        if (rail <= 300U) return BEMF_CAL_PT_FAIL;
        uint32_t mag = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv
                                               : s_bemf2_mv - s_bemf1_mv;
        uint32_t stall = rail / BEMF_CAL_STALL_DIV;
        if (stall < BEMF_CAL_STALL_MV) stall = BEMF_CAL_STALL_MV;
        if (mag <= stall) return BEMF_CAL_PT_STALL;
        if ((float)mag > (float)rail * s_bemf_max_fraction) return BEMF_CAL_PT_CEILING;
        uint32_t f = (uint32_t)mag * BEMF_CAL_FRAC_SCALE / rail;
        sum += f;
        if (f < minimum) minimum = f;
        if (f > maximum) maximum = f;
    }
    uint32_t avg = sum / n;
    uint32_t spread = maximum - minimum;
    int32_t quality = 100 - (int32_t)(spread * 100U / (avg + 1U));
    if (quality < 0) quality = 0;
    if (!settled) quality /= 2;
    *frac_out = (uint16_t)avg;
    *quality_out = (uint8_t)quality;
    *settle_out = (uint16_t)elapsed;
    return BEMF_CAL_PT_OK;
}

/* A calibration started immediately after a stop can catch a still-coasting
 * rotor. The residual back-EMF biases the first low-speed points upward and
 * the curve then commands too high a target at low throttle. Refuse to start
 * unless the motor is (nearly) at rest. */
static bool bemf_cal_rest_check(uint32_t generation)
{
    motor_bemf_cal_error_t reason = MOTOR_BEMF_CAL_ERR_NONE;
    esp_err_t code = ESP_OK;
    if (!bemf_sample_window()) {
        reason = s_bemf_sample_error == ESP_ERR_TIMEOUT ? MOTOR_BEMF_CAL_ERR_CONTROL
                                                        : MOTOR_BEMF_CAL_ERR_ADC;
        code = s_bemf_sample_error;
    } else {
        uint32_t rail = rail_snapshot();
        uint32_t mag = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv
                                               : s_bemf2_mv - s_bemf1_mv;
        if (rail <= 300U) {
            reason = MOTOR_BEMF_CAL_ERR_RAIL;
            code = ESP_ERR_INVALID_STATE;
        } else if (mag > rail / BEMF_CAL_REST_DIV) {
            ESP_LOGW(TAG, "calibration refused: motor not at rest (bemf=%lu rail=%lu mV)",
                     (unsigned long)mag, (unsigned long)rail);
            reason = MOTOR_BEMF_CAL_ERR_CURVE;
            code = ESP_ERR_INVALID_STATE;
        }
    }
    if (reason != MOTOR_BEMF_CAL_ERR_NONE) {
        portENTER_CRITICAL(&s_output_mux);
        if (generation == s_output_generation) {
            bemf_cal_worker_fail_locked(reason, code);
        }
        portEXIT_CRITICAL(&s_output_mux);
        return false;
    }
    portENTER_CRITICAL(&s_output_mux);
    bool current = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
    portEXIT_CRITICAL(&s_output_mux);
    return current;
}

static void bemf_cal_profile_fill(motor_bemf_profile_t *p, const settings_bemf_cal_t *cal)
{
    if (cal->count == 0U) return;
    if (p->planned_points == 0U) p->planned_points = cal->count;
    if (p->measured_points == 0U) p->measured_points = cal->count;
    if (p->quality == 0U) p->quality = 100U;
    if (p->settle_ms_avg == 0U) {
        p->settle_ms_avg = s_cal_job.cfg.settle_ms != 0U ? s_cal_job.cfg.settle_ms
                                                        : (uint16_t)BEMF_CAL_SETTLE_MS;
    }
    p->first_move_speed = cal->speed[0];
    p->first_frac = cal->frac[0];

    uint32_t rail = bemf_cal_rail_mv();
    if (rail < 300U) rail = 0U;
    float sum_g = 0.0f;
    uint8_t n = 0;
    uint32_t prev_duty = 0;
    bool have_prev = false;
    for (uint8_t i = 0; i < cal->count; ++i) {
        uint32_t duty = speed_duty(cal->speed[i]);
        if (have_prev && duty > prev_duty && cal->frac[i] >= cal->frac[i - 1U]) {
            int32_t dbemf = ((int32_t)cal->frac[i] - (int32_t)cal->frac[i - 1U]) *
                            (int32_t)(rail ? rail : 1000U) / (int32_t)BEMF_CAL_FRAC_SCALE;
            int32_t dduty = (int32_t)(duty - prev_duty);
            if (dbemf > 0 && dduty > 0) {
                sum_g += (float)dbemf / (float)dduty;
                ++n;
            }
        }
        prev_duty = duty;
        have_prev = true;
    }
    if (n > 0U && sum_g > 0.0f) {
        float g = sum_g / (float)n; /* mV of BEMF per duty count */
        float kp = clampf(1.0f / g, 0.10f, 2.00f);
        float ki = clampf(kp * 0.05f, 0.00f, 0.30f);
        float kd = clampf(kp * 0.10f, 0.00f, 0.80f);
        p->plant_gain_x1000 = (uint16_t)clampf(g * 1000.0f, 0.0f, 65535.0f);
        p->kp_x100 = (uint16_t)clampf(kp * 100.0f, 0.0f, 65535.0f);
        p->ki_x1000 = (uint16_t)clampf(ki * 1000.0f, 0.0f, 65535.0f);
        p->kd_x100 = (uint16_t)clampf(kd * 100.0f, 0.0f, 65535.0f);
    }
}

static bool bemf_cal_apply_pid(const motor_bemf_profile_t *p)
{
    float kp = (float)p->kp_x100 / 100.0f;
    float ki = (float)p->ki_x1000 / 1000.0f;
    float kd = (float)p->kd_x100 / 100.0f;
    if (kp <= 0.0f) return false;
    uint8_t cv54 = (uint8_t)clampf((kp - 0.10f) / 1.90f * 255.0f, 0.0f, 255.0f);
    uint8_t cv55 = (uint8_t)clampf(ki / 0.30f * 255.0f, 0.0f, 255.0f);
    uint8_t cv56 = (uint8_t)clampf(kd / 0.80f * 255.0f, 0.0f, 255.0f);
    if (settings_cv_write(54U, cv54) != ESP_OK ||
        settings_cv_write(55U, cv55) != ESP_OK ||
        settings_cv_write(56U, cv56) != ESP_OK ||
        settings_cv_commit() != ESP_OK) {
        ESP_LOGW(TAG, "BEMF recommended PID could not be stored");
        return false;
    }
    portENTER_CRITICAL(&s_output_mux);
    s_pid_reload = 100U;
    portEXIT_CRITICAL(&s_output_mux);
    ESP_LOGI(TAG, "BEMF tuned PID: CV54=%u CV55=%u CV56=%u (kp=%.2f ki=%.3f kd=%.2f)",
             (unsigned)cv54, (unsigned)cv55, (unsigned)cv56, (double)kp, (double)ki, (double)kd);
    return true;
}

static void bemf_cal_finish(uint32_t generation, settings_bemf_cal_t *cal,
                            bool complete, const motor_bemf_profile_t *profile,
                            bool apply_pid)
{
    motor_bemf_profile_t prof = {0};
    if (profile != NULL) prof = *profile;
    if (cal->count > 0U) {
        bemf_cal_profile_fill(&prof, cal);
    }

    bool current = false;
    portENTER_CRITICAL(&s_output_mux);
    pwm_write_locked(0, true);
    s_target_speed = 0;
    s_target_forward = true;
    s_applied_state = 0x100U;
    s_last_duty = 0;
    s_cal_drive_active = false;
    s_cal_drive_deadline_us = 0;
    s_stop_requested = true;
    current = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
    if (complete && current) s_cal_result = MOTOR_BEMF_CAL_SAVING;
    portEXIT_CRITICAL(&s_output_mux);

    bool succeeded = false;
    motor_bemf_cal_error_t reason = MOTOR_BEMF_CAL_ERR_NONE;
    esp_err_t code = ESP_OK;
    uint16_t table[127];
    if (complete && current) {
        if (!bemf_cal_prepare_table(cal, table)) {
            reason = MOTOR_BEMF_CAL_ERR_CURVE;
            code = ESP_ERR_INVALID_ARG;
        } else if ((code = settings_bemf_cal_save_guarded(cal, bemf_cal_save_authorized,
                                                          &generation)) != ESP_OK) {
            reason = MOTOR_BEMF_CAL_ERR_STORAGE;
        } else if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) == pdTRUE) {
            portENTER_CRITICAL(&s_output_mux);
            current = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
            if (current) {
                s_cal_count = cal->count;
                memcpy(s_cal_speed, cal->speed, cal->count * sizeof(*cal->speed));
                memcpy(s_cal_frac, cal->frac, cal->count * sizeof(*cal->frac));
                memcpy(s_cal_frac_table, table, sizeof(table));
                s_cal_valid = true;
                s_reset_requested = true;
                succeeded = true;
            }
            portEXIT_CRITICAL(&s_output_mux);
            xSemaphoreGive(s_control_mutex);
        } else {
            reason = MOTOR_BEMF_CAL_ERR_CONTROL;
            code = ESP_ERR_TIMEOUT;
        }
    }
    if (succeeded && apply_pid && prof.kp_x100 != 0U) {
        prof.pid_applied = bemf_cal_apply_pid(&prof);
    }
    if (reason != MOTOR_BEMF_CAL_ERR_NONE) {
        portENTER_CRITICAL(&s_output_mux);
        if (generation == s_output_generation && !s_cal_cancelled) {
            bemf_cal_worker_fail_locked(reason, code);
        }
        portEXIT_CRITICAL(&s_output_mux);
    }
    if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) == pdTRUE) {
        control_reset();
        xSemaphoreGive(s_control_mutex);
    } else {
        succeeded = false;
        if (reason == MOTOR_BEMF_CAL_ERR_NONE) {
            reason = MOTOR_BEMF_CAL_ERR_CONTROL;
            code = ESP_ERR_TIMEOUT;
        }
    }
    portENTER_CRITICAL(&s_output_mux);
    s_cal_profile = prof;
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
    if (succeeded) {
        s_cal_total = cal->count;
    }
    s_cal_active = false;
    s_cal_task = NULL;
    portEXIT_CRITICAL(&s_output_mux);
    vTaskDelete(NULL);
}

static void bemf_cal_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    settings_bemf_cal_t cal = {0};
    cal.count = BEMF_CAL_POINTS;
    bool complete = true;
    bool current = false;
    if (!bemf_cal_rest_check(generation)) {
        complete = false;
    }
    for (uint8_t i = 0; complete && i < BEMF_CAL_POINTS; ++i) {
        uint8_t spd = BEMF_CAL_SPEEDS[i];
        if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
            portENTER_CRITICAL(&s_output_mux);
            if (generation == s_output_generation) bemf_cal_worker_fail_locked(MOTOR_BEMF_CAL_ERR_CONTROL, ESP_ERR_TIMEOUT);
            portEXIT_CRITICAL(&s_output_mux);
            complete = false;
            break;
        }
        esp_err_t snapshot_err = settings_cv_snapshot(s_tick_cv);
        if (snapshot_err != ESP_OK) {
            xSemaphoreGive(s_control_mutex);
            portENTER_CRITICAL(&s_output_mux);
            if (generation == s_output_generation) bemf_cal_worker_fail_locked(MOTOR_BEMF_CAL_ERR_CONTROL, snapshot_err);
            portEXIT_CRITICAL(&s_output_mux);
            complete = false;
            break;
        }
        s_tick_cv_active = true;
        uint32_t duty = speed_duty(spd);
        uint32_t boost_duty = (i == 0U) ? speed_duty(BEMF_CAL_BOOST_SPEED) : 0U;
        s_tick_cv_active = false;
        xSemaphoreGive(s_control_mutex);
        portENTER_CRITICAL(&s_output_mux);
        current = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
        if (current) {
            s_target_speed = spd;
            s_target_forward = true;
        }
        portEXIT_CRITICAL(&s_output_mux);
        if (!current) {
            complete = false;
            break;
        }
        /* The first step is the most likely to stall (no kickstart in the
         * open-loop calibration). Give it a short, bounded boost before
         * settling at the real step duty. */
        if (i == 0U) {
            complete = bemf_cal_sample_apply(generation, boost_duty, NULL);
            for (uint32_t elapsed = 0; complete && elapsed < BEMF_CAL_BOOST_MS;
                 elapsed += BEMF_CAL_SAMPLE_MS) {
                vTaskDelay(pdMS_TO_TICKS(BEMF_CAL_SAMPLE_MS));
                complete = bemf_cal_sample_apply(generation, boost_duty, NULL);
            }
        }
        if (!complete || !bemf_cal_sample_apply(generation, duty, NULL)) {
            complete = false;
            break;
        }
        for (uint32_t elapsed = 0; complete && elapsed < BEMF_CAL_SETTLE_MS; elapsed += BEMF_CAL_SAMPLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(BEMF_CAL_SAMPLE_MS));
            complete = bemf_cal_sample_apply(generation, duty, NULL);
        }
        if (!complete) {
            break;
        }
        /* A free/unloaded motor at the top of the open-loop curve can produce
         * more than the acceptance ceiling. Instead of aborting the whole run,
         * stop at the last measurable step and save a partial curve: the
         * remaining speed steps stay flat at the last valid fraction, so the
         * PID target stays below the ceiling. */
        {
            uint32_t settle_rail = rail_snapshot();
            uint32_t settle_mag = s_bemf1_mv > s_bemf2_mv ? s_bemf1_mv - s_bemf2_mv
                                                         : s_bemf2_mv - s_bemf1_mv;
            if (settle_rail > 300U &&
                (float)settle_mag > (float)settle_rail * s_bemf_max_fraction) {
                if (i < 2U) {
                    portENTER_CRITICAL(&s_output_mux);
                    if (generation == s_output_generation) {
                        bemf_cal_worker_fail_locked(MOTOR_BEMF_CAL_ERR_CURVE, ESP_ERR_INVALID_ARG);
                    }
                    portEXIT_CRITICAL(&s_output_mux);
                    complete = false;
                } else {
                    bool still_current = false;
                    portENTER_CRITICAL(&s_output_mux);
                    still_current = generation == s_output_generation && !s_cal_cancelled;
                    if (still_current) {
                        cal.count = (uint8_t)i;
                        s_cal_step = (uint8_t)i;
                        s_cal_measured = (uint8_t)i;
                    }
                    portEXIT_CRITICAL(&s_output_mux);
                    if (still_current) {
                        ESP_LOGW(TAG, "BEMF calibration ceiling at speed %u; saving %u points",
                                 (unsigned)spd, (unsigned)i);
                    }
                    complete = still_current;
                }
                break;
            }
        }

        uint32_t samples[BEMF_CAL_SAMPLES];
        uint32_t sum = 0;
        uint32_t n = 0;
        bool retried = false;
        do {
            n = 0;
            for (uint8_t j = 0; complete && j < BEMF_CAL_SAMPLES; ++j) {
                uint32_t fraction = 0;
                complete = bemf_cal_sample_apply(generation, duty, &fraction);
                if (!complete) break;
                samples[n++] = fraction;
                vTaskDelay(pdMS_TO_TICKS(BEMF_CAL_SAMPLE_MS));
            }
            if (!complete || n != BEMF_CAL_SAMPLES) {
                break;
            }
            uint32_t first = (samples[0] + samples[1] + samples[2] + samples[3]) / 4U;
            uint32_t last = (samples[4] + samples[5] + samples[6] + samples[7]) / 4U;
            uint32_t delta = first > last ? first - last : last - first;
            if (retried || delta <= BEMF_CAL_STABLE_DELTA) {
                break;
            }
            /* Not settled yet (a heavy rotor/flywheel accelerating, or the
             * first-step boost still coasting down): re-measure this point
             * after one more settle window instead of saving a biased
             * fraction. Bounded to one retry per step. */
            retried = true;
            for (uint32_t elapsed = 0; complete && elapsed < BEMF_CAL_SETTLE_MS;
                 elapsed += BEMF_CAL_SAMPLE_MS) {
                vTaskDelay(pdMS_TO_TICKS(BEMF_CAL_SAMPLE_MS));
                complete = bemf_cal_sample_apply(generation, duty, NULL);
            }
        } while (complete);
        if (complete && n == BEMF_CAL_SAMPLES) {
            for (uint32_t j = 0; j < n; ++j) {
                sum += samples[j];
            }
            cal.speed[i] = spd;
            cal.frac[i] = (uint16_t)(sum / n);
            portENTER_CRITICAL(&s_output_mux);
            if (generation == s_output_generation && !s_cal_cancelled) {
                s_cal_step = (uint8_t)(i + 1U);
                s_cal_measured = (uint8_t)(i + 1U);
                s_cal_run_speed[i] = spd;
                s_cal_run_frac[i] = cal.frac[i];
            } else {
                complete = false;
            }
            portEXIT_CRITICAL(&s_output_mux);
        }
    }
    motor_bemf_profile_t profile = {0};
    profile.mode = MOTOR_BEMF_MODE_FAST;
    profile.planned_points = BEMF_CAL_POINTS;
    profile.measured_points = cal.count;
    profile.quality = 100U;
    bemf_cal_finish(generation, &cal, complete, &profile, false);
}

static bool bemf_cal_job_ok(uint32_t generation)
{
    portENTER_CRITICAL(&s_output_mux);
    bool ok = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
    portEXIT_CRITICAL(&s_output_mux);
    return ok;
}

static bool bemf_cal_publish(uint32_t generation, uint8_t step, uint8_t total,
                             uint8_t measured, const uint8_t *spd,
                             const uint16_t *frac, uint8_t count)
{
    bool ok;
    if (count > MOTOR_BEMF_CAL_MAX_POINTS) count = MOTOR_BEMF_CAL_MAX_POINTS;
    portENTER_CRITICAL(&s_output_mux);
    ok = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
    if (ok) {
        s_cal_step = step;
        s_cal_total = total;
        s_cal_measured = measured;
        for (uint8_t i = 0; i < count; ++i) {
            s_cal_run_speed[i] = spd[i];
            s_cal_run_frac[i] = frac[i];
        }
    }
    portEXIT_CRITICAL(&s_output_mux);
    return ok;
}

static esp_err_t bemf_cal_point_duty(uint8_t spd, uint32_t *duty, uint32_t *boost)
{
    if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = settings_cv_snapshot(s_tick_cv);
    if (err == ESP_OK) {
        s_tick_cv_active = true;
        *duty = speed_duty(spd);
        uint8_t bs = (uint8_t)(spd + 12U > 126U ? 126U : spd + 12U);
        if (bs < BEMF_CAL_BOOST_SPEED) bs = BEMF_CAL_BOOST_SPEED;
        *boost = speed_duty(bs);
        s_tick_cv_active = false;
    }
    xSemaphoreGive(s_control_mutex);
    return err;
}

static bool bemf_cal_set_target(uint32_t generation, uint8_t spd)
{
    bool ok;
    portENTER_CRITICAL(&s_output_mux);
    ok = generation == s_output_generation && !s_cal_cancelled && !s_inhibited;
    if (ok) {
        s_target_speed = spd;
        s_target_forward = true;
    }
    portEXIT_CRITICAL(&s_output_mux);
    return ok;
}

static bool bemf_cal_run_boost(uint32_t generation, uint32_t duty,
                               const motor_bemf_cal_config_t *cfg)
{
    if (cfg->boost_ms == 0U) return true;
    if (!bemf_cal_sample_apply(generation, duty, NULL)) return false;
    for (uint32_t elapsed = 0; elapsed < cfg->boost_ms; elapsed += cfg->interval_ms) {
        vTaskDelay(pdMS_TO_TICKS(cfg->interval_ms));
        if (!bemf_cal_sample_apply(generation, duty, NULL)) return false;
    }
    return true;
}

static bemf_cal_pt_t bemf_cal_attempt(uint32_t generation, uint32_t duty,
                                      const motor_bemf_cal_config_t *cfg,
                                      uint16_t *frac, uint8_t *quality,
                                      uint16_t *settle)
{
    uint16_t settle_min = cfg->settle_ms;
    for (uint8_t attempt = 0;; ++attempt) {
        bemf_cal_pt_t st = bemf_cal_read_point(generation, duty, cfg, settle_min,
                                               frac, quality, settle);
        if (st != BEMF_CAL_PT_OK) return st;
        if (*quality >= BEMF_CAL_MIN_QUALITY || attempt >= cfg->retries) return st;
        uint16_t next = (uint16_t)(settle_min + cfg->settle_ms / 2U);
        if (next <= settle_min || next > cfg->settle_max_ms) return st;
        settle_min = next;
    }
}

static void bemf_cal_insert_point(uint8_t *spd, uint16_t *frac, uint8_t *quality,
                                  uint16_t *settle, uint16_t *duty, uint8_t *count,
                                  uint8_t s, uint16_t f, uint8_t q, uint16_t st,
                                  uint16_t du)
{
    if (*count >= MOTOR_BEMF_CAL_MAX_POINTS) return;
    uint8_t i = *count;
    while (i > 0U && spd[i - 1U] > s) {
        spd[i] = spd[i - 1U];
        frac[i] = frac[i - 1U];
        quality[i] = quality[i - 1U];
        settle[i] = settle[i - 1U];
        duty[i] = duty[i - 1U];
        --i;
    }
    spd[i] = s;
    frac[i] = f;
    quality[i] = q;
    settle[i] = st;
    duty[i] = du;
    ++(*count);
    for (uint8_t k = 1; k < *count; ++k) {
        if (frac[k] < frac[k - 1U]) frac[k] = frac[k - 1U];
    }
}

static bool bemf_cal_attempted(const uint8_t *attempted, uint8_t n, uint8_t s)
{
    for (uint8_t i = 0; i < n; ++i) {
        if (attempted[i] == s) return true;
    }
    return false;
}

typedef struct {
    uint8_t s1, s2;
    uint16_t f1, f2;
    uint8_t q1, q2;
    uint32_t score;
} bemf_cal_interval_t;

static uint8_t bemf_cal_build_extra(const uint8_t *spd, const uint16_t *frac,
                                    const uint8_t *quality, uint8_t n,
                                    const uint8_t *attempted, uint8_t att_n,
                                    uint8_t target, uint8_t *out)
{
    if (n == 0U) return 0;
    uint8_t want = target > n ? (uint8_t)(target - n) : 0U;
    if (want > (uint8_t)(BEMF_CAL_MAX_PLAN - n)) {
        want = (uint8_t)(BEMF_CAL_MAX_PLAN - n);
    }
    if (want == 0U) return 0;

    bemf_cal_interval_t iv[MOTOR_BEMF_CAL_MAX_POINTS + 1U];
    uint8_t ivn = 0;
    if (spd[0] > 12U) {
        iv[ivn++] = (bemf_cal_interval_t){0, spd[0], 0, frac[0], 100, quality[0], 0};
    }
    for (uint8_t i = 1; i < n; ++i) {
        iv[ivn++] = (bemf_cal_interval_t){spd[i - 1U], spd[i], frac[i - 1U], frac[i],
                                          quality[i - 1U], quality[i], 0};
    }
    uint32_t ceiling_frac = (uint32_t)(s_bemf_max_fraction * (float)BEMF_CAL_FRAC_SCALE);
    if (spd[n - 1U] < 120U && frac[n - 1U] < ceiling_frac * 80U / 100U) {
        iv[ivn++] = (bemf_cal_interval_t){spd[n - 1U], 126U, frac[n - 1U], frac[n - 1U],
                                          quality[n - 1U], 100U, 0};
    }

    uint32_t prev_slope = 0;
    bool have_slope = false;
    for (uint8_t i = 0; i < ivn; ++i) {
        uint8_t d = (uint8_t)(iv[i].s2 - iv[i].s1);
        if (d < 3U) {
            iv[i].score = 0;
            continue;
        }
        uint32_t slope = (uint32_t)(iv[i].f2 - iv[i].f1) * BEMF_CAL_FRAC_SCALE / d;
        uint32_t curvature = 0;
        if (have_slope) {
            curvature = slope > prev_slope ? slope - prev_slope : prev_slope - slope;
        }
        prev_slope = slope;
        have_slope = true;
        iv[i].score = (uint32_t)d * 8U + (uint32_t)(iv[i].f2 - iv[i].f1) * 2U +
                      (uint32_t)(200U - iv[i].q1 - iv[i].q2) + curvature / 4U;
        if (iv[i].s1 == 0U) iv[i].score += 60U;
        if (iv[i].s2 == 126U) iv[i].score += 30U;
    }

    uint8_t out_n = 0;
    while (out_n < want) {
        uint8_t best = 0xFF;
        for (uint8_t i = 0; i < ivn; ++i) {
            if (iv[i].score == 0U) continue;
            if (best == 0xFF || iv[i].score > iv[best].score) best = i;
        }
        if (best == 0xFF) break;
        iv[best].score = 0;
        uint8_t d = (uint8_t)(iv[best].s2 - iv[best].s1);
        uint8_t mid = (uint8_t)(iv[best].s1 + d / 2U);
        bool found = false;
        uint8_t chosen = 0;
        for (uint8_t off = 0; off <= d / 2U && !found; ++off) {
            uint8_t cand[2] = { (uint8_t)(mid + off), (uint8_t)(mid - off) };
            for (uint8_t k = 0; k < 2U; ++k) {
                uint8_t c = cand[k];
                if (c <= iv[best].s1 || c >= iv[best].s2) continue;
                if (bemf_cal_attempted(attempted, att_n, c)) continue;
                bool dup = false;
                for (uint8_t j = 0; j < out_n; ++j) {
                    if (out[j] == c) { dup = true; break; }
                }
                if (dup) continue;
                chosen = c;
                found = true;
                break;
            }
        }
        if (!found) continue;
        out[out_n++] = chosen;
    }
    /* Ascending measurement order keeps the motor's speed changes small. */
    for (uint8_t i = 1; i < out_n; ++i) {
        uint8_t v = out[i];
        uint8_t j = i;
        while (j > 0U && out[j - 1U] > v) {
            out[j] = out[j - 1U];
            --j;
        }
        out[j] = v;
    }
    return out_n;
}

static void bemf_cal_adaptive_task(void *arg)
{
    uint32_t generation = (uint32_t)(uintptr_t)arg;
    motor_bemf_cal_config_t cfg = s_cal_job.cfg;
    uint8_t plan[BEMF_CAL_MAX_PLAN];
    uint8_t plan_count = s_cal_job.plan_count;
    memcpy(plan, s_cal_job.plan, plan_count);

    settings_bemf_cal_t cal = {0};
    motor_bemf_profile_t prof = {0};
    prof.mode = (uint8_t)cfg.mode;
    prof.planned_points = cfg.points;

    uint8_t r_spd[MOTOR_BEMF_CAL_MAX_POINTS] = {0};
    uint16_t r_frac[MOTOR_BEMF_CAL_MAX_POINTS] = {0};
    uint8_t r_quality[MOTOR_BEMF_CAL_MAX_POINTS] = {0};
    uint16_t r_settle[MOTOR_BEMF_CAL_MAX_POINTS] = {0};
    uint16_t r_duty[MOTOR_BEMF_CAL_MAX_POINTS] = {0};
    uint8_t r_n = 0;
    uint8_t attempted[MOTOR_BEMF_CAL_MAX_POINTS] = {0};
    uint8_t att_n = 0;
    uint32_t quality_sum = 0;
    uint32_t settle_sum = 0;
    uint8_t stalled = 0;
    uint8_t ceiling_speed = 0;
    bool complete = true;

    if (!bemf_cal_rest_check(generation)) {
        complete = false;
    }
    if (complete && !bemf_cal_publish(generation, 0, cfg.points, 0, r_spd, r_frac, 0)) {
        complete = false;
    }

    for (uint8_t i = 0; complete && i < plan_count; ++i) {
        uint8_t spd = plan[i];
        if (!bemf_cal_job_ok(generation)) {
            complete = false;
            break;
        }
        uint32_t duty = 0;
        uint32_t boost = 0;
        esp_err_t derr = bemf_cal_point_duty(spd, &duty, &boost);
        if (derr != ESP_OK) {
            portENTER_CRITICAL(&s_output_mux);
            if (generation == s_output_generation) {
                bemf_cal_worker_fail_locked(MOTOR_BEMF_CAL_ERR_CONTROL, derr);
            }
            portEXIT_CRITICAL(&s_output_mux);
            complete = false;
            break;
        }
        if (!bemf_cal_set_target(generation, spd)) {
            complete = false;
            break;
        }
        if (att_n < MOTOR_BEMF_CAL_MAX_POINTS) attempted[att_n++] = spd;
        if (i == 0U && cfg.boost_ms > 0U) {
            if (!bemf_cal_run_boost(generation, boost, &cfg)) {
                complete = false;
                break;
            }
        }

        uint16_t frac = 0;
        uint8_t quality = 0;
        uint16_t settle = 0;
        bemf_cal_pt_t res = bemf_cal_attempt(generation, duty, &cfg, &frac, &quality, &settle);
        if (res == BEMF_CAL_PT_FAIL) {
            complete = false;
            break;
        }
        if (res == BEMF_CAL_PT_CEILING) {
            ceiling_speed = spd;
            break;
        }
        if (res == BEMF_CAL_PT_STALL) {
            if (cfg.retries > 0U && cfg.boost_ms > 0U) {
                if (!bemf_cal_run_boost(generation, boost, &cfg)) {
                    complete = false;
                    break;
                }
                res = bemf_cal_attempt(generation, duty, &cfg, &frac, &quality, &settle);
                if (res == BEMF_CAL_PT_FAIL) {
                    complete = false;
                    break;
                }
                if (res == BEMF_CAL_PT_CEILING) {
                    ceiling_speed = spd;
                    break;
                }
            }
        }
        if (res == BEMF_CAL_PT_OK) {
            bemf_cal_insert_point(r_spd, r_frac, r_quality, r_settle, r_duty, &r_n,
                                  spd, frac, quality, settle, (uint16_t)duty);
            quality_sum += quality;
            settle_sum += settle;
        } else if (res == BEMF_CAL_PT_STALL) {
            ++stalled;
        }
        if (!bemf_cal_publish(generation, r_n, cfg.points, r_n,
                              r_spd, r_frac, r_n)) {
            complete = false;
            break;
        }
    }

    /* Second adaptive pass: spend the remaining point budget where the
     * measured curve is steep, curved or low-quality. */
    if (complete && ceiling_speed == 0U && cfg.adaptive && r_n > 0U && r_n < cfg.points) {
        uint8_t extra[MOTOR_BEMF_CAL_MAX_POINTS];
        uint8_t extra_n = bemf_cal_build_extra(r_spd, r_frac, r_quality, r_n,
                                               attempted, att_n, cfg.points, extra);
        for (uint8_t e = 0; complete && e < extra_n; ++e) {
            uint8_t spd = extra[e];
            if (r_n >= MOTOR_BEMF_CAL_MAX_POINTS) break;
            if (!bemf_cal_job_ok(generation)) {
                complete = false;
                break;
            }
            uint32_t duty = 0;
            uint32_t boost = 0;
            esp_err_t derr = bemf_cal_point_duty(spd, &duty, &boost);
            if (derr != ESP_OK) {
                portENTER_CRITICAL(&s_output_mux);
                if (generation == s_output_generation) {
                    bemf_cal_worker_fail_locked(MOTOR_BEMF_CAL_ERR_CONTROL, derr);
                }
                portEXIT_CRITICAL(&s_output_mux);
                complete = false;
                break;
            }
            if (!bemf_cal_set_target(generation, spd)) {
                complete = false;
                break;
            }
            if (att_n < MOTOR_BEMF_CAL_MAX_POINTS) attempted[att_n++] = spd;

            uint16_t frac = 0;
            uint8_t quality = 0;
            uint16_t settle = 0;
            bemf_cal_pt_t res = bemf_cal_attempt(generation, duty, &cfg, &frac, &quality, &settle);
            if (res == BEMF_CAL_PT_FAIL) {
                complete = false;
                break;
            }
            if (res == BEMF_CAL_PT_CEILING) {
                ceiling_speed = spd;
                break;
            }
            if (res == BEMF_CAL_PT_STALL) {
                if (cfg.retries > 0U && cfg.boost_ms > 0U) {
                    if (!bemf_cal_run_boost(generation, boost, &cfg)) {
                        complete = false;
                        break;
                    }
                    res = bemf_cal_attempt(generation, duty, &cfg, &frac, &quality, &settle);
                    if (res == BEMF_CAL_PT_FAIL) {
                        complete = false;
                        break;
                    }
                    if (res == BEMF_CAL_PT_CEILING) {
                        ceiling_speed = spd;
                        break;
                    }
                }
            }
            if (res == BEMF_CAL_PT_OK) {
                bemf_cal_insert_point(r_spd, r_frac, r_quality, r_settle, r_duty, &r_n,
                                      spd, frac, quality, settle, (uint16_t)duty);
                quality_sum += quality;
                settle_sum += settle;
            } else if (res == BEMF_CAL_PT_STALL) {
                ++stalled;
            }
            if (!bemf_cal_publish(generation, r_n, cfg.points, r_n,
                                  r_spd, r_frac, r_n)) {
                complete = false;
                break;
            }
        }
    }

    if (complete) {
        if (r_n < BEMF_CAL_MIN_PLAN) {
            portENTER_CRITICAL(&s_output_mux);
            if (generation == s_output_generation && !s_cal_cancelled) {
                bemf_cal_worker_fail_locked(MOTOR_BEMF_CAL_ERR_CURVE, ESP_ERR_INVALID_STATE);
            }
            portEXIT_CRITICAL(&s_output_mux);
            complete = false;
        } else {
            cal.count = r_n;
            for (uint8_t i = 0; i < r_n; ++i) {
                uint16_t f = r_frac[i];
                if (i > 0U && f < cal.frac[i - 1U]) f = cal.frac[i - 1U];
                cal.speed[i] = r_spd[i];
                cal.frac[i] = f;
            }
        }
    }

    prof.measured_points = r_n;
    prof.stalled_points = stalled;
    prof.ceiling_speed = ceiling_speed;
    prof.quality = r_n > 0U ? (uint8_t)(quality_sum / r_n) : 0U;
    prof.settle_ms_avg = r_n > 0U ? (uint16_t)(settle_sum / r_n) : cfg.settle_ms;
    bemf_cal_finish(generation, &cal, complete, &prof, cfg.apply_pid);
}

static esp_err_t bemf_cal_admit(const bemf_cal_job_t *job, void (*task)(void *),
                                uint32_t stack)
{
    /* The motor tick owns this mutex for its BEMF coast window (~1 ms). A
     * zero-time take made an operator start fail sporadically and be reported
     * as "calibration running"; wait one tick instead. */
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(MOTOR_TICK_MS)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_output_mux);
    if (!s_init || s_inhibited || s_feedback_fault || s_cal_active || s_cal_config_busy ||
        s_cal_task != NULL || !s_bemf_adc_ready ||
        s_bemf_mutex == NULL || s_control_mutex == NULL || s_rail_mv <= 300U ||
        esp_timer_get_time() - s_rail_update_us >= FEEDBACK_FRESH_US ||
        s_target_speed != 0U || s_applied_speed != 0U ||
        (s_applied_state & 0xFFU) != 0U || s_last_duty != 0U) {
        portEXIT_CRITICAL(&s_output_mux);
        xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = ++s_output_generation;
    if (job != NULL) {
        s_cal_job = *job;
    } else {
        memset(&s_cal_job, 0, sizeof(s_cal_job));
        s_cal_job.cfg.mode = MOTOR_BEMF_MODE_FAST;
        s_cal_job.plan_count = BEMF_CAL_POINTS;
        memcpy(s_cal_job.plan, BEMF_CAL_FAST_PLAN, BEMF_CAL_POINTS);
    }
    memset(&s_cal_profile, 0, sizeof(s_cal_profile));
    s_cal_profile.mode = (uint8_t)s_cal_job.cfg.mode;
    s_cal_profile.planned_points = job != NULL ? s_cal_job.cfg.points
                                               : (uint8_t)BEMF_CAL_POINTS;
    s_cal_active = true;
    s_cal_cancelled = false;
    s_stop_requested = false;
    s_reset_requested = true;
    s_cal_step = 0;
    s_cal_total = job != NULL ? s_cal_job.cfg.points : (uint8_t)BEMF_CAL_POINTS;
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
    /* Do not publish a task handle after creation: the worker may already have
     * completed on the other core. The pre-create reservation owns lifecycle. */
    if (xTaskCreate(task, "bemf_cal", stack, (void *)(uintptr_t)generation, 6, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_output_mux);
        bemf_cal_stop_locked(MOTOR_BEMF_CAL_ERR_START, ESP_ERR_NO_MEM);
        s_cal_active = false;
        portEXIT_CRITICAL(&s_output_mux);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void motor_bemf_cal_default_config(motor_bemf_cal_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MOTOR_BEMF_MODE_AUTO;
    cfg->apply_pid = false;
    bemf_cal_apply_mode_defaults(cfg);
}

esp_err_t motor_bemf_cal_start_ex(const motor_bemf_cal_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bemf_cal_job_t job;
    if (!bemf_cal_build_job(&job, cfg)) {
        return ESP_ERR_INVALID_ARG;
    }
    return bemf_cal_admit(&job, bemf_cal_adaptive_task, 4096);
}

esp_err_t motor_bemf_cal_start(void)
{
    /* Legacy entry point: keep the original 10-point/400 ms profile for
     * compatibility. Use motor_bemf_cal_start_ex() for adaptive profiles. */
    return bemf_cal_admit(NULL, bemf_cal_task, 3072);
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
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    portENTER_CRITICAL(&s_output_mux);
    info->active = s_cal_active;
    info->step = s_cal_step;
    info->total = s_cal_total;
    info->valid = s_cal_valid;
    info->run_id = s_cal_run_id;
    info->result = s_cal_result;
    info->reason = s_cal_reason;
    info->error_code = s_cal_error_code;
    info->profile = s_cal_profile;
    if (s_cal_active) {
        /* Report the points measured so far while running. */
        uint8_t done = s_cal_measured;
        if (s_cal_step > done) done = s_cal_step;
        if (done > MOTOR_BEMF_CAL_MAX_POINTS) done = MOTOR_BEMF_CAL_MAX_POINTS;
        if (done > s_cal_total) done = s_cal_total;
        info->count = done;
        for (uint8_t i = 0; i < done; ++i) {
            info->speed[i] = s_cal_run_speed[i];
            info->frac[i] = s_cal_run_frac[i];
        }
    } else {
        /* Active points: stored calibration, or the firmware base curve. */
        info->count = s_cal_count;
        for (uint8_t i = 0; i < s_cal_count && i < MOTOR_BEMF_CAL_MAX_POINTS; ++i) {
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

/* Admit one BEMF configuration change (NVS clear/reload) without taking the
 * motor control mutex across flash operations: a >100 ms erase would starve the
 * 10 ms control tick and trip the independent motor-stall watchdog. */
static bool cal_config_begin(void)
{
    if (s_control_mutex == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_output_mux);
    bool admitted = !s_cal_active && !s_cal_config_busy;
    if (admitted) {
        s_cal_config_busy = true;
    }
    portEXIT_CRITICAL(&s_output_mux);
    return admitted;
}

esp_err_t motor_bemf_cal_clear(void)
{
    if (!cal_config_begin()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = settings_bemf_cal_clear();
    if (err == ESP_OK) {
        bemf_cal_reload();
    }
    portENTER_CRITICAL(&s_output_mux);
    s_cal_config_busy = false;
    if (err == ESP_OK) {
        s_cal_result = MOTOR_BEMF_CAL_IDLE;
        s_cal_reason = MOTOR_BEMF_CAL_ERR_NONE;
        s_cal_error_code = ESP_OK;
        s_cal_step = 0;
        s_cal_total = BEMF_CAL_POINTS;
        s_cal_measured = 0;
        memset(&s_cal_profile, 0, sizeof(s_cal_profile));
    }
    portEXIT_CRITICAL(&s_output_mux);
    return err;
}

esp_err_t motor_bemf_cal_reload(void)
{
    if (!cal_config_begin()) {
        return ESP_ERR_INVALID_STATE;
    }
    bemf_cal_reload();
    portENTER_CRITICAL(&s_output_mux);
    s_cal_config_busy = false;
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

void motor_set_bemf_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_output_mux);
    s_bemf_enabled = enabled;
    s_reset_requested = true;
    if (!enabled) {
        s_feedback_fault = false;
        s_feedback_reset_requested = true;
    }
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
    if (d == NULL) {
        return;
    }
    memset(d, 0, sizeof(*d));
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_output_mux);
    d->rail_mv = s_rail_mv;
    d->bemf1_mv = s_bemf1_mv;
    d->bemf2_mv = s_bemf2_mv;
    d->bemf_filtered_mv = (uint32_t)s_bemf_filtered;
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
    d->target = (int32_t)s_last_target;
    portEXIT_CRITICAL(&s_output_mux);
    xSemaphoreGive(s_control_mutex);
}

esp_err_t motor_bemf_adc_dump_checked(uint16_t *b1_raw, uint16_t *b2_raw, uint16_t *rail_raw)
{
    if (b1_raw != NULL) {
        *b1_raw = 0;
    }
    if (b2_raw != NULL) {
        *b2_raw = 0;
    }
    if (rail_raw != NULL) {
        *rail_raw = 0;
    }
    if (b1_raw == NULL && b2_raw == NULL && rail_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bemf_adc_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!motor_bemf_lock()) {
        return ESP_ERR_TIMEOUT;
    }
    int b1 = b1_raw != NULL ? motor_adc_read_raw(PIN_BEMF1) : 0;
    int b2 = b2_raw != NULL ? motor_adc_read_raw(PIN_BEMF2) : 0;
    int rail = rail_raw != NULL ? motor_adc_read_raw(PIN_RAIL_SENSE) : 0;
    motor_bemf_unlock();
    if (b1 < 0 || b2 < 0 || rail < 0) {
        return ESP_FAIL;
    }
    if (b1_raw != NULL) { *b1_raw = (uint16_t)b1; }
    if (b2_raw != NULL) { *b2_raw = (uint16_t)b2; }
    if (rail_raw != NULL) { *rail_raw = (uint16_t)rail; }
    return ESP_OK;
}

/* Diagnostic: force a coast window and read the BEMF terminals (in mV). */
void motor_bemf_coast_read(uint16_t *b1_mv, uint16_t *b2_mv)
{
    if (b1_mv != NULL) {
        *b1_mv = 0;
    }
    if (b2_mv != NULL) {
        *b2_mv = 0;
    }
    if (!s_bemf_adc_ready || s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_output_mux);
    uint32_t generation = s_output_generation;
    uint32_t duty = s_last_duty;
    bool forward = (s_applied_state & 0x100U) != 0U;
    bool busy = s_cal_active;
    portEXIT_CRITICAL(&s_output_mux);
    if (busy) {
        xSemaphoreGive(s_control_mutex);
        return;
    }
    /* bemf_sample_window() locks the ADC mutex itself. */
    bool fresh = bemf_sample_window();
    if (fresh && b1_mv != NULL) {
        *b1_mv = (uint16_t)s_bemf1_mv;
    }
    if (fresh && b2_mv != NULL) {
        *b2_mv = (uint16_t)s_bemf2_mv;
    }
    /* Restore the last drive so repeated diagnostic polling does not leave the
     * bridge coasted between motor ticks. */
    (void)apply_pwm(duty, forward, generation);
    xSemaphoreGive(s_control_mutex);
}

void motor_bemf_base_info(motor_bemf_base_info_t *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    uint8_t n = (BEMF_CAL_BASE.count > MOTOR_BEMF_BASE_MAX_POINTS)
                    ? MOTOR_BEMF_BASE_MAX_POINTS : BEMF_CAL_BASE.count;
    info->count = n;
    for (uint8_t i = 0; i < n; ++i) {
        info->speed[i] = BEMF_CAL_BASE.speed[i];
        info->frac[i] = BEMF_CAL_BASE.frac[i];
    }
    info->start_frac = (uint16_t)(BEMF_TARGET_START_FRAC * (float)BEMF_CAL_FRAC_SCALE);
    info->full_frac = (n > 0) ? BEMF_CAL_BASE.frac[n - 1]
                              : (uint16_t)(BEMF_TARGET_FULL_FRAC * (float)BEMF_CAL_FRAC_SCALE);
}

esp_err_t motor_set_rail_voltage_mv(uint32_t rail_mv)
{
    portENTER_CRITICAL(&s_output_mux);
    s_rail_mv = rail_mv;
    s_rail_update_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_output_mux);
    return ESP_OK;
}

bool motor_bemf_lock(void)
{
    if (s_bemf_mutex != NULL) {
        return xSemaphoreTake(s_bemf_mutex, pdMS_TO_TICKS(10)) == pdTRUE;
    }
    return false;
}

void motor_bemf_unlock(void)
{
    if (s_bemf_mutex != NULL) {
        xSemaphoreGive(s_bemf_mutex);
    }
}
