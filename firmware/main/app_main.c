#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "audio.h"
#include "dcc.h"
#include "motor.h"
#include "pinmap.h"
#include "provision.h"
#include "settings.h"
#include "sound.h"
#include "storage.h"
#include "track.h"
#include "web.h"

static const char *TAG = "soft_decoder";
static QueueHandle_t s_safety_ready;
static bool s_motor_stall_latched;
static portMUX_TYPE s_safety_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_function_cleanup_requested;

static void update_decoder_address(void)
{
    dcc_reload_config();
}

static void on_dcc_speed(uint8_t speed128, bool forward)
{
    if (track_is_dc_mode()) {
        return;
    }
    if (web_control_rails_begin(NULL)) {
        /* Log every decoded speed change so the actual command received from
         * the throttle can be compared with what the motor does (only on a
         * change, so a refreshing command station does not flood the log). */
        static bool have_last;
        static uint8_t last_speed;
        static bool last_forward;
        if (!have_last || speed128 != last_speed || forward != last_forward) {
            have_last = true;
            last_speed = speed128;
            last_forward = forward;
            ESP_LOGI(TAG, "DCC speed: %u %s", (unsigned)speed128,
                     forward ? "fwd" : "rev");
        }
        if (motor_set_speed(speed128, forward) == ESP_OK) {
            track_note_dcc_motor_command();
        /* Re-evaluate the function direction/speed gates (web.c skips the
         * update when nothing actually changed). */
            web_motion_changed(speed128, forward);
        }
        web_control_rails_end();
    }
}

/* Both the DCC stream (rails mode) and the web UI apply a function through the
 * same web_apply_function(): mapped AUX/light outputs plus the mapped sound
 * slots, so the reaction is identical regardless of the control source. */
static void on_dcc_function(uint8_t fn, bool state)
{
    if (track_is_dc_mode()) {
        return;
    }
    /* Control source isolation: in web mode the DCC stream must not drive
     * functions/light/AUX — everything (including AUX) is web-controlled. */
    if (!web_control_rails_begin(NULL)) {
        return;
    }
    web_apply_function(fn, state);
    web_control_rails_end();
}

static esp_err_t on_dcc_cv_write(uint16_t cv, uint8_t value, bool service_mode)
{
    esp_err_t err = settings_cv_write(cv, value);
    if (err != ESP_OK) {
        return err;
    }
    /* Flash writes run in the persistence worker, never in the DCC callback. */
    settings_cv_commit_deferred();
    if (cv == 63U || (cv == 8U && value == 8U)) {
        uint8_t volume = 0;
        (void)settings_cv_read(63U, &volume);
        web_master_volume_changed((uint8_t)((uint16_t)volume * 100U / 255U));
    }
    web_log_event("CV", "CV %u = %u (%s)", (unsigned)cv, (unsigned)value,
                  service_mode ? "сервис" : "DCC");
    if (cv == 1 || cv == 8 || cv == 17 || cv == 18 || cv == 19 || cv == 21 ||
        cv == 22 || cv == 29) {
        update_decoder_address();
    }
    return ESP_OK;
}

static bool on_dcc_cv_read(uint16_t cv, uint8_t *out_value)
{
    return settings_cv_read(cv, out_value) == ESP_OK;
}

static void clear_functions(void)
{
    for (uint8_t fn = 0; fn < SETTINGS_FUNC_MAP_COUNT; ++fn) {
        /* Mapping-aware: turns off the outputs and stops the sounds each F is
         * configured to drive. */
        web_apply_function(fn, false);
    }
    /* Silence the scheme engine (including the latched prime mover) as well. */
    sound_stop_all();
}

static void on_dcc_reset(void)
{
    if (!web_control_rails_begin(NULL)) {
        return;
    }
    motor_emergency_stop();
    track_note_dcc_motor_command();
    clear_functions();
    web_log_event("DCC", "сброс декодера (broadcast)");
    ESP_LOGI(TAG, "Decoder reset (broadcast)");
    web_control_rails_end();
}

static void on_dcc_emergency_stop(void)
{
    if (!web_control_rails_begin(NULL)) {
        return;
    }
    motor_emergency_stop();
    track_note_dcc_motor_command();
    web_motion_changed(0, true);
    web_control_rails_end();
}

static void on_dcc_hard_reset(void)
{
    if (!web_control_rails_begin(NULL)) {
        return;
    }
    if (settings_cv_hard_reset() == ESP_OK) {
        update_decoder_address();
    }
    web_control_rails_end();
}

static bool dcc_packet_timed_out(int64_t now)
{
    if (!web_control_is_rails() || track_is_dc_mode()) {
        return false;
    }
    uint8_t cv11 = 0;
    (void)settings_cv_read(11, &cv11);
    return cv11 != 0U && now - dcc_last_packet_us() > (int64_t)cv11 * 20000LL;
}

static void persistence_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        portENTER_CRITICAL(&s_safety_mux);
        bool cleanup = s_function_cleanup_requested;
        s_function_cleanup_requested = false;
        bool stalled = s_motor_stall_latched;
        portEXIT_CRITICAL(&s_safety_mux);
        if (cleanup && (stalled || dcc_packet_timed_out(esp_timer_get_time()))) {
            clear_functions();
        }
        esp_err_t err = settings_pending_flush();
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Settings flush deferred: %s", esp_err_to_name(err));
        }
    }
}

static void safety_step(bool *timeout_active)
{
    int64_t now = esp_timer_get_time();
    int64_t tick = motor_last_tick_us();
    if (!s_motor_stall_latched && (tick == 0 || now - tick > 200000LL)) {
        portENTER_CRITICAL(&s_safety_mux);
        s_motor_stall_latched = true;
        s_function_cleanup_requested = true;
        portEXIT_CRITICAL(&s_safety_mux);
        (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
        dcc_set_control_enabled(false);
        ESP_LOGE(TAG, "Motor task stalled: output inhibited until restart");
    }
    bool timed_out = dcc_packet_timed_out(now);
    if (timed_out != *timeout_active) {
        (void)motor_set_inhibit_reason(MOTOR_INHIBIT_DCC_TIMEOUT, timed_out);
        if (timed_out) {
            portENTER_CRITICAL(&s_safety_mux);
            s_function_cleanup_requested = true;
            portEXIT_CRITICAL(&s_safety_mux);
            ESP_LOGW(TAG, "CV11 packet timeout: output inhibited");
        }
    }
    *timeout_active = timed_out;
}

static void safety_task(void *arg)
{
    (void)arg;
    bool timeout_active = false;
    esp_err_t ready = esp_task_wdt_add(NULL);
    int64_t deadline = esp_timer_get_time() + 2000000LL;
    if (ready == ESP_OK) {
        while (motor_last_tick_us() == 0 && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(10));
            (void)esp_task_wdt_reset();
        }
        int64_t tick = motor_last_tick_us();
        if (tick == 0 || esp_timer_get_time() - tick > 200000LL) {
            ready = ESP_ERR_TIMEOUT;
        }
    }
    if (ready != ESP_OK) {
        (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
    }
    /* Queue remains allocated for the boot lifetime: a late worker may publish
     * after the main task's readiness deadline without touching freed memory. */
    (void)xQueueSend(s_safety_ready, &ready, 0);
    if (ready != ESP_OK) {
        (void)esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        (void)esp_task_wdt_reset();
        safety_step(&timeout_active);
    }
}

static esp_err_t safety_start(void)
{
    s_safety_ready = xQueueCreate(1, sizeof(esp_err_t));
    if (s_safety_ready == NULL ||
        xTaskCreate(safety_task, "safety", 3072, NULL, 6, NULL) != pdPASS) {
        (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ready = ESP_ERR_TIMEOUT;
    if (xQueueReceive(s_safety_ready, &ready, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ready = ESP_ERR_TIMEOUT;
    }
    if (ready != ESP_OK) {
        (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
    }
    return ready;
}

static void ensure_audio_dir(void)
{
    mkdir("/userdata/audio", 0755);
}

/* Rebuild the track list from storage (moved to the track component so it can
 * be unit-tested); this wrapper maps the result onto the web journal. */
static void recover_tracks_from_storage(void)
{
    track_recover_result_t r;
    track_recover_from_storage("/userdata/audio", "/userdata", &r);
    if (r.error != ESP_OK) {
        ESP_LOGW(TAG, "Track recovery incomplete: %s (retry=%u)",
                 esp_err_to_name(r.error), r.retry_pending ? 1U : 0U);
    }
    if (r.had_nvs) {
        ESP_LOGI(TAG, "track list present in NVS (%u slots)", (unsigned)r.count);
    } else if (r.from_manifest) {
        web_log_event("Звуки", "метаданные восстановлены из манифеста (%u слотов)",
                      (unsigned)r.count);
    } else if (r.files_found == 0) {
        web_log_event("Звуки", "список в NVS пуст, .wav файлы на хранилище не найдены");
    } else if (r.rebuilt) {
        ESP_LOGW(TAG, "Track list rebuilt from storage: %u file(s)", (unsigned)r.count);
    } else {
        ESP_LOGW(TAG, "Track rebuild: %u file(s), none bindable", (unsigned)r.files_found);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Soft Decoder V3 boot (reset=%u)", (unsigned)esp_reset_reason());

    ESP_ERROR_CHECK(pinmap_validate());
    /* Coast the H-bridge immediately: until motor_init() the IN1/IN2 pins would
     * float (storage/NVS init takes a moment), which can make the motor twitch. */
    motor_boot_safe();
    ESP_ERROR_CHECK(storage_init());
    /* Storage is optional: without the external W25Q128 (or if its LittleFS
     * fails to mount) the decoder must keep running with sound disabled, so a
     * mount failure is logged and not treated as fatal. */
    esp_err_t storage_err = storage_mount();
    if (storage_err != ESP_OK) {
        ESP_LOGW(TAG, "Storage unavailable (%s): sound features disabled",
                 esp_err_to_name(storage_err));
    }
    ESP_LOGI(TAG, "Storage: %s", storage_get_backend() == STORAGE_BACKEND_EXTERNAL_NOR
                                    ? "external NOR" : "none");

    ESP_ERROR_CHECK(settings_init());

    /* Serial provisioning: if the host sends "PROV" on UART0 during the boot
     * window, erase the external NOR, receive the sound files and restart. */
    if (provision_try()) {
        ESP_LOGI(TAG, "Provisioning handled, restarting...");
    }

    ensure_audio_dir();

    ESP_ERROR_CHECK(motor_init());
    {
        esp_err_t err = audio_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Audio init failed: %s", esp_err_to_name(err));
        }
    }

    /* Sound scheme engine: needs audio + storage + settings; must run before
     * the DCC callbacks can apply a function key. */
    {
        esp_err_t err = sound_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Sound init failed: %s", esp_err_to_name(err));
        }
    }

    ESP_ERROR_CHECK(dcc_init());

    /* Restore manifest metadata BEFORE web_init(): web_init() caches the
     * categories and the function map in RAM, so recovering afterwards would
     * leave the defaults active for the whole session. web_log_event() is safe
     * before web_init() (it buffers into the journal). */
    recover_tracks_from_storage();

    {
        esp_err_t err = web_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Control consumers not ready: %s", esp_err_to_name(err));
            (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
            return;
        }
    }

    {
        esp_err_t err = track_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Track init failed: %s", esp_err_to_name(err));
            (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
            return;
        }
    }

    if (xTaskCreate(persistence_task, "settings", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Persistence worker create failed");
        (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
        return;
    }

    esp_err_t safety_err = safety_start();
    if (safety_err != ESP_OK) {
        ESP_LOGE(TAG, "Safety not ready: %s; motion and OTA confirmation denied",
                 esp_err_to_name(safety_err));
        return;
    }
    dcc_register_speed_cb(on_dcc_speed);
    dcc_register_function_cb(on_dcc_function);
    dcc_register_cv_write_cb(on_dcc_cv_write);
    dcc_register_cv_read_cb(on_dcc_cv_read);
    dcc_register_reset_cb(on_dcc_reset);
    dcc_register_emergency_stop_cb(on_dcc_emergency_stop);
    dcc_register_hard_reset_cb(on_dcc_hard_reset);
    update_decoder_address();
    esp_err_t ready_err = web_set_actuation_ready(true);
    if (ready_err != ESP_OK) {
        (void)motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true);
        ESP_LOGE(TAG, "Control admission failed: %s", esp_err_to_name(ready_err));
        return;
    }
    dcc_set_control_enabled(true);
    provision_listener_start();

    /* OTA rollback guard: after esp_ota_set_boot_partition() the new image boots
     * as PENDING_VERIFY. Confirm it once every peripheral came up so the
     * bootloader keeps it; if this firmware crashes before this point (or the
     * watchdog reboots it), the previous image is restored automatically. On a
     * plain USB flash the state is not PENDING_VERIFY and the call is a no-op. */
    {
        esp_err_t ota_err = esp_ota_mark_app_valid_cancel_rollback();
        if (ota_err == ESP_OK) {
            ESP_LOGI(TAG, "OTA image confirmed");
        } else if (ota_err != ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            ESP_LOGW(TAG, "OTA confirm failed: %s", esp_err_to_name(ota_err));
        }
    }

    ESP_LOGI(TAG, "Boot complete");
}
