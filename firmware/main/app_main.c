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

static void update_decoder_address(void)
{
    dcc_reload_config();
}

static void on_dcc_speed(uint8_t speed128, bool forward)
{
    if (track_is_dc_mode()) {
        return;
    }
    if (web_control_is_rails()) {
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
        (void)motor_set_speed(speed128, forward);
        /* Re-evaluate the function direction/speed gates (web.c skips the
         * update when nothing actually changed). */
        web_motion_changed(speed128, forward);
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
    if (!web_control_is_rails()) {
        return;
    }
    web_apply_function(fn, state);
}

static void on_dcc_cv_write(uint16_t cv, uint8_t value, bool service_mode)
{
    if (settings_cv_write(cv, value) == ESP_OK) {
        /* Deferred: the flash commit happens in safety_task after the CVs have
         * been stable, so this DCC callback (and service-mode programming) does
         * not block the real-time task inside a flash program/erase. */
        settings_cv_commit_deferred();
        /* CV63 aliases the master volume: apply it live (updates the cached
         * config + audio + persistence) so the next settings_save keeps it. */
        if (cv == 63U) {
            web_master_volume_changed((uint8_t)((uint16_t)value * 100U / 255U));
        }
    }
    web_log_event("CV", "CV %u = %u (%s)", (unsigned)cv, (unsigned)value,
                  service_mode ? "сервис" : "DCC");
    if (cv == 1 || cv == 17 || cv == 18 || cv == 19 || cv == 29) {
        update_decoder_address();
    }
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
    if (!web_control_is_rails()) {
        return;
    }
    motor_emergency_stop();
    clear_functions();
    web_log_event("DCC", "сброс декодера (broadcast)");
    ESP_LOGI(TAG, "Decoder reset (broadcast)");
}

static void safety_task(void *arg)
{
    (void)arg;
    bool timeout_active = false;
    /* Subscribe to the task watchdog: if this loop ever wedges, the system
     * reboots instead of leaving the motor bridge driven. The loop yields every
     * 50 ms and commits settings/flashes CVs, so reset it here. */
    (void)esp_task_wdt_add(NULL);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        (void)esp_task_wdt_reset();
        int64_t now_ms = esp_timer_get_time();

        /* Fail-safe: if the motor task has not completed a tick recently it is
         * stuck (e.g. in a flash/NVS operation) and a normal motor_stop() would
         * never be applied. Coast the bridge directly from here. */
        int64_t motor_tick = motor_last_tick_us();
        if (motor_tick != 0 && (now_ms - motor_tick) > 200000LL) {
            ESP_LOGW(TAG, "Motor task stalled: emergency coast");
            motor_emergency_stop();
        }

        /* Flush a staged (deferred) settings write once the UI has been idle,
         * so slider drags do not each trigger a flash program/erase cycle. */
        settings_pending_flush();

        /* CV11 packet timeout: value x 20 ms without an addressed packet.
         * Only applies while the rails (DCC) control the decoder — in web mode
         * there may be no DCC stream at all, so a missing stream must not stop
         * the web-controlled motor/functions. */
        bool timed_out = false;
        if (web_control_is_rails()) {
            uint8_t cv11 = 0;
            (void)settings_cv_read(11, &cv11);
            if (cv11 != 0U) {
                int64_t now = esp_timer_get_time();
                timed_out = (now - dcc_last_packet_us()) > (int64_t)cv11 * 20000LL;
            }
        }
        if (timed_out && !timeout_active) {
            ESP_LOGW(TAG, "CV11 packet timeout: stop");
            web_log_event("DCC", "таймаут пакетов: стоп");
            motor_emergency_stop();
            clear_functions();
        }
        timeout_active = timed_out;
    }
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
    dcc_register_speed_cb(on_dcc_speed);
    dcc_register_function_cb(on_dcc_function);
    dcc_register_cv_write_cb(on_dcc_cv_write);
    dcc_register_cv_read_cb(on_dcc_cv_read);
    dcc_register_reset_cb(on_dcc_reset);
    update_decoder_address();

    /* Restore manifest metadata BEFORE web_init(): web_init() caches the
     * categories and the function map in RAM, so recovering afterwards would
     * leave the defaults active for the whole session. web_log_event() is safe
     * before web_init() (it buffers into the journal). */
    recover_tracks_from_storage();

    {
        esp_err_t err = web_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Web init failed: %s", esp_err_to_name(err));
        }
    }

    {
        esp_err_t err = track_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Track init failed: %s", esp_err_to_name(err));
        }
    }

    if (xTaskCreate(safety_task, "safety", 3072, NULL, 6, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Safety task create failed");
    }

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
