#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motor.h"
#include "pinmap.h"
#include "settings.h"
#include "web.h"

/* White-box: expose raw_to_mv / dc_speed_step / track_adc_step. */
#define static
#include "../../components/track/src/track.c"
#undef static

#include "../../test_libs/teststubs/stubs.c"

/* ---- collaborators of track.c ---- */
static uint8_t g_cv[SETTINGS_CV_COUNT + 1];
static bool g_cv_fail;
static bool g_web_rails;
static bool g_bemf_lock;

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (g_cv_fail || out == NULL || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = g_cv[idx];
    return ESP_OK;
}

bool web_control_is_rails(void)
{
    return g_web_rails;
}

/* motor.c stand-ins (only the symbols track.c references). */
static uint8_t g_last_speed;
static bool g_last_forward;
static bool g_motor_stopped;
static uint32_t g_rail_set;
static uint32_t g_speed_calls;

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    g_last_speed = speed128;
    g_last_forward = forward;
    g_speed_calls++;
    return ESP_OK;
}

void motor_stop(void)
{
    g_motor_stopped = true;
}

esp_err_t motor_set_rail_voltage_mv(uint32_t rail_mv)
{
    g_rail_set = rail_mv;
    return ESP_OK;
}

bool motor_bemf_lock(void)
{
    return g_bemf_lock;
}

void motor_bemf_unlock(void)
{
}

void setUp(void)
{
    memset(g_cv, 0, sizeof(g_cv));
    g_cv_fail = false;
    g_web_rails = false;
    g_bemf_lock = true;
    g_motor_stopped = false;
    g_last_speed = 0;
    g_last_forward = false;
    g_rail_set = 0;
    g_speed_calls = 0;
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    mock_adc_ok = 1;
    mock_gpio_get_level = 0;
    mock_adc1_config_width_ok = 1;
    mock_adc1_config_atten_ok = 1;
    mock_task_create_ok = 1;
    s_dc_forward = true;
    s_dc_forward_votes = 0;
    s_iter_cap = 0;
}

void tearDown(void)
{
}

/* ---- DC mode flag (CV29 bit 2 = 0x04) ---- */

static void test_track_dc_mode_flag(void)
{
    g_cv[29] = 0x02;
    TEST_ASSERT_FALSE(track_is_dc_mode());
    g_cv[29] = 0x06; /* 0x02 | 0x04 */
    TEST_ASSERT_TRUE(track_is_dc_mode());
    g_cv[29] = 0x04;
    TEST_ASSERT_TRUE(track_is_dc_mode());
}

static void test_track_dc_mode_cv_read_failure(void)
{
    g_cv[29] = 0x06;
    g_cv_fail = true;
    TEST_ASSERT_FALSE(track_is_dc_mode());
}

/* ---- raw_to_mv ---- */

static void test_track_raw_to_mv(void)
{
    TEST_ASSERT_EQUAL_UINT32(0, raw_to_mv(-1));
    TEST_ASSERT_EQUAL_UINT32(0, raw_to_mv(0));
    TEST_ASSERT_EQUAL_UINT32(3100, raw_to_mv(4095));
    /* Clamped above the ADC range. */
    TEST_ASSERT_EQUAL_UINT32(3100, raw_to_mv(5000));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(2048 * 3100 / 4095), raw_to_mv(2048));
}

/* ---- dc_speed_step ---- */

static void test_track_dc_speed_deadband(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, dc_speed_step(0));
    TEST_ASSERT_EQUAL_UINT8(0, dc_speed_step(110)); /* deadband edge */
    TEST_ASSERT_EQUAL_UINT8(0, dc_speed_step(111)); /* rounds down */
}

static void test_track_dc_speed_full_range(void)
{
    TEST_ASSERT_EQUAL_UINT8(126, dc_speed_step(2500));
    TEST_ASSERT_EQUAL_UINT8(126, dc_speed_step(3000)); /* clamped */
    /* Midpoint of the usable band (110..2500) -> half of 126. */
    TEST_ASSERT_EQUAL_UINT8(63, dc_speed_step((110 + 2500) / 2));
}

/* ---- track_adc_step ---- */

static void test_track_step_samples_rail(void)
{
    bool wd = false;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    track_adc_step(&wd);
    TEST_ASSERT_EQUAL_UINT32(raw_to_mv(2048), g_rail_set);
    TEST_ASSERT_FALSE(wd); /* not in DC mode -> not driving */
}

static void test_track_step_bemf_lock_skips_sample(void)
{
    bool wd = false;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 1000;
    track_adc_step(&wd);
    uint32_t rail = g_rail_set;

    g_bemf_lock = false;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 4000;
    track_adc_step(&wd);
    TEST_ASSERT_EQUAL_UINT32(rail, g_rail_set); /* unchanged: no sample taken */
}

static void test_track_step_adc_error_keeps_rail(void)
{
    bool wd = false;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 1000;
    track_adc_step(&wd);
    uint32_t rail = g_rail_set;

    mock_adc_ok = 0; /* adc1_get_raw -> -1 */
    track_adc_step(&wd);
    TEST_ASSERT_EQUAL_UINT32(rail, g_rail_set);
}

static void test_track_step_drives_in_dc_mode(void)
{
    g_cv[29] = 0x06;
    g_web_rails = true;
    mock_gpio_get_level = 0; /* forward */
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;

    bool wd = false;
    track_adc_step(&wd);

    TEST_ASSERT_TRUE(wd);
    TEST_ASSERT_EQUAL_UINT8(dc_speed_step(raw_to_mv(2048)), g_last_speed);
    TEST_ASSERT_TRUE(g_last_forward);
}

static void test_track_step_reverse_polarity_debounce(void)
{
    g_cv[29] = 0x06;
    g_web_rails = true;
    mock_gpio_get_level = 1; /* reverse */
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 1500;

    bool wd = false;
    /* Direction flips only after DC_POLARITY_DEBOUNCE agreeing samples. */
    for (int i = 0; i < 3; ++i) {
        track_adc_step(&wd);
        TEST_ASSERT_TRUE(g_last_forward);
    }
    track_adc_step(&wd);
    TEST_ASSERT_FALSE(g_last_forward);
}

static void test_track_step_stops_when_leaving_dc(void)
{
    g_cv[29] = 0x06;
    g_web_rails = true;
    mock_gpio_get_level = 0;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 1500;

    bool wd = false;
    track_adc_step(&wd); /* driving */
    TEST_ASSERT_TRUE(wd);

    g_web_rails = false; /* leave rails control */
    track_adc_step(&wd);
    TEST_ASSERT_TRUE(g_motor_stopped);
    TEST_ASSERT_FALSE(wd);
}

static void test_track_task_loop_bounded(void)
{
    g_cv[29] = 0x06;
    g_web_rails = true;
    mock_gpio_get_level = 0;
    s_iter_cap = 1; /* test hook: run the loop once and return */
    track_adc_task(NULL);
    TEST_ASSERT_EQUAL_UINT32(1, g_speed_calls);
}

/* ---- track_init ---- */

static void test_track_init_ok(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, track_init());
}

static void test_track_init_width_failure(void)
{
    mock_adc1_config_width_ok = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, track_init());
}

static void test_track_init_atten_failure(void)
{
    mock_adc1_config_atten_ok = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, track_init());
}

static void test_track_init_no_mem(void)
{
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, track_init());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_track_dc_mode_flag);
    RUN_TEST(test_track_dc_mode_cv_read_failure);
    RUN_TEST(test_track_raw_to_mv);
    RUN_TEST(test_track_dc_speed_deadband);
    RUN_TEST(test_track_dc_speed_full_range);
    RUN_TEST(test_track_step_samples_rail);
    RUN_TEST(test_track_step_bemf_lock_skips_sample);
    RUN_TEST(test_track_step_adc_error_keeps_rail);
    RUN_TEST(test_track_step_drives_in_dc_mode);
    RUN_TEST(test_track_step_reverse_polarity_debounce);
    RUN_TEST(test_track_step_stops_when_leaving_dc);
    RUN_TEST(test_track_task_loop_bounded);
    RUN_TEST(test_track_init_ok);
    RUN_TEST(test_track_init_width_failure);
    RUN_TEST(test_track_init_atten_failure);
    RUN_TEST(test_track_init_no_mem);
    return UNITY_END();
}
