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

/* White-box: expose raw_to_mv / dc_speed_step. */
#define static
#include "../../components/track/src/track.c"
#undef static

#include "../../test_libs/teststubs/stubs.c"

/* ---- collaborators of track.c ---- */
static uint8_t g_cv[SETTINGS_CV_COUNT + 1];
static bool g_web_rails;

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (out == NULL || idx < 1 || idx > SETTINGS_CV_COUNT) {
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

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    g_last_speed = speed128;
    g_last_forward = forward;
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
    return true;
}

void motor_bemf_unlock(void)
{
}

void setUp(void)
{
    memset(g_cv, 0, sizeof(g_cv));
    g_web_rails = false;
    g_motor_stopped = false;
    g_last_speed = 0;
    g_last_forward = false;
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    mock_adc_ok = 1;
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

static void test_track_init_ok(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, track_init());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_track_dc_mode_flag);
    RUN_TEST(test_track_raw_to_mv);
    RUN_TEST(test_track_dc_speed_deadband);
    RUN_TEST(test_track_dc_speed_full_range);
    RUN_TEST(test_track_init_ok);
    return UNITY_END();
}
