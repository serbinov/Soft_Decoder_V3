#ifndef MOCK_DRIVER_MCPWM_H
#define MOCK_DRIVER_MCPWM_H

#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"

typedef enum {
    MCPWM_UNIT_0 = 0,
    MCPWM_UNIT_1,
} mcpwm_unit_t;

typedef enum {
    MCPWM_TIMER_0 = 0,
    MCPWM_TIMER_1,
    MCPWM_TIMER_2,
} mcpwm_timer_t;

typedef enum {
    MCPWM_OPR_A = 0,
    MCPWM_OPR_B,
} mcpwm_operator_t;

typedef enum {
    MCPWM0A = 0,
    MCPWM0B,
    MCPWM1A,
    MCPWM1B,
    MCPWM2A,
    MCPWM2B,
} mcpwm_io_signals_t;

typedef enum {
    MCPWM_UP_COUNTER = 1,
    MCPWM_DOWN_COUNTER,
    MCPWM_UP_DOWN_COUNTER,
} mcpwm_counter_type_t;

typedef enum {
    MCPWM_DUTY_MODE_0 = 0,
    MCPWM_DUTY_MODE_1,
} mcpwm_duty_type_t;

typedef struct {
    uint32_t frequency;
    float cmpr_a;
    float cmpr_b;
    mcpwm_counter_type_t counter_mode;
    mcpwm_duty_type_t duty_mode;
} mcpwm_config_t;

/* Test hook: last duty (%) written per (unit, timer, operator). */
extern float mock_mcpwm_duty[2][3][2];

esp_err_t mcpwm_gpio_init(mcpwm_unit_t unit, mcpwm_io_signals_t io_signal, gpio_num_t gpio_num);
esp_err_t mcpwm_init(mcpwm_unit_t unit, mcpwm_timer_t timer, const mcpwm_config_t *conf);
esp_err_t mcpwm_set_duty(mcpwm_unit_t unit, mcpwm_timer_t timer,
                         mcpwm_operator_t op, float duty);
esp_err_t mcpwm_set_duty_type(mcpwm_unit_t unit, mcpwm_timer_t timer,
                              mcpwm_operator_t op, mcpwm_duty_type_t type);

#endif /* MOCK_DRIVER_MCPWM_H */
