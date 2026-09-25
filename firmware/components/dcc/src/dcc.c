#include "dcc.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "pinmap.h"

static const char *TAG = "dcc";

/* Half-period classification (NMRA S-9.1): "1" ~58 us, "0" ~100+ us.
 * Single 82 us threshold (as NmraDcc MAX_ONEBITHALF) is tolerant to the
 * asymmetric half-periods of the rail-side FET; 35 us is the glitch floor,
 * 292 us the longest valid "0" half-period. */
#define DCC_MIN_HALF_PERIOD_US      35
#define DCC_ONE_HALF_MAX_US         82
#define DCC_MAX_ZERO_HALF_PERIOD_US 292
#define DCC_MAX_EDGE_GAP_US         2000

#define DCC_PACKET_MAX 8
#define DCC_QUEUE_LEN  256

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

static QueueHandle_t s_queue;
static volatile int64_t s_last_edge_us;
static volatile int64_t s_last_packet_us;
/* Half-period samples dropped by the ISR because the parser task did not keep
 * up. Any drop invalidates the half-period pairing, so the task re-acquires
 * framing on the next packet preamble instead of decoding shifted garbage. */
static volatile uint32_t s_isr_overruns;

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
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

static dcc_speed_cb_t s_speed_cb;
static dcc_function_cb_t s_function_cb;
static dcc_cv_write_cb_t s_cv_write_cb;
static dcc_cv_read_cb_t s_cv_read_cb;
static dcc_reset_cb_t s_reset_cb;

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

static bool address_matches(uint16_t addr, bool long_addr, bool *out_consist)
{
    if (out_consist != NULL) {
        *out_consist = false;
    }
    /* Consist (CV19): short address only, 1..127. */
    if (!long_addr && s_consist_addr != 0U && addr == s_consist_addr) {
        if (out_consist != NULL) {
            *out_consist = true;
        }
        return true;
    }
    if (s_decoder_long_addr == long_addr && addr == s_decoder_addr) {
        return true;
    }
    return false;
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

static void dispatch(const uint8_t *packet, uint8_t len)
{
    if (len < 3) {
        return;
    }

    uint8_t checksum = 0;
    for (uint8_t i = 0; i < (uint8_t)(len - 1); ++i) {
        checksum ^= packet[i];
    }
    if (checksum != packet[len - 1]) {
        return;
    }

    bool service_mode = (s_last_preamble_count >= 20U) &&
                        (packet[0] == 0xFFU || (packet[0] >= 112U && packet[0] <= 126U));

    /* Extract address. Service-mode packets use a single address byte
     * (0xFF broadcast or 112..126) and never a long address, even though
     * 0xFF has bit 7 set. */
    uint16_t addr = 0;
    bool long_addr = false;
    uint8_t idx = 1;
    if (!service_mode && (packet[0] & 0x80U) != 0) {
        long_addr = true;
        addr = (uint16_t)((packet[0] & 0x3FU) << 8) | packet[1];
        idx = 2;
    } else {
        addr = packet[0];
    }

    /* Service mode (NMRA S-9.2.3, Direct Mode): 0b0111CCAA. */
    if (service_mode) {
        s_last_packet_us = esp_timer_get_time();
        /* dispatch() guarantees len >= 3 and service packets use idx == 1. */
        uint8_t instr = packet[idx];

        if ((instr & 0xFCU) == 0x7CU) { /* Write Byte */
            if ((idx + 2U) < (uint8_t)(len - 1)) {
                uint16_t cv = service_cv_address(instr, packet[idx + 1]);
                uint8_t value = packet[idx + 2];
                if (s_cv_write_cb != NULL) {
                    s_cv_write_cb(cv, value, true);
                }
                (void)dcc_service_ack();
            }
        } else if ((instr & 0xFCU) == 0x74U) { /* Verify Byte: ACK on match */
            if ((idx + 2U) < (uint8_t)(len - 1)) {
                uint16_t cv = service_cv_address(instr, packet[idx + 1]);
                uint8_t expected = packet[idx + 2];
                uint8_t current = 0;
                if (read_cv(cv, &current) && current == expected) {
                    (void)dcc_service_ack();
                }
            }
        } else if ((instr & 0xFCU) == 0x78U) { /* Bit Manipulation */
            if ((idx + 2U) < (uint8_t)(len - 1)) {
                uint16_t cv = service_cv_address(instr, packet[idx + 1]);
                uint8_t bdata = packet[idx + 2];
                uint8_t bit = bdata & 0x07U;
                uint8_t value = (uint8_t)((bdata >> 3) & 0x01U);
                bool write = (bdata & 0x10U) != 0U;
                uint8_t current = 0;
                if (read_cv(cv, &current)) {
                    if (write) {
                        uint8_t mask = (uint8_t)(1U << bit);
                        uint8_t new_val = (value != 0U) ? (uint8_t)(current | mask)
                                                        : (uint8_t)(current & (uint8_t)~mask);
                        if (s_cv_write_cb != NULL) {
                            s_cv_write_cb(cv, new_val, true);
                        }
                    } else {
                        bool bit_set = (current & (uint8_t)(1U << bit)) != 0U;
                        if (bit_set == (value != 0U)) {
                            (void)dcc_service_ack();
                        }
                    }
                }
            }
        }
        return;
    }

    /* Idle. */
    if (packet[0] == 0xFFU) {
        return;
    }

    bool is_broadcast = (addr == 0x0000U);
    bool via_consist = false;
    if (!is_broadcast && !address_matches(addr, long_addr, &via_consist)) {
        return;
    }
    s_last_packet_us = esp_timer_get_time();

    if (idx >= (uint8_t)(len - 1)) {
        return;
    }
    uint8_t instr = packet[idx];

    if (is_broadcast) {
        if (instr == 0x00U && len == 3U) {
            if (s_reset_cb != NULL) {
                s_reset_cb();
            }
            return;
        }
        if (instr == 0x01U) {
            if (s_speed_cb != NULL) {
                s_speed_cb(0, true);
            }
            return;
        }
    }

    /* 128-step speed (0x3F + extended byte). */
    if (instr == 0x3FU && (idx + 1U) < (uint8_t)(len - 1)) {
        uint8_t ext = packet[idx + 1];
        bool forward = (ext & 0x80U) != 0;
        if (via_consist && s_consist_reverse) {
            forward = !forward;
        }
        uint8_t raw = ext & 0x7FU;
        uint8_t speed = (raw >= 2U) ? (uint8_t)(raw - 1U) : 0U;
        if (s_speed_cb != NULL) {
            s_speed_cb(speed, forward);
        }
        return;
    }

    /* 14/28-step speed (01xxxxxx). */
    if ((instr & 0xC0U) == 0x40U) {
        bool forward = (instr & 0x20U) != 0;
        if (via_consist && s_consist_reverse) {
            forward = !forward;
        }
        uint8_t speed = 0;
        if (s_speed_mode_14) {
            uint8_t step14 = instr & 0x0FU;
            if (step14 >= 2U) {
                speed = (uint8_t)(((uint16_t)(step14 - 1U) * 126U) / 13U);
            }
            if (s_function_cb != NULL) {
                s_function_cb(0, (instr & 0x10U) != 0);
            }
        } else {
            uint8_t code = (uint8_t)(((instr & 0x0FU) << 1) | ((instr & 0x10U) >> 4));
            if (code >= 2U) {
                speed = (uint8_t)(((uint32_t)(code - 2U) * 126U) / 29U) + 1U;
                if (speed > 126U) {
                    speed = 126U;
                }
            }
        }
        if (s_speed_cb != NULL) {
            s_speed_cb(speed, forward);
        }
        return;
    }

    /* Function group 1: F0-F4. */
    if ((instr & 0xE0U) == 0x80U) {
        if (s_function_cb != NULL) {
            s_function_cb(0, (instr & 0x10U) != 0);
            s_function_cb(1, (instr & 0x01U) != 0);
            s_function_cb(2, (instr & 0x02U) != 0);
            s_function_cb(3, (instr & 0x04U) != 0);
            s_function_cb(4, (instr & 0x08U) != 0);
        }
        return;
    }

    /* Function group 2: F5-F8 / F9-F12. */
    if ((instr & 0xF0U) == 0xB0U) {
        uint8_t base = ((instr & 0x10U) != 0) ? 9U : 5U;
        if (s_function_cb != NULL) {
            s_function_cb(base + 0, (instr & 0x01U) != 0);
            s_function_cb(base + 1, (instr & 0x02U) != 0);
            s_function_cb(base + 2, (instr & 0x04U) != 0);
            s_function_cb(base + 3, (instr & 0x08U) != 0);
        }
        return;
    }

    /* Extended functions F13-F20 / F21-F28. */
    if ((instr == 0xDEU || instr == 0xDFU) && (idx + 1U) < (uint8_t)(len - 1)) {
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
    if ((instr & 0xF0U) == 0xE0U && (idx + 2U) < (uint8_t)(len - 1)) {
        uint8_t cc = (uint8_t)((instr >> 2) & 0x03U);
        uint16_t cv = service_cv_address(instr, packet[idx + 1]);
        uint8_t data = packet[idx + 2];

        if (cc == 0x03U) { /* Write Byte */
            if (s_cv_write_cb != NULL) {
                s_cv_write_cb(cv, data, false);
            }
        } else if (cc == 0x02U) { /* Bit Manipulation: 111 C D BBB */
            uint8_t bit = data & 0x07U;
            uint8_t value = (uint8_t)((data >> 3) & 0x01U);
            bool write = (data & 0x10U) != 0U;
            uint8_t current = 0;
            if (write && read_cv(cv, &current)) {
                uint8_t mask = (uint8_t)(1U << bit);
                uint8_t new_val = (value != 0U) ? (uint8_t)(current | mask)
                                                : (uint8_t)(current & (uint8_t)~mask);
                if (s_cv_write_cb != NULL) {
                    s_cv_write_cb(cv, new_val, false);
                }
            }
            /* Verify Bit in ops mode: no ACK without RailCom. */
        }
        /* CC=01 Verify Byte: no ACK without RailCom. */
        return;
    }

    /* Ops-mode short form (S-9.2.1): CV23 acceleration / CV24 deceleration. */
    if ((instr == 0xF2U || instr == 0xF3U) && (idx + 1U) < (uint8_t)(len - 1)) {
        uint16_t cv = (instr == 0xF2U) ? 23U : 24U;
        uint8_t value = packet[idx + 1];
        if (s_cv_write_cb != NULL) {
            s_cv_write_cb(cv, value, false);
        }
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
    int64_t prev = s_last_edge_us;
    if (prev <= 0) {
        s_last_edge_us = now;
        return;
    }
    int64_t dt = now - prev;
    if (dt > DCC_MAX_EDGE_GAP_US) {
        s_last_edge_us = now;
        return;
    }
    if (dt < DCC_MIN_HALF_PERIOD_US) {
        /* Glitch shorter than a half-period: ignore the edge and keep the
         * reference time so the next real edge yields a valid half-period. */
        return;
    }
    s_last_edge_us = now;

    dcc_half_t hp = { .dt_us = (uint16_t)dt };
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &hp, &woken) != pdTRUE) {
        s_isr_overruns++;
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_dcc_iter_cap;

static void dcc_task(void *arg)
{
    (void)arg;

    /* Install the ISR service on CPU1 (this task is pinned there) so the
     * level-3 ~10 kHz DCC ISR does not interrupt the Wi-Fi MAC (which runs on
     * CPU0 and would otherwise drop frames, throttling uploads to ~2 KB/s). */
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        gpio_isr_handler_add((gpio_num_t)PIN_DCC_IN, dcc_isr, NULL);
    } else {
        ESP_LOGE(TAG, "ISR service install failed: %s", esp_err_to_name(err));
    }

    reset_parser();
    uint32_t iters = 0;
    while (s_dcc_iter_cap == 0U || iters < s_dcc_iter_cap) {
        dcc_half_t hp;
        if (xQueueReceive(s_queue, &hp, portMAX_DELAY) == pdTRUE) {
            if (s_isr_overruns != 0U) {
                s_isr_overruns = 0U;
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

esp_err_t dcc_init(void)
{
    s_queue = xQueueCreate(DCC_QUEUE_LEN, sizeof(dcc_half_t));
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
    ESP_ERROR_CHECK(gpio_config(&cfg));

    BaseType_t ok = xTaskCreatePinnedToCore(dcc_task, "dcc", 8192, NULL, 10, NULL, 1);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DCC decoder started (GPIO%d, CPU1)", PIN_DCC_IN);
    return ESP_OK;
}

void dcc_set_address(uint16_t addr, bool long_addr)
{
    s_decoder_addr = addr;
    s_decoder_long_addr = long_addr;
}

void dcc_set_speed_step_mode(bool mode_14)
{
    s_speed_mode_14 = mode_14;
}

void dcc_set_consist(uint8_t address, bool reverse_dir)
{
    s_consist_addr = (uint8_t)(address & 0x7FU);
    s_consist_reverse = reverse_dir;
}

void dcc_reload_config(void)
{
    uint8_t cv1 = 3, cv29 = 0x02, cv17 = 0, cv18 = 0, cv19 = 0;
    (void)read_cv(1, &cv1);
    (void)read_cv(29, &cv29);
    (void)read_cv(17, &cv17);
    (void)read_cv(18, &cv18);
    (void)read_cv(19, &cv19);

    bool long_addr = (cv29 & 0x20U) != 0;
    /* CV29 bit 1 (0x02): 0 = 14 speed steps, 1 = 28/128 speed steps (NMRA). */
    bool mode_14 = (cv29 & 0x02U) == 0;
    uint16_t addr = long_addr ? (uint16_t)(((cv17 & 0x3FU) << 8) | cv18) : cv1;

    /* Publish the new configuration atomically: the DCC task reads these
     * fields from the task/ISR side while this runs on the httpd task. */
    portENTER_CRITICAL(&s_cfg_mux);
    dcc_set_address(addr, long_addr);
    dcc_set_speed_step_mode(mode_14);
    dcc_set_consist((uint8_t)(cv19 & 0x7FU), (cv19 & 0x80U) != 0U);
    portEXIT_CRITICAL(&s_cfg_mux);
    ESP_LOGI(TAG, "Decoder address: %u (%s)", (unsigned)addr,
             long_addr ? "long" : "short");
}

void dcc_register_speed_cb(dcc_speed_cb_t cb) { s_speed_cb = cb; }
void dcc_register_function_cb(dcc_function_cb_t cb) { s_function_cb = cb; }
void dcc_register_cv_write_cb(dcc_cv_write_cb_t cb) { s_cv_write_cb = cb; }
void dcc_register_cv_read_cb(dcc_cv_read_cb_t cb) { s_cv_read_cb = cb; }
void dcc_register_reset_cb(dcc_reset_cb_t cb) { s_reset_cb = cb; }

int64_t dcc_last_packet_us(void)
{
    return s_last_packet_us;
}

esp_err_t dcc_service_ack(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_ACK_LOAD,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level((gpio_num_t)PIN_ACK_LOAD, 1);
    vTaskDelay(pdMS_TO_TICKS(6));
    gpio_set_level((gpio_num_t)PIN_ACK_LOAD, 0);
    return ESP_OK;
}
