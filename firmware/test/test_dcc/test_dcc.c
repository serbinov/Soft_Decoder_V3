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
static int      g_emergency_calls, g_hard_reset_calls;
static esp_err_t g_write_result;

static uint8_t  g_read_returns;
static bool     g_read_ok;

/* Full CV image for dcc_reload_config() tests. */
#define TEST_CV_MAX 513
static uint8_t  g_cv_table[TEST_CV_MAX];
static bool     g_cv_table_ok;

static esp_err_t on_cv_write(uint16_t cv, uint8_t value, bool service_mode)
{
    g_cv_index = cv;
    g_cv_value = value;
    g_cv_service = service_mode;
    g_cv_write_calls++;
    return g_write_result;
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
static void on_emergency(void) { g_emergency_calls++; }
static void on_hard_reset(void) { g_hard_reset_calls++; }

static void startup_hook(void (*task)(void *), void *arg)
{
    (void)task;
    esp_err_t result = dcc_receiver_start();
    (void)xQueueSend((QueueHandle_t)arg, &result, 0);
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
    g_emergency_calls = g_hard_reset_calls = 0;
    g_write_result = ESP_OK;
    g_read_ok = false;
    g_read_returns = 0;
    g_cv_table_ok = false;
    memset(g_cv_table, 0, sizeof(g_cv_table));
    mock_gpio_set_level_count = 0;
    mock_vtask_delay_count = 0;
    mock_timer_now_us = 0;
    mock_queue_create_fail = 0;
    mock_queue_create_fail_after = -1;
    mock_queue_create_calls = 0;
    mock_task_create_ok = 1;
    mock_task_create_fail_after = -1;
    mock_task_create_calls = 0;
    mock_queue_send_calls = 0;
    mock_queue_send_fail = 0;
    mock_queue_send_fail_after = -1;
    mock_gpio_isr_install_err = 0;
    mock_gpio_isr_add_err = 0;
    mock_task_startup_hook = startup_hook;
    /* Drop queues left over from a dcc_init() test so each test starts clean. */
    if (s_queue != NULL) {
        vQueueDeleteWithCaps(s_queue);
        s_queue = NULL;
    }
    if (s_ack_queue != NULL) {
        vQueueDelete(s_ack_queue);
        s_ack_queue = NULL;
    }
    s_ack_worker = false;
    s_ack_iter_cap = 0;
    s_initialized = false;
    s_last_packet_us = s_last_signal_packet_us = s_last_edge_us = 0;
    s_last_input_edge_us = s_init_us = 0;
    s_isr_overruns = 0;

    s_decoder_addr = 3;
    s_decoder_long_addr = false;
    s_speed_mode_14 = false;
    s_consist_addr = 0;
    s_consist_reverse = false;
    s_service_mode = false;
    s_service_candidate = false;
    s_service_deadline_us = 20000;
    s_service_pending.len = s_ops_pending.len = 0;
    s_control_enabled = true;
    s_control_generation = s_seen_control_generation = 0;
    s_direction_reverse = false;
    s_function_forward = true;
    s_cv21 = s_cv22 = 0;

    reset_parser();

    s_speed_cb = on_speed;
    s_function_cb = on_function;
    s_cv_write_cb = on_cv_write;
    s_cv_read_cb = on_cv_read;
    s_reset_cb = on_reset;
    s_emergency_stop_cb = on_emergency;
    s_hard_reset_cb = on_hard_reset;
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

static void feed_confirmed_packet(const uint8_t *bytes, size_t len, unsigned preamble)
{
    feed_packet(bytes, len, preamble);
    feed_packet(bytes, len, preamble);
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
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ZERO, (uint8_t)classify(293));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ZERO, (uint8_t)classify(5000));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_ZERO, (uint8_t)classify(10000));
    TEST_ASSERT_EQUAL_UINT8(DCC_HALF_NONE, (uint8_t)classify(10001));
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
    feed_confirmed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
}

/* Ops-mode write byte: addr 3, CV1 = 42. MSB-first must decode correctly
 * (regression for the MSB/LSB bit-order bug). */
static void test_dcc_bit_order_ops_write(void)
{
    const uint8_t packet[] = {0x03, 0xEC, 0x00, 0x2A, 0xC5};
    feed_confirmed_packet(packet, sizeof(packet), 12);

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
    feed_confirmed_packet(packet, sizeof(packet), 12);

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
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
    TEST_ASSERT_EQUAL_INT(1, g_emergency_calls);

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

/* Service Mode Direct Write Byte (S-9.2.3): instr, CvLow, data, checksum.
 * There is no address byte: 0x7C 0x02 0x64 -> CV3 = 100. */
static void test_dcc_service_write_direct(void)
{
    const uint8_t packet[] = {0x7C, 0x02, 0x64, 0x7C ^ 0x02 ^ 0x64};
    s_service_mode = true;
    feed_confirmed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(3, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(100, g_cv_value);
    TEST_ASSERT_TRUE(g_cv_service);
}

/* Service Mode Direct Verify Byte: match -> ACK pulse (2 gpio_set_level). */
static void test_dcc_service_verify_match(void)
{
    const uint8_t packet[] = {0x74, 0x02, 0x64, 0x74 ^ 0x02 ^ 0x64};
    g_read_ok = true;
    g_read_returns = 100;
    s_service_mode = true;
    feed_confirmed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);
}

static void test_dcc_service_verify_no_match(void)
{
    const uint8_t packet[] = {0x74, 0x02, 0x64, 0x74 ^ 0x02 ^ 0x64};
    g_read_ok = true;
    g_read_returns = 50;
    s_service_mode = true;
    feed_confirmed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
}

/* Service Mode Direct Bit Manipulation: set bit 2 of CV4 to 1.
 * data byte = 0xF0 | (1<<3) | 2 = 0xFA */
static void test_dcc_service_bit_write(void)
{
    const uint8_t packet[] = {0x78, 0x03, 0xFA, 0x78 ^ 0x03 ^ 0xFA};
    g_read_ok = true;
    g_read_returns = 0x00;
    s_service_mode = true;
    feed_confirmed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(4, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(0x04, g_cv_value);
    TEST_ASSERT_TRUE(g_cv_service);
    /* A successful bit write must also ACK (NMRA S-9.2.3). */
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);
}

/* Service mode is entered by a reset with an extended preamble on the
 * programming track; afterwards service instructions are decoded. */
static void test_dcc_service_mode_entered_by_reset(void)
{
    const uint8_t reset[] = {0x00, 0x00, 0x00};
    feed_packet(reset, sizeof(reset), 22);
    TEST_ASSERT_TRUE(s_service_candidate);
    TEST_ASSERT_FALSE(s_service_mode);

    const uint8_t packet[] = {0x7C, 0x02, 0x64, 0x7C ^ 0x02 ^ 0x64};
    feed_confirmed_packet(packet, sizeof(packet), 22);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(3, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(100, g_cv_value);
}

/* Short address 124 must not be mistaken for a service write: a 128-step
 * packet for that address is also 4 bytes and starts with 0x7C. */
static void test_dcc_short_addr_124_not_service(void)
{
    s_decoder_addr = 124;
    s_decoder_long_addr = false;
    s_service_mode = false;
    /* addr 124, 128-step, forward bit 0, speed code 65. */
    const uint8_t packet[] = {124, 0x3F, 0x41, 124 ^ 0x3F ^ 0x41};
    feed_packet(packet, sizeof(packet), 22);

    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT8(64, g_speed);
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
    const uint8_t packet[] = {0x00, 0x61, 0x61};
    feed_packet(packet, sizeof(packet), 12);

    TEST_ASSERT_EQUAL_INT(0, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
    TEST_ASSERT_EQUAL_INT(1, g_emergency_calls);
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
        { 0x42, 1,   false }, /* code 4 -> step 1 */
        { 0x52, 5,   false }, /* code 5 -> step 2 */
        { 0x59, 70,  false }, /* code 19 -> step 16 */
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
    TEST_ASSERT_EQUAL_UINT16(65, g_speed);
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
    TEST_ASSERT_EQUAL_UINT16(65, g_speed);
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
     * data 0xF9: D=1 (write), value=1, bit=1. */
    const uint8_t packet[] = {0x03, 0xE8, 0x04, 0xF9, 0x03 ^ 0xE8 ^ 0x04 ^ 0xF9};
    feed_confirmed_packet(packet, sizeof(packet), 12);
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
    const uint8_t packet[] = {0x03, 0xE8, 0x04, 0xE9, 0x03 ^ 0xE8 ^ 0x04 ^ 0xE9};
    feed_packet(packet, sizeof(packet), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
}

/* Ops-mode write to a CV above 255 (instruction low bits carry the high bits). */
static void test_dcc_ops_write_high_cv(void)
{
    const uint8_t packet[] = {0x03, 0xED, 0x2B, 0x55, 0x03 ^ 0xED ^ 0x2B ^ 0x55};
    feed_confirmed_packet(packet, sizeof(packet), 12);
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
    /* 1010_DDDD controls F9..F12. */
    const uint8_t packet[] = {0x03, 0xAA, 0x03 ^ 0xAA};
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
    /* 0x78 0x03 -> CV4; data 0x0A = value=1 bit=2, D=0 -> verify. */
    const uint8_t packet[] = {0x78, 0x03, 0xEA, 0x78 ^ 0x03 ^ 0xEA};
    s_service_mode = true;
    feed_confirmed_packet(packet, sizeof(packet), 22);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);

    /* Mismatch -> no ACK. */
    mock_gpio_set_level_count = 0;
    g_cv_table[4] = 0x00;
    feed_packet(packet, sizeof(packet), 22);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
}

/* Service mode requires the long (>=20 ones) preamble even after entry. */
static void test_dcc_service_needs_long_preamble(void)
{
    const uint8_t packet[] = {0x7C, 0x02, 0x64, 0x7C ^ 0x02 ^ 0x64};
    s_service_mode = true;
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
    feed_confirmed_packet(packet, sizeof(packet), 12);
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

/* With the ACK worker available, dcc_service_ack() must only enqueue: the DCC
 * parser task must not block on the pulse. */
static void test_dcc_service_ack_queued(void)
{
    s_ack_queue = xQueueCreate(DCC_ACK_QUEUE_LEN, sizeof(uint8_t));
    TEST_ASSERT_NOT_NULL(s_ack_queue);
    s_ack_worker = true;
    mock_queue_send_calls = 0;
    mock_gpio_set_level_count = 0;
    mock_vtask_delay_count = 0;

    TEST_ASSERT_EQUAL(ESP_OK, dcc_service_ack());
    TEST_ASSERT_EQUAL_INT(1, mock_queue_send_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
    TEST_ASSERT_EQUAL_INT(0, mock_vtask_delay_count);

    vQueueDelete(s_ack_queue);
    s_ack_queue = NULL;
}

/* A full ACK queue drops the redundant ACK without blocking. */
static void test_dcc_service_ack_queue_full(void)
{
    s_ack_queue = xQueueCreate(DCC_ACK_QUEUE_LEN, sizeof(uint8_t));
    TEST_ASSERT_NOT_NULL(s_ack_queue);
    s_ack_worker = true;
    mock_queue_send_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, dcc_service_ack());
    mock_queue_send_fail = 0;
    vQueueDelete(s_ack_queue);
    s_ack_queue = NULL;
}

/* The worker turns a queued token into the 6 ms high/low pulse. */
static void test_dcc_ack_task_pulse(void)
{
    s_ack_queue = xQueueCreate(DCC_ACK_QUEUE_LEN, sizeof(uint8_t));
    TEST_ASSERT_NOT_NULL(s_ack_queue);
    uint8_t token = 1U;
    TEST_ASSERT_TRUE(xQueueSend(s_ack_queue, &token, 0) == pdTRUE);
    mock_gpio_set_level_count = 0;
    mock_vtask_delay_count = 0;

    s_ack_iter_cap = 1;
    dcc_ack_task(NULL);
    s_ack_iter_cap = 0;

    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);
    TEST_ASSERT_EQUAL_INT(1, mock_vtask_delay_count);
    vQueueDelete(s_ack_queue);
    s_ack_queue = NULL;
}

/* ---- coverage: remaining branches (ISR / task / init / parser edges) ---- */

static void test_dcc_read_cv_null_callback(void)
{
    /* Ops-mode long-form bit manipulation, write, value=1, bit 0, CV4. */
    const uint8_t pkt[] = { 0x03, 0xE8, 0x03, 0x18, 0x03 ^ 0xE8 ^ 0x03 ^ 0x18 };
    s_cv_read_cb = NULL; /* read_cv() cannot read -> no write */
    feed_packet(pkt, sizeof(pkt), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
}

static void test_dcc_service_short_packet(void)
{
    /* 3-byte service packet: valid checksum but no room for the data byte. */
    const uint8_t pkt[] = { 0x7C, 0x02, 0x7C ^ 0x02 };
    s_service_mode = true;
    feed_packet(pkt, sizeof(pkt), 20);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
}

static void test_dcc_service_bit_manip_clear(void)
{
    g_read_ok = true;
    g_read_returns = 0xFF;
    /* Service bit manipulation: write 0 to bit 0 of CV4 -> clear the bit. */
    const uint8_t pkt[] = { 0x78, 0x03, 0xF0, 0x78 ^ 0x03 ^ 0xF0 };
    s_service_mode = true;
    feed_confirmed_packet(pkt, sizeof(pkt), 20);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT16(4, g_cv_index);
    TEST_ASSERT_EQUAL_UINT8(0xFE, g_cv_value);
    TEST_ASSERT_TRUE(g_cv_service);
}

static void test_dcc_ops_bit_manip_clear(void)
{
    g_read_ok = true;
    g_read_returns = 0xFF;
    const uint8_t pkt[] = { 0x03, 0xE8, 0x03, 0xF0, 0x03 ^ 0xE8 ^ 0x03 ^ 0xF0 };
    feed_confirmed_packet(pkt, sizeof(pkt), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_UINT8(0xFE, g_cv_value);
}

static void test_dcc_addressed_packet_too_short(void)
{
    /* Long address, 3-byte packet: room for the address but no instruction. */
    s_decoder_addr = 10;
    s_decoder_long_addr = true;
    const uint8_t pkt[] = { 0xC0, 0x0A, 0xC0 ^ 0x0A };
    feed_packet(pkt, sizeof(pkt), 12);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
    TEST_ASSERT_EQUAL_INT(0, g_fn_calls);
}

static void test_dcc_consist_reverse_128_step(void)
{
    s_decoder_addr = 3;
    s_consist_addr = 7; /* matches via the consist */
    s_consist_reverse = true;
    /* 128-step: addr 7, 0x3F, ext=0x8A (forward, speed 10). */
    const uint8_t pkt[] = { 0x07, 0x3F, 0x8A, 0x07 ^ 0x3F ^ 0x8A };
    feed_packet(pkt, sizeof(pkt), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_FALSE(g_forward); /* flipped by the consist reverse flag */
}

static void test_dcc_feed_half_period_kind_change(void)
{
    reset_parser();
    feed_half_period(58);  /* ONE pending */
    feed_half_period(100); /* ZERO -> kind change -> re-arm */
    TEST_ASSERT_EQUAL(DCC_HALF_ZERO, s_pending_half);
}

static void test_dcc_isr_paths(void)
{
    mock_queue_create_fail = 0;
    TEST_ASSERT_EQUAL(ESP_OK, dcc_init());

    /* prev <= 0 -> just store the reference time. */
    s_last_edge_us = 0;
    mock_timer_now_us = 1000;
    dcc_isr(NULL);
    TEST_ASSERT_EQUAL_INT64(1000, s_last_edge_us);

    /* dt > MAX -> re-acquire. */
    s_last_edge_us = 1000;
    mock_timer_now_us = 1000 + DCC_MAX_EDGE_GAP_US + 1;
    dcc_isr(NULL);
    TEST_ASSERT_EQUAL_INT64(mock_timer_now_us, s_last_edge_us);

    /* dt < MIN -> ignore the edge, keep the reference time. */
    s_last_edge_us = 5000;
    mock_timer_now_us = 5000 + (DCC_MIN_HALF_PERIOD_US - 1);
    dcc_isr(NULL);
    TEST_ASSERT_EQUAL_INT64(5000, s_last_edge_us);

    /* Valid edge -> enqueued. */
    s_last_edge_us = 100;
    mock_timer_now_us = 200;
    dcc_isr(NULL);
    TEST_ASSERT_EQUAL_INT64(200, s_last_edge_us);

    /* Queue send failure -> overrun counter. */
    s_last_edge_us = 300;
    mock_timer_now_us = 400;
    s_isr_overruns = 0;
    mock_queue_send_fail = 1;
    dcc_isr(NULL);
    TEST_ASSERT_TRUE(s_isr_overruns != 0U);
    mock_queue_send_fail = 0;
}

static void test_dcc_task_processes_queue(void)
{
    mock_queue_create_fail = 0;
    mock_gpio_isr_install_err = 0;
    TEST_ASSERT_EQUAL(ESP_OK, dcc_init());
    dcc_half_t hp = { .dt_us = 58 };
    TEST_ASSERT_TRUE(xQueueSend(s_queue, &hp, 0) == pdTRUE);

    s_dcc_iter_cap = 1;
    s_isr_overruns = 1; /* forces the reset-on-overrun path */
    dcc_task(NULL);
    s_dcc_iter_cap = 0;
}

static void test_dcc_task_isr_install_failure(void)
{
    mock_gpio_isr_install_err = ESP_FAIL; /* covers the ESP_LOGE branch */
    mock_queue_create_fail = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, dcc_init());
    TEST_ASSERT_NULL(s_queue);
    mock_gpio_isr_install_err = 0;
}

static void test_dcc_init_failures(void)
{
    mock_queue_create_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, dcc_init());
    mock_queue_create_fail = 0;

    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, dcc_init());
    mock_task_create_ok = 1;
    TEST_ASSERT_EQUAL(ESP_OK, dcc_init());
}

/* First xQueueCreate (half periods) succeeds, second (ACK) fails. */
static void test_dcc_init_ack_queue_fail(void)
{
    mock_queue_create_fail_after = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, dcc_init());
    mock_queue_create_fail_after = -1;
    TEST_ASSERT_NULL(s_ack_queue);
}

/* ACK task creation failure leaves the decoder running with the inline ACK.
 * The queue stays allocated (dcc_task may still reference it). */
static void test_dcc_init_ack_task_fail(void)
{
    mock_task_create_fail_after = 1; /* dcc task ok, ack task fails */
    TEST_ASSERT_EQUAL(ESP_OK, dcc_init());
    mock_task_create_fail_after = -1;
    TEST_ASSERT_NOT_NULL(s_ack_queue);
    TEST_ASSERT_FALSE(s_ack_worker);

    /* With no worker the inline path is used even though the queue exists. */
    mock_gpio_set_level_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, dcc_service_ack());
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);
}

static void test_dcc_register_callbacks(void)
{
    dcc_register_speed_cb(on_speed);
    dcc_register_function_cb(on_function);
    dcc_register_cv_write_cb(on_cv_write);
    dcc_register_cv_read_cb(on_cv_read);
    dcc_register_reset_cb(on_reset);
    TEST_ASSERT_TRUE(s_speed_cb == on_speed);
    TEST_ASSERT_TRUE(s_function_cb == on_function);
    TEST_ASSERT_TRUE(s_cv_write_cb == on_cv_write);
    TEST_ASSERT_TRUE(s_cv_read_cb == on_cv_read);
    TEST_ASSERT_TRUE(s_reset_cb == on_reset);
}

/* Fuzz / robustness: a long stream of random bits must never crash. */
static void test_fuzz_random_bits(void)
{
    uint32_t seed = 0xC0FFEEu;
    for (int iter = 0; iter < 50000; ++iter) {
        seed = seed * 1103515245u + 12345u;
        consume_bit((uint8_t)(seed >> 31));
    }
    TEST_ASSERT_TRUE(true);
}

static void test_address_partitions_and_signal_clock(void)
{
    dcc_set_address(865, true);
    mock_timer_now_us = 1234;
    const uint8_t accessory[] = {0x83, 0x61, 0x7F, 0x9D};
    feed_packet(accessory, sizeof(accessory), 12);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
    TEST_ASSERT_EQUAL_INT64(1234, dcc_last_signal_packet_us());
    TEST_ASSERT_EQUAL_INT64(0, dcc_last_packet_us());
    const uint8_t reserved[] = {0xE8, 0x61, 0x7F, 0xF6};
    feed_packet(reserved, sizeof(reserved), 12);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
    const uint8_t bad[] = {0x03, 0x7F, 0x00};
    mock_timer_now_us = 2000;
    feed_packet(bad, sizeof(bad), 12);
    TEST_ASSERT_EQUAL_INT64(1234, dcc_last_signal_packet_us());
}

static void test_stop_emergency_vectors_all_modes(void)
{
    const uint8_t emergency[][3] = {{0x03,0x61,0x62}, {0x03,0x71,0x72}};
    const uint8_t stop[][3] = {{0x03,0x60,0x63}, {0x03,0x70,0x73}};
    for (int mode = 0; mode < 2; mode++) {
        s_speed_mode_14 = mode != 0;
        for (int i = 0; i < 2; i++) {
            feed_packet(emergency[i], 3, 12);
            TEST_ASSERT_EQUAL_INT(mode * 2 + i + 1, g_emergency_calls);
            TEST_ASSERT_EQUAL_INT(mode * 2 + i, g_speed_calls);
            feed_packet(stop[i], 3, 12);
            TEST_ASSERT_EQUAL_UINT8(0, g_speed);
        }
    }
}

static void test_function_group2_f5_f8(void)
{
    const uint8_t p[] = {0x03,0xB1,0xB2};
    feed_packet(p, sizeof(p), 12);
    TEST_ASSERT_TRUE(g_fn_state[5]);
    TEST_ASSERT_FALSE(g_fn_state[9]);
    TEST_ASSERT_EQUAL_INT(4, g_fn_calls);
}

static void test_ops_confirmation_invalidation(void)
{
    const uint8_t write[] = {0x03,0xEC,0x00,0x2A,0xC5};
    const uint8_t other[] = {0x04,0x60,0x64};
    const uint8_t own[] = {0x03,0x60,0x63};
    const uint8_t broadcast[] = {0x00,0x60,0x60};
    feed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    feed_packet(other, sizeof(other), 12);
    feed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    feed_packet(write, sizeof(write), 12);
    feed_packet(own, sizeof(own), 12);
    feed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    feed_packet(broadcast, sizeof(broadcast), 12);
    feed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    feed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(2, g_cv_write_calls);
}

static void test_service_entry_deadline_and_other_address_exit(void)
{
    const uint8_t reset[] = {0,0,0};
    const uint8_t write[] = {0x7C,0x02,0x64,0x1A};
    const uint8_t other[] = {0x04,0x60,0x64};
    feed_packet(reset, sizeof(reset), 22);
    feed_packet(write, sizeof(write), 22);
    TEST_ASSERT_TRUE(s_service_mode);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    feed_packet(other, sizeof(other), 12);
    TEST_ASSERT_FALSE(s_service_mode);
    feed_confirmed_packet(write, sizeof(write), 22);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    feed_packet(reset, sizeof(reset), 22);
    mock_timer_now_us += 20000;
    feed_confirmed_packet(write, sizeof(write), 22);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    feed_packet(reset, sizeof(reset), 22);
    feed_packet(write, sizeof(write), 22);
    const uint8_t reset_again[] = {0,0,0};
    feed_packet(reset_again, sizeof(reset_again), 22);
    feed_packet(write, sizeof(write), 22);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    feed_packet(write, sizeof(write), 22);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
}

static void test_service_failure_and_verify_confirmation(void)
{
    const uint8_t reset[] = {0,0,0};
    const uint8_t write[] = {0x7C,0x02,0x64,0x1A};
    const uint8_t verify[] = {0x74,0x02,0x64,0x12};
    feed_packet(reset, sizeof(reset), 22);
    g_write_result = ESP_FAIL;
    feed_confirmed_packet(write, sizeof(write), 22);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
    g_read_ok = true;
    g_read_returns = 100;
    feed_packet(verify, sizeof(verify), 22);
    TEST_ASSERT_EQUAL_INT(0, mock_gpio_set_level_count);
    feed_packet(verify, sizeof(verify), 22);
    TEST_ASSERT_EQUAL_INT(2, mock_gpio_set_level_count);
}

static void test_unsupported_xpom_and_service_lengths(void)
{
    const uint8_t xpom[] = {0x03,0xEC,0x01,0x02,0x04,0x2A,0xC2};
    feed_confirmed_packet(xpom, sizeof(xpom), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    const uint8_t reset[] = {0,0,0};
    feed_packet(reset, sizeof(reset), 22);
    const uint8_t extra[] = {0x7C,0x01,0x02,0x03,0x7C};
    feed_confirmed_packet(extra, sizeof(extra), 22);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
}

static void test_consist_speed_functions_and_cv_routing(void)
{
    dcc_set_consist(2, false);
    const uint8_t own[] = {0x03,0x7F,0x7C};
    feed_packet(own, sizeof(own), 12);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
    const uint8_t group1[] = {0x02,0x9F,0x9D};
    feed_packet(group1, sizeof(group1), 12);
    TEST_ASSERT_EQUAL_INT(0, g_fn_calls);
    s_cv21 = 0x01; /* F1 only */
    s_cv22 = 0x05; /* F0 forward, F9 */
    feed_packet(group1, sizeof(group1), 12);
    TEST_ASSERT_TRUE(g_fn_state[0]);
    TEST_ASSERT_TRUE(g_fn_state[1]);
    TEST_ASSERT_FALSE(g_fn_state[2]);
    TEST_ASSERT_EQUAL_INT(2, g_fn_calls);
    const uint8_t group2[] = {0x02,0xAF,0xAD};
    feed_packet(group2, sizeof(group2), 12);
    TEST_ASSERT_TRUE(g_fn_state[9]);
    TEST_ASSERT_FALSE(g_fn_state[10]);
    const uint8_t write[] = {0x02,0xEC,0x00,0x2A,0xC4};
    feed_confirmed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    const uint8_t short_cv[] = {0x02,0xF2,0x10,0xE0};
    feed_packet(short_cv, sizeof(short_cv), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
    dcc_set_address(2, false);
    const uint8_t same[] = {0x02,0x7F,0x7D};
    feed_packet(same, sizeof(same), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
}

static void test_direction_inversion_and_control_gate(void)
{
    g_cv_table_ok = true;
    g_cv_table[1] = 3;
    g_cv_table[29] = 3;
    dcc_reload_config();
    const uint8_t speed[] = {0x03,0x7F,0x7C};
    feed_packet(speed, sizeof(speed), 12);
    TEST_ASSERT_FALSE(g_forward);
    dcc_set_control_enabled(false);
    mock_timer_now_us = 500;
    feed_packet(speed, sizeof(speed), 12);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_INT64(500, dcc_last_signal_packet_us());
    dcc_set_control_enabled(true);
    feed_packet(speed, sizeof(speed), 12);
    TEST_ASSERT_EQUAL_INT(2, g_speed_calls);
}

static void test_hard_reset_is_separate_operation(void)
{
    const uint8_t hard[] = {0x00,0x01,0x01};
    feed_packet(hard, sizeof(hard), 12);
    TEST_ASSERT_EQUAL_INT(1, g_hard_reset_calls);
    TEST_ASSERT_EQUAL_INT(1, g_reset_calls);
    TEST_ASSERT_EQUAL_INT(1, g_emergency_calls);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls); /* Never substitute factory CV8 reset. */
}

static void test_stretched_zero_pipeline(void)
{
    /* Asymmetric zero halves are valid: one stretched, the other normal. */
    const uint8_t packet[] = {0x03,0x7F,0x7C};
    for (int i = 0; i < 12; i++) {
        feed_half_period(58); feed_half_period(58);
    }
    for (size_t i = 0; i < sizeof(packet); i++) {
        feed_half_period(5000); feed_half_period(100);
        for (int bit = 7; bit >= 0; bit--) {
            bool one = (packet[i] & (1U << bit)) != 0;
            feed_half_period(one ? 58 : 10000);
            feed_half_period(one ? 58 : 100);
        }
    }
    feed_half_period(58); feed_half_period(58);
    TEST_ASSERT_EQUAL_INT(1, g_speed_calls);
    TEST_ASSERT_EQUAL_UINT8(126, g_speed);
}

static void test_signal_static_uses_all_edges_and_boot_deadline(void)
{
    s_queue = xQueueCreate(DCC_QUEUE_LEN, sizeof(dcc_half_t));
    s_init_us = 1000;
    mock_timer_now_us = 31000;
    TEST_ASSERT_FALSE(dcc_signal_is_static(30000));
    mock_timer_now_us++;
    TEST_ASSERT_TRUE(dcc_signal_is_static(30000));
    dcc_isr(NULL);
    mock_timer_now_us += 20; /* Rejected parser glitch is still an input edge. */
    dcc_isr(NULL);
    mock_timer_now_us += 30000;
    TEST_ASSERT_FALSE(dcc_signal_is_static(30000));
    mock_timer_now_us++;
    TEST_ASSERT_TRUE(dcc_signal_is_static(30000));
}

static void test_init_handler_failure_and_existing_service_rejected(void)
{
    mock_gpio_isr_add_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, dcc_init());
    TEST_ASSERT_NULL(s_queue);
    TEST_ASSERT_NULL(s_ack_queue);
    TEST_ASSERT_NULL(s_start_queue);
    TEST_ASSERT_FALSE(s_initialized);
    mock_gpio_isr_add_err = 0;
    mock_gpio_isr_install_err = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, dcc_init());
    TEST_ASSERT_NULL(s_queue);
    mock_gpio_isr_install_err = 0;
    TEST_ASSERT_EQUAL(ESP_OK, dcc_init());
    TEST_ASSERT_EQUAL(ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM, mock_gpio_isr_flags);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, dcc_init());
}

static void test_init_start_queue_failure(void)
{
    mock_queue_create_fail_after = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, dcc_init());
    TEST_ASSERT_NULL(s_queue);
    TEST_ASSERT_NULL(s_ack_queue);
    TEST_ASSERT_NULL(s_start_queue);
}

static void test_control_reenable_invalidates_pending_write(void)
{
    const uint8_t write[] = {0x03,0xEC,0x00,0x2A,0xC5};
    feed_packet(write, sizeof(write), 12);
    dcc_set_control_enabled(false);
    dcc_set_control_enabled(true); /* No intervening received packet. */
    feed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(0, g_cv_write_calls);
    feed_packet(write, sizeof(write), 12);
    TEST_ASSERT_EQUAL_INT(1, g_cv_write_calls);
}

static void test_consist_address_alias_keeps_baseline_functions(void)
{
    dcc_set_address(2, false);
    dcc_set_consist(2, false);
    const uint8_t fn[] = {0x02,0x9F,0x9D};
    feed_packet(fn, sizeof(fn), 12);
    TEST_ASSERT_EQUAL_INT(5, g_fn_calls);
    dcc_set_address(2, true);
    const uint8_t long_speed[] = {0xC0,0x02,0x7F,0xBD};
    feed_packet(long_speed, sizeof(long_speed), 12);
    TEST_ASSERT_EQUAL_INT(0, g_speed_calls);
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
    RUN_TEST(test_dcc_service_ack_queued);
    RUN_TEST(test_dcc_service_ack_queue_full);
    RUN_TEST(test_dcc_ack_task_pulse);
    RUN_TEST(test_dcc_service_write_direct);
    RUN_TEST(test_dcc_service_verify_match);
    RUN_TEST(test_dcc_service_verify_no_match);
    RUN_TEST(test_dcc_service_bit_write);
    RUN_TEST(test_dcc_service_mode_entered_by_reset);
    RUN_TEST(test_dcc_short_addr_124_not_service);
    RUN_TEST(test_dcc_idle_packet);
    RUN_TEST(test_dcc_broadcast_reset);
    RUN_TEST(test_dcc_broadcast_reset_len3_only);
    RUN_TEST(test_dcc_broadcast_unknown_instruction);
    RUN_TEST(test_dcc_broadcast_estop);
    RUN_TEST(test_dcc_read_cv_null_callback);
    RUN_TEST(test_dcc_service_short_packet);
    RUN_TEST(test_dcc_service_bit_manip_clear);
    RUN_TEST(test_dcc_ops_bit_manip_clear);
    RUN_TEST(test_dcc_addressed_packet_too_short);
    RUN_TEST(test_dcc_consist_reverse_128_step);
    RUN_TEST(test_dcc_feed_half_period_kind_change);
    RUN_TEST(test_dcc_isr_paths);
    RUN_TEST(test_dcc_task_processes_queue);
    RUN_TEST(test_dcc_task_isr_install_failure);
    RUN_TEST(test_dcc_init_failures);
    RUN_TEST(test_dcc_init_ack_queue_fail);
    RUN_TEST(test_dcc_init_ack_task_fail);
    RUN_TEST(test_dcc_register_callbacks);
    RUN_TEST(test_fuzz_random_bits);
    RUN_TEST(test_address_partitions_and_signal_clock);
    RUN_TEST(test_stop_emergency_vectors_all_modes);
    RUN_TEST(test_function_group2_f5_f8);
    RUN_TEST(test_ops_confirmation_invalidation);
    RUN_TEST(test_service_entry_deadline_and_other_address_exit);
    RUN_TEST(test_service_failure_and_verify_confirmation);
    RUN_TEST(test_unsupported_xpom_and_service_lengths);
    RUN_TEST(test_consist_speed_functions_and_cv_routing);
    RUN_TEST(test_direction_inversion_and_control_gate);
    RUN_TEST(test_hard_reset_is_separate_operation);
    RUN_TEST(test_stretched_zero_pipeline);
    RUN_TEST(test_signal_static_uses_all_edges_and_boot_deadline);
    RUN_TEST(test_init_handler_failure_and_existing_service_rejected);
    RUN_TEST(test_init_start_queue_failure);
    RUN_TEST(test_control_reenable_invalidates_pending_write);
    RUN_TEST(test_consist_address_alias_keeps_baseline_functions);
    return UNITY_END();
}
