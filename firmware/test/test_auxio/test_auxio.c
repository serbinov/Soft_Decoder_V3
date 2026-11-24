#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "driver/ledc.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pinmap.h"

/* White-box: expose ch_duty / ch_step / s_ch / s_gamma.
 *
 * The driver entry points are renamed for the auxio translation unit only
 * (same pattern as test_audio) so this suite can inject LEDC/MCPWM/GPIO
 * failures without modifying the shared host stubs. The wrappers below call
 * the real stubs when no fault is armed. */
static esp_err_t auxio_test_gpio_config(const gpio_config_t *cfg);
static esp_err_t auxio_test_gpio_set_level(gpio_num_t gpio, uint32_t level);
static esp_err_t auxio_test_ledc_timer_config(const ledc_timer_config_t *timer_conf);
static esp_err_t auxio_test_ledc_channel_config(const ledc_channel_config_t *ledc_conf);
static esp_err_t auxio_test_mcpwm_new_timer(const mcpwm_timer_config_t *config,
                                            mcpwm_timer_handle_t *ret_timer);
static esp_err_t auxio_test_mcpwm_new_operator(const mcpwm_operator_config_t *config,
                                               mcpwm_oper_handle_t *ret_oper);
static esp_err_t auxio_test_mcpwm_operator_connect_timer(mcpwm_oper_handle_t oper,
                                                         mcpwm_timer_handle_t timer);
static esp_err_t auxio_test_mcpwm_new_comparator(mcpwm_oper_handle_t oper,
                                                 const mcpwm_comparator_config_t *config,
                                                 mcpwm_cmpr_handle_t *ret_cmpr);
static esp_err_t auxio_test_mcpwm_new_generator(mcpwm_oper_handle_t oper,
                                                const mcpwm_generator_config_t *config,
                                                mcpwm_gen_handle_t *ret_gen);
static esp_err_t auxio_test_mcpwm_set_timer_event(mcpwm_gen_handle_t generator, int action);
static esp_err_t auxio_test_mcpwm_set_compare_event(mcpwm_gen_handle_t generator, int action);
static esp_err_t auxio_test_mcpwm_timer_enable(mcpwm_timer_handle_t timer);
static esp_err_t auxio_test_mcpwm_timer_start_stop(mcpwm_timer_handle_t timer, int command);

#define gpio_config auxio_test_gpio_config
#define gpio_set_level auxio_test_gpio_set_level
#define ledc_timer_config auxio_test_ledc_timer_config
#define ledc_channel_config auxio_test_ledc_channel_config
#define mcpwm_new_timer auxio_test_mcpwm_new_timer
#define mcpwm_new_operator auxio_test_mcpwm_new_operator
#define mcpwm_operator_connect_timer auxio_test_mcpwm_operator_connect_timer
#define mcpwm_new_comparator auxio_test_mcpwm_new_comparator
#define mcpwm_new_generator auxio_test_mcpwm_new_generator
#define mcpwm_generator_set_action_on_timer_event auxio_test_mcpwm_set_timer_event
#define mcpwm_generator_set_action_on_compare_event auxio_test_mcpwm_set_compare_event
#define mcpwm_timer_enable auxio_test_mcpwm_timer_enable
#define mcpwm_timer_start_stop auxio_test_mcpwm_timer_start_stop

#define static
#include "../../components/auxio/src/auxio.c"
#undef static

#undef gpio_config
#undef gpio_set_level
#undef ledc_timer_config
#undef ledc_channel_config
#undef mcpwm_new_timer
#undef mcpwm_new_operator
#undef mcpwm_operator_connect_timer
#undef mcpwm_new_comparator
#undef mcpwm_new_generator
#undef mcpwm_generator_set_action_on_timer_event
#undef mcpwm_generator_set_action_on_compare_event
#undef mcpwm_timer_enable
#undef mcpwm_timer_start_stop

#include "../../test_libs/teststubs/stubs.c"

/* ---- fault injection for driver calls ---- */

static esp_err_t s_gpio_config_err, s_gpio_level_err;
static int s_gpio_config_calls, s_gpio_level_calls, s_gpio_level_fail_at;
static gpio_config_t s_last_gpio_cfg;

static esp_err_t s_ledc_timer_err, s_ledc_channel_err;
static int s_ledc_channel_calls, s_ledc_channel_fail_at;

static esp_err_t s_mcpwm_new_timer_err;
static esp_err_t s_mcpwm_new_oper_err;
static int s_mcpwm_new_oper_calls, s_mcpwm_new_oper_fail_at;
static esp_err_t s_mcpwm_connect_err;
static int s_mcpwm_connect_calls, s_mcpwm_connect_fail_at;
static esp_err_t s_mcpwm_cmpr_err;
static int s_mcpwm_cmpr_calls, s_mcpwm_cmpr_fail_at;
static esp_err_t s_mcpwm_gen_err;
static int s_mcpwm_gen_calls, s_mcpwm_gen_fail_at;
static esp_err_t s_mcpwm_timer_event_err;
static int s_mcpwm_timer_event_calls, s_mcpwm_timer_event_fail_at;
static esp_err_t s_mcpwm_compare_event_err;
static int s_mcpwm_compare_event_calls, s_mcpwm_compare_event_fail_at;
static esp_err_t s_mcpwm_enable_err, s_mcpwm_start_stop_err;

static bool auxio_fault_hit(esp_err_t err, int *calls, int fail_at)
{
    ++*calls;
    return err != ESP_OK && (fail_at == 0 || *calls == fail_at);
}

static void auxio_reset_faults(void)
{
    s_gpio_config_err = s_gpio_level_err = ESP_OK;
    s_gpio_config_calls = s_gpio_level_calls = 0;
    s_gpio_level_fail_at = 0;
    s_ledc_timer_err = s_ledc_channel_err = ESP_OK;
    s_ledc_channel_calls = s_ledc_channel_fail_at = 0;
    s_mcpwm_new_timer_err = s_mcpwm_new_oper_err = s_mcpwm_connect_err = ESP_OK;
    s_mcpwm_new_oper_calls = s_mcpwm_new_oper_fail_at = 0;
    s_mcpwm_connect_calls = s_mcpwm_connect_fail_at = 0;
    s_mcpwm_cmpr_err = s_mcpwm_gen_err = ESP_OK;
    s_mcpwm_cmpr_calls = s_mcpwm_cmpr_fail_at = 0;
    s_mcpwm_gen_calls = s_mcpwm_gen_fail_at = 0;
    s_mcpwm_timer_event_err = s_mcpwm_compare_event_err = ESP_OK;
    s_mcpwm_timer_event_calls = s_mcpwm_timer_event_fail_at = 0;
    s_mcpwm_compare_event_calls = s_mcpwm_compare_event_fail_at = 0;
    s_mcpwm_enable_err = s_mcpwm_start_stop_err = ESP_OK;
    memset(&s_last_gpio_cfg, 0, sizeof(s_last_gpio_cfg));
}

static esp_err_t auxio_test_gpio_config(const gpio_config_t *cfg)
{
    ++s_gpio_config_calls;
    if (cfg != NULL) {
        s_last_gpio_cfg = *cfg;
    }
    return s_gpio_config_err != ESP_OK ? s_gpio_config_err : gpio_config(cfg);
}

static esp_err_t auxio_test_gpio_set_level(gpio_num_t gpio, uint32_t level)
{
    if (auxio_fault_hit(s_gpio_level_err, &s_gpio_level_calls, s_gpio_level_fail_at)) {
        return s_gpio_level_err;
    }
    return gpio_set_level(gpio, level);
}

static esp_err_t auxio_test_ledc_timer_config(const ledc_timer_config_t *timer_conf)
{
    return s_ledc_timer_err != ESP_OK ? s_ledc_timer_err : ledc_timer_config(timer_conf);
}

static esp_err_t auxio_test_ledc_channel_config(const ledc_channel_config_t *ledc_conf)
{
    if (auxio_fault_hit(s_ledc_channel_err, &s_ledc_channel_calls,
                        s_ledc_channel_fail_at)) {
        return s_ledc_channel_err;
    }
    return ledc_channel_config(ledc_conf);
}

static esp_err_t auxio_test_mcpwm_new_timer(const mcpwm_timer_config_t *config,
                                            mcpwm_timer_handle_t *ret_timer)
{
    return s_mcpwm_new_timer_err != ESP_OK ? s_mcpwm_new_timer_err
                                           : mcpwm_new_timer(config, ret_timer);
}

static esp_err_t auxio_test_mcpwm_new_operator(const mcpwm_operator_config_t *config,
                                               mcpwm_oper_handle_t *ret_oper)
{
    if (auxio_fault_hit(s_mcpwm_new_oper_err, &s_mcpwm_new_oper_calls,
                        s_mcpwm_new_oper_fail_at)) {
        return s_mcpwm_new_oper_err;
    }
    return mcpwm_new_operator(config, ret_oper);
}

static esp_err_t auxio_test_mcpwm_operator_connect_timer(mcpwm_oper_handle_t oper,
                                                         mcpwm_timer_handle_t timer)
{
    if (auxio_fault_hit(s_mcpwm_connect_err, &s_mcpwm_connect_calls,
                        s_mcpwm_connect_fail_at)) {
        return s_mcpwm_connect_err;
    }
    return mcpwm_operator_connect_timer(oper, timer);
}

static esp_err_t auxio_test_mcpwm_new_comparator(mcpwm_oper_handle_t oper,
                                                 const mcpwm_comparator_config_t *config,
                                                 mcpwm_cmpr_handle_t *ret_cmpr)
{
    if (auxio_fault_hit(s_mcpwm_cmpr_err, &s_mcpwm_cmpr_calls, s_mcpwm_cmpr_fail_at)) {
        return s_mcpwm_cmpr_err;
    }
    return mcpwm_new_comparator(oper, config, ret_cmpr);
}

static esp_err_t auxio_test_mcpwm_new_generator(mcpwm_oper_handle_t oper,
                                                const mcpwm_generator_config_t *config,
                                                mcpwm_gen_handle_t *ret_gen)
{
    if (auxio_fault_hit(s_mcpwm_gen_err, &s_mcpwm_gen_calls, s_mcpwm_gen_fail_at)) {
        return s_mcpwm_gen_err;
    }
    return mcpwm_new_generator(oper, config, ret_gen);
}

static esp_err_t auxio_test_mcpwm_set_timer_event(mcpwm_gen_handle_t generator, int action)
{
    if (auxio_fault_hit(s_mcpwm_timer_event_err, &s_mcpwm_timer_event_calls,
                        s_mcpwm_timer_event_fail_at)) {
        return s_mcpwm_timer_event_err;
    }
    return mcpwm_generator_set_action_on_timer_event(generator, action);
}

static esp_err_t auxio_test_mcpwm_set_compare_event(mcpwm_gen_handle_t generator, int action)
{
    if (auxio_fault_hit(s_mcpwm_compare_event_err, &s_mcpwm_compare_event_calls,
                        s_mcpwm_compare_event_fail_at)) {
        return s_mcpwm_compare_event_err;
    }
    return mcpwm_generator_set_action_on_compare_event(generator, action);
}

static esp_err_t auxio_test_mcpwm_timer_enable(mcpwm_timer_handle_t timer)
{
    return s_mcpwm_enable_err != ESP_OK ? s_mcpwm_enable_err
                                        : mcpwm_timer_enable(timer);
}

static esp_err_t auxio_test_mcpwm_timer_start_stop(mcpwm_timer_handle_t timer, int command)
{
    return s_mcpwm_start_stop_err != ESP_OK ? s_mcpwm_start_stop_err
                                            : mcpwm_timer_start_stop(timer, command);
}

void setUp(void)
{
    mock_ledc_update_count = 0;
    memset(mock_ledc_duty, 0, sizeof(mock_ledc_duty));
    mock_mcpwm_reset();
    mock_mutex_create_fail = 0;
    mock_sem_take_fail = 0;
    mock_task_create_ok = 1;
    s_fx_iter_cap = 0;
    auxio_reset_faults();
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

/* An odd period can make the Mars triangle reach half+1; the value must clamp
 * to 255 instead of wrapping through uint8_t. */
static void test_ch_duty_mars_odd_period_clamps(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_MARS;
    ch->pwm_on = 255;
    ch->pwm_off = 0;
    ch->period_ms = 101; /* odd: pos reaches half+1 at the sweep peak */

    uint8_t peak = ch_duty(ch, 0, 50);
    TEST_ASSERT_EQUAL_UINT8(255, peak);
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

static void test_ch_duty_strobe_scales_short_period(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_STROBE;
    ch->pwm_on = 255;
    ch->pwm_off = 0;
    ch->period_ms = 100; /* shorter than the 130 ms reference pattern */

    /* Windows scale to 23..0 ms and 76..100 ms, so both flashes are reachable
     * instead of the second one being clamped away. */
    TEST_ASSERT_EQUAL_UINT8(255, ch_duty(ch, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(255, ch_duty(ch, 0, 22));
    TEST_ASSERT_EQUAL_UINT8(0, ch_duty(ch, 0, 30));
    TEST_ASSERT_EQUAL_UINT8(0, ch_duty(ch, 0, 75));
    TEST_ASSERT_EQUAL_UINT8(255, ch_duty(ch, 0, 76));
    TEST_ASSERT_EQUAL_UINT8(255, ch_duty(ch, 0, 99));
}

static void test_ch_duty_steady_uses_pwm_on(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->enabled = true;
    ch->mode = AUXIO_EFFECT_STEADY;
    ch->pwm_on = 50;
    ch->pwm_off = 200; /* invalid via the API, but ch_duty must still use on */
    ch->period_ms = 800;
    TEST_ASSERT_EQUAL_UINT8(50, ch_duty(ch, 0, 0));
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

static void test_incandescent_scales_brightness_once(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->mode = AUXIO_EFFECT_INCANDESCENT;
    ch->pwm_on = 128;
    ch->enabled = true;
    uint8_t duty = 0;
    for (unsigned i = 0; i < 20; ++i) {
        duty = ch_step(ch, 0, i * 20U);
    }
    TEST_ASSERT_EQUAL_UINT8(255, ch->cur);
    TEST_ASSERT_EQUAL_UINT8(128, duty);
    ch->enabled = false;
    for (unsigned i = 0; i < 30; ++i) {
        duty = ch_step(ch, 0, 400U + i * 20U);
    }
    TEST_ASSERT_EQUAL_UINT8(0, duty);
}

static void test_firebox_initializes_at_large_uptime_and_wraps(void)
{
    auxio_ch_t *ch = &s_ch[0];
    ch->mode = AUXIO_EFFECT_FIREBOX;
    ch->pwm_on = 200;
    ch->enabled = true;
    uint32_t now = 0x90000000U;
    TEST_ASSERT_TRUE(ch_step(ch, 0, now) >= 80U);
    TEST_ASSERT_TRUE(ch->fx_scheduled);
    now = UINT32_MAX - 10U;
    ch->fx_scheduled = false;
    (void)ch_step(ch, 0, now);
    uint32_t next = ch->fx_next_ms;
    TEST_ASSERT_TRUE((uint32_t)(next - now) <= 100U);
    (void)ch_step(ch, 0, next);
    TEST_ASSERT_TRUE(ch->fx_next_ms != next);
    ch->enabled = false;
    (void)ch_step(ch, 0, next);
    TEST_ASSERT_FALSE(ch->fx_scheduled);
    ch->enabled = true;
    TEST_ASSERT_TRUE(ch_step(ch, 0, 0x90000000U) >= 80U);
}

/* ---- configuration API / validation ---- */

static void test_auxio_steady_output_applies_duty(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 100, 0, AUXIO_EFFECT_STEADY, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, true));
    TEST_ASSERT_TRUE(s_ch[0].enabled);
    TEST_ASSERT_EQUAL_UINT8(100, s_ch[0].pwm_on);
    TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_STEADY, s_ch[0].mode);
    /* Channel 0 is LEDC channel 2. */
    TEST_ASSERT_EQUAL_UINT32(100, mock_ledc_duty[LEDC_CHANNEL_2]);

    /* AUX5..AUX7 are MCPWM-backed. */
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(8, 255, 0, AUXIO_EFFECT_STEADY, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(8, true));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, mock_mcpwm_duty[2]);
}

static void test_auxio_set_enabled_validation(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, auxio_set_enabled(AUXIO_CH_COUNT, true));
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

static void test_auxio_off_above_on_rejected(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 50, 200, AUXIO_EFFECT_STEADY, 800));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_config(0, 50, 200, AUXIO_EFFECT_STEADY, 800));
    /* The rejected request must not have changed the previous configuration. */
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 100, 0, AUXIO_EFFECT_STEADY, 800));
    TEST_ASSERT_EQUAL_UINT8(100, s_ch[0].pwm_on);
    TEST_ASSERT_EQUAL_UINT8(0, s_ch[0].pwm_off);
}

static void test_strobe_period_below_task_resolution_rejected(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_config(0, 255, 0, AUXIO_EFFECT_STROBE,
                                   AUXIO_STROBE_MIN_PERIOD_MS - 1U));
    TEST_ASSERT_EQUAL(ESP_OK,
                      auxio_config(0, 255, 0, AUXIO_EFFECT_STROBE,
                                   AUXIO_STROBE_MIN_PERIOD_MS));
}

static void test_auxio_config_preserves_continuous_state(void)
{
    /* A lit incandescent output must keep warming/glowing across a level-only
     * reconfigure instead of being reset to dark. */
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 255, 0, AUXIO_EFFECT_INCANDESCENT, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, true));
    s_ch[0].cur = 200;
    mock_ledc_duty[LEDC_CHANNEL_2] = 0;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 128, 0, AUXIO_EFFECT_INCANDESCENT, 800));
    TEST_ASSERT_TRUE(s_ch[0].enabled);
    TEST_ASSERT_TRUE(s_ch[0].cur >= 200); /* warm-up continued, not restarted */
    TEST_ASSERT_TRUE(mock_ledc_duty[LEDC_CHANNEL_2] > 0);

    /* The firebox keeps its flicker sample and schedule when only the level is
     * reconfigured. */
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(1, 200, 0, AUXIO_EFFECT_FIREBOX, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(1, true));
    s_ch[1].cur = 150;
    s_ch[1].fx_scheduled = true;
    s_ch[1].fx_next_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()) + 100000U;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(1, 200, 0, AUXIO_EFFECT_FIREBOX, 800));
    TEST_ASSERT_TRUE(s_ch[1].enabled);
    TEST_ASSERT_EQUAL_UINT8(150, s_ch[1].cur);
    TEST_ASSERT_TRUE(s_ch[1].fx_scheduled);
}

static void test_auxio_config_clamps_firebox_to_on_level(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 255, 0, AUXIO_EFFECT_FIREBOX, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, true));
    s_ch[0].cur = 240;
    s_ch[0].fx_scheduled = true;
    s_ch[0].fx_next_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()) + 100000U;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 100, 0, AUXIO_EFFECT_FIREBOX, 800));
    TEST_ASSERT_EQUAL_UINT8(100, s_ch[0].cur);
    TEST_ASSERT_TRUE(ch_duty(&s_ch[0], 0, 0) <= 100);
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

static void test_auxio_get_enabled(void)
{
    bool en = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, auxio_get_enabled(AUXIO_CH_COUNT, &en));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, auxio_get_enabled(0, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, true));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_get_enabled(0, &en));
    TEST_ASSERT_TRUE(en);

    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_get_enabled(0, &en));
    mock_sem_take_fail = 0;

    SemaphoreHandle_t saved = s_lock;
    s_lock = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_get_enabled(0, &en));
    s_lock = saved;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_get_enabled(0, &en));
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

/* ---- auxio_boot_safe ---- */

static void test_auxio_boot_safe_drives_all_pins_low(void)
{
    uint64_t expected_mask = 0;
    for (int i = 0; i < AUXIO_CH_COUNT; ++i) {
        expected_mask |= (1ULL << s_gpios[i]);
    }

    auxio_reset_faults();
    mock_gpio_set_level_count = 0;
    s_gpio_level_calls = 0;

    TEST_ASSERT_EQUAL(ESP_OK, auxio_boot_safe());
    TEST_ASSERT_EQUAL_INT(1, s_gpio_config_calls);
    TEST_ASSERT_TRUE(expected_mask == s_last_gpio_cfg.pin_bit_mask);
    int bits = 0;
    for (uint64_t m = s_last_gpio_cfg.pin_bit_mask; m != 0U; m >>= 1) {
        bits += (int)(m & 1U);
    }
    TEST_ASSERT_EQUAL_INT(AUXIO_CH_COUNT, bits);
    TEST_ASSERT_EQUAL(GPIO_MODE_OUTPUT, s_last_gpio_cfg.mode);
    TEST_ASSERT_EQUAL(GPIO_PULLUP_DISABLE, s_last_gpio_cfg.pull_up_en);
    TEST_ASSERT_EQUAL(GPIO_PULLDOWN_ENABLE, s_last_gpio_cfg.pull_down_en);
    TEST_ASSERT_EQUAL(GPIO_INTR_DISABLE, s_last_gpio_cfg.intr_type);
    TEST_ASSERT_EQUAL_INT(AUXIO_CH_COUNT, s_gpio_level_calls);
    TEST_ASSERT_EQUAL_INT(AUXIO_CH_COUNT, mock_gpio_set_level_count);
}

static void test_auxio_boot_safe_failure_paths(void)
{
    /* gpio_config() failing aborts before any pin is driven. */
    auxio_reset_faults();
    mock_gpio_set_level_count = 0;
    s_gpio_config_err = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, auxio_boot_safe());
    TEST_ASSERT_EQUAL_INT(0, s_gpio_level_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);

    /* A mid-loop gpio_set_level() failure is propagated and stops the loop. */
    auxio_reset_faults();
    mock_gpio_set_level_count = 0;
    s_gpio_level_err = ESP_ERR_TIMEOUT;
    s_gpio_level_fail_at = 4;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_boot_safe());
    TEST_ASSERT_EQUAL_INT(4, s_gpio_level_calls);
    TEST_ASSERT_EQUAL_INT(3, mock_gpio_set_level_count);
}

/* ---- auxio_init: repeated init, channel boundaries, rollback ---- */

static void test_auxio_init_repeated_and_channel_boundaries(void)
{
    /* setUp() already initialized once; a repeated call must rebuild every
     * channel from scratch without stale handles. */
    auxio_reset_faults();
    TEST_ASSERT_EQUAL(ESP_OK, auxio_init());

    TEST_ASSERT_NOT_NULL(s_mcpwm_timer);
    TEST_ASSERT_NOT_NULL(s_mcpwm_oper[0]);
    TEST_ASSERT_NOT_NULL(s_mcpwm_oper[1]);

    /* AUXIO_CH_F0F..AUXIO_CH_AUX4 map to LEDC channels 2..7. */
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_TRUE(s_ch[i].ledc);
        TEST_ASSERT_EQUAL_INT(LEDC_CHANNEL_2 + i, (int)s_ch[i].ledc_ch);
        TEST_ASSERT_NULL(s_ch[i].mcpwm_cmpr);
        TEST_ASSERT_NULL(s_ch[i].mcpwm_gen);
    }
    /* AUX5..AUX7 are MCPWM-backed with the default steady/off config. */
    for (int i = 6; i < AUXIO_CH_COUNT; ++i) {
        TEST_ASSERT_FALSE(s_ch[i].ledc);
        TEST_ASSERT_NOT_NULL(s_ch[i].mcpwm_cmpr);
        TEST_ASSERT_NOT_NULL(s_ch[i].mcpwm_gen);
        TEST_ASSERT_EQUAL_UINT8(255, s_ch[i].pwm_on);
        TEST_ASSERT_EQUAL_UINT8(0, s_ch[i].pwm_off);
        TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_STEADY, s_ch[i].mode);
        TEST_ASSERT_EQUAL_UINT16(800, s_ch[i].period_ms);
        TEST_ASSERT_FALSE(s_ch[i].enabled);
    }
}

static void test_auxio_deinit_clamps_oversized_count(void)
{
    TEST_ASSERT_TRUE(s_ch[0].ledc);
    TEST_ASSERT_NOT_NULL(s_mcpwm_timer);

    auxio_deinit_channels_locked((size_t)AUXIO_CH_COUNT + 5U);

    TEST_ASSERT_NULL(s_mcpwm_timer);
    TEST_ASSERT_NULL(s_mcpwm_oper[0]);
    TEST_ASSERT_NULL(s_mcpwm_oper[1]);
    for (int i = 0; i < AUXIO_CH_COUNT; ++i) {
        TEST_ASSERT_FALSE(s_ch[i].ledc);
        TEST_ASSERT_NULL(s_ch[i].mcpwm_cmpr);
        TEST_ASSERT_NULL(s_ch[i].mcpwm_gen);
    }
}

/* Arm one init-time fault, verify the error is propagated and the rollback
 * leaves no stale handles, then restore a clean initialized state. */
static void auxio_expect_init_fault(const char *label, esp_err_t *err,
                                    int *fail_at, int fail_at_value,
                                    esp_err_t expected)
{
    auxio_reset_faults();
    *err = expected;
    if (fail_at != NULL) {
        *fail_at = fail_at_value;
    }
    TEST_ASSERT_EQUAL(expected, auxio_init());
    TEST_ASSERT_NULL_MESSAGE(s_mcpwm_timer, label);
    TEST_ASSERT_NULL_MESSAGE(s_mcpwm_oper[0], label);
    TEST_ASSERT_NULL_MESSAGE(s_mcpwm_oper[1], label);
    TEST_ASSERT_FALSE_MESSAGE(s_ch[0].ledc, label);
    TEST_ASSERT_FALSE_MESSAGE(s_ch[AUXIO_CH_COUNT - 1].ledc, label);
    TEST_ASSERT_NULL_MESSAGE(s_ch[0].mcpwm_cmpr, label);
    TEST_ASSERT_NULL_MESSAGE(s_ch[AUXIO_CH_COUNT - 1].mcpwm_cmpr, label);

    auxio_reset_faults();
    TEST_ASSERT_EQUAL(ESP_OK, auxio_init());
}

static void test_auxio_init_failure_rollbacks(void)
{
    const esp_err_t e = ESP_ERR_INVALID_STATE;

    auxio_expect_init_fault("ledc timer", &s_ledc_timer_err, NULL, 0, e);
    auxio_expect_init_fault("mcpwm timer", &s_mcpwm_new_timer_err, NULL, 0, e);

    auxio_expect_init_fault("mcpwm operator 0", &s_mcpwm_new_oper_err,
                            &s_mcpwm_new_oper_fail_at, 1, e);
    auxio_expect_init_fault("mcpwm connect 0", &s_mcpwm_connect_err,
                            &s_mcpwm_connect_fail_at, 1, e);
    auxio_expect_init_fault("mcpwm operator 1", &s_mcpwm_new_oper_err,
                            &s_mcpwm_new_oper_fail_at, 2, e);
    auxio_expect_init_fault("mcpwm connect 1", &s_mcpwm_connect_err,
                            &s_mcpwm_connect_fail_at, 2, e);

    /* First and mid-loop LEDC channel failures must unwind only what was
     * configured (i + 1 channels) and still clear the shared MCPWM handles. */
    auxio_expect_init_fault("ledc channel 0", &s_ledc_channel_err,
                            &s_ledc_channel_fail_at, 1, e);
    auxio_expect_init_fault("ledc channel 3", &s_ledc_channel_err,
                            &s_ledc_channel_fail_at, 4, e);

    auxio_expect_init_fault("mcpwm comparator", &s_mcpwm_cmpr_err,
                            &s_mcpwm_cmpr_fail_at, 1, e);
    auxio_expect_init_fault("mcpwm generator", &s_mcpwm_gen_err,
                            &s_mcpwm_gen_fail_at, 1, e);
    auxio_expect_init_fault("mcpwm timer event", &s_mcpwm_timer_event_err,
                            &s_mcpwm_timer_event_fail_at, 1, e);
    auxio_expect_init_fault("mcpwm compare event", &s_mcpwm_compare_event_err,
                            &s_mcpwm_compare_event_fail_at, 1, e);

    auxio_expect_init_fault("mcpwm timer enable", &s_mcpwm_enable_err, NULL, 0, e);
    auxio_expect_init_fault("mcpwm timer start", &s_mcpwm_start_stop_err, NULL, 0, e);
}

/* ---- auxio_set_enabled / effect paths ---- */

static void test_auxio_set_enabled_drives_safe_off(void)
{
    /* LEDC-backed channel: enabling applies pwm_on, disabling drives zero. */
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 200, 0, AUXIO_EFFECT_STEADY, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, true));
    TEST_ASSERT_EQUAL_UINT32(200, mock_ledc_duty[LEDC_CHANNEL_2]);
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, false));
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_2]);

    bool en = true;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_get_enabled(0, &en));
    TEST_ASSERT_FALSE(en);

    /* MCPWM-backed channel: safety-off must also reach zero duty. */
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(8, 153, 0, AUXIO_EFFECT_STEADY, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(8, true));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 60.0f, mock_mcpwm_duty[2]);
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(8, false));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, mock_mcpwm_duty[2]);
}

static void test_auxio_api_null_lock_returns_timeout(void)
{
    SemaphoreHandle_t saved = s_lock;
    s_lock = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_set_enabled(0, true));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      auxio_set_effect(0, true, 10, 0, AUXIO_EFFECT_MARS, 800));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      auxio_config(0, 10, 0, AUXIO_EFFECT_BEACON, 800));
    s_lock = saved;
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, false));
}

static void test_auxio_lock_busy_preserves_state(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 55, 5, AUXIO_EFFECT_STEADY, 800));
    TEST_ASSERT_EQUAL(ESP_OK, auxio_set_enabled(0, true));
    TEST_ASSERT_TRUE(s_ch[0].enabled);

    /* A busy lock must fail the call before any channel field is mutated. */
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, auxio_set_enabled(0, false));
    TEST_ASSERT_TRUE(s_ch[0].enabled);

    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      auxio_set_effect(0, false, 10, 0, AUXIO_EFFECT_MARS, 800));
    TEST_ASSERT_EQUAL_UINT8(55, s_ch[0].pwm_on);
    TEST_ASSERT_EQUAL_UINT8(5, s_ch[0].pwm_off);
    TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_STEADY, s_ch[0].mode);
    TEST_ASSERT_EQUAL_UINT16(800, s_ch[0].period_ms);

    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT,
                      auxio_config(0, 10, 0, AUXIO_EFFECT_BEACON, 900));
    TEST_ASSERT_EQUAL_UINT8(55, s_ch[0].pwm_on);
    TEST_ASSERT_EQUAL_UINT8(5, s_ch[0].pwm_off);
    TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_STEADY, s_ch[0].mode);
    TEST_ASSERT_EQUAL_UINT16(800, s_ch[0].period_ms);

    mock_sem_take_fail = 0;
}

static void test_auxio_invalid_effect_args_preserve_state(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, auxio_config(0, 77, 11, AUXIO_EFFECT_BEACON, 900));

    /* Invalid channel, mode, period and off > on are all rejected by both
     * effect entry points. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(AUXIO_CH_COUNT, true, 77, 11,
                                       AUXIO_EFFECT_BEACON, 900));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 77, 11, AUXIO_EFFECT_COUNT, 900));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 77, 11, AUXIO_EFFECT_BEACON, 99));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 11, 77, AUXIO_EFFECT_BEACON, 900));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_set_effect(0, true, 77, 11, AUXIO_EFFECT_STROBE,
                                       AUXIO_STROBE_MIN_PERIOD_MS - 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_config(0, 77, 11, AUXIO_EFFECT_BEACON,
                                   AUXIO_PERIOD_MAX_MS + 1U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      auxio_config(AUXIO_CH_COUNT, 77, 11,
                                   AUXIO_EFFECT_BEACON, 900));

    /* The last good configuration is untouched by the rejected requests. */
    TEST_ASSERT_EQUAL_UINT8(77, s_ch[0].pwm_on);
    TEST_ASSERT_EQUAL_UINT8(11, s_ch[0].pwm_off);
    TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_BEACON, s_ch[0].mode);
    TEST_ASSERT_EQUAL_UINT16(900, s_ch[0].period_ms);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gamma_table_endpoints_and_monotonic);
    RUN_TEST(test_ch_duty_steady);
    RUN_TEST(test_ch_duty_incandescent);
    RUN_TEST(test_ch_duty_firebox);
    RUN_TEST(test_ch_duty_mars_triangle);
    RUN_TEST(test_ch_duty_mars_odd_period_clamps);
    RUN_TEST(test_ch_duty_ditch_alternates);
    RUN_TEST(test_ch_duty_beacon_decay);
    RUN_TEST(test_ch_duty_strobe_double_flash);
    RUN_TEST(test_ch_duty_strobe_scales_short_period);
    RUN_TEST(test_ch_duty_steady_uses_pwm_on);
    RUN_TEST(test_ch_duty_short_period_falls_back_to_max);
    RUN_TEST(test_ch_step_incandescent_warm_and_cool);
    RUN_TEST(test_ch_step_firebox_flicker_range);
    RUN_TEST(test_incandescent_scales_brightness_once);
    RUN_TEST(test_firebox_initializes_at_large_uptime_and_wraps);
    RUN_TEST(test_auxio_steady_output_applies_duty);
    RUN_TEST(test_auxio_set_enabled_validation);
    RUN_TEST(test_auxio_set_effect_validation);
    RUN_TEST(test_auxio_config_validation_and_state);
    RUN_TEST(test_auxio_off_above_on_rejected);
    RUN_TEST(test_strobe_period_below_task_resolution_rejected);
    RUN_TEST(test_auxio_config_preserves_continuous_state);
    RUN_TEST(test_auxio_config_clamps_firebox_to_on_level);
    RUN_TEST(test_ch_step_steady_resets_cur);
    RUN_TEST(test_ch_duty_unknown_mode_default);
    RUN_TEST(test_apply_now_and_api_timeouts);
    RUN_TEST(test_apply_now_null_lock);
    RUN_TEST(test_auxio_set_enabled_applies);
    RUN_TEST(test_auxio_get_enabled);
    RUN_TEST(test_auxio_init_mutex_fail);
    RUN_TEST(test_auxio_init_task_fail);
    RUN_TEST(test_effect_task_runs_once);
    RUN_TEST(test_auxio_boot_safe_drives_all_pins_low);
    RUN_TEST(test_auxio_boot_safe_failure_paths);
    RUN_TEST(test_auxio_init_repeated_and_channel_boundaries);
    RUN_TEST(test_auxio_deinit_clamps_oversized_count);
    RUN_TEST(test_auxio_init_failure_rollbacks);
    RUN_TEST(test_auxio_set_enabled_drives_safe_off);
    RUN_TEST(test_auxio_api_null_lock_returns_timeout);
    RUN_TEST(test_auxio_lock_busy_preserves_state);
    RUN_TEST(test_auxio_invalid_effect_args_preserve_state);
    return UNITY_END();
}
