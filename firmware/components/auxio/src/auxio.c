#include "auxio.h"

#include <string.h>

#include "driver/ledc.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pinmap.h"

static const char *TAG = "auxio";

#define AUXIO_PWM_FREQ_HZ 20000

typedef struct {
    bool enabled;
    uint8_t pwm_on;
    uint8_t pwm_off;
    auxio_effect_t mode;
    uint16_t period_ms;
    uint8_t cur;          /* current level for stateful effects */
    uint32_t fx_next_ms;  /* next change time for firebox flicker */
    bool fx_scheduled;
    bool ledc;
    ledc_channel_t ledc_ch;
    mcpwm_cmpr_handle_t mcpwm_cmpr;
} auxio_ch_t;

static auxio_ch_t s_ch[AUXIO_CH_COUNT];
static SemaphoreHandle_t s_lock;

/* 8-bit gamma table (~2.2): linear brightness steps -> PWM, so fades and
 * pulses look perceptually even instead of "digital". Precomputed to avoid
 * pulling in the FPU/libm powf() (and its ~1.5 KB of code) at boot. */
static const uint8_t s_gamma[256] = {
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   1,
      1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,   2,   2,   2,   2,   2,
      3,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,   5,   6,   6,   6,
      6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,  11,  11,  12,
     12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,  19,  19,
     20,  20,  21,  22,  22,  23,  23,  24,  25,  25,  26,  26,  27,  28,  28,  29,
     30,  30,  31,  32,  33,  33,  34,  35,  35,  36,  37,  38,  39,  39,  40,  41,
     42,  43,  43,  44,  45,  46,  47,  48,  49,  49,  50,  51,  52,  53,  54,  55,
     56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,
     73,  74,  75,  76,  77,  78,  79,  81,  82,  83,  84,  85,  87,  88,  89,  90,
     91,  93,  94,  95,  97,  98,  99, 100, 102, 103, 105, 106, 107, 109, 110, 111,
    113, 114, 116, 117, 119, 120, 121, 123, 124, 126, 127, 129, 130, 132, 133, 135,
    137, 138, 140, 141, 143, 145, 146, 148, 149, 151, 153, 154, 156, 158, 159, 161,
    163, 165, 166, 168, 170, 172, 173, 175, 177, 179, 181, 182, 184, 186, 188, 190,
    192, 194, 196, 197, 199, 201, 203, 205, 207, 209, 211, 213, 215, 217, 219, 221,
    223, 225, 227, 229, 231, 234, 236, 238, 240, 242, 244, 246, 248, 251, 253, 255,
};

static const int s_gpios[AUXIO_CH_COUNT] = {
    PIN_AUX_F0F, PIN_AUX_F0R, PIN_AUX1, PIN_AUX2, PIN_AUX3,
    PIN_AUX4, PIN_AUX5, PIN_AUX6, PIN_AUX7,
};

/* AUX5..AUX7 are MCPWM-backed (LEDC has only 8 channels and the motor owns
 * LEDC_CHANNEL_0/1). One 20 kHz timer feeds two operators: operator 0 drives
 * AUX5/AUX6, operator 1 drives AUX7. */
#define AUXIO_MCPWM_RES_HZ 10000000U
#define AUXIO_MCPWM_PERIOD_TICKS (AUXIO_MCPWM_RES_HZ / AUXIO_PWM_FREQ_HZ)

static mcpwm_timer_handle_t s_mcpwm_timer;
static mcpwm_oper_handle_t s_mcpwm_oper[2];

static void apply_duty(auxio_ch_t *ch, uint8_t duty)
{
    if (ch->ledc) {
        (void)ledc_set_duty(LEDC_LOW_SPEED_MODE, ch->ledc_ch, duty);
        (void)ledc_update_duty(LEDC_LOW_SPEED_MODE, ch->ledc_ch);
    } else {
        uint32_t ticks = ((uint32_t)duty * AUXIO_MCPWM_PERIOD_TICKS) / 255U;
        (void)mcpwm_comparator_set_compare_value(ch->mcpwm_cmpr, ticks);
    }
}

/* Duty for a channel at time now_ms. Pure: does not change persisted state
 * (the firebox/incandescent state lives in ch->cur and is advanced by ch_step). */
static uint8_t ch_duty(const auxio_ch_t *ch, uint8_t idx, uint32_t now_ms)
{
    uint8_t hi = ch->pwm_on;
    uint8_t lo = ch->pwm_off;
    uint8_t maxv = (hi > lo) ? hi : lo;
    uint8_t minv = (hi > lo) ? lo : hi;

    /* Incandescent keeps glowing while it cools down. */
    if (!ch->enabled) {
        if (ch->mode != AUXIO_EFFECT_INCANDESCENT || ch->cur == 0U) {
            return 0;
        }
    }

    /* Effects that do not depend on the period. */
    if (ch->mode == AUXIO_EFFECT_INCANDESCENT) {
        return (uint8_t)(((uint32_t)maxv * s_gamma[ch->cur]) / 255U);
    }
    if (ch->mode == AUXIO_EFFECT_FIREBOX) {
        return ch->cur;
    }
    if (ch->mode == AUXIO_EFFECT_STEADY || ch->period_ms < 20U) {
        return maxv;
    }

    uint32_t period = ch->period_ms;
    uint32_t phase = now_ms % period;

    switch (ch->mode) {
        case AUXIO_EFFECT_MARS: {
            /* triangle 0..255..0, mapped through gamma => sinusoidal-ish sweep */
            uint32_t half = period / 2U;
            uint32_t pos = (phase < half) ? phase : (period - phase);
            uint32_t t_raw = (pos * 255U) / (half ? half : 1U);
            uint8_t t = (uint8_t)(t_raw > 255U ? 255U : t_raw);
            uint8_t mid = (uint8_t)(maxv / 2U);
            return (uint8_t)(mid + ((uint32_t)(maxv - mid) * s_gamma[t]) / 255U);
        }
        case AUXIO_EFFECT_DITCH: {
            /* Two adjacent outputs alternate (phase offset by channel parity). */
            uint32_t offs = ((uint32_t)(idx & 1U) * period) / 2U;
            uint32_t p = (now_ms + offs) % period;
            return (p < period / 2U) ? maxv : minv;
        }
        case AUXIO_EFFECT_BEACON: {
            /* sharp flash at phase 0, then quadratic decay to a non-zero floor */
            uint8_t floor = (uint8_t)(maxv / 5U);
            uint32_t remain = period - phase;
            /* remain <= period, so t <= 255 by construction. */
            uint32_t t = (uint32_t)((255ULL * remain * remain) /
                                    ((uint64_t)period * period));
            return (uint8_t)(floor + (((uint32_t)(maxv - floor) * t) / 255U));
        }
        case AUXIO_EFFECT_STROBE:
            /* double strobe: two short flashes, then a pause */
            return ((phase < 30U) || (phase >= 100U && phase < 130U)) ? maxv : minv;
        default:
            return maxv;
    }
}

/* Advance stateful effects and return the duty to apply. Called only from the
 * effect task (one step per tick). */
static uint8_t ch_step(auxio_ch_t *ch, uint8_t idx, uint32_t now_ms)
{
    uint8_t hi = ch->pwm_on;
    uint8_t lo = ch->pwm_off;
    uint8_t maxv = (hi > lo) ? hi : lo;

    if (ch->mode == AUXIO_EFFECT_INCANDESCENT) {
        uint8_t target = ch->enabled ? 255U : 0U;
        uint8_t step_up = 24U;   /* ~200 ms warm-up */
        uint8_t step_dn = 12U;   /* ~400 ms cool-down */
        if (ch->cur < target) {
            ch->cur = (uint8_t)((target - ch->cur <= step_up) ? target
                                                              : ch->cur + step_up);
        } else if (ch->cur > target) {
            ch->cur = (uint8_t)((ch->cur - target <= step_dn) ? target
                                                              : ch->cur - step_dn);
        }
    } else if (ch->mode == AUXIO_EFFECT_FIREBOX) {
        if (!ch->enabled) {
            ch->cur = 0U;
            ch->fx_scheduled = false;
        } else if (!ch->fx_scheduled || (int32_t)(now_ms - ch->fx_next_ms) >= 0) {
            uint32_t floor40 = ((uint32_t)maxv * 40U) / 100U;
            uint32_t range = (uint32_t)maxv - floor40;
            uint32_t r = (now_ms * 1103515245U) + 12345U + (uint32_t)idx * 2654435761U;
            r ^= r >> 15;
            ch->cur = (uint8_t)(floor40 + (range ? (r % range) : 0U));
            ch->fx_next_ms = now_ms + 30U + (r % 71U);
            ch->fx_scheduled = true;
        }
    } else {
        ch->cur = 0U;
    }
    return ch_duty(ch, idx, now_ms);
}

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_fx_iter_cap;

static void effect_task(void *arg)
{
    (void)arg;
    uint32_t iters = 0;
    while (s_fx_iter_cap == 0U || iters < s_fx_iter_cap) {
        if (s_lock != NULL) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
        }
        uint32_t now = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
        for (int i = 0; i < AUXIO_CH_COUNT; ++i) {
            apply_duty(&s_ch[i], ch_step(&s_ch[i], (uint8_t)i, now));
        }
        if (s_lock != NULL) {
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        iters++;
    }
}

esp_err_t auxio_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* LEDC timer 1 (8-bit) shared by the first six outputs; the motor keeps
     * timer 0 / channels 0-1 at 10-bit. */
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = AUXIO_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    /* MCPWM: one 20 kHz timer, two operators (AUX5/AUX6 on operator 0,
     * AUX7 on operator 1). */
    mcpwm_timer_config_t mcpwm_timer = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = AUXIO_MCPWM_RES_HZ,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = AUXIO_MCPWM_PERIOD_TICKS,
    };
    ESP_ERROR_CHECK(mcpwm_new_timer(&mcpwm_timer, &s_mcpwm_timer));
    mcpwm_operator_config_t mcpwm_oper = { .group_id = 0 };
    ESP_ERROR_CHECK(mcpwm_new_operator(&mcpwm_oper, &s_mcpwm_oper[0]));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_mcpwm_oper[0], s_mcpwm_timer));
    ESP_ERROR_CHECK(mcpwm_new_operator(&mcpwm_oper, &s_mcpwm_oper[1]));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(s_mcpwm_oper[1], s_mcpwm_timer));

    for (int i = 0; i < AUXIO_CH_COUNT; ++i) {
        auxio_ch_t *ch = &s_ch[i];
        memset(ch, 0, sizeof(*ch));
        ch->pwm_on = 255;
        ch->pwm_off = 0;
        ch->mode = AUXIO_EFFECT_STEADY;
        ch->period_ms = 800;
        ch->enabled = false;

        if (i < 6) {
            ledc_channel_config_t c = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel = (ledc_channel_t)(LEDC_CHANNEL_2 + i),
                .timer_sel = LEDC_TIMER_1,
                .intr_type = LEDC_INTR_DISABLE,
                .gpio_num = s_gpios[i],
                .duty = 0,
                .hpoint = 0,
            };
            ESP_ERROR_CHECK(ledc_channel_config(&c));
            ch->ledc = true;
            ch->ledc_ch = (ledc_channel_t)(LEDC_CHANNEL_2 + i);
        } else {
            mcpwm_oper_handle_t oper = s_mcpwm_oper[(i - 6) >= 2 ? 1 : 0];
            ch->ledc = false;
            mcpwm_comparator_config_t cmp_cfg = {
                .flags.update_cmp_on_tez = true,
            };
            ESP_ERROR_CHECK(mcpwm_new_comparator(oper, &cmp_cfg, &ch->mcpwm_cmpr));
            mcpwm_generator_config_t gen_cfg = {
                .gen_gpio_num = s_gpios[i],
            };
            mcpwm_gen_handle_t gen = NULL;
            ESP_ERROR_CHECK(mcpwm_new_generator(oper, &gen_cfg, &gen));
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(gen,
                MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                             MCPWM_TIMER_EVENT_EMPTY,
                                             MCPWM_GEN_ACTION_HIGH)));
            ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(gen,
                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                               ch->mcpwm_cmpr,
                                               MCPWM_GEN_ACTION_LOW)));
        }
    }

    ESP_ERROR_CHECK(mcpwm_timer_enable(s_mcpwm_timer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(s_mcpwm_timer, MCPWM_TIMER_START_NO_STOP));

    if (xTaskCreate(effect_task, "aux_fx", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "AUXIO initialized (%u outputs: 6 LEDC + 3 MCPWM @ %u Hz)",
             (unsigned)AUXIO_CH_COUNT, AUXIO_PWM_FREQ_HZ);
    return ESP_OK;
}

static esp_err_t apply_now(uint8_t channel)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    uint32_t now = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
    apply_duty(&s_ch[channel], ch_duty(&s_ch[channel], channel, now));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t auxio_set_enabled(uint8_t channel, bool enabled)
{
    if (channel >= AUXIO_CH_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ch[channel].enabled = enabled;
    xSemaphoreGive(s_lock);
    return apply_now(channel);
}

esp_err_t auxio_set_output(uint8_t channel, bool enabled, uint8_t pwm)
{
    if (channel >= AUXIO_CH_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ch[channel].enabled = enabled;
    s_ch[channel].pwm_on = pwm;
    s_ch[channel].pwm_off = 0;
    s_ch[channel].mode = AUXIO_EFFECT_STEADY;
    s_ch[channel].period_ms = 800;
    s_ch[channel].cur = 0;
    s_ch[channel].fx_next_ms = 0;
    s_ch[channel].fx_scheduled = false;
    xSemaphoreGive(s_lock);
    return apply_now(channel);
}

esp_err_t auxio_set_effect(uint8_t channel, bool enabled,
                           uint8_t pwm_on, uint8_t pwm_off,
                           auxio_effect_t mode, uint16_t period_ms)
{
    if (channel >= AUXIO_CH_COUNT || period_ms < 100U ||
        period_ms > AUXIO_PERIOD_MAX_MS || (uint32_t)mode >= AUXIO_EFFECT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ch[channel].enabled = enabled;
    s_ch[channel].pwm_on = pwm_on;
    s_ch[channel].pwm_off = pwm_off;
    s_ch[channel].mode = mode;
    s_ch[channel].period_ms = period_ms;
    s_ch[channel].cur = 0;
    s_ch[channel].fx_next_ms = 0;
    s_ch[channel].fx_scheduled = false;
    xSemaphoreGive(s_lock);
    return apply_now(channel);
}

esp_err_t auxio_get_enabled(uint8_t channel, bool *out_enabled)
{
    if (channel >= AUXIO_CH_COUNT || out_enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out_enabled = s_ch[channel].enabled;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t auxio_config(uint8_t channel, uint8_t pwm_on, uint8_t pwm_off,
                       auxio_effect_t mode, uint16_t period_ms)
{
    if (channel >= AUXIO_CH_COUNT || period_ms < 20U ||
        period_ms > AUXIO_PERIOD_MAX_MS || (uint32_t)mode >= AUXIO_EFFECT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_ch[channel].pwm_on = pwm_on;
    s_ch[channel].pwm_off = pwm_off;
    s_ch[channel].mode = mode;
    s_ch[channel].period_ms = period_ms;
    s_ch[channel].cur = 0;
    s_ch[channel].fx_next_ms = 0;
    s_ch[channel].fx_scheduled = false;
    xSemaphoreGive(s_lock);
    return apply_now(channel);
}
