#ifndef MOCK_DRIVER_MCPWM_PRELUDE_H
#define MOCK_DRIVER_MCPWM_PRELUDE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Minimal mcpwm_prelude host stub. Comparators record the duty (%) implied by
 * the last compare value and the active timer period, so tests can assert the
 * applied duty: mock_mcpwm_duty[comparator_creation_index]. */

typedef struct mcpwm_timer_ctx *mcpwm_timer_handle_t;
typedef struct mcpwm_oper_ctx *mcpwm_oper_handle_t;
typedef struct mcpwm_compr_ctx *mcpwm_cmpr_handle_t;
typedef struct mcpwm_gen_ctx *mcpwm_gen_handle_t;

typedef struct {
    int group_id;
    int clk_src;
    uint32_t resolution_hz;
    int count_mode;
    uint32_t period_ticks;
} mcpwm_timer_config_t;

typedef struct {
    int group_id;
} mcpwm_operator_config_t;

typedef struct {
    struct {
        unsigned int update_cmp_on_tez : 1;
    } flags;
} mcpwm_comparator_config_t;

typedef struct {
    int gen_gpio_num;
} mcpwm_generator_config_t;

#define MCPWM_TIMER_CLK_SRC_DEFAULT   0
#define MCPWM_TIMER_COUNT_MODE_UP     0
#define MCPWM_TIMER_DIRECTION_UP      0
#define MCPWM_TIMER_EVENT_EMPTY       0
#define MCPWM_GEN_ACTION_HIGH         0
#define MCPWM_GEN_ACTION_LOW          0
#define MCPWM_TIMER_START_NO_STOP     0

#define MCPWM_GEN_TIMER_EVENT_ACTION(dir, evt, act)   0
#define MCPWM_GEN_COMPARE_EVENT_ACTION(dir, cmp, act) 0

extern uint32_t mock_mcpwm_period_ticks;
extern float mock_mcpwm_duty[16];
extern int mock_mcpwm_count;

/* Reset counters and recorded duties (call from test setUp). */
void mock_mcpwm_reset(void);

esp_err_t mcpwm_new_timer(const mcpwm_timer_config_t *config, mcpwm_timer_handle_t *ret_timer);
esp_err_t mcpwm_new_operator(const mcpwm_operator_config_t *config, mcpwm_oper_handle_t *ret_oper);
esp_err_t mcpwm_operator_connect_timer(mcpwm_oper_handle_t oper, mcpwm_timer_handle_t timer);
esp_err_t mcpwm_new_comparator(mcpwm_oper_handle_t oper,
                               const mcpwm_comparator_config_t *config,
                               mcpwm_cmpr_handle_t *ret_cmpr);
esp_err_t mcpwm_new_generator(mcpwm_oper_handle_t oper,
                              const mcpwm_generator_config_t *config,
                              mcpwm_gen_handle_t *ret_gen);
esp_err_t mcpwm_generator_set_action_on_timer_event(mcpwm_gen_handle_t generator, int action);
esp_err_t mcpwm_generator_set_action_on_compare_event(mcpwm_gen_handle_t generator, int action);
esp_err_t mcpwm_comparator_set_compare_value(mcpwm_cmpr_handle_t cmpr, uint32_t value);
esp_err_t mcpwm_timer_enable(mcpwm_timer_handle_t timer);
esp_err_t mcpwm_timer_start_stop(mcpwm_timer_handle_t timer, int command);

#endif /* MOCK_DRIVER_MCPWM_PRELUDE_H */
