#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Pre-include every header motor.c pulls in (directly or transitively) so the
 * `static` strip below cannot change their linkage rules. */
#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pinmap.h"
#include "settings.h"
#include "bemf_cal_base.h"

/* White-box: expose the motor's static helpers (speed_duty, bemf_update,
 * bemf_cal_build_table, motor_tick, load_pid, ...). */
#define static
#include "../../components/motor/src/motor.c"
#undef static

#include "../../test_libs/teststubs/stubs.c"

/* ---- settings.c substitute ------------------------------------------- */
/* motor.c talks to the CV store and the stored BEMF calibration. A tiny
 * in-memory model keeps this suite free of the NVS/FreeRTOS machinery. */
static uint8_t g_cv[SETTINGS_CV_COUNT + 1];
static settings_bemf_cal_t g_cal;
static bool g_cal_valid;

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
    g_cal = *cal;
    g_cal_valid = true;
    return ESP_OK;
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

void setUp(void)
{
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

    TEST_ASSERT_EQUAL(ESP_OK, motor_init());
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

static void test_bemf_cal_table_enforces_monotonic(void)
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

    TEST_ASSERT_TRUE(s_cal_valid);
    TEST_ASSERT_EQUAL_UINT16(200, s_cal_frac_table[64]);
    TEST_ASSERT_EQUAL_UINT16(725, s_cal_frac_table[126]);
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
    mock_adc_raw[PIN_BEMF1 - 1] = 4095;
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    uint16_t b1 = 0, b2 = 0;
    motor_bemf_coast_read(&b1, &b2);
    TEST_ASSERT_EQUAL_UINT16(3100, b1);
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
    TEST_ASSERT_TRUE(s_cal_valid);

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

static void test_bemf_cal_apply_caps_count(void)
{
    settings_bemf_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.count = BEMF_CAL_POINTS + 2U;
    for (int i = 0; i < cal.count; ++i) {
        cal.speed[i] = (uint8_t)(10 + i * 5);
        cal.frac[i] = (uint16_t)(100 + i * 50);
    }
    bemf_cal_apply(&cal);
    TEST_ASSERT_EQUAL_UINT8(BEMF_CAL_POINTS, s_cal_count);
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

static void test_bemf_cal_task_runs(void)
{
    mock_adc_raw[PIN_BEMF1 - 1] = 1000; /* ~757 mV */
    mock_adc_raw[PIN_BEMF2 - 1] = 950;  /* ~719 mV */
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(1000));
    s_bemf_valid = false;
    s_bemf_filtered = 0.0f;
    bemf_cal_task(NULL); /* runs to completion (delay/delete are stubs) */
    TEST_ASSERT_FALSE(s_cal_active);
    TEST_ASSERT_TRUE(g_cal_valid);
    /* Calibration must leave the motor truly stopped, not at step 126. */
    TEST_ASSERT_EQUAL_UINT8(0, s_applied_speed);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_integral);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s_pid_prev_error);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);

    /* Rail too low -> n stays 0 -> frac cleared. */
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(0));
    bemf_cal_task(NULL);
    TEST_ASSERT_FALSE(s_cal_active);
}

static void test_motor_bemf_cal_start_states(void)
{
    s_cal_task = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());

    s_cal_task = (TaskHandle_t)1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, motor_bemf_cal_start());
    s_cal_task = NULL;

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
    TEST_ASSERT_TRUE(motor_bemf_lock());
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

/* While the BEMF measurement is unavailable the previous error must decay too,
 * otherwise the first valid tick sees a huge derivative and spikes the duty. */
static void test_motor_tick_pid_unavailable_decays_derivative(void)
{
    motor_set_bemf_enabled(true);
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
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 95.0f, s_pid_integral);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 95.0f, s_pid_prev_error);
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_speed_duty_zero);
    RUN_TEST(test_speed_duty_linear_curve);
    RUN_TEST(test_speed_duty_non_monotonic_no_wrap);
    RUN_TEST(test_speed_duty_28_point_table);
    RUN_TEST(test_bemf_cal_table_interpolation);
    RUN_TEST(test_bemf_cal_table_invalid_when_low);
    RUN_TEST(test_bemf_cal_table_enforces_monotonic);
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
    RUN_TEST(test_bemf_cal_apply_caps_count);
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
    RUN_TEST(test_motor_tick_pid_unavailable_decays_derivative);
    return UNITY_END();
}
