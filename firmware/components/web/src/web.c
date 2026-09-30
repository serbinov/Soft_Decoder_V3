#include "web.h"
#include "web_html.h"
#include "web_util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"

#include "audio.h"
#include "auxio.h"
#include "sound.h"
#include "sound_store.h"
#include "dcc.h"
#include "motor.h"
#include "pinmap.h"
#include "settings.h"
#include "storage.h"

static const char *TAG = "web";

#define WEB_FN_COUNT 29
/* Storage roots are overridable so the host tests can point them at a
 * temporary directory (production uses /userdata). */
#ifndef WEB_USERDATA_DIR
#define WEB_USERDATA_DIR "/userdata"
#endif
#ifndef WEB_AUDIO_DIR
#define WEB_AUDIO_DIR WEB_USERDATA_DIR "/audio"
#endif
#define AUDIO_DIR WEB_AUDIO_DIR
#define UPLOAD_MAX (2 * 1024 * 1024)

/* File-I/O wrappers used by the combined-OTA sound writer; overridable so the
 * host tests can inject fwrite/fflush/fclose failures. */
#ifndef WEB_FWRITE
#define WEB_FWRITE(p, sz, n, f) fwrite((p), (sz), (n), (f))
#endif
#ifndef WEB_FFLUSH
#define WEB_FFLUSH(f) fflush(f)
#endif
#ifndef WEB_FCLOSE
#define WEB_FCLOSE(f) fclose(f)
#endif

static httpd_handle_t s_server;
static httpd_handle_t s_progress_server;
static bool s_wifi_started;
static char s_ap_ip[16];
static uint8_t s_ap_ip_bytes[4];
static char s_sta_ip[16];

/* In-flight upload progress (served by the second httpd on port 81 so the
 * main httpd task, which is blocked inside the upload recv loop, does not
 * have to answer the poll itself). */
static volatile int s_up_total;
static volatile int s_up_received;
static volatile bool s_up_active;
static volatile bool s_up_ota;
static volatile uint8_t s_up_slot;

/* Test hooks for the otherwise unbounded background loops: 0 runs forever
 * (production); the host tests set a small cap so the loop body can be
 * exercised deterministically. */
static uint32_t s_autooff_iter_cap;
static uint32_t s_dns_iter_cap;
/* Set false when the AP is auto-stopped so the DNS task exits and frees its
 * socket instead of blocking forever on a dead interface. */
static volatile bool s_dns_run;
static uint32_t s_pipe_iter_cap;
/* Test hook: make pipe_upload observe a writer write error. */
static bool s_pipe_write_err_inject;

static bool s_fn[WEB_FN_COUNT];

static settings_config_t s_cfg;
static uint8_t s_track_cat[SETTINGS_MAX_TRACKS];
static bool s_track_cat_loaded;
static settings_func_map_t s_func_map[SETTINGS_FUNC_MAP_COUNT];
/* Canonical function bindings (SOUND_ENGINE_IMPLEMENTATION.md section 8.4).
 * Populated from NVS; when the store is empty they are migrated once from the
 * legacy s_func_map. While the list is empty the apply path falls back to
 * web_util_func_desired() so legacy behaviour is byte-for-byte unchanged. */
static func_binding_t s_func_bind[FUNC_BIND_MAX];
static size_t s_func_bind_count;
static settings_aux_cfg_t s_aux_cfg[SETTINGS_AUX_COUNT];
static uint16_t s_func_last_mask[SETTINGS_FUNC_MAP_COUNT];
/* Serialises the shared function-apply state (s_fn + track scratch buffer)
 * between the DCC callback and the HTTP handler. */
static SemaphoreHandle_t s_func_mutex;
static settings_track_t s_func_tracks[SETTINGS_MAX_TRACKS];
static bool s_motion_forward = true;
static uint8_t s_motion_speed;
static bool s_motion_initialized;

/* Heap size of the /api/audio/tracks JSON frame (was a 8 KB stack buffer). */
#define WEB_TRACKS_JSON_MAX 8192
/* Heap size of the sound-scheme project list (up to 64 names). */
#define WEB_PROJECTS_JSON_MAX 8192
#define WEB_PROJECTS_MAX 64

/* ---------- event log (web journal) ---------- */
/* Ring buffer of control events (F commands, AUX, sounds, direction, speed,
 * CV writes, ...). The page polls /api/log and appends new entries, so the
 * journal shows everything that drives an output, not just web clicks. */
#define WEB_EVLOG_MAX      40
#define WEB_EVLOG_TAG_MAX  24
#define WEB_EVLOG_TEXT_MAX 96

typedef struct {
    uint32_t seq;
    char tag[WEB_EVLOG_TAG_MAX];
    char text[WEB_EVLOG_TEXT_MAX];
} web_evlog_t;

static web_evlog_t s_evlog[WEB_EVLOG_MAX];
static uint32_t s_evlog_head;
static uint32_t s_evlog_seq;
static SemaphoreHandle_t s_evlog_mutex;

/* Largest length <= n that does not cut a UTF-8 character in half. Prevents
 * the journal from showing a broken glyph when a tag/text is truncated. */
static size_t utf8_safe_len(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t clen = 1;
        if ((c & 0xE0U) == 0xC0U) {
            clen = 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            clen = 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            clen = 4;
        }
        if (i + clen > n) {
            break;
        }
        i += clen;
    }
    return i;
}

void web_log_event(const char *tag, const char *fmt, ...)
{
    if (s_evlog_mutex == NULL) {
        return;
    }
    web_evlog_t ev;
    memset(ev.tag, 0, sizeof(ev.tag));
    memset(ev.text, 0, sizeof(ev.text));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);
    ev.text[utf8_safe_len(ev.text, strlen(ev.text))] = '\0';
    if (tag != NULL) {
        size_t tn = utf8_safe_len(tag, sizeof(ev.tag) - 1U);
        memcpy(ev.tag, tag, tn);
        ev.tag[tn] = '\0';
    }
    (void)xSemaphoreTake(s_evlog_mutex, portMAX_DELAY);
    s_evlog_seq++;
    ev.seq = s_evlog_seq;
    s_evlog[s_evlog_head] = ev;
    s_evlog_head = (s_evlog_head + 1U) % WEB_EVLOG_MAX;
    xSemaphoreGive(s_evlog_mutex);
}

static const char *const WEB_OUT_NAMES[9] = {
    "F0F", "F0R", "AUX1", "AUX2", "AUX3", "AUX4", "AUX5", "AUX6", "AUX7"
};

static void web_format_outputs(uint16_t mask, char *out, size_t n)
{
    size_t used = 0;
    if (n == 0) {
        return;
    }
    out[0] = '\0';
    if (mask == 0U) {
        buf_appendf(out, n, &used, "%s", "—");
        return;
    }
    for (uint8_t b = 0; b < 9U; ++b) {
        if ((mask & (uint16_t)(1U << b)) != 0U) {
            buf_appendf(out, n, &used, "%s%s", used ? "," : "", WEB_OUT_NAMES[b]);
        }
    }
}

static const char *web_dir_name(uint8_t d)
{
    return d == SETTINGS_FUNC_DIR_FWD ? "вперёд"
           : d == SETTINGS_FUNC_DIR_REV ? "назад"
                                        : "—";
}

static const char *web_spd_name(uint8_t s)
{
    return s == SETTINGS_FUNC_SPD_MOVING ? "движ."
           : s == SETTINGS_FUNC_SPD_STOP ? "стоп"
                                         : "—";
}

/* ---------- helpers ---------- */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_html(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

/* ---------- function outputs ---------- */

bool web_get_function_state(uint8_t fn)
{
    if (fn >= WEB_FN_COUNT) {
        return false;
    }
    bool state;
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    state = s_fn[fn];
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
    return state;
}

bool web_control_is_rails(void)
{
    return s_cfg.control_source == 0;
}

bool web_fs_busy(void)
{
    return s_up_active;
}

uint8_t web_get_voice_volume(uint8_t fn)
{
    return web_util_voice_volume(fn, s_track_cat, s_track_cat_loaded,
                                 s_cfg.engine_volume, s_cfg.effects_volume);
}

void web_master_volume_changed(uint8_t vol0_100)
{
    if (vol0_100 > 100U) {
        vol0_100 = 100U;
    }
    s_cfg.master_volume = vol0_100;
    (void)audio_set_volume(vol0_100);
    (void)settings_save_deferred(&s_cfg);
}

/* Effect-specific cycle period (ms): Mars ~1.75 Hz, beacon ~1.1 Hz, strobe
 * and ditch use their own cadence; others ignore it. */
static uint16_t aux_effect_period(uint8_t effect)
{
    switch (effect) {
        case AUXIO_EFFECT_MARS:
            return 570U;
        case AUXIO_EFFECT_BEACON:
            return 900U;
        case AUXIO_EFFECT_STROBE:
            return 1000U;
        case AUXIO_EFFECT_DITCH:
            return 600U;
        default:
            return 800U;
    }
}

static uint8_t func_out_channel(uint8_t bit)
{
    if (bit == 0U) {
        return AUXIO_CH_F0F;
    }
    if (bit == 1U) {
        return AUXIO_CH_F0R;
    }
    return (uint8_t)(AUXIO_CH_AUX1 + (bit - 2U));
}

/* Rebuild the canonical bindings from the legacy function map (one-time
 * migration / legacy-map resync) and persist them. */
static void func_bind_migrate_from_map(void)
{
    /* Keep canonical SOUND/LOGIC bindings (created via the binding API); only
     * the legacy-derived OUTPUT/SLOT records are rebuilt from the map, so a
     * legacy /api/func-map POST cannot wipe custom sound bindings. */
    func_binding_t keep[FUNC_BIND_MAX];
    size_t keep_n = 0;
    for (size_t i = 0; i < s_func_bind_count && keep_n < FUNC_BIND_MAX; ++i) {
        if (s_func_bind[i].used != 0U && s_func_bind[i].target_type != FUNC_TARGET_OUTPUT &&
            s_func_bind[i].target_type != FUNC_TARGET_SLOT) {
            keep[keep_n++] = s_func_bind[i];
        }
    }
    size_t n = 0;
    if (settings_func_bind_legacy_convert(s_func_map, SETTINGS_FUNC_MAP_COUNT,
                                          s_func_bind, &n) == ESP_OK) {
        for (size_t i = 0; i < keep_n && n < FUNC_BIND_MAX; ++i) {
            s_func_bind[n++] = keep[i];
        }
        s_func_bind_count = n;
        (void)settings_func_bind_save(s_func_bind, n);
        sound_reload_bindings();
    }
}

/* Load the canonical bindings, migrating the legacy map when the store is
 * empty. Safe to call once at init. */
static void func_bind_load(void)
{
    size_t n = 0;
    if (settings_func_bind_load(s_func_bind, &n) == ESP_OK && n > 0U) {
        s_func_bind_count = n;
    } else {
        func_bind_migrate_from_map();
    }
}

/* Desired output mask for one function: canonical bindings when present,
 * otherwise the legacy per-function map (verbatim old behaviour). */
static uint16_t func_desired_locked(uint8_t fn, bool fn_on)
{
    if (s_func_bind_count > 0U) {
        uint8_t dir = s_motion_forward ? FUNC_DIR_FWD : FUNC_DIR_REV;
        uint8_t st = (s_motion_speed != 0U) ? FUNC_STATE_MOVING : FUNC_STATE_STOPPED;
        return fn_on ? func_eval(s_func_bind, s_func_bind_count, fn, st, dir, NULL, NULL) : 0U;
    }
    return web_util_func_desired(&s_func_map[fn], fn_on, s_motion_forward, s_motion_speed);
}

/* Caller must hold s_func_mutex (or be single-threaded at init). */
static void func_apply_output_locked(uint8_t fn)
{
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        return;
    }
    uint16_t desired = func_desired_locked(fn, s_fn[fn]);

    /* Apply only the delta versus what this function drove last time, so a
     * channel removed from the mask is switched off even while F stays on. */
    uint16_t prev = s_func_last_mask[fn];
    uint16_t turn_off = (uint16_t)(prev & ~desired);
    uint16_t turn_on = (uint16_t)(desired & ~prev);
    for (uint8_t bit = 0; bit < 9U; ++bit) {
        uint16_t b = (uint16_t)(1U << bit);
        if ((turn_off & b) != 0U) {
            auxio_set_enabled(func_out_channel(bit), false);
        } else if ((turn_on & b) != 0U) {
            auxio_set_enabled(func_out_channel(bit), true);
        }
    }
    s_func_last_mask[fn] = desired;
}

void web_motion_changed(uint8_t speed, bool forward)
{
    if (s_motion_initialized && speed == s_motion_speed && forward == s_motion_forward) {
        return;
    }
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    s_motion_initialized = true;
    s_motion_speed = speed;
    s_motion_forward = forward;
    for (uint8_t f = 0; f < SETTINGS_FUNC_MAP_COUNT; ++f) {
        func_apply_output_locked(f);
    }
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
    web_log_event("DCC", "скор %u, напр %s", (unsigned)speed,
                  forward ? "вперёд" : "назад");
}

void web_func_audio_slots(uint8_t fn, uint8_t *slot_a, uint8_t *slot_b)
{
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        if (slot_a != NULL) {
            *slot_a = 0U;
        }
        if (slot_b != NULL) {
            *slot_b = 0U;
        }
        return;
    }
    if (slot_a != NULL) {
        *slot_a = s_func_map[fn].slot_a;
    }
    if (slot_b != NULL) {
        *slot_b = s_func_map[fn].slot_b;
    }
}

/* Apply a function state exactly like the DCC decoder does: drive the mapped
 * AUX/light outputs and start/stop the mapped sound slots. Both the DCC
 * callback (rails mode) and POST /api/function (web mode) go through here, so
 * the reaction is identical regardless of the control source. Removed the old
 * hard-coded F2..F8 -> AUX1..AUX7 binding; everything now comes from the map. */
void web_apply_function(uint8_t fn, bool state)
{
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        return;
    }
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }

    bool changed = (s_fn[fn] != state);
    s_fn[fn] = state;
    func_apply_output_locked(fn);

    char sounds[48];
    size_t sounds_used = 0;
    sounds[0] = '\0';

    /* With an active sound scheme the engine owns the function-key routing and
     * the reserved engine voices, so the legacy slot mapping below is skipped. */
    bool scheme_on = sound_scheme_enabled();
    if (changed && scheme_on) {
        sound_function(fn, state);
    }

    if (changed && !scheme_on && fn >= 1U && fn <= 20U) {
        uint8_t slot_a = 0, slot_b = 0;
        web_func_audio_slots(fn, &slot_a, &slot_b);
        uint8_t voice_a = (uint8_t)(fn - 1U);
        /* A second voice is offered only for F1..F10 (voices 10..19), so the
         * two voices of one function never overlap F11..F20 primary voices. */
        bool use_b = (slot_b != 0U) && (fn <= (AUDIO_MAX_VOICES / 2U));
        uint8_t voice_b = use_b ? (uint8_t)(voice_a + AUDIO_MAX_VOICES / 2U) : voice_a;

        if (!state || (slot_a == 0U && !use_b)) {
            (void)audio_voice_stop(voice_a);
            if (voice_b != voice_a) {
                (void)audio_voice_stop(voice_b);
            }
        } else {
            /* Track lookup buffer is static: the DCC task stack is small and
             * the httpd task must not carry ~4 KB of tracks either. */
            size_t count = 0;
            if (settings_tracks_load(s_func_tracks, &count) == ESP_OK) {
                const uint8_t want[2] = { slot_a, use_b ? slot_b : 0U };
                const uint8_t voice[2] = { voice_a, voice_b };
                for (uint8_t k = 0; k < 2U; ++k) {
                    bool started = false;
                    if (want[k] != 0U) {
                        for (size_t i = 0; i < count; ++i) {
                            if (s_func_tracks[i].slot == want[k] &&
                                s_func_tracks[i].enabled &&
                                s_func_tracks[i].file[0] != '\0') {
                                char abs_path[160];
                                snprintf(abs_path, sizeof(abs_path), WEB_USERDATA_DIR "/%s",
                                         s_func_tracks[i].file);
                                (void)audio_voice_play(voice[k], abs_path, true,
                                                       web_get_voice_volume(fn));
                                started = true;
                                break;
                            }
                        }
                    }
                    if (started) {
                        buf_appendf(sounds, sizeof(sounds), &sounds_used,
                                    "%s%u", sounds_used ? "," : "", (unsigned)want[k]);
                    } else {
                        (void)audio_voice_stop(voice[k]);
                    }
                }
            }
        }
    }

    if (changed) {
        const settings_func_map_t *m = &s_func_map[fn];
        uint16_t des = func_desired_locked(fn, state);
        char aux[48];
        char tag[8];
        web_format_outputs(des, aux, sizeof(aux));
        snprintf(tag, sizeof(tag), "F%u", (unsigned)fn);
        web_log_event(tag, "%s | AUX: %s | звук: %s | напр: %s | скор: %s",
                      state ? "ВКЛ" : "ВЫКЛ", aux,
                      sounds_used ? sounds : "—",
                      web_dir_name(m->dir), web_spd_name(m->speed));
    }

    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
}

bool web_func_map_get(uint8_t fn, uint8_t *slot_a, uint8_t *slot_b, uint16_t *aux,
                      uint8_t *dir, uint8_t *speed)
{
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        return false;
    }
    if (slot_a != NULL) {
        *slot_a = s_func_map[fn].slot_a;
    }
    if (slot_b != NULL) {
        *slot_b = s_func_map[fn].slot_b;
    }
    if (aux != NULL) {
        *aux = s_func_map[fn].aux_mask;
    }
    if (dir != NULL) {
        *dir = s_func_map[fn].dir;
    }
    if (speed != NULL) {
        *speed = s_func_map[fn].speed;
    }
    return true;
}

bool web_func_map_set(uint8_t fn, uint8_t slot_a, uint8_t slot_b, uint16_t aux,
                      uint8_t dir, uint8_t speed)
{
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        return false;
    }
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    s_func_map[fn].slot_a = slot_a;
    s_func_map[fn].slot_b = slot_b;
    s_func_map[fn].aux_mask = aux;
    s_func_map[fn].dir = dir;
    s_func_map[fn].speed = speed;
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
    if (settings_func_map_save(s_func_map, SETTINGS_FUNC_MAP_COUNT) != ESP_OK) {
        return false;
    }
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    /* The legacy map changed: resync the canonical bindings so the apply path
     * (which prefers them) reflects the edit. */
    func_bind_migrate_from_map();
    func_apply_output_locked(fn);
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
    {
        char auxs[48];
        char slots[24];
        size_t su = 0;
        char tag[8];
        web_format_outputs(aux, auxs, sizeof(auxs));
        slots[0] = '\0';
        buf_appendf(slots, sizeof(slots), &su, "%s",
                    slot_a != 0U ? "" : "—");
        if (slot_a != 0U) {
            buf_appendf(slots, sizeof(slots), &su, "%u", (unsigned)slot_a);
        }
        if (slot_b != 0U) {
            buf_appendf(slots, sizeof(slots), &su, ",%u", (unsigned)slot_b);
        }
        snprintf(tag, sizeof(tag), "F%u", (unsigned)fn);
        web_log_event(tag, "карта: AUX %s | звук %s | напр %s | скор %s",
                      auxs, slots, web_dir_name(dir), web_spd_name(speed));
    }
    return true;
}

static void outputs_init(void)
{
    if (auxio_init() != ESP_OK) {
        ESP_LOGE(TAG, "auxio init failed");
    }
}

/* ---------- WiFi ---------- */

#define AP_IP_DEFAULT "192.168.100.1"

static volatile uint8_t s_ap_sta_count;
static int64_t s_ap_started_us;
static int64_t s_last_client_us; /* last connect/disconnect; auto-off base */

/* Parse a dotted-quad IPv4 address. 0.0.0.0 is rejected (not usable as an AP
 * address). Returns false when the text cannot be parsed. */
static bool parse_ip4(const char *s, esp_ip4_addr_t *out)
{
    esp_ip4_addr_t a = { 0 };
    if (s == NULL || s[0] == '\0' || out == NULL) {
        return false;
    }
    if (esp_netif_str_to_ip4(s, &a) != ESP_OK || a.addr == 0) {
        return false;
    }
    *out = a;
    return true;
}

/* Wi-Fi event log: without it a dropped/deauthenticated client is invisible,
 * which makes a link problem indistinguishable from an HTTP problem. */
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "AP stopped");
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                const wifi_event_ap_staconnected_t *e = data;
                if (s_ap_sta_count < 255U) {
                    s_ap_sta_count++;
                }
                s_last_client_us = esp_timer_get_time();
                ESP_LOGI(TAG, "STA %02x:%02x:%02x:%02x:%02x:%02x connected (aid=%d), clients=%u",
                         e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                         (int)e->aid, (unsigned)s_ap_sta_count);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                const wifi_event_ap_stadisconnected_t *e = data;
                if (s_ap_sta_count > 0U) {
                    s_ap_sta_count--;
                }
                s_last_client_us = esp_timer_get_time();
                ESP_LOGW(TAG, "STA disconnected (aid=%d, reason=%d), clients=%u",
                         (int)e->aid, (int)e->reason, (unsigned)s_ap_sta_count);
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
        const ip_event_assigned_ip_to_client_t *e = data;
        ESP_LOGI(TAG, "Client got IP " IPSTR, IP2STR(&e->ip));
    }
}

/* Pick the least-used of the three non-overlapping 2.4 GHz channels (1/6/11)
 * so the AP does not have to share the crowded default channel 1. Returns 1
 * (the historical default) on any scan problem. */
static uint8_t wifi_pick_channel(void)
{
    wifi_scan_config_t sc = { 0 };
    sc.show_hidden = true;
    sc.scan_time.active.min = 80;
    sc.scan_time.active.max = 200;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return 1;
    }
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK || num == 0) {
        (void)esp_wifi_clear_ap_list(); /* free the scan result set */
        return 1;
    }
    if (num > 16U) {
        num = 16U;
    }
    wifi_ap_record_t recs[16];
    uint16_t got = num;
    if (esp_wifi_scan_get_ap_records(&got, recs) != ESP_OK) {
        (void)esp_wifi_clear_ap_list();
        return 1;
    }
    static const uint8_t chans[3] = { 1, 6, 11 };
    int score[3] = { 0, 0, 0 };
    for (uint16_t i = 0; i < got; ++i) {
        if (recs[i].rssi < -80) {
            continue; /* far away: barely contributes to channel load */
        }
        for (int k = 0; k < 3; ++k) {
            if (recs[i].primary == chans[k]) {
                score[k]++;
            }
        }
    }
    int best = 0;
    for (int k = 1; k < 3; ++k) {
        if (score[k] < score[best]) {
            best = k;
        }
    }
    return chans[best];
}

/* Power save: stop the AP when no client has been connected for the configured
 * time (auto_off_min, 0 = disabled). */
static void wifi_auto_off_task(void *arg)
{
    (void)arg;
    uint32_t iters = 0;
    while (s_autooff_iter_cap == 0U || iters < s_autooff_iter_cap) {
        iters++;
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint8_t minutes = s_cfg.auto_off_min;
        if (!s_wifi_started || minutes == 0U || s_ap_sta_count != 0U) {
            continue;
        }
        /* Time since the last client was seen (or since AP start when no one
         * ever connected), not since boot, so a client that was connected for
         * a long time does not cause an immediate off right after leaving. */
        if (esp_timer_get_time() - s_last_client_us <
            (int64_t)minutes * 60LL * 1000000LL) {
            continue;
        }
        ESP_LOGW(TAG, "WiFi auto-off: no client for %u min, stopping AP",
                 (unsigned)minutes);
        (void)esp_wifi_stop();
        s_wifi_started = false;
        s_dns_run = false; /* let the DNS hijack task exit and free its socket */
        vTaskDelete(NULL);
    }
}

static esp_err_t wifi_start(const settings_config_t *cfg)
{
    if (cfg->wifi_mode == 0) {
        return ESP_OK;
    }

    (void)nvs_flash_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    /* Temporary station interface: used only to scan for the quietest channel
     * at boot. It stays unused afterwards (AP-only design). */
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&wcfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    /* The config lives in our own NVS store; keep the driver from writing. */
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT,
                                              &wifi_event_handler, NULL, NULL);

    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, cfg->ap_ssid[0] ? cfg->ap_ssid : "ADDITIPUS AURA-X",
            sizeof(ap.ap.ssid) - 1U);
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (cfg->ap_password[0] != '\0' && strlen(cfg->ap_password) >= 8U) {
        strncpy((char *)ap.ap.password, cfg->ap_password, sizeof(ap.ap.password) - 1U);
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else if (cfg->ap_password[0] != '\0') {
        /* A short password would leave the AP open without the user noticing. */
        ESP_LOGW(TAG, "AP password ignored (<8 chars): access point stays open");
    }

    /* Choose the AP channel BEFORE the AP is started. Scanning needs the radio
     * up, so bring the station up briefly, scan, then stop it. Doing this
     * before the AP starts avoids switching the AP channel right after it came
     * up (which dropped clients that were already associating and delayed
     * their DHCP/IP). */
    if (sta_netif != NULL) {
        if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK && esp_wifi_start() == ESP_OK) {
            uint8_t ch = wifi_pick_channel();
            (void)esp_wifi_stop();
            if (ch != ap.ap.channel) {
                ap.ap.channel = ch;
                ESP_LOGI(TAG, "AP channel set to %u (least used of 1/6/11)", (unsigned)ch);
            }
        }
    }

    /* AP-only: a stored STA/APSTA mode is ignored. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    /* Access point address: configurable (default 192.168.100.1). */
    esp_ip4_addr_t ap_addr = { 0 };
    if (!parse_ip4(cfg->ap_ip, &ap_addr)) {
        (void)esp_netif_str_to_ip4(AP_IP_DEFAULT, &ap_addr);
    }
    memcpy(s_ap_ip_bytes, &ap_addr.addr, sizeof(s_ap_ip_bytes));

    if (ap_netif != NULL) {
        esp_netif_ip_info_t ip = { 0 };
        ip.ip.addr = ap_addr.addr;
        ip.gw.addr = ap_addr.addr;
        ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip);
        esp_netif_dhcps_start(ap_netif);

        /* Hand the AP IP to DHCP clients as their DNS server: every name
         * resolves to the board so connectivity probes land on the HTTP
         * server (captive portal opens the page automatically). */
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ap_addr.addr;
        (void)esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);

    /* No radio power-save: a sleeping AP radio throttles uploads to ~2 KB/s. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_ap_sta_count = 0;
    s_ap_started_us = esp_timer_get_time();
    s_last_client_us = s_ap_started_us;
    s_wifi_started = true;

    snprintf(s_ap_ip, sizeof(s_ap_ip), "%s", AP_IP_DEFAULT);
    if (ap_netif != NULL) {
        esp_netif_ip_info_t actual = { 0 };
        if (esp_netif_get_ip_info(ap_netif, &actual) == ESP_OK) {
            snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&actual.ip));
            memcpy(s_ap_ip_bytes, &actual.ip.addr, sizeof(s_ap_ip_bytes));
        }
    }
    ESP_LOGI(TAG, "Wi-Fi started (AP-only, channel %u, ip=%s)",
             (unsigned)ap.ap.channel, s_ap_ip);
    return ESP_OK;
}

/* ---------- captive portal ---------- */

static esp_err_t captive_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    /* Captive portal: redirect every unknown path to the page so the phone's
     * connectivity probe opens it automatically (the DNS hijack routes those
     * probes here). Targets the configured AP address. */
    char loc[40];
    snprintf(loc, sizeof(loc), "http://%s/", s_ap_ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, NULL, 0);
}

static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns: socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "dns: bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DNS hijack running on :53");

    /* Receive timeout so the loop can observe s_dns_run after the AP is
     * stopped instead of blocking forever on recvfrom. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_dns_run = true;
    uint8_t q[DNS_MSG_MAX];
    uint8_t r[DNS_MSG_MAX];
    uint32_t iters = 0;
    while ((s_dns_iter_cap == 0U || iters < s_dns_iter_cap) && s_dns_run) {
        iters++;
        struct sockaddr_in from = { 0 };
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, q, sizeof(q), 0, (struct sockaddr *)&from, &flen);
        if (n <= 12) {
            continue;
        }
        size_t rlen = dns_build_response(q, n, r, s_ap_ip_bytes);
        if (rlen > 0) {
            (void)sendto(sock, r, rlen, 0, (struct sockaddr *)&from, flen);
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

static void start_dns_hijack(void)
{
    if (xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 9, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dns: task create failed");
    }
}

/* ---------- handlers ---------- */

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_html(req, s_page_html);
}

static esp_err_t control_source_get(httpd_req_t *req)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"source\":\"%s\"}",
             s_cfg.control_source == 1 ? "web" : "rails");
    return send_json(req, json);
}

static esp_err_t control_source_post(httpd_req_t *req)
{
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    if (!parse_query(query, "source", buf, sizeof(buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing source");
    }
    s_cfg.control_source = (strcasecmp(buf, "web") == 0 || strcmp(buf, "1") == 0) ? 1 : 0;
    (void)settings_save(&s_cfg);
    web_log_event("Источник", "%s", s_cfg.control_source == 1 ? "Веб" : "Рельсы");
    /* Rail control defaults to DCC: the analog (DC) bit of CV29 is cleared so
     * the decoder never drives the motor from the raw rail voltage unless the
     * user explicitly enables analog mode with the DC checkbox. */
    if (s_cfg.control_source == 0) {
        uint8_t cv29 = 0;
        (void)settings_cv_read(29, &cv29);
        cv29 &= (uint8_t)~0x04U;
        (void)settings_cv_write(29, cv29);
        (void)settings_cv_commit();
    }
    motor_stop();
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"source\":\"%s\"}",
             s_cfg.control_source == 1 ? "web" : "rails");
    return send_json(req, json);
}

static esp_err_t mode_get(httpd_req_t *req)
{
    uint8_t cv29 = 0;
    (void)settings_cv_read(29, &cv29);
    bool dc = (cv29 & 0x04U) != 0;
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", dc ? "dc" : "dcc");
    return send_json(req, json);
}

static esp_err_t mode_post(httpd_req_t *req)
{
    char query[32] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    if (!parse_query(query, "mode", buf, sizeof(buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mode");
    }
    bool dc = (strcasecmp(buf, "dc") == 0);
    uint8_t cv29 = 0;
    (void)settings_cv_read(29, &cv29);
    if (dc) {
        cv29 |= 0x04U;
    } else {
        cv29 &= (uint8_t)~0x04U;
    }
    (void)settings_cv_write(29, cv29);
    (void)settings_cv_commit();
    motor_stop();
    web_log_event("Режим", "%s", dc ? "DC (аналог)" : "DCC");
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", dc ? "dc" : "dcc");
    return send_json(req, json);
}

static esp_err_t bemf_cal_get(httpd_req_t *req)
{
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    char json[1024];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used,
                "{\"ok\":true,\"active\":%s,\"progress\":%u,\"total\":%u,"
                "\"valid\":%s,\"stored\":%s,\"use\":%s,\"points\":[",
                info.active ? "true" : "false",
                (unsigned)info.step, (unsigned)info.total,
                info.valid ? "true" : "false",
                info.stored ? "true" : "false",
                motor_get_bemf_enabled() ? "true" : "false");
    for (uint8_t i = 0; i < info.count; ++i) {
        uint16_t pct = (uint16_t)((info.frac[i] * 100U) / 1024U);
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"speed\":%u,\"frac\":%u}",
                    i == 0 ? "" : ",", (unsigned)info.speed[i], (unsigned)pct);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

static esp_err_t bemf_base_get(httpd_req_t *req)
{
    motor_bemf_base_info_t info;
    motor_bemf_base_info(&info);
    char json[1024];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used,
                "{\"ok\":true,\"start\":%u,\"full\":%u,\"points\":[",
                (unsigned)((info.start_frac * 100U) / 1024U),
                (unsigned)((info.full_frac * 100U) / 1024U));
    for (uint8_t i = 0; i < info.count; ++i) {
        uint16_t pct = (uint16_t)((info.frac[i] * 100U) / 1024U);
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"speed\":%u,\"frac\":%u}",
                    i == 0 ? "" : ",", (unsigned)info.speed[i], (unsigned)pct);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

static esp_err_t bemf_cal_post(httpd_req_t *req)
{
    char query[32] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (parse_bool(query, "reset", false)) {
        esp_err_t err = motor_bemf_cal_clear();
        return send_json(req, err == ESP_OK
                                  ? "{\"ok\":true}"
                                  : "{\"ok\":false,\"error\":\"calibration busy\"}");
    }
    esp_err_t err = motor_bemf_cal_start();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, "{\"ok\":false,\"error\":\"calibration running\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"start failed\"}");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t bemf_use_get(httpd_req_t *req)
{
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"enabled\":%s}",
             motor_get_bemf_enabled() ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t bemf_use_post(httpd_req_t *req)
{
    char query[32] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    bool enabled = parse_bool(query, "enabled", motor_get_bemf_enabled());
    motor_set_bemf_enabled(enabled);
    (void)settings_bemf_use_save(enabled);
    web_log_event("BEMF", "регулятор %s", enabled ? "включён" : "выключен, только ШИМ");
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"enabled\":%s}",
             enabled ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t motor_get(httpd_req_t *req)
{
    uint8_t speed = 0;
    bool forward = true;
    motor_get_status(&speed, &forward);
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":true,\"speed\":%u,\"forward\":%s}",
             (unsigned)speed, forward ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t motor_post(httpd_req_t *req)
{
    if (web_control_is_rails()) {
        return send_json(req, "{\"ok\":false,\"error\":\"rails control active\"}");
    }

    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));

    uint8_t speed = 0;
    bool has_speed = parse_u8(query, "speed", &speed);
    bool forward = parse_bool(query, "forward", false);
    bool has_forward = parse_query(query, "forward", (char[16]){0}, 16);

    uint8_t cur = 0;
    bool cur_fwd = true;
    motor_get_status(&cur, &cur_fwd);
    if (!has_speed) {
        speed = cur;
    }
    if (!has_forward) {
        forward = cur_fwd;
    }
    if (speed > 126) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "speed out of range");
    }
    if (speed != cur || forward != cur_fwd) {
        web_log_event("Мотор", "скор %u, напр %s", (unsigned)speed,
                      forward ? "вперёд" : "назад");
    }
    (void)motor_set_speed(speed, forward);

    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":true,\"speed\":%u,\"forward\":%s}",
             (unsigned)speed, forward ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t functions_get(httpd_req_t *req)
{
    char json[128];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"states\":[");
    /* Snapshot under the same lock the DCC callback uses. */
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) {
        buf_appendf(json, sizeof(json), &used, "%s%d",
                    fn == 0 ? "" : ",", s_fn[fn] ? 1 : 0);
    }
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

static esp_err_t function_post(httpd_req_t *req)
{
    if (web_control_is_rails()) {
        return send_json(req, "{\"ok\":false,\"error\":\"rails control active\"}");
    }
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t fn = 0;
    if (!parse_u8(query, "fn", &fn) || fn >= WEB_FN_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fn out of range");
    }
    bool state = parse_bool(query, "state", false);
    web_apply_function(fn, state);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"fn\":%u,\"state\":%s}",
             (unsigned)fn, state ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t aux_effect_post(httpd_req_t *req)
{
    if (web_control_is_rails()) {
        return send_json(req, "{\"ok\":false,\"error\":\"rails control active\"}");
    }
    char query[128] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t ch = 0, pwm_on = 255, pwm_off = 0, mode = 0;
    uint16_t period = 800;
    if (!parse_u8(query, "ch", &ch) || ch >= AUXIO_CH_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch out of range");
    }
    bool enabled = parse_bool(query, "on", false);
    parse_u8(query, "pwm_on", &pwm_on);
    parse_u8(query, "pwm_off", &pwm_off);
    parse_u8(query, "mode", &mode);
    parse_u16(query, "period", &period);

    auxio_effect_t effect = ((uint32_t)mode < AUXIO_EFFECT_COUNT)
                                ? (auxio_effect_t)mode
                                : AUXIO_EFFECT_STEADY;
    (void)auxio_set_effect(ch, enabled, pwm_on, pwm_off, effect, period);
    web_log_event("AUX", "%s: %s, PWM %u/%u, режим %u",
                  WEB_OUT_NAMES[ch], enabled ? "вкл" : "выкл",
                  (unsigned)pwm_on, (unsigned)pwm_off, (unsigned)mode);

    char json[96];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"ch\":%u,\"on\":%s,\"mode\":%u}",
             (unsigned)ch, enabled ? "true" : "false", (unsigned)mode);
    return send_json(req, json);
}

static esp_err_t audio_status_get(httpd_req_t *req)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"playing\":%s,\"active_slot\":%u,\"master_volume\":%u,"
             "\"engine_volume\":%u,\"effects_volume\":%u}",
             audio_is_playing() ? "true" : "false",
             (unsigned)s_cfg.active_slot,
             (unsigned)s_cfg.master_volume,
             (unsigned)s_cfg.engine_volume,
             (unsigned)s_cfg.effects_volume);
    return send_json(req, json);
}

static esp_err_t audio_tracks_get(httpd_req_t *req)
{
    /* Heap, not stack: setting_track_t[20] + an 8 KB JSON frame would be ~12 KB
     * of the 16 KB httpd task stack. */
    settings_track_t *tracks = malloc(sizeof(settings_track_t) * SETTINGS_MAX_TRACKS);
    char *json = malloc(WEB_TRACKS_JSON_MAX);
    if (tracks == NULL || json == NULL) { free(tracks); free(json); return ESP_ERR_NO_MEM; }

    size_t count = 0;
    (void)settings_tracks_load(tracks, &count);
    ESP_LOGI(TAG, "GET /api/audio/tracks -> %u", (unsigned)count);

    size_t used = 0;
    buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "{\"ok\":true,\"cats\":[");
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "%s%d",
                    i == 0 ? "" : ",", (int)s_track_cat[i]);
    }
    buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "],\"tracks\":[");
    for (size_t i = 0; i < count; ++i) {
        char f[SETTINGS_TRACK_FILE_MAX * 2];
        char l[SETTINGS_TRACK_LABEL_MAX * 2];
        json_escape(tracks[i].file, f, sizeof(f));
        json_escape(tracks[i].label, l, sizeof(l));
        buf_appendf(json, WEB_TRACKS_JSON_MAX, &used,
                    "%s{\"slot\":%u,\"file\":\"%s\",\"label\":\"%s\",\"enabled\":%s}",
                    i == 0 ? "" : ",", (unsigned)tracks[i].slot, f, l,
                    tracks[i].enabled ? "true" : "false");
    }
    buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "]}");
    esp_err_t serr = send_json(req, json);
    ESP_LOGI(TAG, "tracks json len=%u send=%s: %.360s", (unsigned)used,
             esp_err_to_name(serr), json);
    free(tracks);
    free(json);
    return serr;
}

static esp_err_t audio_play_post(httpd_req_t *req)
{
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0;
    parse_u8(query, "slot", &slot);

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    (void)settings_tracks_load(tracks, &count);
    for (size_t i = 0; i < count; ++i) {
        if (tracks[i].slot == slot && tracks[i].file[0] != '\0') {
            s_cfg.active_slot = slot;
            (void)settings_save_deferred(&s_cfg);
            char abs_path[160];
            snprintf(abs_path, sizeof(abs_path), WEB_USERDATA_DIR "/%s", tracks[i].file);
            esp_err_t err = audio_voice_play(0, abs_path, false, web_get_voice_volume(slot));
            if (err == ESP_OK) {
                web_log_event("Звук", "слот %u", (unsigned)slot);
            }
            char json[64];
            snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
            return send_json(req, json);
        }
    }
    return send_json(req, "{\"ok\":false,\"error\":\"no file\"}");
}

static esp_err_t audio_stop_post(httpd_req_t *req)
{
    (void)audio_stop();
    web_log_event("Звук", "стоп (все)");
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t audio_volume_post(httpd_req_t *req)
{
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t m = 20, e = 20, f = 20;
    parse_u8(query, "master", &m);
    parse_u8(query, "engine", &e);
    parse_u8(query, "effects", &f);
    if (m > 100) m = 100;
    if (e > 100) e = 100;
    if (f > 100) f = 100;
    bool vol_changed = (m != s_cfg.master_volume || e != s_cfg.engine_volume ||
                        f != s_cfg.effects_volume);
    s_cfg.master_volume = m;
    s_cfg.engine_volume = e;
    s_cfg.effects_volume = f;
    /* Deferred: a slider drag must not trigger one flash write per move. */
    (void)settings_save_deferred(&s_cfg);
    (void)audio_set_volume(m);
    if (vol_changed) {
        web_log_event("Звук", "громкость: общ %u, двиг %u, эфф %u",
                      (unsigned)m, (unsigned)e, (unsigned)f);
    }
    return send_json(req, "{\"ok\":true}");
}

/* Deep-queue write pipeline (from Soft_Decoder_TEST_speed): the httpd task
 * only drains the socket into a queue of 6x8 KB buffers while a dedicated
 * writer task programs the flash, so the TCP window is never stalled by
 * SPIFFS page-program bursts (a shallow pipeline made the client back off and
 * capped the upload well below the link limit). */
#define PIPE_NUM_BUFS      6
#define PIPE_BUF_SIZE      8192
#define PIPE_WRITER_STACK  16384
#define PIPE_WRITER_PRIO   10
#define PIPE_TIMEOUT_MS    30000
#define PIPE_WR_POLL_MS    250
/* Upload progress log cadence (bytes); overridable for host tests. */
#ifndef WEB_UP_PROGRESS_STEP
#define WEB_UP_PROGRESS_STEP 262144
#endif

typedef struct {
    int idx;
    size_t len;
    bool last;
} pipe_item_t;

typedef struct {
    FILE *f;
    QueueHandle_t write_q;
    QueueHandle_t free_q;
    SemaphoreHandle_t done_sem;
    TaskHandle_t task;
    uint8_t *bufs[PIPE_NUM_BUFS];
    esp_err_t write_err;
    volatile bool abort;
} pipe_ctx_t;

static void pipe_writer(void *arg)
{
    pipe_ctx_t *ctx = (pipe_ctx_t *)arg;
    uint32_t iters = 0;
    while (s_pipe_iter_cap == 0U || iters < s_pipe_iter_cap) {
        iters++;
        pipe_item_t it;
        /* Bounded receive so an aborted upload still terminates the writer
         * even if the end marker could not be queued. */
        if (xQueueReceive(ctx->write_q, &it, pdMS_TO_TICKS(PIPE_WR_POLL_MS)) != pdPASS) {
            if (ctx->abort) {
                break;
            }
            continue;
        }
        if (it.len > 0) {
            if (fwrite(ctx->bufs[it.idx], 1, it.len, ctx->f) != it.len) {
                if (ctx->write_err == ESP_OK) {
                    ctx->write_err = ESP_FAIL;
                }
            }
            xQueueSend(ctx->free_q, &it.idx, portMAX_DELAY);
        }
        if (it.last) {
            break;
        }
    }
    xSemaphoreGive(ctx->done_sem);
    vTaskDelete(NULL);
}

static esp_err_t pipe_upload(httpd_req_t *req, FILE *f, int *out_total)
{
    pipe_ctx_t ctx = { .f = f, .write_err = ESP_OK };
    if (s_pipe_write_err_inject) {
        ctx.write_err = ESP_FAIL;
    }
    ctx.write_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(pipe_item_t));
    ctx.free_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(int));
    ctx.done_sem = xSemaphoreCreateBinary();
    if (ctx.write_q == NULL || ctx.free_q == NULL || ctx.done_sem == NULL) {
        if (ctx.write_q != NULL) { vQueueDelete(ctx.write_q); }
        if (ctx.free_q != NULL) { vQueueDelete(ctx.free_q); }
        if (ctx.done_sem != NULL) { vSemaphoreDelete(ctx.done_sem); }
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < PIPE_NUM_BUFS; ++i) {
        ctx.bufs[i] = malloc(PIPE_BUF_SIZE);
        if (ctx.bufs[i] == NULL) {
            for (int j = 0; j < i; ++j) { free(ctx.bufs[j]); }
            vQueueDelete(ctx.write_q);
            vQueueDelete(ctx.free_q);
            vSemaphoreDelete(ctx.done_sem);
            return ESP_ERR_NO_MEM;
        }
        int seed = i;
        xQueueSend(ctx.free_q, &seed, 0);
    }
    if (xTaskCreatePinnedToCore(pipe_writer, "pipe_wr", PIPE_WRITER_STACK, &ctx,
                                PIPE_WRITER_PRIO, &ctx.task, 1) != pdPASS) {
        for (int j = 0; j < PIPE_NUM_BUFS; ++j) { free(ctx.bufs[j]); }
        vQueueDelete(ctx.write_q);
        vQueueDelete(ctx.free_q);
        vSemaphoreDelete(ctx.done_sem);
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    int total = 0;
    int idle = 0;
    int fill_idx = -1;
    size_t fill_len = 0;
    bool fail = false;
    uint32_t next_prog = 0;

    if (xQueueReceive(ctx.free_q, &fill_idx, portMAX_DELAY) != pdPASS) {
        fail = true;
    }

    while (!fail && remaining > 0) {
        size_t want = (remaining > (int)(PIPE_BUF_SIZE - fill_len))
                          ? (size_t)(PIPE_BUF_SIZE - fill_len)
                          : (size_t)remaining;
        int r = httpd_req_recv(req, (char *)ctx.bufs[fill_idx] + fill_len, (int)want);
        if (r > 0) {
            idle = 0;
            fill_len += (size_t)r;
            total += r;
            remaining -= r;
            s_up_received = total;
            if (total - (int)next_prog >= WEB_UP_PROGRESS_STEP) {
                next_prog = (uint32_t)total;
                ESP_LOGI(TAG, "UP: progress %d/%d", total, req->content_len);
            }
            if (fill_len == PIPE_BUF_SIZE || remaining == 0) {
                pipe_item_t it = { .idx = fill_idx, .len = fill_len, .last = (remaining == 0) };
                if (xQueueSend(ctx.write_q, &it, pdMS_TO_TICKS(PIPE_TIMEOUT_MS)) != pdPASS) {
                    fail = true;
                    break;
                }
                if (remaining > 0) {
                    if (xQueueReceive(ctx.free_q, &fill_idx, pdMS_TO_TICKS(PIPE_TIMEOUT_MS)) != pdPASS) {
                        fail = true;
                        break;
                    }
                    fill_len = 0;
                }
            }
        } else if ((r == 0 || r == -3) && ++idle < 4) {
            ESP_LOGI(TAG, "UP: recv idle #%d (got %d/%d)", idle, total, req->content_len);
            continue;
        } else {
            ESP_LOGW(TAG, "UP: recv failed (%d, got %d/%d)", r, total, req->content_len);
            fail = true;
            break;
        }
    }

    pipe_item_t end = { .idx = 0, .len = 0, .last = true };
    if (xQueueSend(ctx.write_q, &end, pdMS_TO_TICKS(PIPE_TIMEOUT_MS)) != pdPASS) {
        /* Queue stayed full (writer wedged): flag the abort so the writer's
         * bounded receive makes it exit instead of blocking this task forever. */
        ctx.abort = true;
    }
    /* The writer always terminates now: it exits on the end marker or on the
     * abort flag within PIPE_WR_POLL_MS, so it is safe to wait and then free
     * the buffers/queues it was using. */
    (void)xSemaphoreTake(ctx.done_sem, portMAX_DELAY);

    *out_total = total;
    esp_err_t werr = ctx.write_err;
    for (int j = 0; j < PIPE_NUM_BUFS; ++j) {
        free(ctx.bufs[j]);
    }
    vQueueDelete(ctx.write_q);
    vQueueDelete(ctx.free_q);
    vSemaphoreDelete(ctx.done_sem);

    if (fail || total != req->content_len) {
        return ESP_FAIL;
    }
    if (werr != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t audio_upload_post(httpd_req_t *req)
{
    if (!storage_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    }
    if (req->content_len <= 0 || req->content_len > UPLOAD_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }

    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0;
    parse_u8(query, "slot", &slot);

    char hdr[160] = { 0 };
    httpd_req_get_hdr_value_str(req, "X-File-Name", hdr, sizeof(hdr));

    char decoded[128] = { 0 };
    url_decode(hdr, decoded, sizeof(decoded));

    char name[64] = { 0 };
    if (decoded[0] != '\0') {
        sanitize_name(decoded, name, sizeof(name));
    }

    /* Load the track list first: it is also used to auto-assign a slot when the
     * request did not carry a valid one (slot 0 is not displayable/deletable). */
    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    bool have_tracks = (settings_tracks_load(tracks, &count) == ESP_OK);
    if (slot == 0U) {
        for (uint16_t cand = 1U; cand <= SETTINGS_MAX_TRACKS && slot == 0U; ++cand) {
            bool used = false;
            for (size_t i = 0; have_tracks && i < count; ++i) {
                if (tracks[i].slot == cand) { used = true; break; }
            }
            if (!used) { slot = cand; }
        }
        if (slot == 0U) {
            return send_json(req, "{\"ok\":false,\"error\":\"track slots full\"}");
        }
    }
    if (name[0] == '\0') {
        snprintf(name, sizeof(name), "slot%u.wav", (unsigned)slot);
    }

    char path[160];
    snprintf(path, sizeof(path), AUDIO_DIR "/%s", name);
    /* Relative path (no /userdata prefix): matches the V0 convention and the
     * web UI which strips the "audio/" prefix for display. */
    char rel_file[128];
    snprintf(rel_file, sizeof(rel_file), "audio/%s", name);

    ESP_LOGI(TAG, "UP: enter slot=%u name=%s len=%d", (unsigned)slot, name, req->content_len);

    /* Remember the previous track file for this slot so a re-upload does not
     * leave an orphaned file (orphans fill the FS and make SPIFFS GC, which
     * collapses upload speed). It is removed only AFTER the new file is
     * written and validated: writing first stays fast (the old pages are not
     * GC'd mid-upload) and the old track survives a failed upload. */
    char old_path[160] = { 0 };
    if (have_tracks) {
        for (size_t i = 0; i < count; ++i) {
            if (tracks[i].slot == slot && tracks[i].file[0] != '\0' &&
                strcmp(tracks[i].file, rel_file) != 0) {
                snprintf(old_path, sizeof(old_path), WEB_USERDATA_DIR "/%s", tracks[i].file);
                break;
            }
        }
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "UP: open %s failed", path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }
    ESP_LOGI(TAG, "UP: opened %s", path);
    /* 16 KB stdio buffer: batches the SPIFFS flush so page-program time is
     * amortized (without it every 512 B write caps the upload at ~80 KB/s). */
    (void)setvbuf(f, NULL, _IOFBF, 16384);

    int total = 0;
    s_up_slot = slot;
    s_up_total = req->content_len;
    s_up_received = 0;
    s_up_active = true;
    s_up_ota = false;
    esp_err_t perr = pipe_upload(req, f, &total);
    s_up_active = false;

    /* Always close the stream before removing the file: a short-circuit
     * would leave the handle open and make remove() fail on the host and on
     * LittleFS. */
    int close_rc = fflush(f);
    close_rc |= fsync(fileno(f));
    close_rc |= fclose(f);
    if (close_rc != 0) {
        ESP_LOGE(TAG, "UP: flush/close failed total=%d/%d", total, req->content_len);
        (void)remove(path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }

    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "UP: fail total=%d/%d err=%s", total, req->content_len, esp_err_to_name(perr));
        (void)remove(path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }
    ESP_LOGI(TAG, "UP: done total=%d", total);

    if (audio_validate_wav(path) != ESP_OK) {
        (void)remove(path);
        return send_json(req, "{\"ok\":false,\"error\":\"invalid wav\"}");
    }

    /* Bind the track to the slot, replacing any previous entry for it (also
     * compacts duplicate slot entries left over from earlier uploads so a
     * stale copy can never win the display / playback lookup). */
    size_t w = 0;
    bool replaced = false;
    for (size_t i = 0; i < count; ++i) {
        if (tracks[i].slot == slot) {
            if (replaced) {
                continue; /* drop duplicate entry for this slot */
            }
            strncpy(tracks[i].file, rel_file, sizeof(tracks[i].file) - 1);
            tracks[i].file[sizeof(tracks[i].file) - 1] = '\0';
            strncpy(tracks[i].label, name, sizeof(tracks[i].label) - 1);
            tracks[i].label[sizeof(tracks[i].label) - 1] = '\0';
            tracks[i].enabled = true;
            replaced = true;
        }
        if (w != i) {
            tracks[w] = tracks[i];
        }
        w++;
    }
    if (!replaced && w < SETTINGS_MAX_TRACKS) {
        tracks[w].slot = slot;
        strncpy(tracks[w].file, rel_file, sizeof(tracks[w].file) - 1);
        tracks[w].file[sizeof(tracks[w].file) - 1] = '\0';
        strncpy(tracks[w].label, name, sizeof(tracks[w].label) - 1);
        tracks[w].label[sizeof(tracks[w].label) - 1] = '\0';
        tracks[w].enabled = true;
        w++;
        replaced = true;
    }
    if (!replaced) {
        ESP_LOGE(TAG, "UP: no free track slot for slot=%u", (unsigned)slot);
        (void)remove(path);
        return send_json(req, "{\"ok\":false,\"error\":\"track slots full\"}");
    }
    count = w;

    esp_err_t save_err = settings_tracks_save(tracks, count);
    ESP_LOGI(TAG, "UP: track bound slot=%u count=%u save=%s",
             (unsigned)slot, (unsigned)count, esp_err_to_name(save_err));
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "UP: tracks save failed, keeping old track");
        (void)remove(path);
        return send_json(req, "{\"ok\":false,\"error\":\"tracks save failed\"}");
    }

    /* Now drop the previous track file (deferred to keep the upload fast). */
    if (old_path[0] != '\0') {
        (void)remove(old_path);
        ESP_LOGI(TAG, "UP: removed old track %s", old_path);
    }

    char esc_file[320];
    json_escape(rel_file, esc_file, sizeof(esc_file));
    char esc_label[128];
    json_escape(name, esc_label, sizeof(esc_label));
    char json[768];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"slot\":%u,\"file\":\"%s\",\"label\":\"%s\",\"enabled\":true,\"bytes\":%d}",
             (unsigned)slot, esc_file, esc_label, req->content_len);
    return send_json(req, json);
}

static esp_err_t audio_track_delete_post(httpd_req_t *req)
{
    char query[32] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0;
    parse_u8(query, "slot", &slot);

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    char del_file[SETTINGS_TRACK_FILE_MAX] = { 0 };
    if (settings_tracks_load(tracks, &count) == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (tracks[i].slot == slot && tracks[i].file[0] != '\0' && del_file[0] == '\0') {
                strncpy(del_file, tracks[i].file, sizeof(del_file) - 1);
                del_file[sizeof(del_file) - 1] = '\0';
            }
        }
        size_t w = 0;
        for (size_t i = 0; i < count; ++i) {
            if (tracks[i].slot == slot) {
                continue;
            }
            if (w != i) {
                tracks[w] = tracks[i];
            }
            w++;
        }
        count = w;
    }

    esp_err_t save_err = settings_tracks_save(tracks, count);
    if (save_err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"tracks save failed\"}");
    }

    /* Delete the bound file only if no remaining track still references it. */
    if (del_file[0] != '\0') {
        bool in_use = false;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(tracks[i].file, del_file) == 0) {
                in_use = true;
                break;
            }
        }
        if (!in_use) {
            char path[160];
            snprintf(path, sizeof(path), WEB_USERDATA_DIR "/%s", del_file);
            if (remove(path) == 0) {
                ESP_LOGI(TAG, "UP: removed track file %s", path);
            }
        }
    }

    if (s_cfg.active_slot == slot) {
        s_cfg.active_slot = 0;
        (void)settings_save(&s_cfg);
        (void)audio_stop();
    }

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"slot\":%u}", (unsigned)slot);
    return send_json(req, json);
}

static esp_err_t track_category_post(httpd_req_t *req)
{
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0, cat = 0;
    if (!parse_u8(query, "slot", &slot) || slot < 1 || slot > SETTINGS_MAX_TRACKS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot out of range");
    }
    if (!parse_u8(query, "cat", &cat) || cat > SETTINGS_TRACK_CAT_EFFECTS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cat out of range");
    }
    s_track_cat[slot - 1U] = cat;
    (void)settings_track_cats_save(s_track_cat, SETTINGS_MAX_TRACKS);
    web_log_event("Звук", "слот %u: %s", (unsigned)slot,
                  cat == SETTINGS_TRACK_CAT_ENGINE ? "двигатель" : "эффект");

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"slot\":%u,\"cat\":%u}",
             (unsigned)slot, (unsigned)cat);
    return send_json(req, json);
}

static esp_err_t cv_read_get(httpd_req_t *req)
{
    char query[32] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint16_t idx = 0;
    if (!parse_u16(query, "index", &idx) || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }
    uint8_t value = 0;
    (void)settings_cv_read(idx, &value);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"index\":%u,\"value\":%u}",
             (unsigned)idx, (unsigned)value);
    return send_json(req, json);
}

static esp_err_t aux_cfg_get(httpd_req_t *req)
{
    char json[512];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"aux\":[");
    for (uint8_t i = 0; i < SETTINGS_AUX_COUNT; ++i) {
        buf_appendf(json, sizeof(json), &used, "%s{\"level\":%u,\"effect\":%u}",
                    i == 0U ? "" : ",", (unsigned)s_aux_cfg[i].level,
                    (unsigned)s_aux_cfg[i].effect);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

static esp_err_t aux_cfg_post(httpd_req_t *req)
{
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t ch = 0, level = 0, fx = 0;
    if (!parse_u8(query, "ch", &ch) || ch >= SETTINGS_AUX_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ch");
    }
    if (!parse_u8(query, "level", &level) || level > 100U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad level");
    }
    if (!parse_u8(query, "effect", &fx) || (uint32_t)fx >= AUXIO_EFFECT_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad effect");
    }
    s_aux_cfg[ch].level = level;
    s_aux_cfg[ch].effect = fx;
    uint8_t pwm = (uint8_t)((uint16_t)level * 255U / 100U);
    (void)auxio_config(ch, pwm, 0, (auxio_effect_t)fx, aux_effect_period(fx));
    if (settings_aux_cfg_save(s_aux_cfg, SETTINGS_AUX_COUNT) != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    web_log_event("AUX", "%s: уровень %u%%, эффект %u",
                  WEB_OUT_NAMES[ch], (unsigned)level, (unsigned)fx);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"ch\":%u}", (unsigned)ch);
    return send_json(req, json);
}

/* Persist a candidate binding list and adopt it under the apply lock. */
static bool web_func_bind_commit(func_binding_t *tmp, size_t n)
{
    if (settings_func_bind_save(tmp, n) != ESP_OK) {
        return false;
    }
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    memcpy(s_func_bind, tmp, sizeof(s_func_bind));
    s_func_bind_count = n;
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
    /* Push the new list to the engine so it takes effect without a reboot. */
    sound_reload_bindings();
    return true;
}

static bool web_func_bind_add(const func_binding_t *b)
{
    func_binding_t tmp[FUNC_BIND_MAX];
    memcpy(tmp, s_func_bind, sizeof(tmp));
    size_t n = s_func_bind_count;
    if (settings_func_bind_add(tmp, &n, b) != ESP_OK) {
        return false;
    }
    return web_func_bind_commit(tmp, n);
}

static bool web_func_bind_remove(size_t idx)
{
    func_binding_t tmp[FUNC_BIND_MAX];
    memcpy(tmp, s_func_bind, sizeof(tmp));
    size_t n = s_func_bind_count;
    if (settings_func_bind_remove(tmp, &n, idx) != ESP_OK) {
        return false;
    }
    return web_func_bind_commit(tmp, n);
}

/* 64 bindings x ~124 bytes worst case = ~7.9 KB, plus frame overhead. */
#define WEB_FUNC_BIND_JSON_MAX 8192

/* GET /api/func-map?view=bind[|matrix] -> the canonical binding list. */
static esp_err_t func_bind_get(httpd_req_t *req)
{
    char *json = malloc(WEB_FUNC_BIND_JSON_MAX);
    if (json == NULL) {
        return send_json(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    size_t used = 0;
    buf_appendf(json, WEB_FUNC_BIND_JSON_MAX, &used, "{\"ok\":true,\"count\":%u,\"binds\":[",
                (unsigned)s_func_bind_count);
    for (size_t i = 0; i < s_func_bind_count; ++i) {
        const func_binding_t *b = &s_func_bind[i];
        buf_appendf(json, WEB_FUNC_BIND_JSON_MAX, &used,
                    "%s{\"fn\":%u,\"type\":%u,\"id\":%u,\"dir\":%u,\"state\":%u,"
                    "\"mode\":%u,\"flags\":%u,\"short\":%u,\"short_ms\":%u,"
                    "\"min_ms\":%u,\"fade_ms\":%u}",
                    i == 0U ? "" : ",", (unsigned)b->fn, (unsigned)b->target_type,
                    (unsigned)b->target_id, (unsigned)b->dir, (unsigned)b->state,
                    (unsigned)b->mode, (unsigned)b->flags, (unsigned)b->short_table,
                    (unsigned)b->short_ms, (unsigned)b->min_ms, (unsigned)b->fade_ms);
    }
    buf_appendf(json, WEB_FUNC_BIND_JSON_MAX, &used, "]}");
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

/* POST /api/func-map?bind=1 -> add or (bind=1&remove=1&idx=N) remove a binding. */
static esp_err_t func_map_post_binding(httpd_req_t *req, const char *query)
{
    if (parse_bool(query, "remove", false)) {
        uint8_t idx = 0;
        if (!parse_u8(query, "idx", &idx) || (size_t)idx >= s_func_bind_count) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
        }
        if (!web_func_bind_remove(idx)) {
            return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
        }
        return send_json(req, "{\"ok\":true}");
    }

    uint8_t fn = 0, type = 0, id = 0, dir = 0, state = 0, mode = 0, flags = 0, short_table = 0;
    uint16_t short_ms = 0, min_ms = 0, fade_ms = 0;
    if (!parse_u8(query, "fn", &fn) || fn >= WEB_FN_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fn");
    }
    if (!parse_u8(query, "type", &type) || type < FUNC_TARGET_OUTPUT || type > FUNC_TARGET_LOGIC) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad type");
    }
    if (!parse_u8(query, "id", &id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
    }
    (void)parse_u8(query, "dir", &dir);
    (void)parse_u8(query, "state", &state);
    (void)parse_u8(query, "mode", &mode);
    (void)parse_u8(query, "flags", &flags);
    (void)parse_u8(query, "short", &short_table);
    (void)parse_u16(query, "short_ms", &short_ms);
    (void)parse_u16(query, "min_ms", &min_ms);
    (void)parse_u16(query, "fade_ms", &fade_ms);

    func_binding_t b;
    memset(&b, 0, sizeof(b));
    b.used = 1;
    b.fn = fn;
    b.target_type = type;
    b.target_id = id;
    b.dir = dir;
    b.state = state;
    b.mode = mode;
    b.flags = flags;
    b.short_table = short_table;
    b.short_ms = short_ms;
    b.min_ms = min_ms;
    b.fade_ms = fade_ms;
    if (!web_func_bind_add(&b)) {
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"count\":%u}", (unsigned)s_func_bind_count);
    return send_json(req, json);
}

static esp_err_t func_map_get(httpd_req_t *req)
{
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char view[16] = { 0 };
    if (parse_query(query, "view", view, sizeof(view)) &&
        (strcmp(view, "bind") == 0 || strcmp(view, "matrix") == 0)) {
        return func_bind_get(req);
    }

    char json[1024];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"map\":[");
    for (uint8_t f = 0; f <= 10U; ++f) {
        uint8_t sa = 0, sb = 0, dir = 0, spd = 0;
        uint16_t aux = 0;
        (void)web_func_map_get(f, &sa, &sb, &aux, &dir, &spd);
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"a\":%u,\"b\":%u,\"aux\":%u,\"dir\":%u,\"speed\":%u}",
                    f == 0U ? "" : ",", (unsigned)sa, (unsigned)sb,
                    (unsigned)aux, (unsigned)dir, (unsigned)spd);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

static esp_err_t func_map_post(httpd_req_t *req)
{
    char query[128] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (parse_bool(query, "bind", false)) {
        return func_map_post_binding(req, query);
    }
    uint8_t fn = 0, sa = 0, sb = 0, dir = 0, spd = 0;
    uint16_t aux = 0;
    if (!parse_u8(query, "fn", &fn) || fn > 10U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fn");
    }
    if (!parse_u8(query, "a", &sa) || sa > SETTINGS_MAX_TRACKS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad a");
    }
    if (!parse_u8(query, "b", &sb) || sb > SETTINGS_MAX_TRACKS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad b");
    }
    if (!parse_u16(query, "aux", &aux) || (aux & (uint16_t)~SETTINGS_FUNC_OUT_ALL) != 0U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad aux");
    }
    if (!parse_u8(query, "dir", &dir) || dir > SETTINGS_FUNC_DIR_REV) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad dir");
    }
    if (!parse_u8(query, "speed", &spd) || spd > SETTINGS_FUNC_SPD_STOP) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad speed");
    }
    if (!web_func_map_set(fn, sa, sb, aux, dir, spd)) {
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"fn\":%u}", (unsigned)fn);
    return send_json(req, json);
}

/* ---------- sound scheme REST (R5.1) ---------- */

static esp_err_t sound_state_handler(httpd_req_t *req)
{
    sound_status_t st;
    sound_status_get(&st);
    char esc[SOUND_FILE_MAX * 2];
    json_escape(st.name, esc, sizeof(esc));
    char json[320];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"type\":%u,\"name\":\"%s\",\"enabled\":%s,\"engine\":%s,"
             "\"table\":%u,\"phase\":%u,\"speed\":%u,\"forward\":%s}",
             (unsigned)st.type, esc, st.enabled ? "true" : "false",
             st.engine ? "true" : "false", (unsigned)st.table, (unsigned)st.phase,
             (unsigned)st.speed, st.forward ? "true" : "false");
    return send_json(req, json);
}

/* Full scheme view: 31 tables x (metadata + 3 file names) + arrays + extras. */
#define WEB_SCHEME_JSON_MAX 16384

static esp_err_t sound_scheme_handler(httpd_req_t *req)
{
    char *json = malloc(WEB_SCHEME_JSON_MAX);
    if (json == NULL) {
        return send_json(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    /* Serialize via the fine-grained accessors: copying the whole ~26 KB scheme
     * per request would churn the internal heap for a read-only view. */
    sound_engine_t eng;
    (void)sound_engine_get(&eng);
    uint8_t type = sound_type_get();
    sound_status_t st;
    sound_status_get(&st);
    /* CV30 is the effective skip-flag source the engine reads, so report it. */
    uint8_t flags = eng.flags;
    (void)settings_cv_read(30, &flags);
    sound_brake_t brake;
    (void)sound_brake_get(&brake);
    char esc[SOUND_FILE_MAX * 2];
    json_escape(st.name, esc, sizeof(esc));
    size_t used = 0;
    buf_appendf(json, WEB_SCHEME_JSON_MAX, &used,
                "{\"ok\":true,\"type\":%u,\"name\":\"%s\",\"start_fn\":%u,\"sync\":%u,"
                "\"flags\":%u,\"start\":%u,\"stop\":%u,\"shutdown\":%u,\"cyl_min\":%u,"
                "\"cyl_max\":%u,\"cyl_inc\":%u,"
                "\"drive\":[%u,%u,%u,%u,%u],\"accel\":[%u,%u,%u,%u,%u],"
                "\"coast\":[%u,%u,%u,%u,%u],\"brake\":{\"max\":%u,\"min\":%u},\"tables\":[",
                (unsigned)type, esc, (unsigned)eng.engine_start_fn,
                eng.sync_motion ? 1U : 0U, (unsigned)flags,
                (unsigned)eng.start_table, (unsigned)eng.stop_table,
                (unsigned)eng.shutdown_table, (unsigned)eng.cyl_shift_min,
                (unsigned)eng.cyl_shift_max, (unsigned)eng.cyl_shift_inc,
                (unsigned)eng.drive[0], (unsigned)eng.drive[1], (unsigned)eng.drive[2],
                (unsigned)eng.drive[3], (unsigned)eng.drive[4],
                (unsigned)eng.accel[0], (unsigned)eng.accel[1], (unsigned)eng.accel[2],
                (unsigned)eng.accel[3], (unsigned)eng.accel[4],
                (unsigned)eng.coast[0], (unsigned)eng.coast[1], (unsigned)eng.coast[2],
                (unsigned)eng.coast[3], (unsigned)eng.coast[4],
                (unsigned)brake.max_on_speed, (unsigned)brake.min_brake_speed);
    for (uint8_t i = 1U; i < SOUND_MAX_TABLES; ++i) {
        sound_table_t t;
        (void)sound_table_get(i, &t);
        json_escape(t.name, esc, sizeof(esc));
        char e_init[SOUND_FILE_MAX * 2];
        char e_loop[SOUND_FILE_MAX * 2];
        char e_end[SOUND_FILE_MAX * 2];
        json_escape(t.init[0].file, e_init, sizeof(e_init));
        json_escape(t.loop[0].file, e_loop, sizeof(e_loop));
        json_escape(t.end[0].file, e_end, sizeof(e_end));
        buf_appendf(json, WEB_SCHEME_JSON_MAX, &used,
                    "%s{\"i\":%u,\"used\":%u,\"name\":\"%s\",\"min\":%u,\"max\":%u,"
                    "\"rate\":%u,\"min_plays\":%u,\"max_plays\":%u,\"end\":%u,"
                    "\"nacc\":%u,\"ndec\":%u,\"init\":\"%s\",\"loop\":\"%s\",\"endf\":\"%s\"}",
                    i == 1U ? "" : ",", (unsigned)i, t.used ? 1U : 0U, esc,
                    (unsigned)t.min_speed, (unsigned)t.max_speed, (unsigned)t.rate_scale,
                    (unsigned)t.min_plays, (unsigned)t.max_plays, (unsigned)t.end_table,
                    (unsigned)t.next_accel, (unsigned)t.next_decel,
                    e_init, e_loop, e_end);
    }
    buf_appendf(json, WEB_SCHEME_JSON_MAX, &used, "],\"extras\":[");
    for (uint8_t j = 0; j < SOUND_MAX_EXTRAS; ++j) {
        sound_extra_t e;
        (void)sound_extra_get(j, &e);
        buf_appendf(json, WEB_SCHEME_JSON_MAX, &used,
                    "%s{\"i\":%u,\"table\":%u,\"fn\":%u,\"dir\":%u,\"state\":%u,\"mode\":%u,"
                    "\"vol\":%u,\"rmin\":%u,\"rmax\":%u}",
                    j == 0U ? "" : ",", (unsigned)j, (unsigned)e.table, (unsigned)e.fn,
                    (unsigned)e.dir, (unsigned)e.state, (unsigned)e.mode,
                    (unsigned)e.volume, (unsigned)e.random_min_ms, (unsigned)e.random_max_ms);
    }
    buf_appendf(json, WEB_SCHEME_JSON_MAX, &used, "]}");
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

static esp_err_t sound_scheme_post_handler(httpd_req_t *req)
{
    char query[512] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    sound_engine_t eng;
    (void)sound_engine_get(&eng);
    uint8_t type = sound_type_get();
    uint8_t v = 0;
    if (parse_u8(query, "type", &v)) {
        if (v > SOUND_SCHEME_ELECTRIC) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad type");
        }
        type = v;
    }
    if (parse_u8(query, "start_fn", &v)) { eng.engine_start_fn = v; }
    if (parse_u8(query, "sync", &v)) { eng.sync_motion = (v != 0U); }
    if (parse_u8(query, "flags", &v)) {
        eng.flags = v;
        /* CV30 is what the engine actually reads: keep the two in sync. */
        (void)settings_cv_write(30, v);
        settings_cv_commit_deferred();
    }
    if (parse_u8(query, "start", &v)) { eng.start_table = v; }
    if (parse_u8(query, "stop", &v)) { eng.stop_table = v; }
    if (parse_u8(query, "shutdown", &v)) { eng.shutdown_table = v; }
    if (parse_u8(query, "cyl_min", &v)) { eng.cyl_shift_min = v; }
    if (parse_u8(query, "cyl_max", &v)) { eng.cyl_shift_max = v; }
    if (parse_u8(query, "cyl_inc", &v)) { eng.cyl_shift_inc = v; }
    for (uint8_t i = 0; i < SOUND_ENGINE_STEPS; ++i) {
        char key[10];
        snprintf(key, sizeof(key), "drive%u", (unsigned)i);
        if (parse_u8(query, key, &v)) { eng.drive[i] = v; }
        snprintf(key, sizeof(key), "accel%u", (unsigned)i);
        if (parse_u8(query, key, &v)) { eng.accel[i] = v; }
        snprintf(key, sizeof(key), "coast%u", (unsigned)i);
        if (parse_u8(query, key, &v)) { eng.coast[i] = v; }
    }
    sound_brake_t brake;
    (void)sound_brake_get(&brake);
    bool brake_changed = false;
    if (parse_u8(query, "brake_max", &v)) { brake.max_on_speed = v; brake_changed = true; }
    if (parse_u8(query, "brake_min", &v)) { brake.min_brake_speed = v; brake_changed = true; }
    if (brake_changed) {
        (void)sound_brake_set(&brake);
    }
    (void)sound_type_set(type);
    (void)sound_engine_set(&eng);
    if (sound_scheme_save() != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"save\"}");
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t sound_table_post(httpd_req_t *req)
{
    char query[256] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t idx = 0;
    if (!parse_u8(query, "i", &idx) || idx == SOUND_TABLE_NONE || idx >= SOUND_MAX_TABLES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad i");
    }
    sound_table_t tbl;
    (void)sound_table_get(idx, &tbl);
    sound_table_t *t = &tbl;
    uint8_t v = 0;
    if (parse_u8(query, "used", &v)) { t->used = (v != 0U); }
    if (parse_u8(query, "min", &v)) { t->min_speed = v; }
    if (parse_u8(query, "max", &v)) { t->max_speed = v; }
    if (parse_u8(query, "rate", &v)) { t->rate_scale = v; }
    if (parse_u8(query, "min_plays", &v)) { t->min_plays = v; }
    if (parse_u8(query, "max_plays", &v)) { t->max_plays = v; }
    if (parse_u8(query, "end", &v)) { t->end_table = v; }
    if (parse_u8(query, "nacc", &v)) { t->next_accel = v; }
    if (parse_u8(query, "ndec", &v)) { t->next_decel = v; }
    char s[SOUND_FILE_MAX] = { 0 };
    if (parse_query(query, "name", s, sizeof(s))) {
        snprintf(t->name, sizeof(t->name), "%.*s", (int)(sizeof(t->name) - 1U), s);
    }
    if (parse_query(query, "init", s, sizeof(s))) { snprintf(t->init[0].file, SOUND_FILE_MAX, "%s", s); }
    if (parse_query(query, "loop", s, sizeof(s))) { snprintf(t->loop[0].file, SOUND_FILE_MAX, "%s", s); }
    if (parse_query(query, "endf", s, sizeof(s))) { snprintf(t->end[0].file, SOUND_FILE_MAX, "%s", s); }
    (void)sound_table_set(idx, &tbl);
    if (sound_scheme_save() != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"save\"}");
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"i\":%u}", (unsigned)idx);
    return send_json(req, json);
}

static esp_err_t sound_extra_post(httpd_req_t *req)
{
    char query[192] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t idx = 0;
    if (!parse_u8(query, "i", &idx) || idx >= SOUND_MAX_EXTRAS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad i");
    }
    sound_extra_t ex;
    (void)sound_extra_get(idx, &ex);
    sound_extra_t *e = &ex;
    uint8_t v = 0;
    uint16_t u = 0;
    if (parse_u8(query, "table", &v)) { e->table = v; }
    if (parse_u8(query, "fn", &v)) { e->fn = v; }
    if (parse_u8(query, "dir", &v)) { e->dir = v; }
    if (parse_u8(query, "state", &v)) { e->state = v; }
    if (parse_u8(query, "mode", &v)) { e->mode = v; }
    if (parse_u8(query, "vol", &v)) { e->volume = v; }
    if (parse_u16(query, "rmin", &u)) { e->random_min_ms = u; }
    if (parse_u16(query, "rmax", &u)) { e->random_max_ms = u; }
    (void)sound_extra_set(idx, &ex);
    if (sound_scheme_save() != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"save\"}");
    }
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"i\":%u}", (unsigned)idx);
    return send_json(req, json);
}

static esp_err_t sound_lint_handler(httpd_req_t *req)
{
    char rep[160];
    int n = sound_lint(rep, sizeof(rep));
    char esc[320];
    json_escape(rep, esc, sizeof(esc));
    char json[400];
    snprintf(json, sizeof(json), "{\"ok\":true,\"problems\":%d,\"report\":\"%s\"}", n, esc);
    return send_json(req, json);
}

/* ---------- sound scheme project files (create/select/delete/export/import) */

/* GET /api/sound/projects -> the stored .mds files and which one is active. */
static esp_err_t sound_projects_get(httpd_req_t *req)
{
    char (*names)[SOUND_FILE_MAX] = malloc(sizeof(char[WEB_PROJECTS_MAX][SOUND_FILE_MAX]));
    char *json = malloc(WEB_PROJECTS_JSON_MAX);
    if (names == NULL || json == NULL) {
        free(names);
        free(json);
        return send_json(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    size_t count = 0;
    (void)sound_scheme_list(names, WEB_PROJECTS_MAX, &count);
    char active[SOUND_FILE_MAX] = { 0 };
    (void)sound_active_name_get(active, sizeof(active));
    char esc[SOUND_FILE_MAX * 2];
    json_escape(active, esc, sizeof(esc));
    size_t used = 0;
    buf_appendf(json, WEB_PROJECTS_JSON_MAX, &used,
                "{\"ok\":true,\"active\":\"%s\",\"count\":%u,\"projects\":[",
                esc, (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        json_escape(names[i], esc, sizeof(esc));
        buf_appendf(json, WEB_PROJECTS_JSON_MAX, &used, "%s{\"name\":\"%s\",\"active\":%s}",
                    i == 0U ? "" : ",", esc,
                    strcmp(names[i], active) == 0 ? "true" : "false");
    }
    buf_appendf(json, WEB_PROJECTS_JSON_MAX, &used, "]}");
    esp_err_t err = send_json(req, json);
    free(names);
    free(json);
    return err;
}

/* POST /api/sound/project — create=<name>[&type=N] | activate=<name> | delete=<name> */
static esp_err_t sound_project_post(httpd_req_t *req)
{
    char query[256] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char name[SOUND_FILE_MAX] = { 0 };
    esp_err_t err;
    if (parse_query(query, "create", name, sizeof(name))) {
        uint8_t type = SOUND_SCHEME_DIESEL;
        (void)parse_u8(query, "type", &type);
        err = sound_scheme_create(name, type);
    } else if (parse_query(query, "activate", name, sizeof(name))) {
        err = sound_load_scheme(name);
    } else if (parse_query(query, "delete", name, sizeof(name))) {
        err = sound_scheme_delete(name);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad action");
    }
    if (err != ESP_OK) {
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return send_json(req, json);
    }
    web_log_event("Звук", "схема %s", name);
    return send_json(req, "{\"ok\":true}");
}

/* GET /api/sound/download?name=N — raw .mds file as an attachment. */
static esp_err_t sound_download_get(httpd_req_t *req)
{
    char query[128] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char name[SOUND_FILE_MAX] = { 0 };
    if (!parse_query(query, "name", name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    uint8_t *buf = malloc(SOUND_STORE_MAX_BYTES);
    if (buf == NULL) {
        return send_json(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    size_t len = 0;
    esp_err_t err = sound_scheme_export(name, buf, SOUND_STORE_MAX_BYTES, &len);
    if (err != ESP_OK) {
        free(buf);
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return send_json(req, json);
    }
    char disp[SOUND_FILE_MAX + 32];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s.mds\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    esp_err_t rerr = httpd_resp_send(req, (const char *)buf, len);
    free(buf);
    return rerr;
}

/* POST /api/sound/upload?name=N[&activate=0] — import a raw .mds body. */
static esp_err_t sound_upload_post(httpd_req_t *req)
{
    if (!storage_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    }
    if (req->content_len != (int)SOUND_STORE_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }
    char query[192] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char name[SOUND_FILE_MAX] = { 0 };
    if (!parse_query(query, "name", name, sizeof(name))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    uint8_t *buf = malloc(SOUND_STORE_MAX_BYTES);
    if (buf == NULL) {
        return send_json(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, (char *)buf + got, req->content_len - got);
        if (r <= 0) {
            break;
        }
        got += r;
    }
    esp_err_t err = (got == req->content_len) ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        bool activate = parse_bool(query, "activate", true);
        err = sound_scheme_import(name, buf, (size_t)got, activate);
    }
    free(buf);
    if (err != ESP_OK) {
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
        return send_json(req, json);
    }
    web_log_event("Звук", "схема загружена %s", name);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t cv_all_get(httpd_req_t *req)
{
    char json[3072];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"count\":%u,\"values\":[",
                (unsigned)SETTINGS_CV_COUNT);
    for (uint16_t i = 1; i <= SETTINGS_CV_COUNT; ++i) {
        uint8_t v = 0;
        (void)settings_cv_read(i, &v);
        buf_appendf(json, sizeof(json), &used, "%s%u", i == 1 ? "" : ",", (unsigned)v);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

static esp_err_t cv_write_post(httpd_req_t *req)
{
    char query[64] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint16_t idx = 0;
    uint8_t value = 0;
    if (!parse_u16(query, "index", &idx) || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }
    if (!parse_u8(query, "value", &value)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad value");
    }
    esp_err_t werr = settings_cv_write(idx, value);
    if (werr != ESP_OK) {
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}",
                 esp_err_to_name(werr));
        return send_json(req, json);
    }
    if (parse_bool(query, "commit", false)) {
        (void)settings_cv_commit();
    }
    /* CV63 aliases the master volume: apply it live and keep the cached config
     * in sync so a later settings_save cannot revert it. */
    if (idx == 63U) {
        web_master_volume_changed((uint8_t)((uint16_t)value * 100U / 255U));
    }
    /* Address / speed-step / consist CVs must take effect immediately (the
     * runtime config is cached inside the DCC decoder, not read per packet).
     * CV8 = 8 is a factory reset of all CVs, so reload those as well. */
    if (idx == 1U || idx == 8U || idx == 17U || idx == 18U || idx == 19U || idx == 29U) {
        dcc_reload_config();
    }
    web_log_event("CV", "CV %u = %u", (unsigned)idx, (unsigned)value);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"index\":%u,\"value\":%u}",
             (unsigned)idx, (unsigned)value);
    return send_json(req, json);
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    char ap_ssid[SETTINGS_SSID_MAX * 2];
    char sta_ssid[SETTINGS_SSID_MAX * 2];
    json_escape(s_cfg.ap_ssid, ap_ssid, sizeof(ap_ssid));
    json_escape(s_cfg.sta_ssid, sta_ssid, sizeof(sta_ssid));

    char json[512];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"mode\":%u,\"ap_ssid\":\"%s\",\"ap_password\":\"%s\","
             "\"sta_ssid\":\"%s\",\"sta_password\":\"%s\",\"port\":%u,"
             "\"auto_off_min\":%u,\"hold\":%u,\"status\":\"%s\",\"ap_ip\":\"%s\",\"sta_ip\":\"%s\"}",
             (unsigned)s_cfg.wifi_mode, ap_ssid,
             s_cfg.ap_password[0] ? "********" : "",
             sta_ssid, s_cfg.sta_password[0] ? "********" : "",
             (unsigned)s_cfg.port,
             (unsigned)s_cfg.auto_off_min, (unsigned)s_cfg.hold,
             s_wifi_started ? "ap_running" : "off", s_cfg.ap_ip, s_sta_ip);
    return send_json(req, json);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char query[1400] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));

    /* Parse into a local copy: on a validation error nothing may be applied,
     * otherwise a later deferred save would persist the partial change. */
    settings_config_t next = s_cfg;
    next.wifi_mode = 1; /* access point only */
    char buf[SETTINGS_PASS_MAX] = { 0 };
    if (parse_query(query, "ap_ssid", buf, sizeof(buf)) && buf[0]) {
        strncpy(next.ap_ssid, buf, sizeof(next.ap_ssid) - 1);
        next.ap_ssid[sizeof(next.ap_ssid) - 1] = '\0';
    }
    memset(buf, 0, sizeof(buf));
    if (parse_query(query, "ap_password", buf, sizeof(buf)) && buf[0]) {
        if (strlen(buf) < 8U) {
            return send_json(req, "{\"ok\":false,\"error\":\"ap password must be >=8 chars\"}");
        }
        strncpy(next.ap_password, buf, sizeof(next.ap_password) - 1);
        next.ap_password[sizeof(next.ap_password) - 1] = '\0';
    }
    if (parse_bool(query, "ap_password_clear", false)) {
        next.ap_password[0] = '\0';
    }
    /* Configurable access point address (default 192.168.100.1). */
    memset(buf, 0, sizeof(buf));
    if (parse_query(query, "ap_ip", buf, sizeof(buf)) && buf[0]) {
        esp_ip4_addr_t parsed = { 0 };
        if (!parse_ip4(buf, &parsed)) {
            return send_json(req, "{\"ok\":false,\"error\":\"invalid ap ip\"}");
        }
        strncpy(next.ap_ip, buf, sizeof(next.ap_ip) - 1);
        next.ap_ip[sizeof(next.ap_ip) - 1] = '\0';
    }
    /* HTTP port is fixed at 80; STA fields are gone (AP is the only mode). */
    next.port = 80;
    parse_u8(query, "auto_off_min", &next.auto_off_min);
    /* Captive-portal redirect is always on so the phone opens the page. */
    next.hold = 0;

    s_cfg = next;
    (void)settings_save(&s_cfg);
    web_log_event("Wi-Fi", "настройки сохранены (IP %s), перезагрузка", s_cfg.ap_ip);
    send_json(req, "{\"ok\":true,\"reboot\":1}");
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t wifi_reset_post(httpd_req_t *req)
{
    settings_config_t def = { 0 };
    def.wifi_mode = 1;
    def.port = 80;
    def.hold = 0;
    strncpy(def.ap_ip, AP_IP_DEFAULT, sizeof(def.ap_ip) - 1);
    strncpy(def.ap_ssid, "ADDITIPUS AURA-X", sizeof(def.ap_ssid) - 1);
    def.master_volume = 20;
    def.engine_volume = 20;
    def.effects_volume = 20;
    (void)settings_save(&def);
    web_log_event("Wi-Fi", "сброс настроек, перезагрузка");
    send_json(req, "{\"ok\":true,\"reboot\":1}");
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* Full factory reset: wipe every stored setting (CVs, WiFi config, volumes,
 * name, control source, sound track slots, BEMF calibration) and reboot into
 * the factory state. Sound files on the storage are kept. */
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    web_log_event("Сброс", "заводские настройки, перезагрузка");
    (void)settings_factory_reset();
    send_json(req, "{\"ok\":true,\"reboot\":1}");
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t device_get(httpd_req_t *req)
{
    char esc[SETTINGS_NAME_MAX * 2];
    json_escape(s_cfg.device_name, esc, sizeof(esc));
    /* 32-bit seconds: fits for >100 years. Do NOT use %lld here — newlib nano
     * (CONFIG_NEWLIB_NANO_FORMAT) has no 64-bit printf conversion. */
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    char json[192];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"name\":\"%s\",\"version\":\"" WEB_FW_VERSION "\",\"uptime\":%lu}",
             esc, (unsigned long)uptime_s);
    return send_json(req, json);
}

static esp_err_t device_post(httpd_req_t *req)
{
    char query[128] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[SETTINGS_NAME_MAX] = { 0 };
    if (parse_query(query, "name", buf, sizeof(buf)) && buf[0]) {
        strncpy(s_cfg.device_name, buf, sizeof(s_cfg.device_name) - 1);
        (void)settings_save(&s_cfg);
        web_log_event("Имя", "%s", s_cfg.device_name);
    }
    char esc[SETTINGS_NAME_MAX * 2];
    json_escape(s_cfg.device_name, esc, sizeof(esc));
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"name\":\"%s\"}", esc);
    return send_json(req, json);
}

static esp_err_t storage_get(httpd_req_t *req)
{
    int64_t t0 = esp_timer_get_time();
    uint64_t free_bytes = 0;
    if (storage_get_free_bytes(&free_bytes) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    }
    int64_t t1 = esp_timer_get_time();
    if (t1 - t0 > 10000) {
        ESP_LOGW(TAG, "storage_get slow: took=%lu us", (unsigned long)(t1 - t0));
    }
    char json[96];
    uint32_t hi = (uint32_t)(free_bytes >> 32);
    uint32_t lo = (uint32_t)(free_bytes & 0xFFFFFFFFu);
    if (hi != 0) {
        snprintf(json, sizeof(json), "{\"ok\":true,\"free\":%lu%09lu}",
                 (unsigned long)hi, (unsigned long)lo);
    } else {
        snprintf(json, sizeof(json), "{\"ok\":true,\"free\":%lu}", (unsigned long)lo);
    }
    return send_json(req, json);
}

/* Buffered reader over the HTTP request body: the OTA handler parses a
 * container whose sections can straddle the receive buffer boundaries. */
typedef struct {
    httpd_req_t *req;
    uint8_t buf[4096];
    size_t len;
    size_t pos;
    int remaining;
    int idle;
} ota_stream_t;

static int ota_stream_read(ota_stream_t *s, uint8_t *dst, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (s->pos < s->len) {
            size_t avail = s->len - s->pos;
            size_t take = (avail < n - got) ? avail : n - got;
            memcpy(dst + got, s->buf + s->pos, take);
            s->pos += take;
            got += take;
            continue;
        }
        if (s->remaining <= 0) {
            break;
        }
        int want = s->remaining > (int)sizeof(s->buf) ? (int)sizeof(s->buf) : s->remaining;
        int r = httpd_req_recv(s->req, (char *)s->buf, want);
        if (r > 0) {
            s->len = (size_t)r;
            s->pos = 0;
            s->remaining -= r;
            s->idle = 0;
            continue;
        }
        if ((r == 0 || r == HTTPD_SOCK_ERR_TIMEOUT) && ++s->idle < 4) {
            continue;
        }
        return -1;
    }
    return (int)got;
}

/* Basename, keep [A-Za-z0-9._-] and UTF-8 bytes, force a .wav extension. */
static void ota_safe_name(const char *in, char *out, size_t out_len)
{
    if (out_len < 6U) {
        if (out_len != 0U) {
            out[0] = '\0';
        }
        return;
    }
    const char *base = in;
    for (const char *p = in; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t w = 0;
    for (const char *p = base; *p != '\0' && w + 5U < out_len; ++p) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[w++] = c;
        } else if ((unsigned char)c >= 0x80U) {
            out[w++] = c; /* keep UTF-8 so Cyrillic names survive */
        } else {
            out[w++] = '_';
        }
    }
    /* Terminate before the extension check: the comparison must not read
     * whatever stale bytes follow the copied name. */
    out[w] = '\0';
    if (w < 4U || strcasecmp(out + (w - 4U), ".wav") != 0) {
        if (w + 4U < out_len) {
            out[w++] = '.';
            out[w++] = 'w';
            out[w++] = 'a';
            out[w++] = 'v';
        }
    }
    out[w] = '\0';
}

static int ota_slot_from_name(const char *name)
{
    if (strncmp(name, "slot", 4) != 0) {
        return 0;
    }
    char *end = NULL;
    long s = strtol(name + 4, &end, 10);
    if (end == name + 4 || *end != '.' || s < 1 || s > SETTINGS_MAX_TRACKS) {
        return 0;
    }
    return (int)s;
}

/* POST /api/ota/update
 * - plain ESP image          -> written to the next OTA partition;
 * - "AURAOTA2" container      -> firmware to the OTA partition and each sound
 *                               file to /userdata, then reboot. Lets one
 *                               upload carry both a firmware and its sounds. */
static esp_err_t ota_update_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > (8 * 1024 * 1024)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ESP_LOGE(TAG, "OTA: no update partition");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
    }

    ota_stream_t *st = calloc(1, sizeof(ota_stream_t));
    uint8_t *buf = malloc(8192);
    if (st == NULL || buf == NULL) {
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    st->req = req;
    st->remaining = req->content_len;

    uint8_t hdr[OTA_CONTAINER_HDR_LEN];
    if (ota_stream_read(st, hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short header");
    }

    ota_container_hdr_t ch;
    bool combined = ota_container_parse(hdr, sizeof(hdr), &ch);
    uint32_t fw_len;
    uint32_t n_files = 0;
    if (combined) {
        fw_len = ch.fw_len;
        n_files = ch.n_files > SETTINGS_MAX_TRACKS ? SETTINGS_MAX_TRACKS : ch.n_files;
    } else {
        fw_len = (uint32_t)req->content_len; /* the 16 bytes are firmware start */
    }
    if (fw_len == 0U || fw_len > update->size) {
        ESP_LOGE(TAG, "OTA: bad firmware length %lu (partition %lu)",
                 (unsigned long)fw_len, (unsigned long)update->size);
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware too large");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin: %s", esp_err_to_name(err));
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }

    s_up_total = req->content_len;
    s_up_received = 0;
    s_up_active = true;
    s_up_ota = true;
    ESP_LOGI(TAG, "OTA: begin %s size=%d combined=%d fw=%lu files=%lu",
             update->label, req->content_len, combined ? 1 : 0,
             (unsigned long)fw_len, (unsigned long)n_files);

    uint32_t written = 0;
    bool fail = false;
    if (!combined) {
        err = esp_ota_write(handle, hdr, sizeof(hdr));
        if (err != ESP_OK) {
            fail = true;
        } else {
            written = (uint32_t)sizeof(hdr);
        }
    }
    while (!fail && written < fw_len) {
        size_t want = fw_len - written;
        if (want > 8192U) {
            want = 8192U;
        }
        int r = ota_stream_read(st, buf, want);
        if (r <= 0) {
            fail = true;
            break;
        }
        err = esp_ota_write(handle, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write: %s", esp_err_to_name(err));
            fail = true;
            break;
        }
        written += (uint32_t)r;
        s_up_received = (int)written;
    }
    if (fail || written != fw_len) {
        ESP_LOGE(TAG, "OTA: abort, fw written=%lu/%lu", (unsigned long)written,
                 (unsigned long)fw_len);
        s_up_active = false;
        (void)esp_ota_abort(handle);
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end: %s", esp_err_to_name(err));
        s_up_active = false;
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot: %s", esp_err_to_name(err));
        s_up_active = false;
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota boot failed");
    }

    uint32_t files_ok = 0;
    uint32_t tracks_written = 0;
    if (combined && n_files > 0U && storage_is_mounted()) {
        settings_track_t tracks[SETTINGS_MAX_TRACKS];
        size_t tn = 0;
        for (uint32_t i = 0; i < n_files; ++i) {
            uint8_t fh[OTA_FILE_HDR_LEN];
            if (ota_stream_read(st, fh, sizeof(fh)) != (int)sizeof(fh)) {
                break;
            }
            ota_file_hdr_t f;
            if (!ota_file_hdr_parse(fh, sizeof(fh), &f)) {
                break;
            }
            char raw[OTA_FILE_NAME_MAX + 1] = { 0 };
            char label[OTA_FILE_LABEL_MAX + 1] = { 0 };
            if (ota_stream_read(st, (uint8_t *)raw, f.name_len) != (int)f.name_len) {
                break;
            }
            if (f.label_len > 0U &&
                ota_stream_read(st, (uint8_t *)label, f.label_len) != (int)f.label_len) {
                break;
            }

            char safe[80];
            ota_safe_name(raw, safe, sizeof(safe));
            char path[160];
            snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s", safe);
            FILE *fp = safe[0] != '\0' ? fopen(path, "wb") : NULL;
            if (fp != NULL) {
                (void)setvbuf(fp, NULL, _IOFBF, 4096);
            }

            uint32_t left = f.data_len;
            bool read_ok = true;
            bool write_ok = fp != NULL;
            while (left > 0U) {
                size_t want = left > 8192U ? 8192U : left;
                int r = ota_stream_read(st, buf, want);
                if (r <= 0) {
                    read_ok = false;
                    break;
                }
                if (fp != NULL && write_ok && WEB_FWRITE(buf, 1, (size_t)r, fp) != (size_t)r) {
                    write_ok = false;
                }
                left -= (uint32_t)r;
                s_up_received = req->content_len - st->remaining - (int)(st->len - st->pos);
            }
            if (fp != NULL) {
                if (write_ok && WEB_FFLUSH(fp) != 0) {
                    write_ok = false;
                }
                if (WEB_FCLOSE(fp) != 0) {
                    write_ok = false;
                }
            }
            if (!read_ok) {
                if (fp != NULL) {
                    (void)remove(path);
                }
                break; /* body desynced: stop */
            }
            if (!write_ok) {
                ESP_LOGW(TAG, "OTA: file '%s' write failed", safe);
                (void)remove(path);
                continue;
            }
            if (audio_validate_wav(path) != ESP_OK) {
                ESP_LOGW(TAG, "OTA: file '%s' invalid wav", safe);
                (void)remove(path);
                continue;
            }
            files_ok++;

            if (tn < SETTINGS_MAX_TRACKS) {
                int slot = ota_slot_from_name(safe);
                if (slot == 0) {
                    slot = 1;
                    while (slot <= SETTINGS_MAX_TRACKS) {
                        bool used = false;
                        for (size_t k = 0; k < tn; ++k) {
                            if (tracks[k].slot == (uint8_t)slot) {
                                used = true;
                            }
                        }
                        if (!used) {
                            break;
                        }
                        slot++;
                    }
                }
                if (slot >= 1 && slot <= SETTINGS_MAX_TRACKS) {
                    tracks[tn].slot = (uint8_t)slot;
                    snprintf(tracks[tn].file, sizeof(tracks[tn].file), "audio/%.120s", safe);
                    if (label[0] != '\0') {
                        snprintf(tracks[tn].label, sizeof(tracks[tn].label), "%.63s", label);
                    } else {
                        snprintf(tracks[tn].label, sizeof(tracks[tn].label), "%.63s", safe);
                        char *dot = strrchr(tracks[tn].label, '.');
                        if (dot != NULL) {
                            *dot = '\0';
                        }
                    }
                    tracks[tn].enabled = true;
                    tn++;
                }
            }
        }
        if (tn > 0U) {
            if (settings_tracks_save(tracks, tn) == ESP_OK) {
                tracks_written = (uint32_t)tn;
            }
        }
        ESP_LOGI(TAG, "OTA: sounds files=%lu tracks=%lu", (unsigned long)files_ok,
                 (unsigned long)tracks_written);
    } else if (combined && n_files > 0U) {
        ESP_LOGW(TAG, "OTA: storage not mounted, %lu file(s) skipped",
                 (unsigned long)n_files);
    }

    s_up_active = false;
    free(st);
    free(buf);

    ESP_LOGI(TAG, "OTA: success, rebooting...");
    if (combined) {
        web_log_event("OTA", "прошивка + %lu звук(ов), перезагрузка",
                      (unsigned long)files_ok);
    } else {
        web_log_event("OTA", "прошивка записана, перезагрузка");
    }
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"bytes\":%d,\"files\":%lu}",
             req->content_len, (unsigned long)files_ok);
    send_json(req, json);
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t task_inputs_get(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"actions\":[1,2,3]}");
}

/* GET /api/log?since=N — control events newer than sequence N. */
static esp_err_t log_get(httpd_req_t *req)
{
    char query[32] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    uint32_t since = 0;
    if (parse_query(query, "since", buf, sizeof(buf)) && buf[0]) {
        since = (uint32_t)strtoul(buf, NULL, 10);
    }

    char json[4096];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used,
                "{\"ok\":true,\"seq\":%lu,\"entries\":[", (unsigned long)s_evlog_seq);
    if (s_evlog_mutex != NULL) {
        (void)xSemaphoreTake(s_evlog_mutex, portMAX_DELAY);
    }
    int emitted = 0;
    for (uint32_t i = 0; i < WEB_EVLOG_MAX; ++i) {
        const web_evlog_t *e = &s_evlog[(s_evlog_head + i) % WEB_EVLOG_MAX];
        if (e->seq == 0U || e->seq <= since) {
            continue;
        }
        /* Stop before starting an entry that might not fully fit: a partially
         * appended entry would leave the JSON document malformed. 400 bytes
         * covers the largest escaped tag+text entry. */
        if (used >= sizeof(json) - 400U) {
            break;
        }
        char et[WEB_EVLOG_TAG_MAX * 2];
        char ex[WEB_EVLOG_TEXT_MAX * 2];
        json_escape(e->tag, et, sizeof(et));
        json_escape(e->text, ex, sizeof(ex));
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"s\":%lu,\"tag\":\"%s\",\"text\":\"%s\"}",
                    emitted++ ? "," : "", (unsigned long)e->seq, et, ex);
    }
    if (s_evlog_mutex != NULL) {
        xSemaphoreGive(s_evlog_mutex);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

/* GET /api/clientlog?m=... — browser JS errors forwarded to the serial log. */
static esp_err_t clientlog_get(httpd_req_t *req)
{
    char query[320] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char msg[224] = { 0 };
    if (parse_query(query, "m", msg, sizeof(msg)) && msg[0]) {
        ESP_LOGE(TAG, "CLIENT ERROR: %s", msg);
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- server ---------- */

/* GET /progress (port 81) — real received-byte count of the in-flight upload.
 * Served from a second httpd because the main httpd task is blocked inside the
 * upload recv loop for the whole body transfer. CORS-enabled for the
 * cross-port fetch from the UI. */
static esp_err_t progress_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char json[128];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"slot\":%u,\"ota\":%s,\"total\":%d,\"received\":%d,\"active\":%s}",
             (unsigned)s_up_slot, s_up_ota ? "true" : "false", s_up_total, s_up_received,
             s_up_active ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t start_progress_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 81;
    cfg.ctrl_port = 32769;
    cfg.stack_size = 4096;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 3;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    if (httpd_start(&s_progress_server, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "progress httpd_start failed");
        return ESP_FAIL;
    }
    httpd_uri_t u = { .uri = "/progress", .method = HTTP_GET, .handler = progress_handler };
    httpd_register_uri_handler(s_progress_server, &u);
    ESP_LOGI(TAG, "Progress server started on port 81");
    return ESP_OK;
}

static void register_route(const char *uri, httpd_method_t method,
                           esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = method, .handler = handler };
    esp_err_t err = httpd_register_uri_handler(s_server, &u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "route %s registration failed: %s", uri, esp_err_to_name(err));
    }
}

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Fixed at 80 (see wifi_post): the captive redirect has no port. */
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.stack_size = 16384;
    cfg.max_uri_handlers = 64;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    /* More sockets so the 3 s status poll + a transfer + the port-81 progress
     * poll never exhaust the pool; short timeouts so one stalled client cannot
     * hold a socket for a whole minute. */
    cfg.max_open_sockets = 8;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 30;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    register_route("/", HTTP_GET, root_handler);
    register_route("/api/control/source", HTTP_GET, control_source_get);
    register_route("/api/control/source", HTTP_POST, control_source_post);
    register_route("/api/mode", HTTP_GET, mode_get);
    register_route("/api/mode", HTTP_POST, mode_post);
    register_route("/api/bemf/cal", HTTP_GET, bemf_cal_get);
    register_route("/api/bemf/base", HTTP_GET, bemf_base_get);
    register_route("/api/bemf/calibrate", HTTP_POST, bemf_cal_post);
    register_route("/api/bemf/use", HTTP_GET, bemf_use_get);
    register_route("/api/bemf/use", HTTP_POST, bemf_use_post);
    register_route("/api/motor", HTTP_GET, motor_get);
    register_route("/api/motor", HTTP_POST, motor_post);
    register_route("/api/functions", HTTP_GET, functions_get);
    register_route("/api/function", HTTP_POST, function_post);
    register_route("/api/aux/effect", HTTP_POST, aux_effect_post);
    register_route("/api/audio/status", HTTP_GET, audio_status_get);
    register_route("/api/audio/tracks", HTTP_GET, audio_tracks_get);
    register_route("/api/audio/play", HTTP_POST, audio_play_post);
    register_route("/api/audio/stop", HTTP_POST, audio_stop_post);
    register_route("/api/audio/volume", HTTP_POST, audio_volume_post);
    register_route("/api/audio/upload", HTTP_POST, audio_upload_post);
    register_route("/api/audio/delete", HTTP_POST, audio_track_delete_post);
    register_route("/api/track/category", HTTP_POST, track_category_post);
    register_route("/api/func-map", HTTP_GET, func_map_get);
    register_route("/api/func-map", HTTP_POST, func_map_post);
    register_route("/api/sound/state", HTTP_GET, sound_state_handler);
    register_route("/api/sound/scheme", HTTP_GET, sound_scheme_handler);
    register_route("/api/sound/scheme", HTTP_POST, sound_scheme_post_handler);
    register_route("/api/sound/table", HTTP_POST, sound_table_post);
    register_route("/api/sound/extra", HTTP_POST, sound_extra_post);
    register_route("/api/sound/lint", HTTP_GET, sound_lint_handler);
    register_route("/api/sound/projects", HTTP_GET, sound_projects_get);
    register_route("/api/sound/project", HTTP_POST, sound_project_post);
    register_route("/api/sound/download", HTTP_GET, sound_download_get);
    register_route("/api/sound/upload", HTTP_POST, sound_upload_post);
    register_route("/api/aux/cfg", HTTP_GET, aux_cfg_get);
    register_route("/api/aux/cfg", HTTP_POST, aux_cfg_post);
    register_route("/api/cv/read", HTTP_GET, cv_read_get);
    register_route("/api/cv/all", HTTP_GET, cv_all_get);
    register_route("/api/cv/write", HTTP_POST, cv_write_post);
    register_route("/api/wifi", HTTP_GET, wifi_get);
    register_route("/api/wifi", HTTP_POST, wifi_post);
    register_route("/api/wifi/reset", HTTP_POST, wifi_reset_post);
    register_route("/api/reset", HTTP_POST, factory_reset_post);
    register_route("/api/device", HTTP_GET, device_get);
    register_route("/api/device", HTTP_POST, device_post);
    register_route("/api/storage", HTTP_GET, storage_get);
    register_route("/api/ota/update", HTTP_POST, ota_update_post);
    register_route("/api/task-inputs", HTTP_GET, task_inputs_get);
    register_route("/api/log", HTTP_GET, log_get);
    register_route("/api/clientlog", HTTP_GET, clientlog_get);

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_handler);

    ESP_LOGI(TAG, "Web UI started on port %u", (unsigned)cfg.server_port);
    return ESP_OK;
}

esp_err_t web_init(void)
{
    memset(s_fn, 0, sizeof(s_fn));
    s_func_mutex = xSemaphoreCreateMutex();
    s_evlog_mutex = xSemaphoreCreateMutex();
    (void)settings_load(&s_cfg);
    (void)audio_set_volume(s_cfg.master_volume);
    {
        size_t cat_count = 0;
        (void)settings_track_cats_load(s_track_cat, &cat_count);
        s_track_cat_loaded = true;
    }
    {
        size_t fmap_count = 0;
        (void)settings_func_map_load(s_func_map, &fmap_count);
    }
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind_count = 0;
    func_bind_load();
    /* The engine may have loaded the (possibly empty) store before web_init
     * migrated it; hand it the authoritative list. */
    sound_reload_bindings();
    {
        size_t aux_count = 0;
        (void)settings_aux_cfg_load(s_aux_cfg, &aux_count);
        for (uint8_t i = 0; i < SETTINGS_AUX_COUNT; ++i) {
            /* Clamp values from a stale/corrupt blob to the valid ranges. */
            if (s_aux_cfg[i].level > 100U) {
                s_aux_cfg[i].level = 100U;
            }
            if ((uint32_t)s_aux_cfg[i].effect >= AUXIO_EFFECT_COUNT) {
                s_aux_cfg[i].effect = AUXIO_EFFECT_STEADY;
            }
            uint8_t pwm = (uint8_t)((uint16_t)s_aux_cfg[i].level * 255U / 100U);
            (void)auxio_config(i, pwm, 0, (auxio_effect_t)s_aux_cfg[i].effect,
                               aux_effect_period(s_aux_cfg[i].effect));
        }
    }

    outputs_init();

    esp_err_t err = wifi_start(&s_cfg);
    if (err != ESP_OK && s_cfg.wifi_mode != 0) {
        ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    }

    if (s_cfg.wifi_mode != 0) {
        start_dns_hijack();
        esp_err_t err = start_http_server();
        if (err == ESP_OK) {
            (void)start_progress_server();
        }
        if (s_cfg.auto_off_min != 0U) {
            if (xTaskCreate(wifi_auto_off_task, "wifi_off", 3072, NULL, 4, NULL) != pdPASS) {
                ESP_LOGW(TAG, "wifi auto-off task create failed");
            }
        }
        return err;
    }
    return ESP_OK;
}
