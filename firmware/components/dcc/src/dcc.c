#include "dcc.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pinmap.h"

static const char *TAG = "dcc";

/* Half-period classification (NMRA S-9.1): "1" ~58 us, "0" ~100+ us.
 * Single 82 us threshold (as NmraDcc MAX_ONEBITHALF) is tolerant to the
 * asymmetric half-periods of the rail-side FET; 35 us is the glitch floor,
 * NMRA receiver zero halves include stretched zero up to 10000 us. */
#define DCC_MIN_HALF_PERIOD_US      35
#define DCC_ONE_HALF_MAX_US         82
#define DCC_MAX_ZERO_HALF_PERIOD_US 10000
#define DCC_MAX_EDGE_GAP_US         DCC_MAX_ZERO_HALF_PERIOD_US
#define DCC_SERVICE_TIMEOUT_US      20000

#define DCC_PACKET_MAX 8
#define DCC_QUEUE_LEN  256

/* Service-mode ACK pulse (NMRA S-9.2.3): ~6 ms. Emitted by a dedicated task so
 * the DCC parser never blocks on it (a blocking delay could overflow the
 * half-period queue during a burst of service-mode packets). */
#define DCC_ACK_PULSE_MS   6
#define DCC_ACK_QUEUE_LEN  8

typedef struct {
    uint16_t dt_us;
} dcc_half_t;

typedef enum {
    DCC_HALF_NONE = 0,
    DCC_HALF_ONE,
    DCC_HALF_ZERO,
} dcc_half_kind_t;

typedef enum {
    DCC_STATE_WAIT_PREAMBLE = 0,
    DCC_STATE_READ_BYTE,
    DCC_STATE_EXPECT_DELIM,
} dcc_state_t;

static DRAM_ATTR QueueHandle_t s_queue;
/* ACK requests (one token each) drained by the dcc_ack task. The queue stays
 * allocated for the process lifetime; s_ack_worker records whether the worker
 * task is actually draining it. */
static QueueHandle_t s_ack_queue;
static bool s_ack_worker;
static DRAM_ATTR volatile int64_t s_last_edge_us;
static DRAM_ATTR int64_t s_last_input_edge_us;
static volatile int64_t s_last_packet_us;
static int64_t s_last_signal_packet_us;
/* Half-period samples dropped by the ISR because the parser task did not keep
 * up. Any drop invalidates the half-period pairing, so the task re-acquires
 * framing on the next packet preamble instead of decoding shifted garbage. */
static DRAM_ATTR volatile uint32_t s_isr_overruns;
static DRAM_ATTR portMUX_TYPE s_edge_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_init_us;
static QueueHandle_t s_start_queue;
static bool s_initialized;

static dcc_state_t s_state;
static uint8_t s_preamble_ones;
static uint8_t s_last_preamble_count;
static uint8_t s_cur_byte;
static uint8_t s_cur_byte_bits;
static uint8_t s_packet[DCC_PACKET_MAX];
static uint8_t s_packet_len;
static dcc_half_kind_t s_pending_half;

static uint16_t s_decoder_addr;
static bool s_decoder_long_addr;
static bool s_speed_mode_14;
static uint8_t s_consist_addr;
static bool s_consist_reverse;
static bool s_direction_reverse;
static uint8_t s_cv21, s_cv22;
static bool s_control_enabled;
static uint32_t s_control_generation, s_seen_control_generation;
static bool s_function_forward = true;
/* True after a service-mode reset (long preamble) until valid main-track
 * traffic is seen. Service instructions are only decoded while set, so short
 * addresses 112..126 are not mistaken for service writes. */
static bool s_service_mode;
static bool s_service_candidate;
static int64_t s_service_deadline_us;
typedef struct {
    uint8_t bytes[DCC_PACKET_MAX];
    uint8_t len;
} dcc_confirmation_t;
static dcc_confirmation_t s_service_pending, s_ops_pending;
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;
/* Protects the 64-bit microsecond timestamps, which are not atomic on the
 * 32-bit core: a plain cross-core read can tear. */
static portMUX_TYPE s_ts_mux = portMUX_INITIALIZER_UNLOCKED;

static void mark_packet(void)
{
    portENTER_CRITICAL(&s_ts_mux);
    s_last_packet_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_ts_mux);
}

static dcc_speed_cb_t s_speed_cb;
static dcc_function_cb_t s_function_cb;
static dcc_cv_write_cb_t s_cv_write_cb;
static dcc_cv_read_cb_t s_cv_read_cb;
static dcc_reset_cb_t s_reset_cb;
static dcc_reset_cb_t s_emergency_stop_cb;
static dcc_reset_cb_t s_hard_reset_cb;

static void dcc_ack_task(void *arg);

static dcc_half_kind_t classify(uint16_t dt_us)
{
    if (dt_us >= DCC_MIN_HALF_PERIOD_US && dt_us < DCC_ONE_HALF_MAX_US) {
        return DCC_HALF_ONE;
    }
    if (dt_us >= DCC_ONE_HALF_MAX_US && dt_us <= DCC_MAX_ZERO_HALF_PERIOD_US) {
        return DCC_HALF_ZERO;
    }
    return DCC_HALF_NONE;
}

static void reset_parser(void)
{
    s_state = DCC_STATE_WAIT_PREAMBLE;
    s_preamble_ones = 0;
    s_cur_byte = 0;
    s_cur_byte_bits = 0;
    s_packet_len = 0;
    s_pending_half = DCC_HALF_NONE;
}

static uint16_t service_cv_address(uint8_t instr, uint8_t cv_low)
{
    return (uint16_t)((((uint16_t)(instr & 0x03U)) << 8U) | cv_low) + 1U;
}

static bool read_cv(uint16_t cv, uint8_t *out_value)
{
    if (s_cv_read_cb == NULL || out_value == NULL) {
        return false;
    }
    return s_cv_read_cb(cv, out_value);
}

static bool confirmed(dcc_confirmation_t *pending, const uint8_t *packet, uint8_t len)
{
    if (pending->len == len && memcmp(pending->bytes, packet, len) == 0) {
        pending->len = 0;
        return true;
    }
    memcpy(pending->bytes, packet, len);
    pending->len = len;
    return false;
}

static void emit_function(uint8_t fn, bool state, bool via_consist, bool forward,
                          uint8_t cv21, uint8_t cv22)
{
    bool allowed = !via_consist;
    if (fn == 0U) {
        allowed |= (cv22 & (forward ? 0x01U : 0x02U)) != 0U;
    } else if (fn <= 8U) {
        allowed |= (cv21 & (1U << (fn - 1U))) != 0U;
    } else if (fn <= 12U) {
        allowed |= (cv22 & (1U << (fn - 7U))) != 0U;
    } else {
        allowed = true; /* CV21/22 govern Groups One and Two only. */
    }
    if (allowed && s_function_cb != NULL) {
        s_function_cb(fn, state);
    }
}

static esp_err_t write_cv(uint16_t cv, uint8_t value, bool service)
{
    return s_cv_write_cb != NULL ? s_cv_write_cb(cv, value, service) : ESP_ERR_INVALID_STATE;
}

static void dispatch(const uint8_t *packet, uint8_t len)
{
    if (len < 3U || len > DCC_PACKET_MAX) {
        return;
    }

    uint8_t checksum = 0;
    for (uint8_t i = 0; i < (uint8_t)(len - 1); ++i) {
        checksum ^= packet[i];
    }
    if (checksum != packet[len - 1]) {
        return;
    }

    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_ts_mux);
    s_last_signal_packet_us = now;
    portEXIT_CRITICAL(&s_ts_mux);

    portENTER_CRITICAL(&s_cfg_mux);
    bool enabled = s_control_enabled;
    uint32_t control_generation = s_control_generation;
    bool mode14 = s_speed_mode_14;
    bool reverse = s_direction_reverse;
    bool consist_reverse = s_consist_reverse;
    uint8_t consist = s_consist_addr, cv21 = s_cv21, cv22 = s_cv22;
    uint16_t decoder_addr = s_decoder_addr;
    bool decoder_long = s_decoder_long_addr;
    portEXIT_CRITICAL(&s_cfg_mux);
    if (control_generation != s_seen_control_generation) {
        s_seen_control_generation = control_generation;
        s_service_mode = s_service_candidate = false;
        s_service_pending.len = s_ops_pending.len = 0;
    }
    if (!enabled) {
        s_service_mode = s_service_candidate = false;
        s_service_pending.len = s_ops_pending.len = 0;
        return;
    }
    if (now >= s_service_deadline_us) {
        s_service_mode = s_service_candidate = false;
        s_service_pending.len = 0;
    }

    /* A reset arms entry; only the immediately following service instruction
     * enters service mode. Every valid non-service packet exits, at ANY address. */
    bool is_reset = (len == 3U) && (packet[0] == 0x00U) && (packet[1] == 0x00U);
    bool service_reset = is_reset && (s_last_preamble_count >= 20U);
    if (service_reset) {
        s_service_candidate = true;
        s_service_deadline_us = now + DCC_SERVICE_TIMEOUT_US;
        s_service_pending.len = 0;
    }

    bool service_mode = (s_service_mode || s_service_candidate) &&
                        s_last_preamble_count >= 20U && len == 4U &&
                        ((packet[0] & 0xFCU) == 0x74U ||
                         (packet[0] & 0xFCU) == 0x78U ||
                         (packet[0] & 0xFCU) == 0x7CU);

    uint16_t addr = 0;
    bool long_addr = false;
    uint8_t idx = 1;
    if (!service_mode && packet[0] >= 0xC0U && packet[0] <= 0xE7U) {
        long_addr = true;
        addr = (uint16_t)((packet[0] & 0x3FU) << 8) | packet[1];
        idx = 2;
    } else {
        addr = packet[0];
    }

    /* Service mode (NMRA S-9.2.3, Direct Mode): the packet is exactly
     * [instr(0b0111CCAA), CvLow, data, checksum] - there is no address byte. */
    if (service_mode) {
        s_service_mode = true;
        s_service_candidate = false;
        s_service_deadline_us = now + DCC_SERVICE_TIMEOUT_US;
        mark_packet();
        s_ops_pending.len = 0;
        if (!confirmed(&s_service_pending, packet, len)) {
            return;
        }
        uint8_t instr = packet[0];
        uint8_t cv_low = packet[1];
        uint8_t data = packet[2];

        if ((instr & 0xFCU) == 0x7CU) { /* Write Byte */
            uint16_t cv = service_cv_address(instr, cv_low);
            if (write_cv(cv, data, true) == ESP_OK) {
                (void)dcc_service_ack();
            }
        } else if ((instr & 0xFCU) == 0x74U) { /* Verify Byte: ACK on match */
            uint16_t cv = service_cv_address(instr, cv_low);
            uint8_t current = 0;
            if (read_cv(cv, &current) && current == data) {
                (void)dcc_service_ack();
            }
        } else if ((instr & 0xFCU) == 0x78U && (data & 0xE0U) == 0xE0U) {
            uint16_t cv = service_cv_address(instr, cv_low);
            uint8_t bit = data & 0x07U;
            uint8_t value = (uint8_t)((data >> 3) & 0x01U);
            bool write = (data & 0x10U) != 0U;
            uint8_t current = 0;
            if (read_cv(cv, &current)) {
                if (write) {
                    uint8_t mask = (uint8_t)(1U << bit);
                    uint8_t new_val = (value != 0U) ? (uint8_t)(current | mask)
                                                    : (uint8_t)(current & (uint8_t)~mask);
                    if (write_cv(cv, new_val, true) == ESP_OK) {
                        (void)dcc_service_ack();
                    }
                } else {
                    bool bit_set = (current & (uint8_t)(1U << bit)) != 0U;
                    if (bit_set == (value != 0U)) {
                        (void)dcc_service_ack();
                    }
                }
            }
        }
        return;
    }

    if (!service_reset) {
        s_service_mode = s_service_candidate = false;
        s_service_pending.len = 0;
    }
    /* Locomotive partitions only: accessory 80..BF and reserved E8..FF
     * never alias a long locomotive address. Their signal clock still counts. */
    if (packet[0] >= 0x80U && (packet[0] < 0xC0U || packet[0] > 0xE7U)) {
        return;
    }

    bool is_broadcast = (addr == 0x0000U);
    bool via_consist = !long_addr && consist != 0U && addr == consist;
    bool own_address = addr == decoder_addr && long_addr == decoder_long;
    if (!is_broadcast && !via_consist && !own_address) {
        return;
    }
    mark_packet();

    if (idx >= (uint8_t)(len - 1)) {
        return;
    }
    uint8_t instr = packet[idx];
    uint8_t payload_len = (uint8_t)(len - idx - 1U);
    bool ops_write = !via_consist && payload_len == 3U &&
                     ((instr & 0xFCU) == 0xECU ||
                      ((instr & 0xFCU) == 0xE8U &&
                       (packet[idx + 2U] & 0xF0U) == 0xF0U));
    bool ops_confirmed = false;
    if (ops_write) {
        ops_confirmed = confirmed(&s_ops_pending, packet, len);
    } else {
        s_ops_pending.len = 0;
    }

    if (payload_len == 1U && (instr == 0x00U || instr == 0x01U)) {
        if (s_emergency_stop_cb != NULL) {
            s_emergency_stop_cb();
        }
        if (instr == 0x01U && s_hard_reset_cb != NULL) {
            s_hard_reset_cb();
            dcc_reload_config();
        }
        s_function_forward = true;
        if (s_reset_cb != NULL) {
            s_reset_cb();
        }
        return;
    }

    bool speed_allowed = is_broadcast || consist == 0U || via_consist;
    bool function_via_consist = via_consist && !own_address;

    /* 128-step speed (0x3F + extended byte). */
    if (instr == 0x3FU && payload_len == 2U && speed_allowed) {
        uint8_t ext = packet[idx + 1];
        bool forward = (ext & 0x80U) != 0;
        forward ^= reverse ^ (via_consist && consist_reverse);
        uint8_t raw = ext & 0x7FU;
        if (raw == 1U) {
            if (s_emergency_stop_cb != NULL) s_emergency_stop_cb();
            return;
        }
        if (is_broadcast && raw != 0U) return;
        s_function_forward = forward;
        uint8_t speed = (raw >= 2U) ? (uint8_t)(raw - 1U) : 0U;
        if (s_speed_cb != NULL) {
            s_speed_cb(speed, forward);
        }
        return;
    }

    /* 14/28-step speed (01xxxxxx). */
    if ((instr & 0xC0U) == 0x40U && payload_len == 1U && speed_allowed) {
        bool forward = (instr & 0x20U) != 0;
        forward ^= reverse ^ (via_consist && consist_reverse);
        uint8_t nibble = instr & 0x0FU;
        if (nibble == 1U) {
            if (s_emergency_stop_cb != NULL) s_emergency_stop_cb();
            return;
        }
        if (is_broadcast && nibble != 0U) return;
        s_function_forward = forward;
        uint8_t speed = 0;
        if (mode14) {
            uint8_t step14 = instr & 0x0FU;
            if (step14 >= 2U) {
                speed = (uint8_t)(((uint16_t)(step14 - 1U) * 126U) / 14U);
            }
            emit_function(0, (instr & 0x10U) != 0, function_via_consist, forward, cv21, cv22);
        } else {
            uint8_t code = (uint8_t)(((instr & 0x0FU) << 1) | ((instr & 0x10U) >> 4));
            if (nibble >= 2U) {
                speed = (uint8_t)(1U + ((uint32_t)(code - 4U) * 125U) / 27U);
            }
        }
        if (s_speed_cb != NULL) {
            s_speed_cb(speed, forward);
        }
        return;
    }

    /* Function group 1: F0-F4. */
    if ((instr & 0xE0U) == 0x80U && payload_len == 1U) {
        /* F0's directional consist mask uses the last accepted direction. */
        bool forward = s_function_forward;
        /* In 14-step mode F0/FL is carried by the speed packet; NMRA S-9.2.1
         * declares the group-one bit 4 meaningless, so it must not clear the
         * light set by the speed packet. */
        if (!mode14) {
            emit_function(0, (instr & 0x10U) != 0, function_via_consist, forward, cv21, cv22);
        }
        for (uint8_t fn = 1; fn <= 4U; fn++) {
            emit_function(fn, (instr & (1U << (fn - 1U))) != 0,
                          function_via_consist, forward, cv21, cv22);
        }
        return;
    }

    /* Function group 2: F5-F8 / F9-F12. */
    if ((instr & 0xE0U) == 0xA0U && payload_len == 1U) {
        uint8_t base = ((instr & 0x10U) != 0) ? 5U : 9U;
        for (uint8_t i = 0; i < 4U; i++) {
            emit_function(base + i, (instr & (1U << i)) != 0,
                          function_via_consist, s_function_forward, cv21, cv22);
        }
        return;
    }

    /* Extended functions F13-F20 / F21-F28. */
    if ((instr == 0xDEU || instr == 0xDFU) && payload_len == 2U) {
        uint8_t bits = packet[idx + 1];
        uint8_t base = (instr == 0xDEU) ? 13U : 21U;
        if (s_function_cb != NULL) {
            for (uint8_t i = 0; i < 8U; ++i) {
                s_function_cb(base + i, ((bits >> i) & 0x01U) != 0);
            }
        }
        return;
    }

    /* Ops-mode (main track) CV access, long form: 1110CCVV.
     *   CC=11 Write Byte, CC=10 Bit Manipulation, CC=01 Verify Byte. */
    if ((instr & 0xF0U) == 0xE0U && payload_len == 3U && !via_consist) {
        uint8_t cc = (uint8_t)((instr >> 2) & 0x03U);
        uint16_t cv = service_cv_address(instr, packet[idx + 1]);
        uint8_t data = packet[idx + 2];

        if (cc == 0x03U && ops_confirmed) {
            (void)write_cv(cv, data, false);
        } else if (cc == 0x02U && ops_confirmed) {
            uint8_t bit = data & 0x07U;
            uint8_t value = (uint8_t)((data >> 3) & 0x01U);
            bool write = (data & 0x10U) != 0U;
            uint8_t current = 0;
            if (write && read_cv(cv, &current)) {
                uint8_t mask = (uint8_t)(1U << bit);
                uint8_t new_val = (value != 0U) ? (uint8_t)(current | mask)
                                                : (uint8_t)(current & (uint8_t)~mask);
                (void)write_cv(cv, new_val, false);
            }
            /* Verify Bit in ops mode: no ACK without RailCom. */
        }
        /* CC=01 Verify Byte: no ACK without RailCom. */
        return;
    }

    /* Ops-mode short form (S-9.2.1): CV23 acceleration / CV24 deceleration. */
    if ((instr == 0xF2U || instr == 0xF3U) && payload_len == 2U) {
        uint16_t cv = (instr == 0xF2U) ? 23U : 24U;
        uint8_t value = packet[idx + 1];
        (void)write_cv(cv, value, false);
    }
}

static void consume_bit(uint8_t bit)
{
    if (s_state == DCC_STATE_WAIT_PREAMBLE) {
        if (bit == 1U) {
            if (s_preamble_ones < 32U) {
                s_preamble_ones++;
            }
            return;
        }
        if (s_preamble_ones >= 10U) {
            s_last_preamble_count = s_preamble_ones;
            s_state = DCC_STATE_READ_BYTE;
            s_cur_byte = 0;
            s_cur_byte_bits = 0;
            s_packet_len = 0;
        }
        s_preamble_ones = 0;
        return;
    }

    if (s_state == DCC_STATE_READ_BYTE) {
        /* MSB-first: the command station transmits each byte with the most
         * significant bit first (verified on the ROCO rail traffic in V0:
         * addr 3 arrives bit-reversed as 0xC0 under LSB-first). */
        s_cur_byte |= (uint8_t)((bit & 0x01U) << (7U - s_cur_byte_bits));
        s_cur_byte_bits++;
        if (s_cur_byte_bits == 8U) {
            if (s_packet_len < DCC_PACKET_MAX) {
                s_packet[s_packet_len++] = s_cur_byte;
            } else {
                reset_parser();
                return;
            }
            s_cur_byte = 0;
            s_cur_byte_bits = 0;
            s_state = DCC_STATE_EXPECT_DELIM;
        }
        return;
    }

    if (s_state == DCC_STATE_EXPECT_DELIM) {
        if (bit == 0U) {
            s_state = DCC_STATE_READ_BYTE;
            return;
        }
        dispatch(s_packet, s_packet_len);
        reset_parser();
        s_preamble_ones = 1;
    }
}

static void feed_half_period(uint16_t dt_us)
{
    dcc_half_kind_t kind = classify(dt_us);
    if (kind == DCC_HALF_NONE) {
        s_pending_half = DCC_HALF_NONE;
        reset_parser();
        return;
    }
    if (s_pending_half == DCC_HALF_NONE) {
        s_pending_half = kind;
        return;
    }
    if (s_pending_half != kind) {
        s_pending_half = kind;
        return;
    }
    consume_bit(kind == DCC_HALF_ONE ? 1U : 0U);
    s_pending_half = DCC_HALF_NONE;
}

static void IRAM_ATTR dcc_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_edge_mux);
    s_last_input_edge_us = now;
    int64_t prev = s_last_edge_us;
    if (prev <= 0) {
        s_last_edge_us = now;
        portEXIT_CRITICAL_ISR(&s_edge_mux);
        return;
    }
    int64_t dt = now - prev;
    if (dt > DCC_MAX_EDGE_GAP_US) {
        s_last_edge_us = now;
        s_isr_overruns++;
        portEXIT_CRITICAL_ISR(&s_edge_mux);
        return;
    }
    if (dt < DCC_MIN_HALF_PERIOD_US) {
        /* Glitch shorter than a half-period: ignore the edge and keep the
         * reference time so the next real edge yields a valid half-period. */
        portEXIT_CRITICAL_ISR(&s_edge_mux);
        return;
    }
    s_last_edge_us = now;
    portEXIT_CRITICAL_ISR(&s_edge_mux);

    dcc_half_t hp = { .dt_us = (uint16_t)dt };
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &hp, &woken) != pdTRUE) {
        portENTER_CRITICAL_ISR(&s_edge_mux);
        s_isr_overruns++;
        portEXIT_CRITICAL_ISR(&s_edge_mux);
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_dcc_iter_cap;

static esp_err_t dcc_receiver_start(void)
{
    /* Install the ISR service on CPU1 (this task is pinned there) so the
     * level-3 ~10 kHz DCC ISR does not interrupt the Wi-Fi MAC (which runs on
     * CPU0 and would otherwise drop frames, throttling uploads to ~2 KB/s). */
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM);
    if (err != ESP_OK) {
        /* An existing service may be non-IRAM or on another core. Do not
         * silently reuse it and claim a flash-safe receiver. */
        return err;
    }
    err = gpio_isr_handler_add((gpio_num_t)PIN_DCC_IN, dcc_isr, NULL);
    if (err != ESP_OK) {
        gpio_uninstall_isr_service();
    }
    return err;
}

static void dcc_task(void *arg)
{
    QueueHandle_t startup = (QueueHandle_t)arg;
    esp_err_t err = dcc_receiver_start();
    if (startup != NULL) {
        (void)xQueueSend(startup, &err, portMAX_DELAY);
    }
    if (err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }

    reset_parser();
    uint32_t iters = 0;
    while (s_dcc_iter_cap == 0U || iters < s_dcc_iter_cap) {
        dcc_half_t hp;
        if (xQueueReceive(s_queue, &hp, portMAX_DELAY) == pdTRUE) {
            portENTER_CRITICAL(&s_edge_mux);
            uint32_t overruns = s_isr_overruns;
            s_isr_overruns = 0U;
            portEXIT_CRITICAL(&s_edge_mux);
            if (overruns != 0U) {
                /* Samples were dropped while a callback (function packet /
                 * sound / NVS) was running: the pairing and framing state can
                 * no longer be trusted. Drop it and re-acquire on the next
                 * preamble instead of mis-decoding the packet tail. */
                reset_parser();
            }
            feed_half_period(hp.dt_us);
        }
        iters++;
    }
}

/* Configure the ACK pin as an output. Idempotent; safe to call again if the
 * inline fallback runs before dcc_init() configured it. */
static esp_err_t ack_pin_config(void)
{
    gpio_config_t ack = {
        .pin_bit_mask = 1ULL << PIN_ACK_LOAD,
        .mode = GPIO_MODE_OUTPUT,
    };
    return gpio_config(&ack);
}

esp_err_t dcc_init(void)
{
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_init_us = esp_timer_get_time();
    s_queue = xQueueCreateWithCaps(DCC_QUEUE_LEN, sizeof(dcc_half_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_DCC_IN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) goto fail;

    /* ACK output is configured once here instead of on every pulse. */
    err = ack_pin_config();
    if (err != ESP_OK) goto fail;
    err = gpio_set_level((gpio_num_t)PIN_ACK_LOAD, 0);
    if (err != ESP_OK) goto fail;

    s_ack_queue = xQueueCreate(DCC_ACK_QUEUE_LEN, sizeof(uint8_t));
    if (s_ack_queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_start_queue = xQueueCreate(1, sizeof(esp_err_t));
    if (s_start_queue == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(dcc_task, "dcc", 8192, s_start_queue, 10, NULL, 1);
    if (ok != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    /* The CPU1 task publishes installation AND handler-add status before this
     * function reports success. No timeout/free race with the startup sender. */
    if (xQueueReceive(s_start_queue, &err, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    vQueueDelete(s_start_queue);
    s_start_queue = NULL;
    if (err != ESP_OK) goto fail;

    /* Non-blocking service-mode ACK worker. If it cannot start, fall back to
     * the inline pulse in dcc_service_ack(). The queue is intentionally NOT
     * freed here: dcc_task is already running and may hold a reference to it. */
    if (xTaskCreate(dcc_ack_task, "dcc_ack", 2048, NULL, 5, NULL) == pdPASS) {
        s_ack_worker = true;
    } else {
        ESP_LOGW(TAG, "ACK task create failed: service-mode ACK is inline");
    }

    ESP_LOGI(TAG, "DCC decoder started (GPIO%d, CPU1)", PIN_DCC_IN);
    s_initialized = true;
    return ESP_OK;

fail:
    if (s_start_queue != NULL) vQueueDelete(s_start_queue);
    if (s_ack_queue != NULL) vQueueDelete(s_ack_queue);
    if (s_queue != NULL) vQueueDeleteWithCaps(s_queue);
    s_start_queue = s_ack_queue = s_queue = NULL;
    s_ack_worker = false;
    return err;
}

void dcc_set_address(uint16_t addr, bool long_addr)
{
    portENTER_CRITICAL(&s_cfg_mux);
    s_decoder_addr = addr;
    s_decoder_long_addr = long_addr;
    portEXIT_CRITICAL(&s_cfg_mux);
}

void dcc_set_speed_step_mode(bool mode_14)
{
    portENTER_CRITICAL(&s_cfg_mux);
    s_speed_mode_14 = mode_14;
    portEXIT_CRITICAL(&s_cfg_mux);
}

void dcc_set_consist(uint8_t address, bool reverse_dir)
{
    portENTER_CRITICAL(&s_cfg_mux);
    s_consist_addr = (uint8_t)(address & 0x7FU);
    s_consist_reverse = reverse_dir;
    portEXIT_CRITICAL(&s_cfg_mux);
}

void dcc_reload_config(void)
{
    uint8_t cv1 = 3, cv29 = 0x02, cv17 = 0, cv18 = 0, cv19 = 0, cv21 = 0, cv22 = 0;
    (void)read_cv(1, &cv1);
    (void)read_cv(29, &cv29);
    (void)read_cv(17, &cv17);
    (void)read_cv(18, &cv18);
    (void)read_cv(19, &cv19);
    (void)read_cv(21, &cv21);
    (void)read_cv(22, &cv22);

    bool long_addr = (cv29 & 0x20U) != 0;
    /* CV29 bit 1 (0x02): 0 = 14 speed steps, 1 = 28/128 speed steps (NMRA). */
    bool mode_14 = (cv29 & 0x02U) == 0;
    uint16_t addr = long_addr ? (uint16_t)(((cv17 & 0x3FU) << 8) | cv18) : cv1;

    /* Publish the new configuration atomically: the DCC task reads these
     * fields from the task/ISR side while this runs on the httpd task. */
    portENTER_CRITICAL(&s_cfg_mux);
    s_decoder_addr = addr;
    s_decoder_long_addr = long_addr;
    s_speed_mode_14 = mode_14;
    s_consist_addr = (uint8_t)(cv19 & 0x7FU);
    s_consist_reverse = (cv19 & 0x80U) != 0U;
    s_direction_reverse = (cv29 & 0x01U) != 0U;
    s_cv21 = cv21;
    s_cv22 = cv22;
    portEXIT_CRITICAL(&s_cfg_mux);
    ESP_LOGI(TAG, "Decoder address: %u (%s)", (unsigned)addr,
             long_addr ? "long" : "short");
}

void dcc_register_speed_cb(dcc_speed_cb_t cb) { s_speed_cb = cb; }
void dcc_register_function_cb(dcc_function_cb_t cb) { s_function_cb = cb; }
void dcc_register_cv_write_cb(dcc_cv_write_cb_t cb) { s_cv_write_cb = cb; }
void dcc_register_cv_read_cb(dcc_cv_read_cb_t cb) { s_cv_read_cb = cb; }
void dcc_register_reset_cb(dcc_reset_cb_t cb) { s_reset_cb = cb; }
void dcc_register_emergency_stop_cb(void (*cb)(void)) { s_emergency_stop_cb = cb; }
void dcc_register_hard_reset_cb(void (*cb)(void)) { s_hard_reset_cb = cb; }

void dcc_set_control_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_cfg_mux);
    if (s_control_enabled != enabled) s_control_generation++;
    s_control_enabled = enabled;
    portEXIT_CRITICAL(&s_cfg_mux);
}

int64_t dcc_last_signal_packet_us(void)
{
    portENTER_CRITICAL(&s_ts_mux);
    int64_t value = s_last_signal_packet_us;
    portEXIT_CRITICAL(&s_ts_mux);
    return value;
}

bool dcc_signal_is_static(uint32_t min_us)
{
    portENTER_CRITICAL(&s_edge_mux);
    int64_t last = s_last_input_edge_us;
    portEXIT_CRITICAL(&s_edge_mux);
    if (last == 0) last = s_init_us;
    return esp_timer_get_time() - last > (int64_t)min_us;
}

int64_t dcc_last_packet_us(void)
{
    portENTER_CRITICAL(&s_ts_mux);
    int64_t v = s_last_packet_us;
    portEXIT_CRITICAL(&s_ts_mux);
    return v;
}

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_ack_iter_cap;

static void dcc_ack_task(void *arg)
{
    (void)arg;
    uint32_t iters = 0;
    while (s_ack_iter_cap == 0U || iters < s_ack_iter_cap) {
        uint8_t token;
        /* portMAX_DELAY: only returns once there is a request to serve. */
        if (xQueueReceive(s_ack_queue, &token, portMAX_DELAY) != pdTRUE) continue;
        if (gpio_set_level((gpio_num_t)PIN_ACK_LOAD, 1) != ESP_OK) continue;
        vTaskDelay(pdMS_TO_TICKS(DCC_ACK_PULSE_MS));
        gpio_set_level((gpio_num_t)PIN_ACK_LOAD, 0);
        iters++;
    }
    vTaskDelete(NULL);
}

esp_err_t dcc_service_ack(void)
{
    /* Queue the pulse so the DCC parser keeps draining half-periods. The pin is
     * configured once in dcc_init(). */
    if (s_ack_worker && s_ack_queue != NULL) {
        uint8_t token = 1U;
        if (xQueueSend(s_ack_queue, &token, 0) != pdTRUE) {
            return ESP_ERR_NO_MEM; /* queue full: a redundant ACK is dropped */
        }
        return ESP_OK;
    }

    /* Fallback for host tests / an early call before dcc_init(): configure the
     * pin (idempotent) and emit the pulse inline (the historical blocking
     * behaviour). */
    esp_err_t err = ack_pin_config();
    if (err != ESP_OK) return err;
    err = gpio_set_level((gpio_num_t)PIN_ACK_LOAD, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(DCC_ACK_PULSE_MS));
    return gpio_set_level((gpio_num_t)PIN_ACK_LOAD, 0);
}
