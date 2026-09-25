#include "pinmap.h"

#include "esp_log.h"

static const char *TAG = "pinmap";

esp_err_t pinmap_validate(void)
{
    /* GPIO23-25 are unavailable on many ESP32-S3 variants. */
    static const int pins[] = {
        PIN_MOTOR_IN1, PIN_MOTOR_IN2, PIN_DCC_IN, PIN_ACK_LOAD,
        PIN_BEMF1, PIN_BEMF2, PIN_RAIL_SENSE, PIN_CURRENT_SENSE,
        PIN_AUDIO_BCLK, PIN_AUDIO_WS, PIN_AUDIO_DOUT, PIN_AUDIO_SD_MODE,
        PIN_STORAGE_CS, PIN_STORAGE_SCK, PIN_STORAGE_MOSI, PIN_STORAGE_MISO,
        PIN_AUX_F0F, PIN_AUX_F0R, PIN_AUX1, PIN_AUX2, PIN_AUX3,
        PIN_AUX4, PIN_AUX5, PIN_AUX6, PIN_AUX7,
        PIN_I2C_SDA, PIN_I2C_SCL,
    };
    const size_t n = sizeof(pins) / sizeof(pins[0]);
    for (size_t i = 0; i < n; ++i) {
        if (pins[i] == 23 || pins[i] == 24 || pins[i] == 25) {
            ESP_LOGE(TAG, "GPIO%d is not available on ESP32-S3", pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    ESP_LOGI(TAG, "pinmap validated (%u pins)", (unsigned)n);
    return ESP_OK;
}
