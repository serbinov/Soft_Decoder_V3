#ifndef MOCK_DRIVER_ADC_H
#define MOCK_DRIVER_ADC_H

#include <stdint.h>

#include "esp_err.h"

/* Minimal ADC1 host stub. The raw value returned by adc1_get_raw() is driven
 * by mock_adc_raw[channel] so tests can feed deterministic measurements. */
typedef enum {
    ADC1_CHANNEL_0 = 0,
    ADC1_CHANNEL_1,
    ADC1_CHANNEL_2,
    ADC1_CHANNEL_3,
    ADC1_CHANNEL_4,
    ADC1_CHANNEL_5,
    ADC1_CHANNEL_6,
    ADC1_CHANNEL_7,
    ADC1_CHANNEL_8,
    ADC1_CHANNEL_9,
} adc1_channel_t;

typedef enum {
    ADC_WIDTH_BIT_9 = 0,
    ADC_WIDTH_BIT_10,
    ADC_WIDTH_BIT_11,
    ADC_WIDTH_BIT_12,
} adc_bits_width_t;

typedef enum {
    ADC_ATTEN_DB_0 = 0,
    ADC_ATTEN_DB_2_5,
    ADC_ATTEN_DB_6,
    ADC_ATTEN_DB_11,
    ADC_ATTEN_DB_12 = 3,
} adc_atten_t;

extern int mock_adc_raw[16];
/* When false, adc1_get_raw() returns -1 (conversion failure). */
extern int mock_adc_ok;

esp_err_t adc1_config_width(adc_bits_width_t width);
esp_err_t adc1_config_channel_atten(adc1_channel_t channel, adc_atten_t atten);
int adc1_get_raw(adc1_channel_t channel);

#endif /* MOCK_DRIVER_ADC_H */
