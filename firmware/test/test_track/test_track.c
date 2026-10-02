#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motor.h"
#include "pinmap.h"
#include "settings.h"
#include "web.h"

bool web_control_rails_begin(uint32_t *generation);
void web_control_rails_end(void);

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
static int64_t g_signal_us, g_addressed_us;
static bool g_signal_static;
static uint32_t g_source_generation;
static bool g_rails_locked;
static int64_t g_settings_delay_us, g_lease_delay_us, g_adc_delay_us;

int64_t dcc_last_signal_packet_us(void) { return g_signal_us; }
int64_t dcc_last_packet_us(void) { return g_addressed_us; }
bool dcc_signal_is_static(uint32_t min_us) { (void)min_us; return g_signal_static; }

bool web_control_rails_begin(uint32_t *generation)
{
    mock_timer_now_us += g_lease_delay_us;
    if (!g_web_rails) return false;
    g_rails_locked = true;
    *generation = g_source_generation;
    return true;
}
void web_control_rails_end(void) { g_rails_locked = false; }

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    mock_timer_now_us += g_settings_delay_us;
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
static esp_err_t g_speed_result;

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    g_last_speed = speed128;
    g_last_forward = forward;
    g_speed_calls++;
    return g_speed_result;
}

void motor_stop(void)
{
    g_motor_stopped = true;
}
void motor_emergency_stop(void) { g_motor_stopped = true; }

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

esp_err_t motor_adc_config_channel(int gpio_num)
{
    (void)gpio_num;
    if (!mock_adc_unit_new_ok || !mock_adc_config_ok) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

int motor_adc_read_raw(int gpio_num)
{
    mock_timer_now_us += g_adc_delay_us;
    if (!mock_adc_ok || gpio_num < 1 || gpio_num > 16) {
        return -1;
    }
    return mock_adc_raw[gpio_num - 1];
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
    g_speed_result = ESP_OK;
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    mock_adc_ok = 1;
    mock_gpio_get_level = 0;
    mock_adc_unit_new_ok = 1;
    mock_adc_config_ok = 1;
    mock_task_create_ok = 1;
    s_dc_forward = true;
    s_dc_forward_votes = 0;
    s_iter_cap = 0;
    s_dc_detected = false;
    s_dc_candidate_votes = 0;
    s_rail_valid = false;
    s_rail_mv = 0;
    s_rail_sample_us = 0;
    s_dcc_motor_owned = false;
    s_dc_generation = 0;
    g_signal_us = g_addressed_us = 0;
    g_signal_static = true;
    g_source_generation = 1;
    g_rails_locked = false;
    g_settings_delay_us = g_lease_delay_us = g_adc_delay_us = 0;
    mock_timer_now_us = 50000;
}

void tearDown(void)
{
}

static void settle_dc(bool *was_driving)
{
    for (int i = 0; i < DC_POLARITY_DEBOUNCE; i++) {
        track_adc_step(was_driving);
        mock_timer_now_us += 50000;
    }
}

/* ---- DC mode flag (CV29 bit 2 = 0x04) ---- */

static void test_track_dc_mode_flag(void)
{
    g_cv[29] = 0x02;
    TEST_ASSERT_FALSE(track_is_dc_mode());
    g_cv[29] = 0x06; /* 0x02 | 0x04 */
    TEST_ASSERT_FALSE(track_is_dc_mode()); /* Permission is not detection. */
    g_cv[29] = 0x04;
    bool wd = false;
    settle_dc(&wd);
    TEST_ASSERT_TRUE(track_is_dc_mode());
    g_cv[29] = 0x02;
    TEST_ASSERT_TRUE(track_is_dc_mode()); /* Detection is independent of permission. */
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

    mock_adc_ok = 0; /* motor_adc_read_raw -> -1 */
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
    settle_dc(&wd);

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
    mock_gpio_get_level = 0;
    settle_dc(&wd);
    mock_gpio_get_level = 1;
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
    settle_dc(&wd);
    TEST_ASSERT_TRUE(wd);

    g_web_rails = false; /* leave rails control */
    track_adc_step(&wd);
    TEST_ASSERT_FALSE(g_motor_stopped); /* Handover, not delayed old-owner stop. */
    TEST_ASSERT_FALSE(wd);
}

static void test_track_task_loop_bounded(void)
{
    g_cv[29] = 0x06;
    g_web_rails = true;
    mock_gpio_get_level = 0;
    s_iter_cap = DC_POLARITY_DEBOUNCE;
    track_adc_task(NULL);
    TEST_ASSERT_EQUAL_UINT32(1, g_speed_calls);
}

/* ---- track_init ---- */

static void test_track_init_ok(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, track_init());
}

static void test_track_init_unit_failure(void)
{
    mock_adc_unit_new_ok = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, track_init());
}

static void test_track_init_channel_failure(void)
{
    mock_adc_config_ok = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, track_init());
}

static void test_track_init_no_mem(void)
{
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, track_init());
}

static void test_digital_activity_blocks_dc_and_exits_immediately(void)
{
    g_cv[29] = 6;
    g_web_rails = true;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    for (int i = 0; i < 8; i++) {
        g_signal_us = mock_timer_now_us;
        track_adc_step(&wd);
        mock_timer_now_us += 25000;
    }
    TEST_ASSERT_FALSE(wd);
    TEST_ASSERT_FALSE(track_is_dc_mode());
    TEST_ASSERT_EQUAL_UINT32(0, g_speed_calls);
    mock_timer_now_us += 50000;
    settle_dc(&wd);
    TEST_ASSERT_TRUE(wd);
    g_signal_us = mock_timer_now_us;
    TEST_ASSERT_FALSE(track_is_dc_mode()); /* Before next50ms iteration. */
    track_adc_step(&wd);
    TEST_ASSERT_FALSE(wd);
    TEST_ASSERT_TRUE(g_motor_stopped);
}

static void test_edges_prevent_sample_aliasing_as_dc(void)
{
    g_cv[29] = 6;
    g_web_rails = true;
    g_signal_static = false;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    settle_dc(&wd);
    TEST_ASSERT_FALSE(wd);
    TEST_ASSERT_FALSE(track_is_dc_mode());
}

static void test_stale_rail_expires_and_stops_dc(void)
{
    g_cv[29] = 6;
    g_web_rails = true;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    settle_dc(&wd);
    TEST_ASSERT_TRUE(wd);
    mock_adc_ok = 0;
    track_adc_step(&wd); /* one missed sample, still fresh */
    TEST_ASSERT_TRUE(wd);
    mock_timer_now_us += 50000;
    TEST_ASSERT_FALSE(track_is_dc_mode());
    track_adc_step(&wd);
    TEST_ASSERT_FALSE(wd);
    TEST_ASSERT_TRUE(g_motor_stopped);
    TEST_ASSERT_EQUAL_UINT32(0, g_rail_set);
}

static void test_dc_cleanup_cannot_overwrite_new_dcc_or_source_generation(void)
{
    g_cv[29] = 6;
    g_web_rails = true;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    settle_dc(&wd);
    g_signal_us = g_addressed_us = mock_timer_now_us;
    track_note_dcc_motor_command(); /* Main holds the rails lease at its write. */
    track_adc_step(&wd);
    TEST_ASSERT_FALSE(g_motor_stopped);
    TEST_ASSERT_FALSE(wd);
    mock_timer_now_us += 50000;
    settle_dc(&wd);
    g_source_generation++; /* rails->web->rails between ADC iterations */
    g_cv[29] = 2;
    track_adc_step(&wd);
    TEST_ASSERT_FALSE(g_motor_stopped);
    TEST_ASSERT_FALSE(g_rails_locked);
}

static void test_analog_permission_and_direction_inversion(void)
{
    g_cv[29] = 2;
    g_web_rails = true;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    settle_dc(&wd);
    TEST_ASSERT_TRUE(track_is_dc_mode());
    TEST_ASSERT_FALSE(wd);
    TEST_ASSERT_EQUAL_UINT32(0, g_speed_calls);
    g_cv[29] = 7;
    track_adc_step(&wd);
    TEST_ASSERT_TRUE(wd);
    TEST_ASSERT_FALSE(g_last_forward);
}

static void test_addressed_function_does_not_preserve_old_dc_motion(void)
{
    g_cv[29] = 6;
    g_web_rails = true;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    settle_dc(&wd);
    g_signal_us = g_addressed_us = mock_timer_now_us;
    track_adc_step(&wd); /* A function/CV callback did not claim motor ownership. */
    TEST_ASSERT_TRUE(g_motor_stopped);
    TEST_ASSERT_FALSE(wd);
}

static void test_rail_age_rechecked_after_source_wait(void)
{
    g_cv[29] = 6;
    g_web_rails = true;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    settle_dc(&wd);
    mock_adc_ok = 0;
    g_lease_delay_us = 100000;
    uint32_t commands = g_speed_calls;
    track_adc_step(&wd);
    TEST_ASSERT_EQUAL_UINT32(commands, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT32(0, g_rail_set);
    TEST_ASSERT_TRUE(g_motor_stopped);
    TEST_ASSERT_FALSE(wd);
}

static void test_successful_sample_timestamp_is_acquisition_time(void)
{
    g_adc_delay_us = 150000;
    bool wd = false;
    track_adc_step(&wd);
    TEST_ASSERT_EQUAL_INT64(mock_timer_now_us, s_rail_sample_us);
    TEST_ASSERT_TRUE(s_rail_valid);
    TEST_ASSERT_EQUAL_UINT32(raw_to_mv(mock_adc_raw[PIN_RAIL_SENSE - 1]), g_rail_set);
}

static void test_failed_dc_command_stops_previous_dc_owner(void)
{
    g_cv[29] = 6;
    g_web_rails = true;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 2048;
    bool wd = false;
    settle_dc(&wd);
    g_speed_result = ESP_ERR_TIMEOUT;
    track_adc_step(&wd);
    TEST_ASSERT_FALSE(wd);
    TEST_ASSERT_TRUE(g_motor_stopped);
    TEST_ASSERT_FALSE(g_rails_locked);
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
    RUN_TEST(test_track_init_unit_failure);
    RUN_TEST(test_track_init_channel_failure);
    RUN_TEST(test_track_init_no_mem);
    RUN_TEST(test_digital_activity_blocks_dc_and_exits_immediately);
    RUN_TEST(test_edges_prevent_sample_aliasing_as_dc);
    RUN_TEST(test_stale_rail_expires_and_stops_dc);
    RUN_TEST(test_dc_cleanup_cannot_overwrite_new_dcc_or_source_generation);
    RUN_TEST(test_analog_permission_and_direction_inversion);
    RUN_TEST(test_addressed_function_does_not_preserve_old_dc_motion);
    RUN_TEST(test_rail_age_rechecked_after_source_wait);
    RUN_TEST(test_successful_sample_timestamp_is_acquisition_time);
    RUN_TEST(test_failed_dc_command_stops_previous_dc_owner);
    return UNITY_END();
}
