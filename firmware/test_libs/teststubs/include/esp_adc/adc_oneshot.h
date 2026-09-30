#ifndef MOCK_ESP_ADC_ADC_ONESHOT_H
#define MOCK_ESP_ADC_ADC_ONESHOT_H

#include <stdint.h>

#include "esp_err.h"

/* Minimal adc_oneshot host stub. The raw value returned by
 * adc_oneshot_read() is driven by mock_adc_raw[channel] (channel = GPIO-1)
 * so tests can feed deterministic measurements. */

typedef enum {
    ADC_UNIT_1 = 0,
    ADC_UNIT_2,
} adc_unit_t;

typedef enum {
    ADC_CHANNEL_0 = 0,
    ADC_CHANNEL_1,
    ADC_CHANNEL_2,
    ADC_CHANNEL_3,
    ADC_CHANNEL_4,
    ADC_CHANNEL_5,
    ADC_CHANNEL_6,
    ADC_CHANNEL_7,
    ADC_CHANNEL_8,
    ADC_CHANNEL_9,
} adc_channel_t;

typedef enum {
    ADC_ATTEN_DB_0 = 0,
    ADC_ATTEN_DB_2_5,
    ADC_ATTEN_DB_6,
    ADC_ATTEN_DB_12,
} adc_atten_t;

typedef enum {
    ADC_BITWIDTH_DEFAULT = 0,
    ADC_BITWIDTH_9,
    ADC_BITWIDTH_10,
    ADC_BITWIDTH_11,
    ADC_BITWIDTH_12,
} adc_bitwidth_t;

typedef struct adc_oneshot_unit_ctx *adc_oneshot_unit_handle_t;

typedef struct {
    adc_unit_t unit_id;
} adc_oneshot_unit_init_cfg_t;

typedef struct {
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;

extern int mock_adc_raw[16];
/* When false, adc_oneshot_read() returns ESP_FAIL. */
extern int mock_adc_ok;
/* When false, adc_oneshot_new_unit() returns ESP_FAIL. */
extern int mock_adc_unit_new_ok;
/* When false, adc_oneshot_config_channel() returns ESP_FAIL. */
extern int mock_adc_config_ok;

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *init_config,
                               adc_oneshot_unit_handle_t *ret_unit);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle,
                                     adc_channel_t channel,
                                     const adc_oneshot_chan_cfg_t *config);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle,
                           adc_channel_t chan, int *out_raw);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle);

#endif /* MOCK_ESP_ADC_ADC_ONESHOT_H */
