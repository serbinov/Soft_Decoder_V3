#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pinmap.h"

#define static
#include "../../components/pinmap/src/pinmap.c"
#undef static
/* The production board map uses the board-specific wrapper; keep the generic
 * checker covered without leaving an unused static function in the firmware. */
#define pinmap_check(pins, n) pinmap_check_impl((pins), (n), false)
#include "../../test_libs/teststubs/stubs.c"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_pinmap_valid(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, pinmap_validate());
}

/* None of the mapped pins may use GPIO23..25 (unavailable on ESP32-S3). */
static void test_pinmap_no_reserved_gpio(void)
{
    const int pins[] = {
        PIN_MOTOR_IN1, PIN_MOTOR_IN2, PIN_DCC_IN, PIN_ACK_LOAD,
        PIN_BEMF1, PIN_BEMF2, PIN_RAIL_SENSE, PIN_CURRENT_SENSE,
        PIN_AUDIO_BCLK, PIN_AUDIO_WS, PIN_AUDIO_DOUT, PIN_AUDIO_SD_MODE,
        PIN_STORAGE_CS, PIN_STORAGE_SCK, PIN_STORAGE_MOSI, PIN_STORAGE_MISO,
        PIN_AUX_F0F, PIN_AUX_F0R, PIN_AUX1, PIN_AUX2, PIN_AUX3,
        PIN_AUX4, PIN_AUX5, PIN_AUX6, PIN_AUX7,
        PIN_I2C_SDA, PIN_I2C_SCL,
    };
    const size_t n = sizeof(pins) / sizeof(pins[0]);
    for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_TRUE(pins[i] != 23 && pins[i] != 24 && pins[i] != 25);
    }
}

/* Two functions must never share the same GPIO. */
static void test_pinmap_no_duplicates(void)
{
    const int pins[] = {
        PIN_MOTOR_IN1, PIN_MOTOR_IN2, PIN_DCC_IN, PIN_ACK_LOAD,
        PIN_BEMF1, PIN_BEMF2, PIN_RAIL_SENSE, PIN_CURRENT_SENSE,
        PIN_AUDIO_BCLK, PIN_AUDIO_WS, PIN_AUDIO_DOUT, PIN_AUDIO_SD_MODE,
        PIN_STORAGE_CS, PIN_STORAGE_SCK, PIN_STORAGE_MOSI, PIN_STORAGE_MISO,
        PIN_AUX_F0F, PIN_AUX_F0R, PIN_AUX1, PIN_AUX2, PIN_AUX3,
        PIN_AUX4, PIN_AUX5, PIN_AUX6, PIN_AUX7,
        PIN_I2C_SDA, PIN_I2C_SCL,
    };
    const size_t n = sizeof(pins) / sizeof(pins[0]);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            TEST_ASSERT_TRUE(pins[i] != pins[j]);
        }
    }
}

/* pinmap_check must reject GPIO23/24/25. */
static void test_pinmap_check_rejects_reserved(void)
{
    int bad23[] = { 1, 23, 5 };
    int bad24[] = { 24 };
    int bad25[] = { 7, 8, 25 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(bad23, 3));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(bad24, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(bad25, 3));
}

static void test_pinmap_check_accepts_valid(void)
{
    int ok[] = { 1, 4, 5, 6 };
    TEST_ASSERT_EQUAL(ESP_OK, pinmap_check(ok, 4));
    TEST_ASSERT_EQUAL(ESP_OK, pinmap_check(ok, 0));
}

/* ESP32-S3 strapping pins must not be assigned by the generic checker. */
static void test_pinmap_check_rejects_strapping(void)
{
    int strap0[] = { 0 };
    int strap3[] = { 3 };
    int strap45[] = { 45 };
    int strap46[] = { 46 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(strap0, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(strap3, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(strap45, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(strap46, 1));
}

/* The board map explicitly whitelists its two documented strap pins. */
static void test_pinmap_check_board_allows_documented_straps(void)
{
    int board[] = { PIN_ACK_LOAD, PIN_AUX_F0R };
    int other[] = { 0, PIN_ACK_LOAD };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(board, 2));
    TEST_ASSERT_EQUAL(ESP_OK, pinmap_check_board(board, 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check_board(other, 2));
}

/* Output validity comes from the SoC mask, not a legacy GPIO_PIN_COUNT bound. */
static void test_pinmap_check_output_validity(void)
{
    int s3_outputs[] = { 34, 35, 36, 37, 48 };
    int too_high[] = { 49 };
    int not_bonded[] = { 22 };
    TEST_ASSERT_EQUAL(ESP_OK, pinmap_check(s3_outputs, 5));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(too_high, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(not_bonded, 1));
}

/* out-of-range, GPIO22 and duplicate assignments must all be rejected. */
static void test_pinmap_check_rejects_bad(void)
{
    int out_hi[] = { 1, 100 };
    int out_lo[] = { -1, 2 };
    int p22[] = { 1, 22 };
    int dup[] = { 4, 4 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(out_hi, 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(out_lo, 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(p22, 2));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, pinmap_check(dup, 2));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pinmap_valid);
    RUN_TEST(test_pinmap_no_reserved_gpio);
    RUN_TEST(test_pinmap_no_duplicates);
    RUN_TEST(test_pinmap_check_rejects_reserved);
    RUN_TEST(test_pinmap_check_accepts_valid);
    RUN_TEST(test_pinmap_check_rejects_strapping);
    RUN_TEST(test_pinmap_check_board_allows_documented_straps);
    RUN_TEST(test_pinmap_check_output_validity);
    RUN_TEST(test_pinmap_check_rejects_bad);
    return UNITY_END();
}
