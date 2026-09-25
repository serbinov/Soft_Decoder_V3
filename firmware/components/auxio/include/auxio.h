#ifndef AUXIO_H
#define AUXIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* AUX outputs (9): F0F, F0R, AUX1..AUX7. First 6 are LEDC-backed, the last 3
 * (AUX5..AUX7) are MCPWM-backed (LEDC has only 8 channels and the motor owns
 * LEDC_CHANNEL_0/1). */
#define AUXIO_CH_F0F  0
#define AUXIO_CH_F0R  1
#define AUXIO_CH_AUX1 2
#define AUXIO_CH_AUX2 3
#define AUXIO_CH_AUX3 4
#define AUXIO_CH_AUX4 5
#define AUXIO_CH_AUX5 6
#define AUXIO_CH_AUX6 7
#define AUXIO_CH_AUX7 8
#define AUXIO_CH_COUNT 9

typedef enum {
    AUXIO_EFFECT_STEADY = 0,       /* plain output (level) */
    AUXIO_EFFECT_INCANDESCENT = 1, /* lamp: smooth warm-up / cool-down + gamma */
    AUXIO_EFFECT_MARS = 2,         /* Mars light / gyralight: sinusoidal sweep */
    AUXIO_EFFECT_DITCH = 3,        /* ditch lights: alternating pair */
    AUXIO_EFFECT_BEACON = 4,       /* rotary beacon: flash then decay to a floor */
    AUXIO_EFFECT_STROBE = 5,       /* double strobe: two short flashes */
    AUXIO_EFFECT_FIREBOX = 6,      /* firebox flicker: random 40..100 % */
} auxio_effect_t;
#define AUXIO_EFFECT_COUNT 7
/* Longest accepted effect period (ms); keeps the beacon decay math inside
 * 32-bit range. */
#define AUXIO_PERIOD_MAX_MS 2000

esp_err_t auxio_init(void);

/* Enable/disable an output without changing its configured effect/PWM. */
esp_err_t auxio_set_enabled(uint8_t channel, bool enabled);

/* Steady output: enabled + PWM level 0..255. */
esp_err_t auxio_set_output(uint8_t channel, bool enabled, uint8_t pwm);

/* Effect output: blink/breath between pwm_off and pwm_on, period in ms. */
esp_err_t auxio_set_effect(uint8_t channel, bool enabled,
                           uint8_t pwm_on, uint8_t pwm_off,
                           auxio_effect_t mode, uint16_t period_ms);

/* Configure level/effect WITHOUT changing the enabled state (used by the
 * PWM/effect settings so re-configuring does not switch the output off). */
esp_err_t auxio_config(uint8_t channel, uint8_t pwm_on, uint8_t pwm_off,
                       auxio_effect_t mode, uint16_t period_ms);

/* Read-only snapshot of a channel's on/off state. Used by the boot self-test;
 * returns ESP_ERR_INVALID_ARG for a bad channel or NULL output. */
esp_err_t auxio_get_enabled(uint8_t channel, bool *out_enabled);

#endif
