#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "driver/ledc.h"
#include "driver/mcpwm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pinmap.h"

/* White-box: expose ch_duty / ch_step / s_ch / s_gamma. */
#define static
#include "../../components/auxio/src/auxio.c"
#undef static

#include "../../test_libs/teststubs/stubs.c"

void setUp(void)
{
    mock_ledc_update_count = 0;
    memset(mock_ledc_duty, 0, sizeof(mock_ledc_duty));
    for (int u = 0; u < 2; ++u) {
        for (int t = 0; t < 3; ++t) {
            for (int o = 0; o < 2; ++o) {
                mock_mcpwm_duty[u][t][o] = -1.0f;
            }
        }
    }
    mock_mutex_create_fail = 0;
    mock_sem_take_fail = 0;
    mock_task_create_ok = 1;
    s_fx_iter_cap = 0;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_init());
}

void tearDown(void)
{
}

/* ---- gamma table ---- */

static void test_gamma_table_endpoints_and_monotonic(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, s_gamma[0]);
    TEST_ASSERT_EQUAL_UINT8(255, s_gamma[255]);
    for (int i = 1; i < 256; ++i) {
        TEST_ASSERT_TRUE(s_gamma[i] >= s_gamma[i - 1]);
    }
}

/* ---- ch_duty: steady / disabled ---- */

static void test_ch_duty_steady(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_STEADY;
    ch->pwm_on = 200;
    ch->pwm_off = 0;
    ch->period_ms = 800;
    TEST_ASSERT_EQUAL_UINT8(200, ch_duty(ch, 0, 1234));

    ch->enabled = false;
    TEST_ASSERT_EQUAL_UINT8(0, ch_duty(ch, 0, 1234));
}

/* ---- ch_duty: effects ---- */

static void test_ch_duty_incandescent(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_INCANDESCENT;
    ch->pwm_on = 255;
    ch->pwm_off = 0;
    ch->cur = 255;
    TEST_ASSERT_EQUAL_UINT8(255, ch_duty(ch, 0, 0));
    ch->cur = 0;
    TEST_ASSERT_EQUAL_UINT8(0, ch_duty(ch, 0, 0));

    /* Disabled but hot: keeps a dim glow while it cools. */
    ch->enabled = false;
    ch->cur = 128;
    TEST_ASSERT_TRUE(ch_duty(ch, 0, 0) > 0);
}

static void test_ch_duty_firebox(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_FIREBOX;
    ch->cur = 77;
    TEST_ASSERT_EQUAL_UINT8(77, ch_duty(ch, 0, 0));

    ch->enabled = false;
    TEST_ASSERT_EQUAL_UINT8(0, ch_duty(ch, 0, 0));
}

static void test_ch_duty_mars_triangle(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_MARS;
    ch->pwm_on = 255;
    ch->pwm_off = 0;
    ch->period_ms = 800;

    uint8_t at_start = ch_duty(ch, 0, 0);
    uint8_t at_peak = ch_duty(ch, 0, 400);
    TEST_ASSERT_EQUAL_UINT8(127, at_start); /* mid + gamma[0] */
    TEST_ASSERT_EQUAL_UINT8(255, at_peak);
    TEST_ASSERT_TRUE(at_peak > at_start);
}

static void test_ch_duty_ditch_alternates(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_DITCH;
    ch->pwm_on = 200;
    ch->pwm_off = 50;
    ch->period_ms = 1000;

    /* Adjacent outputs are complementary (channel parity phase offset). */
    TEST_ASSERT_EQUAL_UINT8(200, ch_duty(ch, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(50, ch_duty(ch, 1, 0));
    TEST_ASSERT_EQUAL_UINT8(50, ch_duty(ch, 0, 600));
    TEST_ASSERT_EQUAL_UINT8(200, ch_duty(ch, 1, 600));
}

static void test_ch_duty_beacon_decay(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_BEACON;
    ch->pwm_on = 100;
    ch->pwm_off = 0;
    ch->period_ms = 900;

    TEST_ASSERT_EQUAL_UINT8(100, ch_duty(ch, 0, 0));      /* full flash */
    TEST_ASSERT_EQUAL_UINT8(20, ch_duty(ch, 0, 899));     /* floor = max/5 */
}

static void test_ch_duty_strobe_double_flash(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_STROBE;
    ch->pwm_on = 255;
    ch->pwm_off = 0;
    ch->period_ms = 1000;

    TEST_ASSERT_EQUAL_UINT8(255, ch_duty(ch, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(0, ch_duty(ch, 0, 50));
    TEST_ASSERT_EQUAL_UINT8(255, ch_duty(ch, 0, 110));
    TEST_ASSERT_EQUAL_UINT8(0, ch_duty(ch, 0, 500));
}

static void test_ch_duty_short_period_falls_back_to_max(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_MARS; /* any effect with period < 20 ms */
    ch->pwm_on = 200;
    ch->pwm_off = 0;
    ch->period_ms = 10;
    TEST_ASSERT_EQUAL_UINT8(200, ch_duty(ch, 0, 0));
}

/* ---- ch_step: stateful effects ---- */

static void test_ch_step_incandescent_warm_and_cool(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->mode = AUXIO_EFFECT_INCANDESCENT;
    ch->pwm_on = 255;
    ch->enabled = true;
    ch->cur = 0;
    (void)ch_step(ch, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(24, ch->cur); /* warm-up step */

    ch->cur = 255;
    ch->enabled = false;
    (void)ch_step(ch, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(243, ch->cur); /* cool-down step */

    /* Reaching the target snaps to it. */
    ch->enabled = true;
    ch->cur = 250;
    (void)ch_step(ch, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(255, ch->cur);
}

static void test_ch_step_firebox_flicker_range(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->mode = AUXIO_EFFECT_FIREBOX;
    ch->pwm_on = 255;
    ch->pwm_off = 0;
    ch->enabled = true;
    ch->cur = 0;
    ch->fx_next_ms = 0;

    (void)ch_step(ch, 0, 1000);
    TEST_ASSERT_TRUE(ch->cur >= 102); /* 40 % floor of 255 */
    TEST_ASSERT_TRUE(ch->cur <= 255);
    TEST_ASSERT_TRUE(ch->fx_next_ms >= 1030 && ch->fx_next_ms <= 1100);

    ch->enabled = false;
    (void)ch_step(ch, 0, 2000);
    TEST_ASSERT_EQUAL_UINT8(0, ch->cur);
}

/* ---- configuration API / validation ---- */

static void test_auxio_set_output_applies_duty(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_output(0, true, 100));
    TEST_ASSERT_TRUE(s_ch[0].enabled);
    TEST_ASSERT_EQUAL_UINT8(100, s_ch[0].pwm_on);
    TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_STEADY, s_ch[0].mode);
    /* Channel 0 is LEDC channel 2. */
    TEST_ASSERT_EQUAL_UINT32(100, mock_ledc_duty[LEDC_CHANNEL_2]);

    /* AUX5..AUX7 are MCPWM-backed. */
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_output(8, true, 255));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, mock_mcpwm_duty[0][MCPWM_TIMER_1][MCPWM_OPR_A]);
}

static void test_auxio_set_enabled_validation(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, auxio_set_enabled(AUXIO_CH_COUNT, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, auxio_set_output(AUXIO_CH_COUNT, true, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(AUXIO_CH_COUNT, true, 1, 0, AUXIO_EFFECT_STEADY, 800));
}

static void test_auxio_set_effect_validation(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 1, 0, AUXIO_EFFECT_STEADY, 99));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 1, 0, AUXIO_EFFECT_STEADY,
                                       AUXIO_PERIOD_MAX_MS + 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 1, 0, AUXIO_EFFECT_COUNT, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_effect(0, true, 10, 0, AUXIO_EFFECT_MARS, 800));
    TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_MARS, s_ch[0].mode);
    TEST_ASSERT_EQUAL_UINT16(800, s_ch[0].period_ms);
}

static void test_auxio_config_validation_and_state(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_config(0, 1, 0, AUXIO_EFFECT_STEADY, 19));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_config(0, 1, 0, AUXIO_EFFECT_COUNT, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 42, 0, AUXIO_EFFECT_BEACON, 900));
    TEST_ASSERT_EQUAL_UINT8(42, s_ch[0].pwm_on);
    TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_BEACON, s_ch[0].mode);
    /* auxio_config must not change the enabled state. */
    TEST_ASSERT_FALSE(s_ch[0].enabled);
}

/* ---- remaining branches ---- */

static void test_ch_step_steady_resets_cur(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_STEADY;
    ch->pwm_on = 200;
    ch->pwm_off = 0;
    ch->cur = 77;
    (void)ch_step(ch, 0, 0);
    TEST_ASSERT_EQUAL_UINT8(0, ch->cur);
}

static void test_ch_duty_unknown_mode_default(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = (auxio_effect_t)99; /* out of range -> switch default */
    ch->pwm_on = 222;
    ch->pwm_off = 0;
    ch->period_ms = 800;
    TEST_ASSERT_EQUAL_UINT8(222, ch_duty(ch, 0, 0));
}

static void test_apply_now_and_api_timeouts(void)
{
    s_ch[0].enabled = true;
    s_ch[0].mode = AUXIO_EFFECT_STEADY;
    s_ch[0].pwm_on = 100;
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, apply_now(0));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_set_enabled(0, true));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_set_output(0, true, 10));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      auxio_set_effect(0, true, 10, 0, AUXIO_EFFECT_MARS, 800));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_config(0, 10, 0, AUXIO_EFFECT_BEACON, 800));
}

static void test_apply_now_null_lock(void)
{
    SemaphoreHandle_t saved = s_lock;
    s_lock = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, apply_now(0));
    s_lock = saved;
}

static void test_auxio_set_enabled_applies(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, true));
    TEST_ASSERT_TRUE(s_ch[0].enabled);
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, false));
    TEST_ASSERT_FALSE(s_ch[0].enabled);
}

static void test_auxio_init_mutex_fail(void)
{
    mock_mutex_create_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, auxio_init());
    mock_mutex_create_fail = 0;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_init()); /* restore s_lock */
}

static void test_auxio_init_task_fail(void)
{
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, auxio_init());
    mock_task_create_ok = 1;
}

static void test_effect_task_runs_once(void)
{
    s_fx_iter_cap = 1;
    mock_ledc_update_count = 0;
    effect_task(NULL);
    TEST_ASSERT_EQUAL_INT(6, mock_ledc_update_count); /* 6 LEDC channels */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gamma_table_endpoints_and_monotonic);
    RUN_TEST(test_ch_duty_steady);
    RUN_TEST(test_ch_duty_incandescent);
    RUN_TEST(test_ch_duty_firebox);
    RUN_TEST(test_ch_duty_mars_triangle);
    RUN_TEST(test_ch_duty_ditch_alternates);
    RUN_TEST(test_ch_duty_beacon_decay);
    RUN_TEST(test_ch_duty_strobe_double_flash);
    RUN_TEST(test_ch_duty_short_period_falls_back_to_max);
    RUN_TEST(test_ch_step_incandescent_warm_and_cool);
    RUN_TEST(test_ch_step_firebox_flicker_range);
    RUN_TEST(test_auxio_set_output_applies_duty);
    RUN_TEST(test_auxio_set_enabled_validation);
    RUN_TEST(test_auxio_set_effect_validation);
    RUN_TEST(test_auxio_config_validation_and_state);
    RUN_TEST(test_ch_step_steady_resets_cur);
    RUN_TEST(test_ch_duty_unknown_mode_default);
    RUN_TEST(test_apply_now_and_api_timeouts);
    RUN_TEST(test_apply_now_null_lock);
    RUN_TEST(test_auxio_set_enabled_applies);
    RUN_TEST(test_auxio_init_mutex_fail);
    RUN_TEST(test_auxio_init_task_fail);
    RUN_TEST(test_effect_task_runs_once);
    return UNITY_END();
}
