#ifndef MOCK_DRIVER_LEDC_H
#define MOCK_DRIVER_LEDC_H

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

typedef enum {
    LEDC_LOW_SPEED_MODE = 0,
    LEDC_HIGH_SPEED_MODE,
} ledc_mode_t;

typedef enum {
    LEDC_TIMER_0 = 0,
    LEDC_TIMER_1,
    LEDC_TIMER_2,
    LEDC_TIMER_3,
} ledc_timer_t;

typedef enum {
    LEDC_CHANNEL_0 = 0,
    LEDC_CHANNEL_1,
    LEDC_CHANNEL_2,
    LEDC_CHANNEL_3,
    LEDC_CHANNEL_4,
    LEDC_CHANNEL_5,
    LEDC_CHANNEL_6,
    LEDC_CHANNEL_7,
} ledc_channel_t;

typedef enum {
    LEDC_TIMER_1_BIT = 0,
    LEDC_TIMER_8_BIT = 7,
    LEDC_TIMER_10_BIT = 9,
    LEDC_TIMER_12_BIT,
} ledc_timer_bit_t;

typedef enum {
    LEDC_INTR_DISABLE = 0,
    LEDC_INTR_FADE_END,
} ledc_intr_type_t;

typedef enum {
    LEDC_AUTO_CLK = 0,
    LEDC_USE_APB_CLK,
    LEDC_USE_RC_FAST_CLK,
} ledc_clk_cfg_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_timer_t timer_num;
    ledc_timer_bit_t duty_resolution;
    uint32_t freq_hz;
    ledc_clk_cfg_t clk_cfg;
} ledc_timer_config_t;

typedef struct {
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_timer_t timer_sel;
    ledc_intr_type_t intr_type;
    gpio_num_t gpio_num;
    uint32_t duty;
    int hpoint;
} ledc_channel_config_t;

/* Test hook: last duty written per channel (indexed by ledc_channel_t). */
extern uint32_t mock_ledc_duty[8];
extern int mock_ledc_update_count;

esp_err_t ledc_timer_config(const ledc_timer_config_t *timer_conf);
esp_err_t ledc_channel_config(const ledc_channel_config_t *ledc_conf);
esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel);

#endif /* MOCK_DRIVER_LEDC_H */
