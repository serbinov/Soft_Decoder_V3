#include "motor.h"

#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
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
#define KICK_TICKS   (120 / MOTOR_TICK_MS)
/* Hi-Z coast window for the back-EMF sample. IN1=IN2=0 coasts the DRV8870 and
 * the free-wheel current dissipates through the body diodes in ~10-50 us.
 * 1 ms gives a generous window after the coast applies; the driver keeps the
 * outputs High-Z even after entering low-power sleep (~1 ms). */
#define BEMF_SETTLE_US 1000

static bool s_init;

/* Timestamp of the last completed motor tick, for the safety task's aliveness
 * check: if the motor task hangs (e.g. inside a flash operation), the bridge is
 * coasted from safety_task because motor_stop() would never be applied. */
static volatile int64_t s_last_tick_us;

static uint8_t s_target_speed;
static bool s_target_forward;

static uint8_t s_applied_speed;
static bool s_applied_forward;

/* Published copy of the applied (speed, direction) pair for cross-task readers:
 * a single 16-bit store/load avoids a torn pair while reversing (REV-M1).
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
static uint32_t s_bemf1_mv;
static uint32_t s_bemf2_mv;
static float s_bemf_filtered;
static bool s_bemf_valid;
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
#define BEMF_CAL_MIN_VALID   50U /* fraction x1024 at full speed for validity */

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
static uint8_t s_cal_count;
static uint8_t s_cal_speed[BEMF_CAL_POINTS];
static uint16_t s_cal_frac[BEMF_CAL_POINTS];

/* Interpolated rail-fraction curve indexed by applied speed 0..126. */
static uint16_t s_cal_frac_table[127];
static bool s_cal_valid;
static uint32_t s_last_duty;
static float s_last_error;
static float s_last_integral;
static float s_last_corr;
static float s_last_target;
static bool s_last_pid_ok;

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
    (void)settings_cv_read(54, &kp);
    (void)settings_cv_read(55, &ki);
    (void)settings_cv_read(56, &kd);
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
    (void)settings_cv_read(2, &cv2);
    (void)settings_cv_read(5, &cv5);
    (void)settings_cv_read(6, &cv6);
    (void)settings_cv_read(29, &cv29);

    if ((cv29 & 0x10U) != 0U) {
        uint32_t pos = (uint32_t)speed * 28U;
        uint8_t i = (uint8_t)(pos / 128U);
        uint8_t f = (uint8_t)(pos % 128U);
        if (i >= 27U) {
            uint8_t cv94 = 0;
            (void)settings_cv_read(94, &cv94);
            return (uint32_t)cv94 * 4U;
        }
        uint8_t a = 0, b = 0;
        (void)settings_cv_read(67 + i, &a);
        (void)settings_cv_read(68 + i, &b);
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

static void apply_pwm(uint32_t duty, bool forward)
{
    /* Update the channel that must turn off before the one that turns on, so
     * IN1 and IN2 are never high at the same time (even briefly) on a reversal.
     * For the DRV8870 both-high is brake, but a discrete H-bridge would see
     * shoot-through. */
    if (duty == 0U) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, 0);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1);
    } else if (forward) {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, duty);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
    } else {
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, duty);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1);
    }
}

/* Shared ADC1 (adc_oneshot). One unit serves the BEMF sample window, the rail
 * sense and the diagnostics dump; channels are 12-bit at 12 dB attenuation.
 * ESP32-S3 ADC1 channels 0..9 map to GPIO1..10. */
#define ADC1_GPIO_MIN 1
#define ADC1_GPIO_MAX 10

static esp_err_t adc_unit_ensure(void)
{
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

static void bemf_sample_window(void)
{
    if (!motor_bemf_lock()) {
        return;
    }
    /* Coast the DRV8870 for the sample window: IN1=IN2=LOW (0% duty on both
     * LEDC channels) = "Coast", both half-bridge outputs high-Z. The motor
     * free-wheels through the body diodes: one terminal is pulled to GND and
     * the other to VM. After the L/R freewheel decays, the GND-side terminal
     * stays near 0 V and the other terminal carries the back-EMF directly, so
     * it is read on the higher sense node without any 2x scaling.
     * IN1=IN2=HIGH would be BRAKE (both outputs shorted to GND) and would
     * zero the measurement. */
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, 0);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1);
    esp_rom_delay_us(BEMF_SETTLE_US);
    int raw1 = motor_adc_read_raw(PIN_BEMF1);
    int raw2 = motor_adc_read_raw(PIN_BEMF2);
    motor_bemf_unlock();
    if (raw1 >= 0) {
        s_bemf1_mv = (uint32_t)raw1 * 3100U / 4095U;
    }
    if (raw2 >= 0) {
        s_bemf2_mv = (uint32_t)raw2 * 3100U / 4095U;
    }
}

static void bemf_update(void)
{
    if (!s_bemf_adc_ready) {
        s_bemf_filtered = 0.0f;
        s_bemf_valid = false;
        return;
    }
    /* Back-EMF is the potential difference across the motor: bEMF =
     * V_OUT1 - V_OUT2. In the coast window one terminal rests near GND while
     * the other carries the back-EMF, so the magnitude of the ADC difference
     * is the pure back-EMF (divider-scaled), independent of direction. */
    int32_t diff = (int32_t)s_bemf1_mv - (int32_t)s_bemf2_mv;
    uint32_t mag = (diff < 0) ? (uint32_t)(-diff) : (uint32_t)diff;
    bool clamped = (s_rail_mv > 0U) && ((float)mag > (float)s_rail_mv * 0.85f);
    if (!clamped) {
        s_bemf_filtered = 0.85f * s_bemf_filtered + 0.15f * (float)mag;
        s_bemf_valid = true;
    } else {
        /* Still in freewheel clamp: reading is the rail, not the back-EMF. */
        s_bemf_filtered *= 0.95f;
        s_bemf_valid = false;
    }
}

/* Build the per-speed rail-fraction lookup (speed 0..126) from the measured
 * calibration points by linear interpolation. A curve is only trusted if the
 * motor actually produced measurable back-EMF at full speed. */
static void bemf_cal_build_table(const settings_bemf_cal_t *cal)
{
    s_cal_valid = false;
    memset(s_cal_frac_table, 0, sizeof(s_cal_frac_table));
    if (cal == NULL || cal->count < 2U) {
        return;
    }
    uint8_t prev_speed = 0;
    uint16_t prev_frac = 0;
    for (uint8_t i = 0; i < cal->count; ++i) {
        uint8_t spd = cal->speed[i];
        uint16_t frac = cal->frac[i];
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
            s_cal_frac_table[s] = (uint16_t)f;
        }
        prev_speed = spd;
        prev_frac = frac;
    }
    for (uint8_t s = (uint8_t)(prev_speed + 1U); s <= 126U; ++s) {
        s_cal_frac_table[s] = prev_frac;
    }
    if (s_cal_frac_table[126] > BEMF_CAL_MIN_VALID) {
        s_cal_valid = true;
    }
}

/* Remember the active calibration points (for the info/dump API) and rebuild
 * the per-speed lookup table from them. */
static void bemf_cal_apply(const settings_bemf_cal_t *cal)
{
    uint8_t n = (cal != NULL) ? cal->count : 0;
    if (n > BEMF_CAL_POINTS) {
        n = BEMF_CAL_POINTS;
    }
    s_cal_count = n;
    for (uint8_t i = 0; i < n; ++i) {
        s_cal_speed[i] = cal->speed[i];
        s_cal_frac[i] = cal->frac[i];
    }
    bemf_cal_build_table(cal);
}

/* (Re)load the stored calibration, falling back to the firmware base curve
 * when none was saved. Used at boot and after a serial/API calibration set. */
static void bemf_cal_reload(void)
{
    settings_bemf_cal_t cal;
    if (settings_bemf_cal_load(&cal) != ESP_OK) {
        cal = BEMF_CAL_BASE;
    }
    bemf_cal_apply(&cal);
}

static void motor_tick(void)
{
    uint8_t cv3 = 0, cv4 = 0;
    (void)settings_cv_read(3, &cv3);
    (void)settings_cv_read(4, &cv4);

    uint8_t target = s_target_speed;
    uint8_t applied = s_applied_speed;
    bool reversing = (s_applied_forward != s_target_forward) && (applied > 0U);

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
    } else if (s_cal_active) {
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
        s_applied_forward = s_target_forward;
    }
    applied_publish(s_applied_speed, s_applied_forward);

    uint32_t duty_base = speed_duty(applied);
    uint32_t duty_final = duty_base;

    /* Kickstart (CV65): boost the duty for a short burst when starting. */
    bool kicking = false;
    if (s_cal_active) {
        s_was_stopped = false;
        s_kick_left = 0;
    } else if (applied == 0U) {
        s_was_stopped = true;
        s_kick_left = 0;
    } else if (s_was_stopped) {
        s_was_stopped = false;
        uint8_t cv65 = 0;
        (void)settings_cv_read(65, &cv65);
        if (cv65 > 0U) {
            uint8_t cv29 = 0;
            (void)settings_cv_read(29, &cv29);
            uint32_t base;
            if ((cv29 & 0x10U) != 0U) {
                /* Table mode: base the kick on the table's top point (CV94),
                 * not on the unused CV5. */
                uint8_t cv94 = 0;
                (void)settings_cv_read(94, &cv94);
                base = (uint32_t)cv94 * 4U;
                if (base == 0U) {
                    base = LEDC_MAX;
                }
            } else {
                uint8_t cv5 = 0;
                (void)settings_cv_read(5, &cv5);
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
    if (s_bemf_enabled && !s_cal_active && s_bemf_adc_ready && applied > 0U) {
        bemf_sample_window();
    }
    if (s_bemf_enabled && !s_cal_active) {
        bemf_update();
    }

    s_pid_reload++;
    if (s_pid_reload >= 100U) {
        load_pid();
        s_pid_reload = 0;
    }

    bool pid_ok = s_bemf_enabled && !s_cal_active && s_bemf_adc_ready &&
                  s_bemf_valid && s_rail_mv > 300U && applied > 0U;
    s_last_pid_ok = pid_ok;
    if (pid_ok) {
        /* PID target back-EMF (as a fraction of the rail voltage): use the
         * stored/motor calibration curve when it is valid so the no-load RPM
         * is held under load, otherwise fall back to the fixed linear ramp. */
        float frac;
        if (s_cal_valid) {
            frac = (float)s_cal_frac_table[applied] / (float)BEMF_CAL_FRAC_SCALE;
        } else {
            frac = BEMF_TARGET_START_FRAC +
                   ((float)applied / 126.0f) * (BEMF_TARGET_FULL_FRAC - BEMF_TARGET_START_FRAC);
        }
        float target_bemf = frac * (float)s_rail_mv;
        float error = target_bemf - s_bemf_filtered;
        float derivative = error - s_pid_prev_error;
        s_pid_prev_error = error;

        float corr = s_pid_kp * error + s_pid_ki * s_pid_integral + s_pid_kd * derivative;
        s_last_error = error;
        s_last_integral = s_pid_integral;
        s_last_corr = corr;
        s_last_target = target_bemf;
        float duty_f = (float)duty_base + corr * 0.4f;
        /* Never let the regulator pull duty below half the open-loop value. */
        if (duty_f < (float)duty_base * 0.5f) {
            duty_f = (float)duty_base * 0.5f;
        }
        if (kicking && duty_f < (float)duty_final) {
            duty_f = (float)duty_final;
        }
        float clamped = clampf(duty_f, 0.0f, (float)LEDC_MAX);

        /* Conditional integration (anti-windup): only accumulate the integral
         * while the output is not saturated in the direction of the error. */
        bool saturating = (clamped >= (float)LEDC_MAX && error > 0.0f) ||
                          (clamped <= (float)duty_base * 0.5f && error < 0.0f);
        if (!saturating) {
            s_pid_integral = clampf(s_pid_integral + error, -20000.0f, 20000.0f);
        }
        duty_final = (uint32_t)clamped;
    } else if (applied == 0U) {
        s_pid_integral = 0.0f;
        s_pid_prev_error = 0.0f;
    } else {
        /* Measurement temporarily unavailable while running: decay instead of
         * resetting so load compensation is not lost. Decay the previous error
         * too, otherwise the first valid tick after the gap sees a huge
         * derivative (kick) and spikes the duty. */
        s_pid_integral *= 0.95f;
        s_pid_prev_error *= 0.95f;
    }

    /* During calibration the cal task owns the bridge directly, so the motor
     * task must not overwrite the duty between samples. */
    if (!s_cal_active) {
        if (s_stop_requested) {
            /* A fail-safe stop from another task must win over this tick's
             * ramped value: force the bridge off and drop the stale state
             * (REV-M3). */
            s_stop_requested = false;
            duty_final = 0;
            s_applied_speed = 0;
            s_applied_forward = true;
            s_ramp_acc = 0;
            s_kick_left = 0;
            s_kick_duty = 0;
            s_was_stopped = true;
            applied_publish(0, true);
        }
        apply_pwm(duty_final, s_applied_forward);
        s_last_duty = duty_final;
    }
    s_last_tick_us = esp_timer_get_time();
}

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_motor_iter_cap;

static void motor_task(void *arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t iters = 0;
    while (s_motor_iter_cap == 0U || iters < s_motor_iter_cap) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(MOTOR_TICK_MS));
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

    s_bemf_mutex = xSemaphoreCreateMutex();
    /* PIN_BEMF1=GPIO4 -> ADC1_CH3, PIN_BEMF2=GPIO5 -> ADC1_CH4,
     * PIN_RAIL_SENSE=GPIO6 -> ADC1_CH5. Enable BEMF only once every channel is
     * configured: otherwise the sample window would coast the bridge for 1 ms
     * every tick while reading -1, silently losing ~10 % duty (REV-M2). */
    esp_err_t adc_err = motor_adc_config_channel(PIN_BEMF1);
    if (adc_err == ESP_OK) {
        adc_err = motor_adc_config_channel(PIN_BEMF2);
    }
    if (adc_err == ESP_OK) {
        adc_err = motor_adc_config_channel(PIN_RAIL_SENSE);
    }
    if (adc_err == ESP_OK) {
        s_bemf_adc_ready = true;
    } else {
        ESP_LOGW(TAG, "ADC init failed (%s): BEMF disabled", esp_err_to_name(adc_err));
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

    if (xTaskCreate(motor_task, "motor", 3072, NULL, 7, NULL) != pdPASS) {
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
    s_target_speed = speed128;
    s_target_forward = forward;
    return ESP_OK;
}

void motor_stop(void)
{
    (void)motor_set_speed(0, true);
}

/* Fail-safe stop: force the bridge off immediately, without waiting for the
 * motor task to apply the target. Safe to call from any task, including when
 * the motor task is suspected to be stuck. */
void motor_emergency_stop(void)
{
    /* Raise the flag before touching the bridge so motor_tick() cannot re-drive
     * a nonzero duty for this stop (REV-M3). */
    s_stop_requested = true;
    apply_pwm(0, true);
    s_target_speed = 0;
    s_applied_speed = 0;
    s_applied_forward = true;
    applied_publish(0, true);
    s_ramp_acc = 0;
    s_was_stopped = true;
    s_kick_left = 0;
    s_kick_duty = 0;
    s_pid_integral = 0.0f;
    s_pid_prev_error = 0.0f;
    s_last_duty = 0;
}

/* Timestamp (us) of the last completed motor tick; 0 before the first tick. */
int64_t motor_last_tick_us(void)
{
    return s_last_tick_us;
}

void motor_get_status(uint8_t *out_speed128, bool *out_forward)
{
    if (out_speed128 != NULL) {
        *out_speed128 = s_target_speed;
    }
    if (out_forward != NULL) {
        *out_forward = s_target_forward;
    }
}

void motor_get_applied_speed(uint8_t *out_speed128, bool *out_forward)
{
    /* One atomic 16-bit load: the pair is always self-consistent (REV-M1). */
    uint16_t st = s_applied_state;
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
static void bemf_cal_task(void *arg)
{
    (void)arg;
    s_cal_active = true;
    s_cal_step = 0;
    for (uint8_t i = 0; i < BEMF_CAL_POINTS; ++i) {
        s_cal_step = (uint8_t)(i + 1);
        uint8_t spd = BEMF_CAL_SPEEDS[i];
        uint32_t duty = speed_duty(spd);
        (void)motor_set_speed(spd, true);
        apply_pwm(duty, true);
        s_last_duty = duty;
        vTaskDelay(pdMS_TO_TICKS(BEMF_CAL_SETTLE_MS));

        uint32_t sum = 0;
        uint32_t n = 0;
        for (uint8_t j = 0; j < BEMF_CAL_SAMPLES; ++j) {
            /* bemf_sample_window() locks the ADC mutex itself. */
            bemf_sample_window();
            bemf_update();
            apply_pwm(duty, true);
            s_last_duty = duty;
            if (s_bemf_valid && s_rail_mv > 300U) {
                sum += (uint32_t)s_bemf_filtered;
                ++n;
            }
            vTaskDelay(pdMS_TO_TICKS(BEMF_CAL_SAMPLE_MS));
        }
        s_cal_speed[i] = spd;
        if (n > 0U && s_rail_mv > 0U) {
            uint32_t frac = (sum / n) * BEMF_CAL_FRAC_SCALE / s_rail_mv;
            s_cal_frac[i] = (frac > BEMF_CAL_FRAC_SCALE) ? (uint16_t)BEMF_CAL_FRAC_SCALE
                                                          : (uint16_t)frac;
        } else {
            s_cal_frac[i] = 0;
        }
    }
    apply_pwm(0, true);
    motor_stop();
    /* Force the applied state to a true stop before releasing the bridge:
     * otherwise the CV4 ramp would ramp down from the last calibration step
     * (126) with real PWM right after the calibration finishes. */
    s_applied_speed = 0;
    s_applied_forward = true;
    applied_publish(0, true);
    s_ramp_acc = 0;
    s_was_stopped = true;
    s_kick_left = 0;
    s_kick_duty = 0;
    s_pid_integral = 0.0f;
    s_pid_prev_error = 0.0f;
    s_bemf_filtered = 0.0f;
    s_bemf_valid = false;
    s_last_duty = 0;
    s_cal_active = false;

    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = BEMF_CAL_POINTS;
    for (uint8_t i = 0; i < BEMF_CAL_POINTS; ++i) {
        cal.speed[i] = s_cal_speed[i];
        cal.frac[i] = s_cal_frac[i];
    }
    (void)settings_bemf_cal_save(&cal);
    bemf_cal_apply(&cal);

    s_cal_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t motor_bemf_cal_start(void)
{
    if (s_cal_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(bemf_cal_task, "bemf_cal", 3072, NULL, 6, &s_cal_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void motor_bemf_cal_info(motor_bemf_cal_info_t *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    info->active = s_cal_active;
    info->step = s_cal_step;
    info->total = BEMF_CAL_POINTS;
    info->valid = s_cal_valid;
    if (s_cal_active) {
        /* Report the points measured so far while running. */
        uint8_t done = s_cal_step >= BEMF_CAL_POINTS ? BEMF_CAL_POINTS : s_cal_step;
        info->count = done;
        for (uint8_t i = 0; i < done; ++i) {
            info->speed[i] = s_cal_speed[i];
            info->frac[i] = s_cal_frac[i];
        }
    } else {
        /* Active points: stored calibration, or the firmware base curve. */
        info->count = s_cal_count;
        for (uint8_t i = 0; i < s_cal_count && i < MOTOR_BEMF_CAL_MAX_POINTS; ++i) {
            info->speed[i] = s_cal_speed[i];
            info->frac[i] = s_cal_frac[i];
        }
        settings_bemf_cal_t cal;
        info->stored = (settings_bemf_cal_load(&cal) == ESP_OK);
    }
}

esp_err_t motor_bemf_cal_clear(void)
{
    if (s_cal_active) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = settings_bemf_cal_clear();
    bemf_cal_reload();
    return err;
}

esp_err_t motor_bemf_cal_reload(void)
{
    if (s_cal_active) {
        return ESP_ERR_INVALID_STATE;
    }
    bemf_cal_reload();
    return ESP_OK;
}

void motor_set_bemf_enabled(bool enabled)
{
    s_bemf_enabled = enabled;
    if (!enabled) {
        /* Drop any accumulated correction so re-enabling starts clean. */
        s_pid_integral = 0.0f;
        s_pid_prev_error = 0.0f;
        s_bemf_filtered = 0.0f;
        s_bemf_valid = false;
    }
}

bool motor_get_bemf_enabled(void)
{
    return s_bemf_enabled;
}

void motor_bemf_diag(motor_bemf_diag_t *d)
{
    if (d == NULL) {
        return;
    }
    d->rail_mv = s_rail_mv;
    d->bemf1_mv = s_bemf1_mv;
    d->bemf2_mv = s_bemf2_mv;
    d->bemf_filtered_mv = (uint32_t)s_bemf_filtered;
    d->bemf_valid = s_bemf_valid;
    d->cal_valid = s_cal_valid;
    d->cal_active = s_cal_active;
    d->cal_step = s_cal_step;
    d->applied_speed = s_applied_speed;
    d->duty = s_last_duty;
    d->error = (int32_t)s_last_error;
    d->integral = (int32_t)s_last_integral;
    d->corr = (int32_t)s_last_corr;
    d->pid_ok = s_last_pid_ok;
    d->target = (int32_t)s_last_target;
}

void motor_bemf_adc_dump(uint16_t *b1_raw, uint16_t *b2_raw, uint16_t *rail_raw)
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
    if (!s_bemf_adc_ready || !motor_bemf_lock()) {
        return;
    }
    if (b1_raw != NULL) {
        int r = motor_adc_read_raw(PIN_BEMF1);
        if (r >= 0) {
            *b1_raw = (uint16_t)r;
        }
    }
    if (b2_raw != NULL) {
        int r = motor_adc_read_raw(PIN_BEMF2);
        if (r >= 0) {
            *b2_raw = (uint16_t)r;
        }
    }
    if (rail_raw != NULL) {
        int r = motor_adc_read_raw(PIN_RAIL_SENSE);
        if (r >= 0) {
            *rail_raw = (uint16_t)r;
        }
    }
    motor_bemf_unlock();
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
    if (!s_bemf_adc_ready) {
        return;
    }
    /* bemf_sample_window() locks the ADC mutex itself. */
    bemf_sample_window();
    if (b1_mv != NULL) {
        *b1_mv = (uint16_t)s_bemf1_mv;
    }
    if (b2_mv != NULL) {
        *b2_mv = (uint16_t)s_bemf2_mv;
    }
    /* Restore the last drive so repeated diagnostic polling does not leave the
     * bridge coasted between motor ticks. */
    apply_pwm(s_last_duty, s_applied_forward);
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
    s_rail_mv = rail_mv;
    return ESP_OK;
}

bool motor_bemf_lock(void)
{
    if (s_bemf_mutex != NULL) {
        return xSemaphoreTake(s_bemf_mutex, pdMS_TO_TICKS(10)) == pdTRUE;
    }
    return true;
}

void motor_bemf_unlock(void)
{
    if (s_bemf_mutex != NULL) {
        xSemaphoreGive(s_bemf_mutex);
    }
}
