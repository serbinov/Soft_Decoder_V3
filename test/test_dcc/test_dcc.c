#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* White-box include: exposes dcc.c static internals for direct testing. The
 * native test env strips `static` only for this translation unit to avoid
 * breaking system headers. All standard headers the component pulls in
 * (directly or via the test stubs, e.g. esp_log.h -> stdio.h) must be included
 * BEFORE `static` is redefined, otherwise their `static inline` definitions
 * gain external linkage and clash with Unity. */
#define static
#include "../../components/dcc/src/dcc.c"
#undef static

/* Host stubs for ESP-IDF services (gpio, freertos, timer, pinmap, ...). */
#include "../../test_libs/teststubs/stubs.c"

/* ---- capture hooks ---- */
static uint16_t g_cv_index;
static uint8_t  g_cv_value;
static bool     g_cv_service;
static int      g_cv_write_calls;

static uint16_t g_speed;
static bool     g_forward;
static int      g_speed_calls;

static int      g_fn_calls;
static bool     g_fn_state[29];

static int      g_reset_calls;

static uint8_t  g_read_returns;
static bool     g_read_ok;

/* Full CV image for dcc_reload_config() tests. */
#define TEST_CV_MAX 513
static uint8_t  g_cv_table[TEST_CV_MAX];
static bool     g_cv_table_ok;

static void on_cv_write(uint16_t cv, uint8_t value, bool service_mode)
{
    g_cv_index = cv;
    g_cv_value = value;
    g_cv_service = service_mode;
    g_cv_write_calls++;
}

static void on_speed(uint8_t speed, bool forward)
{
    g_speed = speed;
    g_forward = forward;
    g_speed_calls++;
}

static void on_function(uint8_t fn, bool state)
{
    g_fn_calls++;
    if (fn < 29U) {
        g_fn_state[fn] = state;
    }
}

static void on_reset(void)
{
    g_reset_calls++;
}

static bool on_cv_read(uint16_t cv, uint8_t *out)
{
    if (g_cv_table_ok) {
        if (out != NULL && cv < TEST_CV_MAX) {
            *out = g_cv_table[cv];
        }
        return true;
    }
    (void)cv;
    if (out != NULL) {
        *out = g_read_returns;
    }
    return g_read_ok;
}

/* ---- helpers ---- */
void setUp(void)
{
    g_cv_write_calls = 0;
    g_cv_index = 0;
    g_cv_value = 0;
    g_cv_service = false;
    g_speed_calls = 0;
    g_speed = 0;
    g_forward = false;
    g_fn_calls = 0;
    memset(g_fn_state, 0, sizeof(g_fn_state));
    g_reset_calls = 0;
    g_read_ok = false;
    g_read_returns = 0;
    g_cv_table_ok = false;
    memset(g_cv_table, 0, sizeof(g_cv_table));
    mock_gpio_set_level_count = 0;
    mock_vtask_delay_count = 0;
    mock_timer_now_us = 0;

    s_decoder_addr = 3;
    s_decoder_long_addr = false;
    s_speed_mode_14 = false;
    s_consist_addr = 0;
    s_consist_reverse = false;

    reset_parser();

    s_speed_cb = on_speed;
    s_function_cb = on_function;
    s_cv_write_cb = on_cv_write;
    s_cv_read_cb = on_cv_read;
    s_reset_cb = on_reset;
}

void tearDown(void)
{
}

/* Feed one full DCC packet bit-by-bit through the bit-level state machine.
 * Bytes are MSB-first (the command station transmits each byte high bit first,
 * verified on the ROCO rail traffic). */
static void feed_packet(const uint8_t *bytes, size_t len, unsigned preamble_ones)
{
    for (unsigned i = 0; i < preamble_ones; ++i) {
        consume_bit(1);
    }
    for (size_t b = 0; b < len; ++b) {
        consume_bit(0); /* start bit */
        for (int bit = 7; bit >= 0; --bit) {
            consume_bit((uint8_t)((bytes[b] >> bit) & 0x01U)); /* MSB first */
        }
    }
    consume_bit(1); /* end bit -> dispatch */
}

/* Feed a full DCC packet through the half-period pipeline: two equal
 * half-periods per bit (58 us = "1", 100 us = "0"). */
static void feed_half_periods(const uint8_t *bytes, size_t len, unsigned preamble_ones)
{
    for (unsigned i = 0; i < preamble_ones; ++i) {
        feed_half_period(58);
        feed_half_period(58);
    }
    for (size_t b = 0; b < len; ++b) {
        feed_half_period(100); /* start bit 0 */
        feed_half_period(100);
        for (int bit = 7; bit >= 0; --bit) {
            if (((bytes[b] >> bit) & 0x01U) != 0) {
                feed_half_period(58);
                feed_half_period(58);
            } else {
                feed_half_period(100);
                feed_half_period(100);
            }
        }
    }
    feed_half_period(58); /* end bit 1 -> dispatch */
    feed_half_period(58);
}

/* ---- tests ---- */

static void test_half_period_classification(void)
{
    /* Одиночный порог 82 мкс (NmraDcc MAX_ONEBITHALF): «1» = 35..81 мкс,
     * «0» = 82..292 мкс, всё прочее — невалидно. */
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ONE, (uint8_t)classify(35));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ONE, (uint8_t)classify(58));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ONE, (uint8_t)classify(81));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ZERO, (uint8_t)classify(82));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ZERO, (uint8_t)classify(100));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ZERO, (uint8_t)classify(292));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_NONE, (uint8_t)classify(10));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_NONE, (uint8_t)classify(34));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_NONE, (uint8_t)classify(293));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_NONE, (uint8_t)classify(5000));
}

/* Глитч (короче 35 мкс) должен сбрасывать незавершённый пакет: следующий
 * корректный пакет после глитча обязан декодироваться. */
static void test_dcc_glitch_resets_parser(void)
{
    for (int i = 0; i < 12; ++i) {
        consume_bit(1);
    }
    consume_bit(0); /* start bit */
    consume_bit(1); /* partial byte... */
    feed_half_period(20); /* glitch -> reset */

    const uint8_t packet[] = {0x03, 0xEC, 0x00, 0x2A, 0xC5};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
}

/* Ops-mode write byte: addr 3, CV1 = 42. MSB-first must decode correctly
 * (regression for the MSB/LSB bit-order bug). */
static void test_dcc_bit_order_ops_write(void)
{
    const uint8_t packet[] = {0x03, 0xEC, 0x00, 0x2A, 0xC5};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(1, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(42, g_cv_value);
    TEST_ASSERT_FALSE(g_cv_service);
}

static void test_dcc_bad_checksum_rejected(void)
{
    const uint8_t packet[] = {0x03, 0xEC, 0x00, 0x2A, 0x00}; /* wrong checksum */
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
}

static void test_dcc_packet_not_for_us_filtered(void)
{
    const uint8_t packet[] = {0x2A, 0xEC, 0x00, 0x2A, 0x2A ^ 0xEC ^ 0x00 ^ 0x2A};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
}

static void test_dcc_long_address(void)
{
    s_decoder_addr = 10;
    s_decoder_long_addr = true;

    const uint8_t packet[] = {0xC0, 0x0A, 0xEC, 0x00, 0x2A,
                              0xC0 ^ 0x0A ^ 0xEC ^ 0x00 ^ 0x2A};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(1, g_cv_index);
}

/* 28-step forward + F0 + max speed => instr 0x7F. */
static void test_dcc_28step_f0_max_speed(void)
{
    const uint8_t packet[] = {0x03, 0x7F, 0x7C};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(126, g_speed);
    TEST_ASSERT_TRUE(g_forward);
}

static void test_dcc_28step_stop_forward(void)
{
    const uint8_t packet[] = {0x03, 0x60, 0x63};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(0, g_speed);
    TEST_ASSERT_TRUE(g_forward);
}

static void test_dcc_128step_speed(void)
{
    const uint8_t packet[] = {0x03, 0x3F, 0x81, 0x03 ^ 0x3F ^ 0x81}; /* raw 1 = estop */
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(0, g_speed);
    TEST_ASSERT_TRUE(g_forward);

    g_speed_calls = 0;
    const uint8_t packet2[] = {0x03, 0x3F, 0x80, 0x03 ^ 0x3F ^ 0x80}; /* raw 0 = stop */
    feed_packet(packet2, sizeof(packet2), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(0, g_speed);

    g_speed_calls = 0;
    const uint8_t packet3[] = {0x03, 0x3F, 0xFE, 0x03 ^ 0x3F ^ 0xFE}; /* raw 126 -> 125 */
    feed_packet(packet3, sizeof(packet3), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(125, g_speed);
}

static void test_dcc_function_f13_group(void)
{
    const uint8_t packet[] = {0x03, 0xDE, 0x01, 0x03 ^ 0xDE ^ 0x01}; /* F13 on */
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(8, g_fn_calls);
    TEST_ASSERT_TRUE(g_fn_state[13]);
    TEST_ASSERT_FALSE(g_fn_state[14]);
    TEST_ASSERT_FALSE(g_fn_state[20]);
}

/* Service Mode Direct Write Byte (S-9.2.3): 0xFF 0x7C 0x02 0x64 -> CV3 = 100 */
static void test_dcc_service_write_direct(void)
{
    const uint8_t packet[] = {0xFF, 0x7C, 0x02, 0x64, 0xFF ^ 0x7C ^ 0x02 ^ 0x64};
    feed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(3, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(100, g_cv_value);
    TEST_ASSERT_TRUE(g_cv_service);
}

/* Service Mode Direct Verify Byte: match -> ACK pulse (2 gpio_set_level). */
static void test_dcc_service_verify_match(void)
{
    const uint8_t packet[] = {0xFF, 0x74, 0x02, 0x64, 0xFF ^ 0x74 ^ 0x02 ^ 0x64};
    g_read_ok = true;
    g_read_returns = 100;
    feed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);
}

static void test_dcc_service_verify_no_match(void)
{
    const uint8_t packet[] = {0xFF, 0x74, 0x02, 0x64, 0xFF ^ 0x74 ^ 0x02 ^ 0x64};
    g_read_ok = true;
    g_read_returns = 50;
    feed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
}

/* Service Mode Direct Bit Manipulation: set bit 2 of CV4 to 1.
 * data byte = 0xF0 | (1<<3) | 2 = 0xFA */
static void test_dcc_service_bit_write(void)
{
    const uint8_t packet[] = {0xFF, 0x78, 0x03, 0xFA, 0xFF ^ 0x78 ^ 0x03 ^ 0xFA};
    g_read_ok = true;
    g_read_returns = 0x00;
    feed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(4, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(0x04, g_cv_value);
    TEST_ASSERT_TRUE(g_cv_service);
}

static void test_dcc_idle_packet(void)
{
    const uint8_t packet[] = {0xFF, 0x00, 0xFF};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
}

static void test_dcc_broadcast_reset(void)
{
    const uint8_t packet[] = {0x00, 0x00, 0x00};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
}

static void test_dcc_broadcast_estop(void)
{
    const uint8_t packet[] = {0x00, 0x01, 0x01};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(0, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(0, g_speed);
}

/* Broadcast reset 00 00 00 (ровно 3 байта) сбрасывает; 6-байтовый broadcast
 * с нулевой инструкцией — НЕ сброс. */
static void test_dcc_broadcast_reset_len3_only(void)
{
    const uint8_t reset3[] = {0x00, 0x00, 0x00};
    feed_packet(reset3, sizeof(reset3), 12);
    TEST_ASSERT_EQUAL_INT(1, g_reset_calls);

    g_reset_calls = 0;
    g_speed_calls = 0;
    const uint8_t not_reset[] = {0x00, 0x00, 0xFC, 0x00, 0x00, 0xFC};
    feed_packet(not_reset, sizeof(not_reset), 12);
    TEST_ASSERT_EQUAL_INT(0, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
}

static void test_dcc_broadcast_unknown_instruction(void)
{
    g_reset_calls = 0;
    g_speed_calls = 0;
    g_fn_calls = 0;
    const uint8_t p[] = {0x00, 0x07, 0x40, 0xFF, 0xC0, 0x80, 0xF8};
    feed_packet(p, sizeof(p), 12);
    TEST_ASSERT_EQUAL_INT(0, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
    TEST_ASSERT_EQUAL_INT(0, g_fn_calls);
}

/* 28-step: скорость — 5-битный код (SSSS<<1)|F0. */
static void test_dcc_28step_code_mapping(void)
{
    const struct {
        uint8_t instr;
        uint16_t expected;
        bool fwd;
    } cases[] = {
        { 0x41, 1,   false }, /* code 2 -> speed 1 */
        { 0x51, 5,   false }, /* code 3 -> speed 5 */
        { 0x59, 74,  false }, /* code 19 -> speed 74 */
        { 0x7F, 126, true  }, /* code 31 -> full 126 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        g_speed_calls = 0;
        uint8_t packet[] = {0x03, cases[i].instr, 0x00};
        packet[2] = (uint8_t)(0x03 ^ cases[i].instr);
        feed_packet(packet, sizeof(packet), 12);
        TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
        TEST_ASSERT_EQUAL_UINT16(cases[i].expected, g_speed);
        TEST_ASSERT_EQUAL(cases[i].fwd, g_forward);
    }
}

/* Длинный адрес 8444 + 28-step: инструкция 0x49 -> скорость 70, реверс. */
static void test_dcc_long_address_28step(void)
{
    s_decoder_addr = 8444;
    s_decoder_long_addr = true;

    const uint8_t packet[] = {0xE0, 0xFC, 0x49, 0x55};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(70, g_speed);
    TEST_ASSERT_FALSE(g_forward);
}

/* 14-step режим: бит F0 — фара, скорость — младший ниббл. */
static void test_dcc_14step_f0_function(void)
{
    s_speed_mode_14 = true;

    const uint8_t packet[] = {0x03, 0x72, 0x71};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(9, g_speed);
    TEST_ASSERT_TRUE(g_forward);
    TEST_ASSERT_TRUE(g_fn_state[0]);
}

/* Сквозной тест через конвейер полупериодов: 128-step 03 3F A9 95 -> speed 40. */
static void test_dcc_half_period_pipeline_speed128(void)
{
    const uint8_t packet[] = {0x03, 0x3F, 0xA9, 0x95};
    feed_half_periods(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(40, g_speed);
    TEST_ASSERT_TRUE(g_forward);
}

/* Сквозной тест функций через конвейер: 03 85 86 -> F1=1, F3=1. */
static void test_dcc_half_period_pipeline_functions(void)
{
    const uint8_t packet[] = {0x03, 0x85, 0x86};
    feed_half_periods(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(5, g_fn_calls);
    TEST_ASSERT_TRUE(g_fn_state[1]);
    TEST_ASSERT_TRUE(g_fn_state[3]);
    TEST_ASSERT_FALSE(g_fn_state[0]);
    TEST_ASSERT_FALSE(g_fn_state[2]);
    TEST_ASSERT_FALSE(g_fn_state[4]);
}

/* ROCO передаёт байты MSB-first: пакет 03 3F 88 B4 -> скорость 7 вперёд. */
static void test_dcc_roco_msb_first_packet(void)
{
    const uint8_t packet[] = {0x03, 0x3F, 0x88, 0xB4};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(7, g_speed);
    TEST_ASSERT_TRUE(g_forward);
}

/* ---- dcc_reload_config (CV1/CV17/CV18/CV19/CV29 -> runtime config) ---- */

/* CV29 bit1 = 0 -> 14 steps, bit1 = 1 -> 28/128 steps (NMRA). Regression for
 * the inverted-polarity bug that made F0 flicker. */
static void test_dcc_reload_config_short_28step(void)
{
    g_cv_table_ok = true;
    g_cv_table[1] = 3;      /* short address */
    g_cv_table[29] = 0x02;  /* 28 steps */
    dcc_reload_config();

    const uint8_t packet[] = {0x03, 0x7F, 0x7C};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(126, g_speed);
    TEST_ASSERT_TRUE(g_forward);
    TEST_ASSERT_EQUAL_INT(0, g_fn_calls); /* 28-step must not touch F0 */
}

static void test_dcc_reload_config_14step(void)
{
    g_cv_table_ok = true;
    g_cv_table[1] = 3;
    g_cv_table[29] = 0x00; /* bit1 = 0 -> 14 steps */
    dcc_reload_config();

    const uint8_t packet[] = {0x03, 0x72, 0x71};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(9, g_speed);
    TEST_ASSERT_TRUE(g_forward);
    TEST_ASSERT_TRUE(g_fn_state[0]); /* 14-step bit4 is F0 */
}

static void test_dcc_reload_config_long_address(void)
{
    g_cv_table_ok = true;
    g_cv_table[29] = 0x22; /* long address + 28 steps */
    g_cv_table[17] = (uint8_t)(0xC0U | (8444U >> 8));
    g_cv_table[18] = (uint8_t)(8444U & 0xFFU);
    dcc_reload_config();

    const uint8_t packet[] = {0xE0, 0xFC, 0x49, 0x55};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(70, g_speed);
    TEST_ASSERT_FALSE(g_forward);

    /* The previous short address is no longer ours. */
    g_speed_calls = 0;
    const uint8_t short_pkt[] = {0x03, 0x7F, 0x7C};
    feed_packet(short_pkt, sizeof(short_pkt), 12);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
}

static void test_dcc_reload_config_consist_reverse(void)
{
    g_cv_table_ok = true;
    g_cv_table[1] = 3;
    g_cv_table[29] = 0x02;
    g_cv_table[19] = (uint8_t)(0x80U | 2U); /* consist addr 2, reverse */
    dcc_reload_config();

    const uint8_t packet[] = {0x02, 0x7F, 0x7D}; /* addr 2, 28-step forward */
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT16(126, g_speed);
    TEST_ASSERT_FALSE(g_forward); /* direction inverted by the consist */
}

/* ---- consist set directly ---- */

static void test_dcc_consist_match_and_reverse(void)
{
    dcc_set_consist(2, true);
    const uint8_t rev[] = {0x02, 0x7F, 0x7D};
    feed_packet(rev, sizeof(rev), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_FALSE(g_forward);

    /* Non-reverse consist keeps the decoded direction. */
    g_speed_calls = 0;
    dcc_set_consist(2, false);
    feed_packet(rev, sizeof(rev), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_TRUE(g_forward);
}

/* ---- Ops-mode short form (CV23/CV24) ---- */

static void test_dcc_ops_short_form_cv23(void)
{
    const uint8_t packet[] = {0x03, 0xF2, 0x10, 0x03 ^ 0xF2 ^ 0x10};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(23, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(0x10, g_cv_value);
    TEST_ASSERT_FALSE(g_cv_service);
}

static void test_dcc_ops_short_form_cv24(void)
{
    const uint8_t packet[] = {0x03, 0xF3, 0x20, 0x03 ^ 0xF3 ^ 0x20};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(24, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(0x20, g_cv_value);
}

/* ---- Ops-mode long form: Bit Manipulation ---- */

static void test_dcc_ops_bit_write(void)
{
    g_cv_table_ok = true;
    g_cv_table[5] = 0x00;
    /* instr 0xE8 = 1110_10_00: cc=2 bit-manip, CV high bits 0, CV5.
     * data 0x19 = 0b0001_1001: D=1 (write), value=1, bit=1. */
    const uint8_t packet[] = {0x03, 0xE8, 0x04, 0x19, 0x03 ^ 0xE8 ^ 0x04 ^ 0x19};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(5, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(0x02, g_cv_value);
    TEST_ASSERT_FALSE(g_cv_service);
}

/* Bit verify in ops mode: no callback (no RailCom ACK). */
static void test_dcc_ops_bit_verify_no_write(void)
{
    g_cv_table_ok = true;
    g_cv_table[5] = 0x02;
    const uint8_t packet[] = {0x03, 0xE8, 0x04, 0x09, 0x03 ^ 0xE8 ^ 0x04 ^ 0x09};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
}

/* Ops-mode write to a CV above 255 (instruction low bits carry the high bits). */
static void test_dcc_ops_write_high_cv(void)
{
    const uint8_t packet[] = {0x03, 0xED, 0x2B, 0x55, 0x03 ^ 0xED ^ 0x2B ^ 0x55};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(300, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(0x55, g_cv_value);
}

/* ---- Extended functions ---- */

static void test_dcc_function_f21_group(void)
{
    const uint8_t packet[] = {0x03, 0xDF, 0x03, 0x03 ^ 0xDF ^ 0x03};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(8, g_fn_calls);
    TEST_ASSERT_TRUE(g_fn_state[21]);
    TEST_ASSERT_TRUE(g_fn_state[22]);
    TEST_ASSERT_FALSE(g_fn_state[20]);
    TEST_ASSERT_FALSE(g_fn_state[23]);
}

static void test_dcc_function_group2_f9_f12(void)
{
    /* Group 2 is a 2-byte packet: 1011_D_FFFF, the low nibble carries the
     * states. D=1 selects F9..F12. 0xBA -> F10 + F12. */
    const uint8_t packet[] = {0x03, 0xBA, 0x03 ^ 0xBA};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(4, g_fn_calls);
    TEST_ASSERT_TRUE(g_fn_state[10]);
    TEST_ASSERT_TRUE(g_fn_state[12]);
    TEST_ASSERT_FALSE(g_fn_state[9]);
    TEST_ASSERT_FALSE(g_fn_state[11]);
}

/* ---- Service mode bit verify (ACK on match) ---- */

static void test_dcc_service_bit_verify(void)
{
    g_cv_table_ok = true;
    g_cv_table[4] = 0x04; /* bit 2 set */
    /* 0xFF 0x78 0x03 -> CV4; data 0x0A = value=1 bit=2, D=0 -> verify. */
    const uint8_t packet[] = {0xFF, 0x78, 0x03, 0x0A, 0xFF ^ 0x78 ^ 0x03 ^ 0x0A};
    feed_packet(packet, sizeof(packet), 22);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);

    /* Mismatch -> no ACK. */
    mock_gpio_set_level_count = 0;
    g_cv_table[4] = 0x00;
    feed_packet(packet, sizeof(packet), 22);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
}

/* Service mode requires the long (>=20 ones) preamble. With the normal
 * preamble a 0xFF-addressed packet is treated as idle. */
static void test_dcc_service_needs_long_preamble(void)
{
    const uint8_t packet[] = {0xFF, 0x7C, 0x02, 0x64, 0xFF ^ 0x7C ^ 0x02 ^ 0x64};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
}

/* Fewer than 10 preamble ones never start a byte. */
static void test_dcc_short_preamble_no_decode(void)
{
    const uint8_t packet[] = {0x03, 0x7F, 0x7C};
    feed_packet(packet, sizeof(packet), 5);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
}

/* A packet longer than DCC_PACKET_MAX resets the parser; the next valid packet
 * still decodes. */
static void test_dcc_oversized_packet_resets(void)
{
    for (int i = 0; i < 12; ++i) {
        consume_bit(1);
    }
    for (int b = 0; b < 9; ++b) {
        consume_bit(0); /* start / separator */
        for (int bit = 0; bit < 8; ++bit) {
            consume_bit(0);
        }
    }
    const uint8_t packet[] = {0x03, 0xEC, 0x00, 0x2A, 0xC5};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
}

/* ---- Timestamp of the last packet addressed to us ---- */

static void test_dcc_last_packet_timestamp(void)
{
    mock_timer_now_us = 1000;
    const uint8_t packet[] = {0x03, 0x7F, 0x7C};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT64(1000, dcc_last_packet_us());

    /* Packet for another address must not refresh the timestamp. */
    mock_timer_now_us = 2000;
    const uint8_t other[] = {0x2A, 0x7F, 0x2A ^ 0x7F};
    feed_packet(other, sizeof(other), 12);
    TEST_ASSERT_EQUAL_INT64(1000, dcc_last_packet_us());

    /* Broadcast (e-stop) addresses everyone: timestamp refreshes. */
    mock_timer_now_us = 3000;
    const uint8_t bc[] = {0x00, 0x01, 0x01};
    feed_packet(bc, sizeof(bc), 12);
    TEST_ASSERT_EQUAL_INT64(3000, dcc_last_packet_us());
}

static void test_dcc_service_ack_pulses_gpio(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, dcc_service_ack());
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);
    TEST_ASSERT_EQUAL_INT(1, mock_vtask_delay_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_half_period_classification);
    RUN_TEST(test_dcc_glitch_resets_parser);
    RUN_TEST(test_dcc_bit_order_ops_write);
    RUN_TEST(test_dcc_bad_checksum_rejected);
    RUN_TEST(test_dcc_packet_not_for_us_filtered);
    RUN_TEST(test_dcc_long_address);
    RUN_TEST(test_dcc_28step_f0_max_speed);
    RUN_TEST(test_dcc_28step_stop_forward);
    RUN_TEST(test_dcc_28step_code_mapping);
    RUN_TEST(test_dcc_long_address_28step);
    RUN_TEST(test_dcc_14step_f0_function);
    RUN_TEST(test_dcc_128step_speed);
    RUN_TEST(test_dcc_function_f13_group);
    RUN_TEST(test_dcc_half_period_pipeline_speed128);
    RUN_TEST(test_dcc_half_period_pipeline_functions);
    RUN_TEST(test_dcc_roco_msb_first_packet);
    RUN_TEST(test_dcc_reload_config_short_28step);
    RUN_TEST(test_dcc_reload_config_14step);
    RUN_TEST(test_dcc_reload_config_long_address);
    RUN_TEST(test_dcc_reload_config_consist_reverse);
    RUN_TEST(test_dcc_consist_match_and_reverse);
    RUN_TEST(test_dcc_ops_short_form_cv23);
    RUN_TEST(test_dcc_ops_short_form_cv24);
    RUN_TEST(test_dcc_ops_bit_write);
    RUN_TEST(test_dcc_ops_bit_verify_no_write);
    RUN_TEST(test_dcc_ops_write_high_cv);
    RUN_TEST(test_dcc_function_f21_group);
    RUN_TEST(test_dcc_function_group2_f9_f12);
    RUN_TEST(test_dcc_service_bit_verify);
    RUN_TEST(test_dcc_service_needs_long_preamble);
    RUN_TEST(test_dcc_short_preamble_no_decode);
    RUN_TEST(test_dcc_oversized_packet_resets);
    RUN_TEST(test_dcc_last_packet_timestamp);
    RUN_TEST(test_dcc_service_ack_pulses_gpio);
    RUN_TEST(test_dcc_service_write_direct);
    RUN_TEST(test_dcc_service_verify_match);
    RUN_TEST(test_dcc_service_verify_no_match);
    RUN_TEST(test_dcc_service_bit_write);
    RUN_TEST(test_dcc_idle_packet);
    RUN_TEST(test_dcc_broadcast_reset);
    RUN_TEST(test_dcc_broadcast_reset_len3_only);
    RUN_TEST(test_dcc_broadcast_unknown_instruction);
    RUN_TEST(test_dcc_broadcast_estop);
    return UNITY_END();
}
