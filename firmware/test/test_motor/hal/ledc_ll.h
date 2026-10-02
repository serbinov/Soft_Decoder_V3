#ifndef TEST_MOTOR_LEDC_LL_H
#define TEST_MOTOR_LEDC_LL_H

#include <stdbool.h>
#include "driver/ledc.h"

/* Host-only LL register model. Never include this path in a target build. */
#define LEDC_LL_GET_HW() ((void *)0)

static inline void ledc_ll_set_duty_int_part(void *hw, ledc_mode_t mode,
                                           ledc_channel_t channel, uint32_t duty)
{
    (void)hw;
    (void)mode;
    mock_ledc_duty[channel] = duty;
}

static inline void ledc_ll_set_idle_level(void *hw, ledc_mode_t mode,
                                        ledc_channel_t channel, uint32_t level)
{
    (void)hw;
    (void)mode;
    (void)channel;
    (void)level;
}

static inline void ledc_ll_set_sig_out_en(void *hw, ledc_mode_t mode,
                                        ledc_channel_t channel, bool enabled)
{
    (void)hw;
    (void)mode;
    if (!enabled) {
        mock_ledc_duty[channel] = 0;
    }
}

static inline void ledc_ll_set_duty_start(void *hw, ledc_mode_t mode, ledc_channel_t channel)
{
    (void)hw;
    (void)mode;
    (void)channel;
}

static inline void ledc_ll_ls_channel_update(void *hw, ledc_mode_t mode, ledc_channel_t channel)
{
    (void)hw;
    (void)mode;
    (void)channel;
    ++mock_ledc_update_count;
}

#endif
