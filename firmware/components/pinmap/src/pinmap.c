#include "pinmap.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "pinmap";

/* ESP32-S3 strapping pins. They may be used by this board only where the
 * schematic documents the external pull state at reset. */
static bool pinmap_is_strapping(int pin)
{
    return pin == 0 || pin == 3 || pin == 45 || pin == 46;
}

/* Board v4.0 uses GPIO3 (ACK) and GPIO46 (AUX F0R) with external pulls. Keep
 * this whitelist in sync with pinmap.h so the generic checker rejects every
 * other strapping assignment. */
static bool pinmap_board_allows_strapping(int pin)
{
    return pin == PIN_ACK_LOAD || pin == PIN_AUX_F0R;
}

static bool pinmap_valid_output(int pin)
{
#ifdef GPIO_IS_VALID_OUTPUT_GPIO
    return GPIO_IS_VALID_OUTPUT_GPIO(pin) != 0;
#else
    /* Host fallback mirroring the ESP32-S3 mask used by this board. Do not use
     * GPIO_PIN_COUNT as the upper bound: it varies between IDF versions. */
    return pin >= 0 && pin <= 48 &&
           pin != 22 && pin != 23 && pin != 24 && pin != 25;
#endif
}

/* Range/output validity, strapping policy and duplicate detection. */
static esp_err_t pinmap_check_impl(const int *pins, size_t n, bool allow_board_strapping)
{
    for (size_t i = 0; i < n; ++i) {
        if (!pinmap_valid_output(pins[i])) {
            ESP_LOGE(TAG, "GPIO%d is not a valid output pin", pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
        if (pinmap_is_strapping(pins[i]) &&
            !(allow_board_strapping && pinmap_board_allows_strapping(pins[i]))) {
            ESP_LOGE(TAG, "GPIO%d is an ESP32-S3 strapping pin", pins[i]);
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = i + 1; j < n; ++j) {
            if (pins[i] == pins[j]) {
                ESP_LOGE(TAG, "GPIO%d assigned twice", pins[i]);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

/* Board-specific check: only the documented strap pins are allowed. */
static esp_err_t pinmap_check_board(const int *pins, size_t n)
{
    return pinmap_check_impl(pins, n, true);
}

esp_err_t pinmap_validate(void)
{
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
    esp_err_t err = pinmap_check_board(pins, n);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "pinmap validated (%u pins)", (unsigned)n);
    }
    return err;
}
