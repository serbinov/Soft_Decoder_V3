#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_system.h"
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
        (void)settings_cv_commit();
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
}

static void on_dcc_reset(void)
{
    if (!web_control_is_rails()) {
        return;
    }
    motor_stop();
    clear_functions();
    web_log_event("DCC", "сброс декодера (broadcast)");
    ESP_LOGI(TAG, "Decoder reset (broadcast)");
}

static void safety_task(void *arg)
{
    (void)arg;
    bool timeout_active = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));

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
            motor_stop();
            clear_functions();
        }
        timeout_active = timed_out;
    }
}

static void ensure_audio_dir(void)
{
    mkdir("/userdata/audio", 0755);
}

static bool name_is_wav(const char *n)
{
    size_t l = strlen(n);
    if (l <= 4U) {
        return false;
    }
    const char *e = n + l - 4U;
    return e[0] == '.' &&
           (e[1] == 'w' || e[1] == 'W') &&
           (e[2] == 'a' || e[2] == 'A') &&
           (e[3] == 'v' || e[3] == 'V');
}

/* Slot number encoded in "slotN.wav", else 0. */
static int slot_from_name(const char *n)
{
    if (strncmp(n, "slot", 4) != 0) {
        return 0;
    }
    char *end = NULL;
    long s = strtol(n + 4, &end, 10);
    if (end == n + 4 || *end != '.' || s < 1 || s > SETTINGS_MAX_TRACKS) {
        return 0;
    }
    return (int)s;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Collect .wav file paths from one directory; `prefix` is prepended to build
 * the storage-relative path (e.g. "audio/"). */
static size_t scan_wavs(const char *dirpath, const char *prefix,
                        char rel[][SETTINGS_TRACK_FILE_MAX], size_t cap)
{
    DIR *dir = opendir(dirpath);
    if (dir == NULL) {
        return 0;
    }
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < cap) {
        if (!name_is_wav(ent->d_name)) {
            continue;
        }
        snprintf(rel[n], SETTINGS_TRACK_FILE_MAX, "%s%.120s", prefix, ent->d_name);
        n++;
    }
    closedir(dir);
    return n;
}

/* If the stored track list is gone (e.g. NVS was erased after a firmware
 * update) but the WAV files are still on the external storage, rebuild the
 * list from the files on disk so the sounds are usable again. Only runs when
 * there is no metadata at all, so an intentionally emptied list stays empty.
 * Reports the result to the web journal so the cause is visible without a
 * serial console. */
static void recover_tracks_from_storage(void)
{
    settings_track_t existing[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    esp_err_t lerr = settings_tracks_load(existing, &count);

    /* Always list what is actually on the storage, for diagnosis. */
    char rel[SETTINGS_MAX_TRACKS][SETTINGS_TRACK_FILE_MAX];
    size_t na = scan_wavs("/userdata/audio", "audio/", rel, SETTINGS_MAX_TRACKS);
    size_t nr = 0;
    if (na == 0) {
        nr = scan_wavs("/userdata", "", rel, SETTINGS_MAX_TRACKS);
    }
    size_t nf = na > 0 ? na : nr;
    ESP_LOGI(TAG, "recover: nvs=%s count=%u | files: audio_dir=%u root_dir=%u",
             esp_err_to_name(lerr), (unsigned)count, (unsigned)na, (unsigned)nr);
    for (size_t i = 0; i < nf; ++i) {
        ESP_LOGI(TAG, "recover: file %u/%u %s", (unsigned)(i + 1), (unsigned)nf, rel[i]);
    }

    if (lerr == ESP_OK && count > 0) {
        web_log_event("Звуки", "список в NVS уже есть (%u слотов)", (unsigned)count);
        return;
    }
    if (nf == 0) {
        web_log_event("Звуки", "список в NVS пуст, .wav файлы на хранилище не найдены");
        return;
    }
    qsort(rel, nf, sizeof(rel[0]), cmp_names); /* deterministic slots */

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    bool used[SETTINGS_MAX_TRACKS + 1];
    memset(used, 0, sizeof(used));
    size_t n = 0;

    /* Pass 1: honour a slot number encoded in the file name (provisioning). */
    for (size_t i = 0; i < nf && n < SETTINGS_MAX_TRACKS; ++i) {
        const char *base = strrchr(rel[i], '/');
        base = base ? base + 1 : rel[i];
        int s = slot_from_name(base);
        if (s == 0 || used[s]) {
            continue;
        }
        used[s] = true;
        tracks[n].slot = (uint8_t)s;
        snprintf(tracks[n].file, sizeof(tracks[n].file), "%.120s", rel[i]);
        snprintf(tracks[n].label, sizeof(tracks[n].label), "Слот %d", s);
        tracks[n].enabled = true;
        n++;
    }
    /* Pass 2: files without a slot prefix get the lowest free slots. */
    for (size_t i = 0; i < nf && n < SETTINGS_MAX_TRACKS; ++i) {
        const char *base = strrchr(rel[i], '/');
        base = base ? base + 1 : rel[i];
        if (slot_from_name(base) != 0) {
            continue;
        }
        int s = 1;
        while (s <= SETTINGS_MAX_TRACKS && used[s]) {
            s++;
        }
        if (s > SETTINGS_MAX_TRACKS) {
            break;
        }
        used[s] = true;
        tracks[n].slot = (uint8_t)s;
        snprintf(tracks[n].file, sizeof(tracks[n].file), "%.120s", rel[i]);
        snprintf(tracks[n].label, sizeof(tracks[n].label), "%.63s", base);
        char *dot = strrchr(tracks[n].label, '.');
        if (dot != NULL) {
            *dot = '\0';
        }
        tracks[n].enabled = true;
        n++;
    }

    if (n == 0) {
        web_log_event("Звуки", "найдено %u файл(ов), но ни один не удалось привязать",
                      (unsigned)nf);
        return;
    }
    esp_err_t err = settings_tracks_save(tracks, n);
    web_log_event("Звуки", "найдено %u файл(ов), список восстановлен (%s)",
                  (unsigned)n, err == ESP_OK ? "сохранён" : esp_err_to_name(err));
    ESP_LOGW(TAG, "Track list rebuilt from storage: %u file(s), save=%s",
             (unsigned)n, esp_err_to_name(err));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Soft Decoder V3 boot (reset=%u)", (unsigned)esp_reset_reason());

    ESP_ERROR_CHECK(pinmap_validate());
    /* Coast the H-bridge immediately: until motor_init() the IN1/IN2 pins would
     * float (storage/NVS init takes a moment), which can make the motor twitch. */
    motor_boot_safe();
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(storage_mount());
    ESP_LOGI(TAG, "Storage: %s", storage_get_backend() == STORAGE_BACKEND_EXTERNAL_NOR
                                    ? "external NOR" : "internal");

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

    ESP_ERROR_CHECK(dcc_init());
    dcc_register_speed_cb(on_dcc_speed);
    dcc_register_function_cb(on_dcc_function);
    dcc_register_cv_write_cb(on_dcc_cv_write);
    dcc_register_cv_read_cb(on_dcc_cv_read);
    dcc_register_reset_cb(on_dcc_reset);
    update_decoder_address();

    {
        esp_err_t err = web_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Web init failed: %s", esp_err_to_name(err));
        }
    }

    /* After web_init so the result is visible in the web journal. */
    recover_tracks_from_storage();

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

    ESP_LOGI(TAG, "Boot complete");
}
