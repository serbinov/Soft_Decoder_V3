#include "track.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "motor.h"
#include "dcc.h"
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
/* S9.2 section C prohibits conversion while packet spacing <=30 ms.
 * Four agreeing 50 ms samples add conservative entry hysteresis. */
#define DC_DIGITAL_ABSENCE_US 30000
#define DC_SAMPLE_EXPIRY_US  100000

static bool s_dc_forward = true;
static uint8_t s_dc_forward_votes = 0;
static uint32_t s_rail_mv;
static int64_t s_rail_sample_us;
static bool s_rail_valid;
static bool s_dc_detected;
static bool s_dc_candidate_forward;
static uint8_t s_dc_candidate_votes;
static uint32_t s_dc_generation;
static bool s_dcc_motor_owned;
static portMUX_TYPE s_track_mux = portMUX_INITIALIZER_UNLOCKED;

void track_note_dcc_motor_command(void)
{
    portENTER_CRITICAL(&s_track_mux);
    s_dcc_motor_owned = true;
    portEXIT_CRITICAL(&s_track_mux);
}

bool track_is_dc_mode(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_track_mux);
    bool detected = s_dc_detected && s_rail_valid &&
                    now - s_rail_sample_us < DC_SAMPLE_EXPIRY_US;
    portEXIT_CRITICAL(&s_track_mux);
    return detected && now - dcc_last_signal_packet_us() > DC_DIGITAL_ABSENCE_US &&
           dcc_signal_is_static(DC_DIGITAL_ABSENCE_US);
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
            portENTER_CRITICAL(&s_track_mux);
            s_rail_mv = raw_to_mv(raw);
            s_rail_sample_us = esp_timer_get_time();
            s_rail_valid = true;
            portEXIT_CRITICAL(&s_track_mux);
        }
    }
    int64_t now = esp_timer_get_time();
    bool fresh = s_rail_valid && now - s_rail_sample_us < DC_SAMPLE_EXPIRY_US;
    (void)motor_set_rail_voltage_mv(fresh ? s_rail_mv : 0U);
    bool digital_absent = now - dcc_last_signal_packet_us() > DC_DIGITAL_ABSENCE_US &&
                          dcc_signal_is_static(DC_DIGITAL_ABSENCE_US);
    bool forward = (gpio_get_level((gpio_num_t)PIN_DCC_IN) == 0);
    portENTER_CRITICAL(&s_track_mux);
    if (!fresh || !digital_absent) {
        s_dc_detected = false;
        s_dc_candidate_votes = 0;
        s_dc_forward_votes = 0;
    } else if (!s_dc_detected) {
        if (s_dc_candidate_votes == 0U || forward != s_dc_candidate_forward) {
            s_dc_candidate_forward = forward;
            s_dc_candidate_votes = 1;
        } else if (++s_dc_candidate_votes >= DC_POLARITY_DEBOUNCE) {
            s_dc_forward = forward;
            s_dc_detected = true;
        }
    } else if (forward == s_dc_forward) {
        s_dc_forward_votes = 0;
    } else if (++s_dc_forward_votes >= DC_POLARITY_DEBOUNCE) {
        s_dc_forward = forward;
        s_dc_forward_votes = 0;
    }
    portEXIT_CRITICAL(&s_track_mux);

    uint8_t cv29 = 0;
    bool permitted = settings_cv_read(29, &cv29) == ESP_OK && (cv29 & 0x04U) != 0U;
    uint32_t generation;
    if (!web_control_rails_begin(&generation)) {
        /* Source transition owns the stop. Former DC owners must never write
         * a delayed zero over a new web command. */
        *was_driving = false;
        return;
    }
    /* CV access/source admission can yield. Recheck the sample at the actual
     * command boundary, not against the beginning of this iteration. */
    now = esp_timer_get_time();
    if (!s_rail_valid || now - s_rail_sample_us >= DC_SAMPLE_EXPIRY_US) {
        (void)motor_set_rail_voltage_mv(0U);
    }
    bool driving = track_is_dc_mode() && permitted;
    if (driving) {
        if (motor_set_speed(dc_speed_step(s_rail_mv),
                            s_dc_forward ^ ((cv29 & 0x01U) != 0U)) == ESP_OK) {
            portENTER_CRITICAL(&s_track_mux);
            s_dc_generation = generation;
            s_dcc_motor_owned = false;
            portEXIT_CRITICAL(&s_track_mux);
        } else {
            driving = false;
        }
    }
    bool dc_owned = false, owner_current = false;
    if (!driving && *was_driving) {
        portENTER_CRITICAL(&s_track_mux);
        dc_owned = s_dcc_motor_owned;
        owner_current = (generation == s_dc_generation);
        portEXIT_CRITICAL(&s_track_mux);
    }
    if (!driving && *was_driving && owner_current && !dc_owned) {
        /* Only an accepted motor command hands ownership to DCC. Merely
         * receiving addressed functions/CVs must not preserve old DC motion. */
        motor_emergency_stop();
    }
    web_control_rails_end();
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
