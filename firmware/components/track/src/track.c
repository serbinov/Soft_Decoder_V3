#include "track.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motor.h"
#include "pinmap.h"
#include "settings.h"
#include "web.h"

static const char *TAG = "track";

/* RAIL_SENSE divider: upper 10k, lower 1.62k -> ADC sees 0.139415 x rail.
 * ADC full scale at 12 dB attenuation is ~3100 mV. Values below are the
 * divider-domain voltage: 2500 mV ~ 18 V rail, 110 mV ~ 0.8 V rail. */
#define DC_RAIL_FULL_MV     2500
#define DC_RAIL_DEADBAND_MV 110

/* Direction follows the DCC_IN static level (right rail positive -> low ->
 * forward); N consecutive agreeing samples are required to flip direction. */
#define DC_POLARITY_DEBOUNCE 4

static bool s_dc_forward = true;
static uint8_t s_dc_forward_votes = 0;
static uint32_t s_rail_mv;

bool track_is_dc_mode(void)
{
    uint8_t cv29 = 0;
    if (settings_cv_read(29, &cv29) != ESP_OK) {
        return false;
    }
    return (cv29 & 0x04U) != 0;
}

static uint32_t raw_to_mv(int raw)
{
    if (raw < 0) {
        return 0;
    }
    if (raw > 4095) {
        raw = 4095;
    }
    return (uint32_t)raw * 3100U / 4095U;
}

static uint8_t dc_speed_step(uint32_t mv)
{
    if (mv <= DC_RAIL_DEADBAND_MV) {
        return 0;
    }
    uint32_t speed = (mv - DC_RAIL_DEADBAND_MV) * 126U /
                     (DC_RAIL_FULL_MV - DC_RAIL_DEADBAND_MV);
    if (speed > 126U) {
        speed = 126U;
    }
    return (uint8_t)speed;
}

/* One iteration of the rail sampling / DC-drive logic. Split out of the task
 * loop so host tests can drive it step by step. */
static void track_adc_step(bool *was_driving)
{
    /* Always sample the rail: it feeds the motor BEMF-PID target. */
    if (motor_bemf_lock()) {
        int raw = motor_adc_read_raw(PIN_RAIL_SENSE);
        motor_bemf_unlock();
        if (raw >= 0) {
            s_rail_mv = raw_to_mv(raw);
        }
    }
    (void)motor_set_rail_voltage_mv(s_rail_mv);

    bool driving = track_is_dc_mode() && web_control_is_rails();
    if (driving) {
        uint8_t speed = dc_speed_step(s_rail_mv);

        bool forward = (gpio_get_level((gpio_num_t)PIN_DCC_IN) == 0);
        if (forward == s_dc_forward) {
            s_dc_forward_votes = 0;
        } else if (++s_dc_forward_votes >= DC_POLARITY_DEBOUNCE) {
            s_dc_forward = forward;
            s_dc_forward_votes = 0;
        }

        (void)motor_set_speed(speed, s_dc_forward);
    } else if (*was_driving) {
        motor_stop();
    }
    *was_driving = driving;
}

/* Test hook: 0 runs forever (production); host tests set a small cap to run the
 * loop a bounded number of times. */
static uint32_t s_iter_cap;

static void track_adc_task(void *arg)
{
    (void)arg;
    bool was_driving = false;
    uint32_t iters = 0;
    while (s_iter_cap == 0U || iters < s_iter_cap) {
        track_adc_step(&was_driving);
        vTaskDelay(pdMS_TO_TICKS(50));
        iters++;
    }
}

esp_err_t track_init(void)
{
    esp_err_t err = motor_adc_config_channel(PIN_RAIL_SENSE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC config failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(track_adc_task, "track_adc", 3072, NULL, 7, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Track module initialized (rail ADC on GPIO%d)", PIN_RAIL_SENSE);
    return ESP_OK;
}
