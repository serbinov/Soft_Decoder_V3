#include "web.h"
#include "web_html.h"
#include "web_util.h"
#include "audio_pack.h"
#include "ima_adpcm.h"
#include "sound_editor_assets.h"
#include "sound_graph_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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
#define WEB_QUERY_MAX 1400

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
static esp_err_t (*s_route_handlers[64])(httpd_req_t *);
static size_t s_route_count;
static bool s_wifi_started;
static char s_ap_ip[16];
static uint8_t s_ap_ip_bytes[4];
static char s_sta_ip[16];

/* In-flight progress for the single asynchronous transfer worker. Port 81
 * remains a read-only progress endpoint alongside the responsive main server. */
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
 * Only a missing store is migrated from s_func_map. A present empty list is
 * authoritative and must not resurrect legacy outputs or bindings. */
static func_binding_t s_func_bind[FUNC_BIND_MAX];
static size_t s_func_bind_count;
static settings_aux_cfg_t s_aux_cfg[SETTINGS_AUX_COUNT];
static uint16_t s_func_last_mask[SETTINGS_FUNC_MAP_COUNT];
/* Serialises the shared function-apply state (s_fn + track scratch buffer)
 * between the DCC callback and the HTTP handler. */
static SemaphoreHandle_t s_func_mutex;
static SemaphoreHandle_t s_control_mutex;
static portMUX_TYPE s_control_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_actuation_ready;
static TaskHandle_t s_maintenance_owner;
static bool s_maintenance_exclusive_fs;
static uint32_t s_control_generation;
static unsigned s_mutation_count;
static TaskHandle_t s_mutation_owner;
static int64_t s_transfer_deadline;
static bool s_transfer_reboot;
static uint32_t s_file_sequence;
static bool s_func_bind_present;
static uint16_t s_outputs_applied;
static audio_voice_handle_t s_legacy_voice[SETTINGS_FUNC_MAP_COUNT][2];
static audio_voice_handle_t s_preview_voice;
static settings_track_t s_func_tracks[SETTINGS_MAX_TRACKS];
static bool s_motion_forward = true;
static uint8_t s_motion_speed;
static bool s_motion_initialized;

static bool actuation_ready(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    bool ready = s_actuation_ready;
    portEXIT_CRITICAL(&s_control_state_mux);
    return ready;
}

static esp_err_t control_mutex_init(void)
{
    if (s_control_mutex != NULL) return ESP_OK;
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_control_state_mux);
    bool adopted = s_control_mutex == NULL;
    if (adopted) s_control_mutex = created;
    portEXIT_CRITICAL(&s_control_state_mux);
    if (!adopted) vSemaphoreDelete(created);
    return ESP_OK;
}

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
    while (i < n && s[i] != '\0') {
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
        for (size_t j = 1; j < clen; ++j) {
            if (s[i + j] == '\0') return i;
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

static bool query_encoding_valid(const char *query)
{
    for (size_t i = 0; query[i] != '\0'; ++i) {
        if (query[i] == '%') {
            if (query[i + 1] == '\0' || query[i + 2] == '\0' ||
                !isxdigit((unsigned char)query[i + 1]) || !isxdigit((unsigned char)query[i + 2]) ||
                (query[i + 1] == '0' && query[i + 2] == '0')) return false;
            i += 2;
        }
    }
    return true;
}

static bool query_has_key(const char *query, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = query; p != NULL && *p != '\0'; p = strchr(p, '&')) {
        if (*p == '&') ++p;
        if (strncmp(p, key, n) == 0 && p[n] == '=') return true;
    }
    return false;
}

static bool query_bool_valid(const char *query, const char *key)
{
    if (!query_has_key(query, key)) return true;
    char value[16];
    if (!parse_query(query, key, value, sizeof(value))) return false;
    return strcmp(value, "1") == 0 || strcmp(value, "0") == 0 ||
           strcasecmp(value, "true") == 0 || strcasecmp(value, "false") == 0 ||
           strcasecmp(value, "yes") == 0 || strcasecmp(value, "no") == 0;
}

static bool transfer_expired(void)
{
    return s_transfer_deadline != 0 && esp_timer_get_time() >= s_transfer_deadline;
}

static esp_err_t close_rejected_body(httpd_req_t *req, esp_err_t response)
{
    /* IDF otherwise purges an unread body synchronously after the handler. */
    if (req->content_len != 0U) {
        httpd_resp_set_hdr(req, "Connection", "close");
        int fd = httpd_req_to_sockfd(req);
        if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
    }
    return response;
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
    bool rails = false;
    if (s_control_mutex != NULL && xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        rails = s_cfg.control_source == 0 && actuation_ready() && !web_maintenance_active();
        xSemaphoreGive(s_control_mutex);
    }
    return rails;
}

bool web_maintenance_active(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    bool active = s_maintenance_owner != NULL;
    portEXIT_CRITICAL(&s_control_state_mux);
    return active;
}

bool web_control_rails_begin(uint32_t *generation)
{
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    if (!actuation_ready() || web_maintenance_active() || s_cfg.control_source != 0) {
        xSemaphoreGive(s_control_mutex);
        return false;
    }
    if (generation != NULL) *generation = s_control_generation;
    return true;
}

void web_control_rails_end(void)
{
    if (s_control_mutex != NULL) xSemaphoreGive(s_control_mutex);
}

esp_err_t web_set_actuation_ready(bool ready)
{
    if (!ready) {
        portENTER_CRITICAL(&s_control_state_mux);
        s_actuation_ready = false;
        portEXIT_CRITICAL(&s_control_state_mux);
    }
    if (!ready) (void)motor_set_inhibited(true);
    esp_err_t err = control_mutex_init();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (ready && web_maintenance_active()) {
        xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t sound_err = sound_set_inhibited(!ready);
    esp_err_t audio_err = audio_set_inhibited(!ready);
    if (!ready && sound_err == ESP_ERR_INVALID_STATE) sound_err = ESP_OK;
    if (!ready && audio_err == ESP_ERR_INVALID_STATE) audio_err = ESP_OK;
    err = sound_err != ESP_OK ? sound_err : audio_err;
    if (err != ESP_OK) {
        ready = false;
        (void)motor_set_inhibited(true);
        (void)sound_set_inhibited(true);
        (void)audio_set_inhibited(true);
    }
    portENTER_CRITICAL(&s_control_state_mux);
    s_actuation_ready = ready;
    portEXIT_CRITICAL(&s_control_state_mux);
    ++s_control_generation;
    bool inhibited = !ready || web_maintenance_active();
    (void)motor_set_inhibited(inhibited);
    xSemaphoreGive(s_control_mutex);
    return err;
}

static bool graph_motor_stopped(void)
{
    uint8_t target = 0, applied = 0;
    motor_get_status(&target, NULL);
    motor_get_applied_speed(&applied, NULL);
    return target == 0U && applied == 0U;
}

static esp_err_t maintenance_reserve_checked(bool stopped)
{
    esp_err_t init_err = control_mutex_init();
    if (init_err != ESP_OK) return init_err;
    if (xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (web_maintenance_active()) {
        xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    /* Check before inhibit can erase the target/applied-motion evidence. */
    if (stopped && (!actuation_ready() || !graph_motor_stopped())) {
        xSemaphoreGive(s_control_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_control_state_mux);
    s_maintenance_owner = xTaskGetCurrentTaskHandle();
    s_maintenance_exclusive_fs = false;
    portEXIT_CRITICAL(&s_control_state_mux);
    ++s_control_generation;
    esp_err_t err = motor_set_inhibited(true);
    esp_err_t sound_err = sound_set_inhibited(true);
    esp_err_t audio_err = audio_set_inhibited(true);
    xSemaphoreGive(s_control_mutex);
    if (err == ESP_OK) err = sound_err;
    if (!actuation_ready() && err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (!actuation_ready() && audio_err == ESP_ERR_INVALID_STATE) audio_err = ESP_OK;
    if (err == ESP_OK) err = audio_err;
    if (err != ESP_OK) {
        /* A failed sound reset is not safe to undo: its old runtime may still
         * be armed. Require an explicit readiness recovery instead. */
        portENTER_CRITICAL(&s_control_state_mux);
        s_actuation_ready = false;
        portEXIT_CRITICAL(&s_control_state_mux);
        web_maintenance_end();
    }
    return err;
}

static esp_err_t maintenance_reserve(void)
{
    return maintenance_reserve_checked(false);
}

static esp_err_t maintenance_quiesce(bool exclusive_fs)
{
    esp_err_t err = ESP_OK;
    if (exclusive_fs) {
        err = storage_maintenance_begin();
        if (err == ESP_OK) s_maintenance_exclusive_fs = true;
    }
    int64_t deadline = esp_timer_get_time() + 2000000;
    while (err == ESP_OK) {
        portENTER_CRITICAL(&s_control_state_mux);
        bool mutations_drained = s_mutation_count == 0U ||
            (s_mutation_count == 1U && s_mutation_owner == xTaskGetCurrentTaskHandle());
        portEXIT_CRITICAL(&s_control_state_mux);
        if (audio_is_quiescent() && storage_is_quiescent() && mutations_drained) break;
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return err;
}

esp_err_t web_maintenance_begin(bool exclusive_fs)
{
    esp_err_t err = maintenance_reserve();
    if (err != ESP_OK) return err;
    err = maintenance_quiesce(exclusive_fs);
    if (err != ESP_OK) web_maintenance_end();
    return err;
}

void web_maintenance_end(void)
{
    if (s_maintenance_owner != xTaskGetCurrentTaskHandle()) return;
    if (s_maintenance_exclusive_fs) storage_maintenance_end();
    if (s_control_mutex == NULL ||
        xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    bool ready = actuation_ready();
    esp_err_t sound_err = sound_set_inhibited(!ready);
    esp_err_t audio_err = audio_set_inhibited(!ready);
    if (ready && (sound_err != ESP_OK || audio_err != ESP_OK)) {
        ready = false;
        (void)sound_set_inhibited(true);
        (void)audio_set_inhibited(true);
    }
    (void)motor_set_inhibited(!ready);
    portENTER_CRITICAL(&s_control_state_mux);
    s_actuation_ready = ready;
    s_maintenance_owner = NULL;
    s_maintenance_exclusive_fs = false;
    portEXIT_CRITICAL(&s_control_state_mux);
    ++s_control_generation;
    xSemaphoreGive(s_control_mutex);
}

bool web_fs_busy(void)
{
    return s_up_active || web_maintenance_active();
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
static esp_err_t func_bind_migrate_from_map(void)
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
    esp_err_t err = settings_func_bind_legacy_convert(s_func_map, SETTINGS_FUNC_MAP_COUNT,
                                                     s_func_bind, &n);
    if (err == ESP_OK) {
        for (size_t i = 0; i < keep_n && n < FUNC_BIND_MAX; ++i) {
            s_func_bind[n++] = keep[i];
        }
        s_func_bind_count = n;
        err = settings_func_bind_save(s_func_bind, n);
    }
    return err;
}

/* Load the canonical bindings, migrating the legacy map when the store is
 * empty. Safe to call once at init. */
static esp_err_t func_bind_load(void)
{
    size_t n = 0;
    esp_err_t err = settings_func_bind_load(s_func_bind, &n);
    if (err == ESP_OK) {
        s_func_bind_count = n;
        s_func_bind_present = true;
    } else if (err == ESP_ERR_NOT_FOUND) {
        err = func_bind_migrate_from_map();
        s_func_bind_present = true;
    } else {
        s_func_bind_count = 0;
        s_func_bind_present = true;
    }
    return err;
}

/* Desired output mask for one function: canonical bindings when present,
 * otherwise the legacy per-function map (verbatim old behaviour). */
static uint16_t func_desired_locked(uint8_t fn, bool fn_on)
{
    if (s_func_bind_present || s_func_bind_count > 0U) {
        uint8_t dir = s_motion_forward ? FUNC_DIR_FWD : FUNC_DIR_REV;
        uint8_t st = (s_motion_speed != 0U) ? FUNC_STATE_MOVING : FUNC_STATE_STOPPED;
        return fn_on ? func_eval(s_func_bind, s_func_bind_count, fn, st, dir, NULL, NULL) : 0U;
    }
    return web_util_func_desired(&s_func_map[fn], fn_on, s_motion_forward, s_motion_speed);
}

/* Caller must hold s_func_mutex (or be single-threaded at init). */
static void func_apply_output_locked(uint8_t fn)
{
    if (!actuation_ready() || web_maintenance_active()) return;
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        return;
    }
    uint16_t desired = 0U;
    for (uint8_t f = 0; f < SETTINGS_FUNC_MAP_COUNT; ++f) {
        s_func_last_mask[f] = func_desired_locked(f, s_fn[f]);
        desired |= s_func_last_mask[f];
    }
    uint16_t prev = s_outputs_applied;
    uint16_t turn_off = (uint16_t)(prev & ~desired);
    uint16_t turn_on = (uint16_t)(desired & ~prev);
    for (uint8_t bit = 0; bit < 9U; ++bit) {
        uint16_t b = (uint16_t)(1U << bit);
        if ((turn_off & b) != 0U) {
            if (auxio_set_enabled(func_out_channel(bit), false) == ESP_OK) s_outputs_applied &= (uint16_t)~b;
        } else if ((turn_on & b) != 0U) {
            if (auxio_set_enabled(func_out_channel(bit), true) == ESP_OK) s_outputs_applied |= b;
        }
    }
}

void web_motion_changed(uint8_t speed, bool forward)
{
    if (!actuation_ready() || web_maintenance_active()) return;
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
    if (fn >= SETTINGS_FUNC_MAP_COUNT || (s_func_bind_present && s_func_bind_count == 0U)) {
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
    if (!actuation_ready() || web_maintenance_active()) return;
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
    bool scheme_on = sound_enabled();
    if (changed && scheme_on) {
        sound_function(fn, state);
    }

    if (changed && !scheme_on && fn >= 1U && fn <= 20U) {
        uint8_t slot_a = 0, slot_b = 0;
        web_func_audio_slots(fn, &slot_a, &slot_b);
        bool use_b = slot_b != 0U;

        if (!state || (slot_a == 0U && !use_b)) {
            audio_voice_release_owned(s_legacy_voice[fn][0]);
            audio_voice_release_owned(s_legacy_voice[fn][1]);
            memset(s_legacy_voice[fn], 0, sizeof(s_legacy_voice[fn]));
        } else {
            /* Track lookup buffer is static: the DCC task stack is small and
             * the httpd task must not carry ~4 KB of tracks either. */
            size_t count = 0;
            if (settings_tracks_load(s_func_tracks, &count) == ESP_OK) {
                const uint8_t want[2] = { slot_a, use_b ? slot_b : 0U };
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
                                audio_voice_handle_t *h = &s_legacy_voice[fn][k];
                                if (audio_voice_get_state(*h) != AUDIO_VOICE_FINISHED ||
                                    audio_voice_alloc_owned(h) == ESP_OK) {
                                    started = audio_voice_play_owned(h, abs_path, true,
                                                                   web_get_voice_volume(fn)) == ESP_OK;
                                }
                                break;
                            }
                        }
                    }
                    if (started) {
                        buf_appendf(sounds, sizeof(sounds), &sounds_used,
                                    "%s%u", sounds_used ? "," : "", (unsigned)want[k]);
                    } else {
                        audio_voice_release_owned(s_legacy_voice[fn][k]);
                        memset(&s_legacy_voice[fn][k], 0, sizeof(s_legacy_voice[fn][k]));
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
    if (fn >= SETTINGS_FUNC_MAP_COUNT || slot_a > SETTINGS_MAX_TRACKS ||
        slot_b > SETTINGS_MAX_TRACKS || web_maintenance_active() || sound_graph_active()) {
        return false;
    }
    settings_func_map_t next[SETTINGS_FUNC_MAP_COUNT];
    func_binding_t old[FUNC_BIND_MAX], bindings[FUNC_BIND_MAX] = { { 0 } };
    if (s_func_mutex == NULL || xSemaphoreTake(s_func_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    memcpy(next, s_func_map, sizeof(next));
    memcpy(old, s_func_bind, sizeof(old));
    size_t old_n = s_func_bind_count;
    xSemaphoreGive(s_func_mutex);
    next[fn].slot_a = slot_a;
    next[fn].slot_b = slot_b;
    next[fn].aux_mask = aux;
    next[fn].dir = dir;
    next[fn].speed = speed;
    size_t n = 0;
    if (settings_func_bind_legacy_convert(next, SETTINGS_FUNC_MAP_COUNT, bindings, &n) != ESP_OK) return false;
    for (size_t i = 0; i < old_n && n < FUNC_BIND_MAX; ++i) {
        if (old[i].used && old[i].target_type != FUNC_TARGET_OUTPUT && old[i].target_type != FUNC_TARGET_SLOT)
            bindings[n++] = old[i];
    }
    if (settings_func_map_save(next, SETTINGS_FUNC_MAP_COUNT) != ESP_OK ||
        settings_func_bind_save(bindings, n) != ESP_OK) return false;
    if (xSemaphoreTake(s_func_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    memcpy(s_func_map, next, sizeof(next));
    memcpy(s_func_bind, bindings, sizeof(bindings));
    s_func_bind_count = n;
    s_func_bind_present = true;
    func_apply_output_locked(fn);
    xSemaphoreGive(s_func_mutex);
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

static esp_err_t outputs_init(void)
{
    esp_err_t err = auxio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auxio init failed");
    }
    return err;
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
    if (s == NULL || s[0] == '\0' || out == NULL) {
        return false;
    }
    unsigned octets[4] = {0};
    const char *p = s;
    for (size_t i = 0; i < 4U; ++i) {
        const char *start = p;
        unsigned digits = 0;
        while (isdigit((unsigned char)*p)) {
            if (++digits > 3U) return false;
            octets[i] = octets[i] * 10U + (unsigned)(*p++ - '0');
        }
        if (digits == 0U || octets[i] > 255U || (digits > 1U && start[0] == '0')) return false;
        if (i < 3U) { if (*p++ != '.') return false; }
        else if (*p != '\0') return false;
    }
    if (octets[0] == 0U || octets[0] == 127U || octets[0] >= 224U ||
        octets[3] == 0U || octets[3] == 255U) return false;
    out->addr = ESP_IP4TOADDR(octets[0], octets[1], octets[2], octets[3]);
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
    if (ap_netif == NULL) return ESP_ERR_NO_MEM;

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
    const char *ssid = cfg->ap_ssid[0] ? cfg->ap_ssid : "ADDITIPUS AURA-X";
    size_t ssid_len = strlen(ssid);
    if (ssid_len > sizeof(ap.ap.ssid)) return ESP_ERR_INVALID_ARG;
    memcpy(ap.ap.ssid, ssid, ssid_len);
    ap.ap.ssid_len = (uint8_t)ssid_len;
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    if (cfg->ap_password[0] != '\0' && strlen(cfg->ap_password) >= 8U) {
        size_t password_len = strlen(cfg->ap_password);
        if (password_len > sizeof(ap.ap.password)) return ESP_ERR_INVALID_ARG;
        if (password_len == 64U) {
            for (size_t i = 0; i < password_len; ++i)
                if (!isxdigit((unsigned char)cfg->ap_password[i])) return ESP_ERR_INVALID_ARG;
        }
        memcpy(ap.ap.password, cfg->ap_password, password_len);
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
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) return err;

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
        err = esp_netif_dhcps_stop(ap_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
        err = esp_netif_set_ip_info(ap_netif, &ip);
        if (err != ESP_OK) return err;

        /* Hand the AP IP to DHCP clients as their DNS server: every name
         * resolves to the board so connectivity probes land on the HTTP
         * server (captive portal opens the page automatically). */
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ap_addr.addr;
        err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        if (err != ESP_OK) return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        (void)esp_wifi_stop();
        return err;
    }

    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);

    /* No radio power-save: a sleeping AP radio throttles uploads to ~2 KB/s. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_ap_sta_count = 0;
    s_ap_started_us = esp_timer_get_time();
    s_last_client_us = s_ap_started_us;
    s_wifi_started = true;

    snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ap_addr));
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
    if (strncmp(req->uri, "/sound-editor", 13) == 0 ||
        strncmp(req->uri, "/api/", 5) == 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
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

static esp_err_t sound_editor_get(httpd_req_t *req)
{
    size_t len = strcspn(req->uri, "?");
    const char *path = req->uri;
    if ((len == 13U && strncmp(path, "/sound-editor", len) == 0) ||
        (len == 14U && strncmp(path, "/sound-editor/", len) == 0)) {
        path = "/sound-editor/blocks.html";
        len = strlen(path);
    }
    for (size_t i = 0; i < sound_editor_asset_count; ++i) {
        const sound_editor_asset_t *a = &sound_editor_assets[i];
        if (strlen(a->path) != len || strncmp(path, a->path, len) != 0) continue;
        httpd_resp_set_type(req, a->mime);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(req, "Cache-Control", a->immutable ? "public, max-age=31536000, immutable" : "no-cache");
        return httpd_resp_send(req, (const char *)a->data, a->length);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "asset not found");
}

static esp_err_t graph_error(httpd_req_t *req, const char *status, const char *message)
{
    char escaped[512], json[576];
    json_escape(message, escaped, sizeof(escaped));
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    httpd_resp_set_status(req, status);
    if (req->content_len != 0U) httpd_resp_set_hdr(req, "Connection", "close");
    return send_json(req, json);
}

static esp_err_t graph_diagnostic_response(httpd_req_t *req, const sg_diagnostic_t *diag, bool validation)
{
    char message[768], code[192], id[SG_ID_CAP * 6], field[192], result[1600];
    json_escape(diag->message, message, sizeof(message));
    json_escape(diag->code[0] ? diag->code : "validation", code, sizeof(code));
    json_escape(diag->id, id, sizeof(id));
    json_escape(diag->field, field, sizeof(field));
    snprintf(result, sizeof(result), "{\"ok\":%s,%s\"diagnostics\":[{\"code\":\"%s\",\"id\":\"%s\",\"field\":\"%s\",\"message\":\"%s\",\"offset\":%lu}]}",
             validation ? "true" : "false", validation ? "\"valid\":false," : "\"error\":\"project validation failed\",", code, id, field, message, (unsigned long)diag->offset);
    if (!validation) httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, result);
}

static esp_err_t graph_result_error(httpd_req_t *req, esp_err_t err)
{
    const char *status = err == ESP_ERR_INVALID_ARG ? "400 Bad Request" :
        err == ESP_ERR_NOT_FOUND ? "404 Not Found" :
        err == ESP_ERR_INVALID_STATE ? "409 Conflict" :
        (err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) ? "503 Service Unavailable" : "500 Internal Server Error";
    return graph_error(req, status, esp_err_to_name(err));
}

static bool graph_query(httpd_req_t *req, char *query, size_t cap)
{
    esp_err_t err = httpd_req_get_url_query_str(req, query, cap);
    return (err == ESP_OK || err == ESP_ERR_NOT_FOUND) && query_encoding_valid(query);
}

static bool graph_param(const char *query, const char *key, char *out, size_t capacity)
{
    char decoded[WEB_QUERY_MAX];
    size_t matches = 0, key_length = strlen(key);
    for (const char *p = query; p != NULL && *p != '\0'; p = strchr(p, '&')) {
        if (*p == '&') ++p;
        if (strncmp(p, key, key_length) == 0 && p[key_length] == '=') ++matches;
    }
    if (matches != 1U || !parse_query(query, key, decoded, sizeof(decoded)) ||
        strlen(decoded) >= capacity) return false;
    memcpy(out, decoded, strlen(decoded) + 1U);
    return true;
}

static bool graph_revision(const char *query, const char *key, uint32_t *revision)
{
    char value[16];
    if (!graph_param(query, key, value, sizeof(value)) || value[0] == '\0') return false;
    uint32_t n = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9' || n > (UINT32_MAX - (uint32_t)(*p - '0')) / 10U) return false;
        n = n * 10U + (uint32_t)(*p - '0');
    }
    *revision = n;
    return true;
}

static esp_err_t graph_capabilities_get(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"format\":\"sound-graph\",\"schemaVersion\":1,\"limits\":{\"states\":57,\"soundStates\":31,\"transitions\":128,\"effects\":24,\"assets\":31,\"jsonBytes\":131072},\"conditions\":[\"fn_press\",\"fn_release\",\"fn_on\",\"fn_off\",\"engine_on\",\"engine_off\",\"speed\",\"accel\",\"decel\",\"sample_done\"]}");
}

static esp_err_t graph_projects_get(httpd_req_t *req)
{
    sg_store_descriptor_t *items = calloc(SG_STORE_MAX_PROJECTS, sizeof(*items));
    char *json = malloc(32768U);
    if (items == NULL || json == NULL) { free(items); free(json); return graph_result_error(req, ESP_ERR_NO_MEM); }
    size_t count = 0, used = 0;
    esp_err_t err = sg_store_list(items, SG_STORE_MAX_PROJECTS, &count);
    if (err == ESP_OK) {
        buf_appendf(json, 32768U, &used, "{\"ok\":true,\"projects\":[");
        for (size_t i = 0; i < count; ++i) {
            char name[SG_NAME_CAP * 6], id[SG_ID_CAP * 6];
            json_escape(items[i].name, name, sizeof(name));
            json_escape(items[i].id, id, sizeof(id));
            buf_appendf(json, 32768U, &used, "%s{\"id\":\"%s\",\"name\":\"%s\",\"revision\":%lu}", i ? "," : "", id, name, (unsigned long)items[i].revision);
        }
        buf_appendf(json, 32768U, &used, "]}");
        err = send_json(req, json);
    } else err = graph_result_error(req, err);
    free(items); free(json);
    return err;
}

static esp_err_t graph_project_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, id[SG_ID_CAP];
    uint32_t revision;
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "id", id, sizeof(id)) || !sg_id_valid(id) ||
        !graph_revision(query, "revision", &revision)) return graph_result_error(req, ESP_ERR_INVALID_ARG);
    char *json = NULL; size_t len = 0; uint32_t actual = 0;
    esp_err_t err = sg_store_read(id, revision, &json, &len, &actual);
    if (err != ESP_OK) return graph_result_error(req, err);
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "{\"ok\":true,\"revision\":%lu,\"project\":", (unsigned long)actual);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send_chunk(req, prefix, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, json, len);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "}", 1);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    free(json);
    return err;
}

static esp_err_t graph_state_get(httpd_req_t *req)
{
    sound_graph_status_t st = {0};
    sound_graph_status_get(&st);
    char json[512], id[SG_ID_CAP * 6]; size_t used = 0;
    json_escape(st.id, id, sizeof(id));
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"active\":%s,\"id\":\"%s\",\"revision\":%lu,\"engine\":%s,\"armed\":%s,\"failedChannels\":%lu,\"states\":[", st.active ? "true" : "false", id, (unsigned long)st.revision, st.engine ? "true" : "false", st.armed ? "true" : "false", (unsigned long)st.failed_channels);
    for (size_t i = 0; i < SG_MAX_EFFECTS + 1; ++i) buf_appendf(json, sizeof(json), &used, "%s%u", i ? "," : "", st.states[i]);
    buf_appendf(json, sizeof(json), &used, "],\"speed\":%u,\"fault\":%s}", (unsigned)st.speed, st.fault ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t graph_asset_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, file[SG_FILE_CAP];
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "file", file, sizeof(file)) || !sg_filename_valid(file)) return graph_result_error(req, ESP_ERR_INVALID_ARG);
    sg_asset_t asset; sg_diagnostic_t diag = {0};
    esp_err_t err = sg_asset_inspect(file, &asset, &diag);
    if (err != ESP_OK) return graph_result_error(req, err);
    char json[512];
    snprintf(json, sizeof(json), "{\"ok\":true,\"asset\":{\"file\":\"%s\",\"size\":%lu,\"crc32\":\"%s\",\"sampleRate\":%lu,\"channels\":%lu,\"bits\":%lu,\"durationMs\":%lu}}", asset.file, (unsigned long)asset.size, asset.crc32, (unsigned long)asset.sampleRate, (unsigned long)asset.channels, (unsigned long)asset.bits, (unsigned long)asset.durationMs);
    return send_json(req, json);
}

static esp_err_t graph_receive(httpd_req_t *req, char **out)
{
    *out = NULL;
    if (req->content_len == 0U || req->content_len > SG_MAX_JSON) {
        return close_rejected_body(req, graph_error(req, req->content_len > SG_MAX_JSON ? "413 Payload Too Large" : "400 Bad Request", "invalid JSON body size"));
    }
    char mime[96];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", mime, sizeof(mime)) != ESP_OK ||
        strncasecmp(mime, "application/json", 16) != 0 || (mime[16] != '\0' && mime[16] != ';' && !isspace((unsigned char)mime[16]))) {
        return close_rejected_body(req, graph_error(req, "415 Unsupported Media Type", "application/json required"));
    }
    const char *parameters = mime + 16;
    while (isspace((unsigned char)*parameters)) ++parameters;
    if (*parameters != '\0' && *parameters != ';')
        return close_rejected_body(req, graph_error(req, "415 Unsupported Media Type", "invalid JSON media type"));
    char *json = malloc(req->content_len + 1U);
    if (json == NULL) return close_rejected_body(req, graph_result_error(req, ESP_ERR_NO_MEM));
    size_t got = 0; unsigned retries = 0;
    int64_t deadline = esp_timer_get_time() + 15000000;
    while (got < req->content_len && esp_timer_get_time() < deadline) {
        size_t wanted = req->content_len - got;
        if (wanted > 4096U) wanted = 4096U;
        int n = httpd_req_recv(req, json + got, wanted);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= 3U) continue;
        if (n <= 0 || (size_t)n > wanted) break;
        got += (size_t)n;
    }
    if (got != req->content_len || esp_timer_get_time() >= deadline || memchr(json, '\0', got) != NULL) {
        free(json);
        return close_rejected_body(req, graph_error(req, "400 Bad Request", "incomplete or NUL JSON body"));
    }
    json[got] = '\0'; *out = json;
    return ESP_OK;
}

static esp_err_t graph_validate_post(httpd_req_t *req)
{
    char *json = NULL;
    esp_err_t response = graph_receive(req, &json);
    if (json == NULL) return response;
    sg_graph_t *graph = malloc(sizeof(*graph)); sg_diagnostic_t diag = {0};
    if (graph == NULL) { free(json); return graph_result_error(req, ESP_ERR_NO_MEM); }
    esp_err_t err = sg_parse(json, req->content_len, graph, &diag);
    free(json); free(graph);
    if (err == ESP_ERR_NO_MEM) return graph_result_error(req, err);
    if (err == ESP_OK) return send_json(req, "{\"ok\":true,\"valid\":true,\"diagnostics\":[]}");
    return graph_diagnostic_response(req, &diag, true);
}

static esp_err_t graph_save_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, id[SG_ID_CAP]; uint32_t expected;
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "id", id, sizeof(id)) || !sg_id_valid(id) || !graph_revision(query, "expectedRevision", &expected)) return close_rejected_body(req, graph_result_error(req, ESP_ERR_INVALID_ARG));
    char *json = NULL;
    esp_err_t response = graph_receive(req, &json);
    if (json == NULL) return response;
    uint32_t revision = 0; sg_diagnostic_t diag = {0};
    esp_err_t err = sg_store_save(id, expected, json, req->content_len, &revision, &diag);
    free(json);
    if (err == ESP_ERR_INVALID_ARG && diag.message[0]) return graph_diagnostic_response(req, &diag, false);
    if (err != ESP_OK) return graph_result_error(req, err);
    char result[64];
    snprintf(result, sizeof(result), "{\"ok\":true,\"revision\":%lu}", (unsigned long)revision);
    return send_json(req, result);
}

static esp_err_t graph_apply_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, id[SG_ID_CAP]; uint32_t revision;
    if (req->content_len != 0U) return close_rejected_body(req, graph_result_error(req, ESP_ERR_INVALID_ARG));
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "id", id, sizeof(id)) || !sg_id_valid(id) || !graph_revision(query, "revision", &revision) || revision == 0U) return graph_result_error(req, ESP_ERR_INVALID_ARG);
    if (!graph_motor_stopped()) return graph_error(req, "409 Conflict", "motor must be stopped");
    char *json = NULL; size_t len = 0; uint32_t actual = 0;
    sg_graph_t *graph = malloc(sizeof(*graph)); sg_diagnostic_t diag = {0};
    if (graph == NULL) return graph_result_error(req, ESP_ERR_NO_MEM);
    sound_graph_prepared_t *prepared = NULL;
    esp_err_t err = sg_store_read(id, revision, &json, &len, &actual);
    if (err == ESP_OK) err = sg_parse(json, len, graph, &diag);
    free(json);
    /* Reserve after preparation so validation/OOM never disturbs live audio. */
    if (err == ESP_OK) err = sound_graph_prepare(graph, id, actual, &prepared);
    bool maintenance = false, lease = false;
    if (err == ESP_OK) {
        err = maintenance_reserve_checked(true);
        maintenance = err == ESP_OK;
    }
    if (err == ESP_OK && !graph_motor_stopped()) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = maintenance_quiesce(false);
    if (err == ESP_OK) { err = storage_access_begin(); lease = err == ESP_OK; }
    if (err == ESP_OK) err = sg_assets_validate(graph, &diag);
    if (err == ESP_OK) err = sg_store_select(id, actual);
    if (err == ESP_OK) {
        sound_graph_commit(prepared); prepared = NULL;
        uint32_t levels = 0;
        if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
        for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) if (s_fn[fn]) levels |= 1UL << fn;
        sound_graph_prime_functions(levels);
        if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
    }
    sound_graph_prepared_free(prepared); free(graph);
    if (lease) storage_access_end();
    if (maintenance) web_maintenance_end();
    if (err != ESP_OK && diag.message[0] && err != ESP_ERR_NO_MEM && err != ESP_ERR_TIMEOUT) return graph_diagnostic_response(req, &diag, false);
    if (err != ESP_OK) return graph_result_error(req, err);
    return send_json(req, "{\"ok\":true}");
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
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    if (!parse_query(query, "source", buf, sizeof(buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing source");
    }
    if (strcasecmp(buf, "web") != 0 && strcmp(buf, "1") != 0 &&
        strcasecmp(buf, "rails") != 0 && strcmp(buf, "0") != 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid source");
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (web_maintenance_active()) {
        xSemaphoreGive(s_control_mutex);
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance\"}");
    }
    settings_config_t next = s_cfg;
    next.control_source = (strcasecmp(buf, "web") == 0 || strcmp(buf, "1") == 0) ? 1 : 0;
    motor_emergency_stop();
    if (next.control_source == 0) {
        uint8_t cv29 = 0;
        esp_err_t err = settings_cv_read(29, &cv29);
        if (err == ESP_OK) err = settings_cv_write(29, cv29 & (uint8_t)~0x04U);
        if (err == ESP_OK) err = settings_cv_commit();
        if (err != ESP_OK) {
            xSemaphoreGive(s_control_mutex);
            return send_json(req, "{\"ok\":false,\"error\":\"mode save failed\"}");
        }
    }
    if (settings_save(&next) != ESP_OK) {
        xSemaphoreGive(s_control_mutex);
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    s_cfg = next;
    ++s_control_generation;
    web_log_event("Источник", "%s", s_cfg.control_source == 1 ? "Веб" : "Рельсы");
    xSemaphoreGive(s_control_mutex);
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
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    if (!parse_query(query, "mode", buf, sizeof(buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mode");
    }
    bool dc = (strcasecmp(buf, "dc") == 0);
    if (!dc && strcasecmp(buf, "dcc") != 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode");
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (web_maintenance_active()) {
        xSemaphoreGive(s_control_mutex);
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance\"}");
    }
    uint8_t cv29 = 0;
    (void)settings_cv_read(29, &cv29);
    if (dc) {
        cv29 |= 0x04U;
    } else {
        cv29 &= (uint8_t)~0x04U;
    }
    esp_err_t err = settings_cv_write(29, cv29);
    if (err == ESP_OK) err = settings_cv_commit();
    motor_emergency_stop();
    ++s_control_generation;
    xSemaphoreGive(s_control_mutex);
    if (err != ESP_OK) return send_json(req, "{\"ok\":false,\"error\":\"mode save failed\"}");
    web_log_event("Режим", "%s", dc ? "DC (аналог)" : "DCC");
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", dc ? "dc" : "dcc");
    return send_json(req, json);
}

static const char *bemf_result_name(motor_bemf_cal_state_t result)
{
    switch (result) {
    case MOTOR_BEMF_CAL_IDLE: return "idle";
    case MOTOR_BEMF_CAL_RUNNING: return "running";
    case MOTOR_BEMF_CAL_SAVING: return "saving";
    case MOTOR_BEMF_CAL_SUCCEEDED: return "succeeded";
    case MOTOR_BEMF_CAL_CANCELLED: return "cancelled";
    case MOTOR_BEMF_CAL_FAILED: return "failed";
    default: return "failed";
    }
}

static const char *bemf_error_name(motor_bemf_cal_error_t reason)
{
    switch (reason) {
    case MOTOR_BEMF_CAL_ERR_NONE: return "none";
    case MOTOR_BEMF_CAL_ERR_ADC: return "adc";
    case MOTOR_BEMF_CAL_ERR_RAIL: return "rail";
    case MOTOR_BEMF_CAL_ERR_TIMEOUT: return "timeout";
    case MOTOR_BEMF_CAL_ERR_CONTROL: return "control";
    case MOTOR_BEMF_CAL_ERR_CURVE: return "curve";
    case MOTOR_BEMF_CAL_ERR_STORAGE: return "storage";
    case MOTOR_BEMF_CAL_ERR_START: return "start";
    default: return "start";
    }
}

/* These actions accept one unambiguous boolean, never duplicate/extra keys. */
static bool bemf_bool_query(const char *query, const char *key, bool *value)
{
    size_t n = strlen(key);
    if (strncmp(query, key, n) != 0 || query[n] != '=' || strchr(query, '&') != NULL ||
        !query_encoding_valid(query) || !query_bool_valid(query, key)) return false;
    *value = parse_bool(query, key, false);
    return true;
}

static int bemf_cal_action(const char *query)
{
    bool value = false;
    if (query[0] == '\0') return 1; /* Existing bodyless callers start. */
    if (bemf_bool_query(query, "start", &value) && value) return 1;
    if (bemf_bool_query(query, "reset", &value) && value) return 2;
    if (bemf_bool_query(query, "cancel", &value) && value) return 3;
    return 0;
}

static esp_err_t bemf_cal_get(httpd_req_t *req)
{
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    char json[1024];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used,
                "{\"ok\":true,\"active\":%s,\"progress\":%u,\"total\":%u,"
                "\"valid\":%s,\"stored\":%s,\"use\":%s,"
                "\"runId\":%lu,\"result\":\"%s\",\"error\":\"%s\",\"errorCode\":%d,\"points\":[",
                info.active ? "true" : "false",
                (unsigned)info.step, (unsigned)info.total,
                info.valid ? "true" : "false",
                info.stored ? "true" : "false",
                motor_get_bemf_enabled() ? "true" : "false",
                (unsigned long)info.run_id, bemf_result_name(info.result),
                bemf_error_name(info.reason), (int)info.error_code);
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
    char query[WEB_QUERY_MAX] = { 0 };
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    int action = bemf_cal_action(query);
    if (httpd_req_get_url_query_len(req) >= sizeof(query) ||
        (query_err != ESP_OK && query_err != ESP_ERR_NOT_FOUND) ||
        !query_encoding_valid(query) || action == 0 || req->content_len != 0U)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid calibration action");
    if (action == 3) {
        esp_err_t err = motor_bemf_cal_cancel();
        return send_json(req, err == ESP_OK ? "{\"ok\":true}" :
                         "{\"ok\":false,\"error\":\"cancel failed\"}");
    }
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!actuation_ready() || web_maintenance_active()) {
        xSemaphoreGive(s_control_mutex);
        return send_json(req, "{\"ok\":false,\"error\":\"actuation unavailable\"}");
    }
    if (action == 2) {
        esp_err_t err = motor_bemf_cal_clear();
        xSemaphoreGive(s_control_mutex);
        return send_json(req, err == ESP_OK
                                  ? "{\"ok\":true}"
                                  : "{\"ok\":false,\"error\":\"calibration busy\"}");
    }
    esp_err_t err = motor_bemf_cal_start();
    xSemaphoreGive(s_control_mutex);
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
    char query[WEB_QUERY_MAX] = { 0 };
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    bool enabled;
    if (httpd_req_get_url_query_len(req) >= sizeof(query) || query_err != ESP_OK ||
        !bemf_bool_query(query, "enabled", &enabled))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid enabled");
    const char *error = NULL;
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        error = "control busy";
    } else {
        if (!actuation_ready() || web_maintenance_active()) error = "actuation unavailable";
        else if (settings_bemf_use_save(enabled) != ESP_OK) error = "storage failed";
        else motor_set_bemf_enabled(enabled);
        enabled = motor_get_bemf_enabled();
        xSemaphoreGive(s_control_mutex);
    }
    if (error != NULL) enabled = motor_get_bemf_enabled();
    else web_log_event("BEMF", "регулятор %s", enabled ? "включён" : "выключен, только ШИМ");
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"enabled\":%s,\"error\":\"%s\"}",
             error == NULL ? "true" : "false", enabled ? "true" : "false", error == NULL ? "" : error);
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
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));

    uint8_t speed = 0;
    bool has_speed = parse_u8(query, "speed", &speed);
    if ((query_has_key(query, "speed") && !has_speed) || !query_bool_valid(query, "forward"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid motor parameters");
    bool forward = parse_bool(query, "forward", false);
    bool has_forward = parse_query(query, "forward", (char[16]){0}, 16);

    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!actuation_ready() || web_maintenance_active() || s_cfg.control_source == 0) {
        xSemaphoreGive(s_control_mutex);
        return send_json(req, "{\"ok\":false,\"error\":\"control unavailable\"}");
    }
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
        xSemaphoreGive(s_control_mutex);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "speed out of range");
    }
    if (speed != cur || forward != cur_fwd) {
        web_log_event("Мотор", "скор %u, напр %s", (unsigned)speed,
                      forward ? "вперёд" : "назад");
    }
    esp_err_t err = motor_set_speed(speed, forward);
    xSemaphoreGive(s_control_mutex);
    if (err != ESP_OK) return send_json(req, "{\"ok\":false,\"error\":\"motor rejected\"}");

    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":true,\"speed\":%u,\"forward\":%s}",
             (unsigned)speed, forward ? "true" : "false");
    return send_json(req, json);
}

/* Emergency stop: cut the motor and all sound immediately. Deliberately not
 * gated on the control source or actuation readiness so the safety action is
 * always available while the board is responsive. */
static esp_err_t emergency_post(httpd_req_t *req)
{
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    memset(s_fn, 0, sizeof(s_fn));
    if (s_func_mutex != NULL) (void)xSemaphoreGive(s_func_mutex);
    ++s_control_generation;
    xSemaphoreGive(s_control_mutex);
    web_log_event("Стоп", "ЭКСТРЕННЫЙ СТОП");
    return send_json(req, "{\"ok\":true}");
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
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t fn = 0;
    if (!parse_u8(query, "fn", &fn) || fn >= WEB_FN_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fn out of range");
    }
    bool state = parse_bool(query, "state", false);
    if (!query_bool_valid(query, "state"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid state");
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!actuation_ready() || web_maintenance_active() || s_cfg.control_source == 0) {
        xSemaphoreGive(s_control_mutex);
        return send_json(req, "{\"ok\":false,\"error\":\"control unavailable\"}");
    }
    web_apply_function(fn, state);
    xSemaphoreGive(s_control_mutex);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"fn\":%u,\"state\":%s}",
             (unsigned)fn, state ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t aux_effect_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t ch = 0, pwm_on = 255, pwm_off = 0, mode = 0;
    uint16_t period = 800;
    if (!parse_u8(query, "ch", &ch) || ch >= AUXIO_CH_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch out of range");
    }
    bool enabled = parse_bool(query, "on", false);
    if (!query_bool_valid(query, "on"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid on");
    parse_u8(query, "pwm_on", &pwm_on);
    parse_u8(query, "pwm_off", &pwm_off);
    parse_u8(query, "mode", &mode);
    parse_u16(query, "period", &period);

    auxio_effect_t effect = ((uint32_t)mode < AUXIO_EFFECT_COUNT)
                                ? (auxio_effect_t)mode
                                : AUXIO_EFFECT_STEADY;
    if (s_control_mutex == NULL || xSemaphoreTake(s_control_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!actuation_ready() || web_maintenance_active() || s_cfg.control_source == 0) {
        xSemaphoreGive(s_control_mutex);
        return send_json(req, "{\"ok\":false,\"error\":\"control unavailable\"}");
    }
    esp_err_t err = auxio_set_effect(ch, enabled, pwm_on, pwm_off, effect, period);
    xSemaphoreGive(s_control_mutex);
    if (err != ESP_OK) return send_json(req, "{\"ok\":false,\"error\":\"AUX rejected\"}");
    web_log_event("AUX", "%s: %s, PWM %u/%u, режим %u",
                  WEB_OUT_NAMES[ch], enabled ? "вкл" : "выкл",
                  (unsigned)pwm_on, (unsigned)pwm_off, (unsigned)mode);

    char json[96];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"ch\":%u,\"on\":%s,\"mode\":%u}",
             (unsigned)ch, enabled ? "true" : "false", (unsigned)mode);
    return send_json(req, json);
}

/* ---------- sound library (unpacked WAV files under the audio dir) ---------
 * The "Sound upload" page lists every WAV on the storage with a play button and
 * a per-file volume. A pack upload replaces the whole library. Volumes persist
 * in a small sidecar text file so they survive a reboot and match by file name
 * (the pack index seeds them on unpack). */

#define WEB_LIB_NAME_MAX  (AUDIO_PACK_NAME_MAX + 1)
#define WEB_LIB_JSON_MAX  (96U * 1024U)
#define AUDIO_VOL_FILE    WEB_AUDIO_DIR "/volumes.txt"
/* Upper bound for one upload: the external NOR partition is 16 MB. */
#define AUDIO_PACK_MAX_BYTES (15U * 1024U * 1024U)

typedef struct {
    char name[WEB_LIB_NAME_MAX];
    uint8_t vol;
} web_vol_t;

/* One installed sound for the list endpoint. `name` is first so the existing
 * name comparator (which strcmp's the element pointer) keeps working. */
typedef struct {
    char name[WEB_LIB_NAME_MAX];
    uint32_t size;
} web_lib_item_t;

static char s_preview_name[WEB_LIB_NAME_MAX];

static bool web_name_is_wav(const char *n)
{
    size_t l = strlen(n);
    if (l <= 4U) return false;
    const char *e = n + l - 4U;
    return e[0] == '.' && (e[1] == 'w' || e[1] == 'W') &&
           (e[2] == 'a' || e[2] == 'A') && (e[3] == 'v' || e[3] == 'V');
}

static int web_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* File size via stdio. Some littlefs VFS ports do not fill st_size/st_mode, so
 * stat() is unreliable here; fopen+fseek/ftell works on every mount. */
static uint32_t web_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    uint32_t size = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end > 0) size = (uint32_t)end;
    }
    fclose(f);
    return size;
}

/* Strip directory and a trailing .wav, then sanitize exactly like the packer
 * and the previous upload path. Empty on failure. */
static bool web_lib_base_name(const char *in, char *out, size_t out_len)
{
    char safe[WEB_LIB_NAME_MAX];
    sanitize_name(in, safe, sizeof(safe));
    size_t sl = strlen(safe);
    if (sl > 4U && strcasecmp(safe + sl - 4U, ".wav") == 0) {
        safe[sl - 4U] = '\0';
        sl -= 4U;
    }
    if (sl == 0U || strcmp(safe, ".") == 0 || strcmp(safe, "..") == 0) {
        return false;
    }
    snprintf(out, out_len, "%s", safe);
    return true;
}

/* Read volumes.txt into `out` (cap entries). Returns the entry count (0 if the
 * file is missing or malformed). Caller must hold a storage lease. */
static size_t web_vol_load(web_vol_t *out, size_t cap)
{
    FILE *f = fopen(AUDIO_VOL_FILE, "r");
    if (f == NULL) return 0;
    char line[160];
    size_t n = 0;
    if (fgets(line, sizeof(line), f) == NULL || strncmp(line, "AURAVOL1", 8) != 0) {
        fclose(f);
        return 0;
    }
    while (n < cap && fgets(line, sizeof(line), f) != NULL) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) continue;
        *tab = '\0';
        long vol = strtol(line, NULL, 10);
        char *name = tab + 1;
        char *nl = strchr(name, '\n');
        if (nl != NULL) *nl = '\0';
        if (name[0] == '\0') continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", name);
        out[n].vol = (uint8_t)(vol < 0 ? 0 : (vol > 100 ? 100 : vol));
        n++;
    }
    fclose(f);
    return n;
}

static bool web_vol_get(const web_vol_t *v, size_t n, const char *name, uint8_t *out)
{
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(v[i].name, name) == 0) {
            *out = v[i].vol;
            return true;
        }
    }
    return false;
}

/* Atomic write of the whole sidecar. Caller must hold a storage lease. */
static esp_err_t web_vol_save(const web_vol_t *v, size_t n)
{
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s.tmp", AUDIO_VOL_FILE);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) return ESP_FAIL;
    bool ok = fputs("AURAVOL1\n", f) >= 0;
    for (size_t i = 0; i < n && ok; ++i) {
        ok = fprintf(f, "%u\t%s\n", (unsigned)v[i].vol, v[i].name) > 0;
    }
    if (fflush(f) != 0) ok = false;
    if (fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) { (void)remove(tmp); return ESP_FAIL; }
    if (rename(tmp, AUDIO_VOL_FILE) != 0) { (void)remove(tmp); return ESP_FAIL; }
    return ESP_OK;
}

/* Per-file volume (default 100), safe to call from any task. */
static uint8_t audio_library_volume_lookup(const char *name)
{
    uint8_t vol = 100;
    web_vol_t *v = malloc(sizeof(*v) * AUDIO_PACK_MAX_FILES);
    if (v == NULL) return vol;
    if (storage_access_begin() != ESP_OK) { free(v); return vol; }
    size_t n = web_vol_load(v, AUDIO_PACK_MAX_FILES);
    (void)web_vol_get(v, n, name, &vol);
    storage_access_end();
    free(v);
    return vol;
}

static esp_err_t audio_library_volume_set(const char *name, uint8_t vol)
{
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) return err;
    web_vol_t *v = malloc(sizeof(*v) * AUDIO_PACK_MAX_FILES);
    if (v == NULL) { storage_access_end(); return ESP_ERR_NO_MEM; }
    size_t n = web_vol_load(v, AUDIO_PACK_MAX_FILES);
    size_t i = 0;
    while (i < n && strcmp(v[i].name, name) != 0) ++i;
    if (i < n) {
        v[i].vol = vol;
    } else if (n < AUDIO_PACK_MAX_FILES) {
        snprintf(v[n].name, sizeof(v[n].name), "%s", name);
        v[n].vol = vol;
        n++;
    } else {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = web_vol_save(v, n);
    storage_access_end();
    free(v);
    return err;
}

/* Sum of the installed WAV sizes (used to size a replacing pack upload). */
static uint64_t audio_library_bytes(void)
{
    uint64_t total = 0;
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d == NULL) return 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!web_name_is_wav(ent->d_name)) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", WEB_AUDIO_DIR, ent->d_name);
        uint32_t sz = web_file_size(path);
        if (sz > 0) total += (uint64_t)sz;
    }
    closedir(d);
    return total;
}

/* Delete the whole installed library (files + volume sidecar). Caller must
 * hold a storage lease and have stopped playback. */
static void audio_library_clear(void)
{
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!web_name_is_wav(ent->d_name)) continue;
            char path[320];
            snprintf(path, sizeof(path), "%s/%s", WEB_AUDIO_DIR, ent->d_name);
            (void)remove(path);
        }
        closedir(d);
    }
    (void)remove(AUDIO_VOL_FILE);
    s_preview_name[0] = '\0';
}

/* ---------- browser playback over HTTP (play a sound on the PC) -----------
 * The block editor and the sound page can play a file in the browser of the PC
 * that has the UI open. PCM16 files are served as-is; IMA ADPCM files are
 * decoded on the fly into a PCM16 WAV, since browsers cannot play ADPCM. */

/* Minimal RIFF probe for the two encodings we store. */
static bool web_wav_probe(FILE *f, uint16_t *format, uint16_t *channels,
                          uint32_t *sample_rate, uint16_t *block_align,
                          uint16_t *bits, uint16_t *spb,
                          uint32_t *data_off, uint32_t *data_len)
{
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long flen = ftell(f);
    if (flen < 12 || fseek(f, 0, SEEK_SET) != 0) return false;
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        return false;
    }
    *format = 0; *channels = 0; *sample_rate = 0; *block_align = 0; *bits = 0; *spb = 0;
    long pos = 12;
    bool has_fmt = false, has_data = false;
    while (pos + 8 <= flen) {
        char cid[4];
        uint32_t csize;
        if (fseek(f, pos, SEEK_SET) != 0) return false;
        if (fread(cid, 1, 4, f) != 4 || fread(&csize, 1, 4, f) != 4) return false;
        long body = pos + 8;
        if ((long)((uint32_t)body + csize) > flen) return false;
        if (!memcmp(cid, "fmt ", 4) && csize >= 16U) {
            uint8_t fm[20];
            size_t want = csize < sizeof(fm) ? csize : sizeof(fm);
            if (fseek(f, body, SEEK_SET) != 0 || fread(fm, 1, want, f) != want) return false;
            *format = (uint16_t)(fm[0] | (fm[1] << 8));
            *channels = (uint16_t)(fm[2] | (fm[3] << 8));
            *sample_rate = (uint32_t)fm[4] | ((uint32_t)fm[5] << 8) |
                           ((uint32_t)fm[6] << 16) | ((uint32_t)fm[7] << 24);
            *block_align = (uint16_t)(fm[12] | (fm[13] << 8));
            *bits = (uint16_t)(fm[14] | (fm[15] << 8));
            if (*format == 0x0011U && want >= 20U) {
                *spb = (uint16_t)(fm[18] | (fm[19] << 8));
            }
            has_fmt = true;
        } else if (!memcmp(cid, "data", 4)) {
            *data_off = (uint32_t)body;
            *data_len = csize;
            has_data = true;
        }
        pos = body + (long)csize + (long)(csize & 1U);
    }
    return has_fmt && has_data;
}

/* GET /api/sound-files — list of installed *.wav names for the block editor. */
static esp_err_t sound_files_list_get(httpd_req_t *req)
{
    char *json = malloc(WEB_LIB_JSON_MAX);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    if (storage_access_begin() != ESP_OK) {
        free(json);
        return send_json(req, "{\"ok\":false,\"error\":\"storage busy\"}");
    }
    size_t used = 0;
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "{\"files\":[");
    bool first = true;
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!web_name_is_wav(ent->d_name)) continue;
            char en[WEB_LIB_NAME_MAX * 2];
            json_escape(ent->d_name, en, sizeof(en));
            buf_appendf(json, WEB_LIB_JSON_MAX, &used, "%s\"%s\"", first ? "" : ",", en);
            first = false;
        }
        closedir(d);
    }
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "]}");
    storage_access_end();
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

/* GET /sound-files/<name> — browser-playable PCM16 WAV. */
static esp_err_t sound_file_get(httpd_req_t *req)
{
    const char *prefix = "/sound-files/";
    char dec[WEB_LIB_NAME_MAX * 2];
    url_decode(req->uri + strlen(prefix), dec, sizeof(dec));
    char base[WEB_LIB_NAME_MAX];
    if (!web_lib_base_name(dec, base, sizeof(base))) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    char path[192];
    snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s.wav", base);
    if (storage_access_begin() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage busy");
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    (void)setvbuf(f, NULL, _IOFBF, 8192);

    uint16_t format, channels, block_align, bits, spb;
    uint32_t rate, data_off, data_len;
    if (!web_wav_probe(f, &format, &channels, &rate, &block_align, &bits, &spb,
                       &data_off, &data_len) ||
        !((format == 1U && bits == 16U) || format == 0x0011U)) {
        fclose(f);
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported audio");
    }
    (void)channels; (void)block_align; (void)bits; (void)spb;

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = ESP_OK;
    if (format == 0x0011U) {
        uint32_t blocks = data_len / IMA_ADPCM_BLOCK_ALIGN;
        uint32_t total = blocks * IMA_ADPCM_SAMPLES_PER_BLOCK;
        uint8_t hdr[AUDIO_PACK_WAV_HDR_MAX];
        size_t hlen = audio_pack_wav_header(hdr, sizeof(hdr), total * 2U, rate, 1, 16);
        if (hlen == 0U || fseek(f, (long)data_off, SEEK_SET) != 0 ||
            httpd_resp_send_chunk(req, (const char *)hdr, (ssize_t)hlen) != ESP_OK) {
            err = ESP_FAIL;
        }
        uint8_t raw[IMA_ADPCM_BLOCK_ALIGN];
        int16_t pcm[IMA_ADPCM_SAMPLES_PER_BLOCK];
        for (uint32_t b = 0; err == ESP_OK && b < blocks; ++b) {
            if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) { err = ESP_FAIL; break; }
            int cnt = ima_adpcm_decode_block(raw, sizeof(raw), pcm, IMA_ADPCM_SAMPLES_PER_BLOCK);
            if (cnt <= 0) { err = ESP_FAIL; break; }
            if (httpd_resp_send_chunk(req, (const char *)pcm, (ssize_t)cnt * 2) != ESP_OK) {
                err = ESP_FAIL;
            }
        }
    } else {
        if (fseek(f, 0, SEEK_SET) != 0) {
            err = ESP_FAIL;
        }
        uint8_t buf[2048];
        size_t got;
        while (err == ESP_OK && (got = fread(buf, 1, sizeof(buf), f)) > 0U) {
            if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)got) != ESP_OK) {
                err = ESP_FAIL;
            }
        }
    }
    if (fclose(f) != 0 && err == ESP_OK) { err = ESP_FAIL; }
    storage_access_end();
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
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
    if (!actuation_ready() || web_maintenance_active())
        return send_json(req, "{\"ok\":false,\"error\":\"actuation unavailable\"}");
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));

    /* Library preview: play one file by base name with its per-file volume. */
    char qname[WEB_LIB_NAME_MAX * 2];
    if (parse_query(query, "name", qname, sizeof(qname)) && qname[0] != '\0') {
        char base[WEB_LIB_NAME_MAX];
        if (!web_lib_base_name(qname, base, sizeof(base)))
            return send_json(req, "{\"ok\":false,\"error\":\"bad name\"}");
        char abs_path[192];
        snprintf(abs_path, sizeof(abs_path), WEB_AUDIO_DIR "/%s.wav", base);
        uint8_t vol = audio_library_volume_lookup(base);
        esp_err_t err = ESP_OK;
        if (audio_voice_get_state(s_preview_voice) == AUDIO_VOICE_FINISHED)
            err = audio_voice_alloc_owned(&s_preview_voice);
        if (err == ESP_OK)
            err = audio_voice_play_owned(&s_preview_voice, abs_path, false, vol);
        if (err != ESP_OK) {
            audio_voice_release_owned(s_preview_voice);
            memset(&s_preview_voice, 0, sizeof(s_preview_voice));
            s_preview_name[0] = '\0';
        } else {
            snprintf(s_preview_name, sizeof(s_preview_name), "%s", base);
            web_log_event("Звук", "плей: %s", base);
        }
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
        return send_json(req, json);
    }

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
            esp_err_t err = ESP_OK;
            if (audio_voice_get_state(s_preview_voice) == AUDIO_VOICE_FINISHED)
                err = audio_voice_alloc_owned(&s_preview_voice);
            if (err == ESP_OK)
                err = audio_voice_play_owned(&s_preview_voice, abs_path, false, web_get_voice_volume(slot));
            if (err != ESP_OK) {
                audio_voice_release_owned(s_preview_voice);
                memset(&s_preview_voice, 0, sizeof(s_preview_voice));
            } else {
                s_preview_name[0] = '\0';
            }
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
    audio_voice_release_owned(s_preview_voice);
    memset(&s_preview_voice, 0, sizeof(s_preview_voice));
    s_preview_name[0] = '\0';
    web_log_event("Звук", "стоп (все)");
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t audio_volume_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
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

/* GET /api/audio/library — every installed WAV with its per-file volume. */
static esp_err_t audio_library_get(httpd_req_t *req)
{
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"storage busy\"}");
    }
    web_lib_item_t *items = malloc(sizeof(*items) * AUDIO_PACK_MAX_FILES);
    web_vol_t *vols = malloc(sizeof(*vols) * AUDIO_PACK_MAX_FILES);
    char *json = malloc(WEB_LIB_JSON_MAX);
    if (items == NULL || vols == NULL || json == NULL) {
        free(items); free(vols); free(json);
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    size_t vn = web_vol_load(vols, AUDIO_PACK_MAX_FILES);
    size_t n = 0;
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while (n < AUDIO_PACK_MAX_FILES && (ent = readdir(d)) != NULL) {
            if (!web_name_is_wav(ent->d_name)) continue;
            size_t l = strlen(ent->d_name);
            if (l - 4U >= WEB_LIB_NAME_MAX) continue;
            memcpy(items[n].name, ent->d_name, l - 4U);
            items[n].name[l - 4U] = '\0';
            items[n].size = 0;
            n++;
        }
        closedir(d);
    }
    /* Read sizes with the directory stream closed (a plain fopen/ftell per
     * file, then sort). */
    for (size_t i = 0; i < n; ++i) {
        char path[320];
        snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s.wav", items[i].name);
        items[i].size = web_file_size(path);
    }
    storage_access_end();
    qsort(items, n, sizeof(*items), web_name_cmp);

    size_t used = 0;
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "{\"ok\":true,\"count\":%u,\"preview\":\"",
                (unsigned)n);
    char pe[WEB_LIB_NAME_MAX * 2];
    json_escape(s_preview_name, pe, sizeof(pe));
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "%s\",\"files\":[", pe);
    for (size_t i = 0; i < n; ++i) {
        uint8_t vol = 100;
        (void)web_vol_get(vols, vn, items[i].name, &vol);
        char en[WEB_LIB_NAME_MAX * 2];
        json_escape(items[i].name, en, sizeof(en));
        buf_appendf(json, WEB_LIB_JSON_MAX, &used,
                    "%s{\"name\":\"%s\",\"size\":%u,\"vol\":%u}",
                    i == 0U ? "" : ",", en, (unsigned)items[i].size, (unsigned)vol);
    }
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "]}");
    err = send_json(req, json);
    free(items);
    free(vols);
    free(json);
    return err;
}

/* POST /api/audio/library/volume?name=NAME&vol=0..100 */
static esp_err_t audio_library_volume_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char raw[WEB_LIB_NAME_MAX * 2];
    uint8_t vol = 100;
    if (!parse_query(query, "name", raw, sizeof(raw)) || raw[0] == '\0' ||
        !parse_u8(query, "vol", &vol) || vol > 100U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid volume");
    }
    char base[WEB_LIB_NAME_MAX];
    if (!web_lib_base_name(raw, base, sizeof(base)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    esp_err_t err = audio_library_volume_set(base, vol);
    if (err != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    if (s_preview_name[0] != '\0' && strcmp(s_preview_name, base) == 0 &&
        audio_voice_get_state(s_preview_voice) != AUDIO_VOICE_FINISHED) {
        (void)audio_voice_set_volume_live(s_preview_voice.voice, vol);
    }
    return send_json(req, "{\"ok\":true}");
}

/* The asynchronous transfer task owns its FILE and buffer until close. No
 * second writer can outlive an aborted request or its stack context. */
#define PIPE_BUF_SIZE      8192
/* Upload progress log cadence (bytes); overridable for host tests. */
#ifndef WEB_UP_PROGRESS_STEP
#define WEB_UP_PROGRESS_STEP 262144
#endif

static esp_err_t pipe_upload(httpd_req_t *req, FILE *f, int *out_total)
{
    uint8_t *buf = malloc(PIPE_BUF_SIZE);
    if (buf == NULL) return ESP_ERR_NO_MEM;
    int64_t deadline = s_transfer_deadline != 0 ? s_transfer_deadline : esp_timer_get_time() + 120000000;
    int remaining = (int)req->content_len;
    int total = 0;
    int idle = 0;
    esp_err_t err = s_pipe_write_err_inject ? ESP_FAIL : ESP_OK;
    uint32_t next_prog = 0;
    uint32_t iterations = 0;
    while (err == ESP_OK && remaining > 0) {
        if (s_pipe_iter_cap != 0U && iterations++ >= s_pipe_iter_cap) { err = ESP_ERR_TIMEOUT; break; }
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        size_t want = remaining > PIPE_BUF_SIZE ? PIPE_BUF_SIZE : (size_t)remaining;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r > 0) {
            idle = 0;
            if ((size_t)r > want || WEB_FWRITE(buf, 1, (size_t)r, f) != (size_t)r) {
                err = ESP_FAIL; break;
            }
            total += r;
            remaining -= r;
            s_up_received = total;
            if (total - (int)next_prog >= WEB_UP_PROGRESS_STEP) {
                next_prog = (uint32_t)total;
                ESP_LOGI(TAG, "UP: progress %d/%d", total, (int)req->content_len);
            }
        } else if ((r == 0 || r == -3) && ++idle < 4) {
            ESP_LOGI(TAG, "UP: recv idle #%d (got %d/%d)", idle, total, (int)req->content_len);
            continue;
        } else {
            ESP_LOGW(TAG, "UP: recv failed (%d, got %d/%d)", r, total, (int)req->content_len);
            err = ESP_FAIL;
            break;
        }
    }

    *out_total = total;
    free(buf);
    return remaining == 0 && err == ESP_OK ? ESP_OK : err == ESP_OK ? ESP_FAIL : err;
}

/* Private files are never WAV orphan-recovery candidates. */
static FILE *create_private_file(char *path, size_t path_len, char *rel, size_t rel_len,
                                 const char *extension)
{
    for (unsigned attempt = 0; attempt < 64U; ++attempt) {
        snprintf(rel, rel_len, "audio/u%08lx_%08lx.%s",
                 (unsigned long)(uint32_t)esp_timer_get_time(), (unsigned long)++s_file_sequence,
                 extension);
        snprintf(path, path_len, WEB_USERDATA_DIR "/%s", rel);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            FILE *f = fdopen(fd, "wb");
            if (f == NULL) { close(fd); (void)remove(path); path[0] = rel[0] = '\0'; }
            return f;
        }
        if (errno != EEXIST) break;
    }
    path[0] = rel[0] = '\0';
    return NULL;
}

static FILE *create_staged_wav(char *path, size_t path_len, char *rel, size_t rel_len)
{
    return create_private_file(path, path_len, rel, rel_len, "tmp");
}

typedef struct {
    char staged[160];
    char canonical[160];
    char backup[160];
    bool original;
    bool published;
} wav_file_tx_t;

typedef struct {
    wav_file_tx_t files[SETTINGS_MAX_TRACKS];
    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    settings_track_t previous[SETTINGS_MAX_TRACKS];
    settings_config_t previous_cfg;
    size_t count, previous_count, file_count;
    char journal[160];
    bool graph_asset_conflict;
} wav_tx_t;

static bool wav_artifacts_clean(wav_tx_t *tx)
{
    if (tx == NULL) return true;
    bool clean = true;
    for (size_t i = 0; i < tx->file_count; ++i) {
        char *paths[] = {tx->files[i].staged, tx->files[i].backup};
        for (size_t j = 0; j < 2U; ++j) {
            if (paths[j][0]) {
                if (remove(paths[j]) != 0 && errno != ENOENT) clean = false;
                else paths[j][0] = '\0';
            }
        }
    }
    if (clean && tx->journal[0]) {
        if (remove(tx->journal) != 0 && errno != ENOENT) clean = false;
        else tx->journal[0] = '\0';
    }
    return clean;
}

static bool wav_files_restore(wav_tx_t *tx, bool metadata_uncertain)
{
    if (tx == NULL) return true;
    bool restored = true;
    for (size_t n = tx->file_count; n > 0U; --n) {
        wav_file_tx_t *file = &tx->files[n - 1U];
        if (!file->published) continue;
        if (file->original) {
            if (rename(file->backup, file->canonical) == 0) {
                file->backup[0] = '\0';
                file->published = false;
            } else restored = false;
        } else if (!metadata_uncertain) {
            if (remove(file->canonical) == 0 || errno == ENOENT) file->published = false;
            else restored = false;
        } else {
            /* An uncertain NVS commit may reference this newly created path. */
            restored = false;
        }
    }
    return restored;
}

static esp_err_t wav_files_prepare(wav_tx_t *tx)
{
    uint64_t required = 65536U; /* bounded reserve for manifest + recovery record */
    for (size_t i = 0; i < tx->file_count; ++i) {
        const char *name = strrchr(tx->files[i].canonical, '/');
        bool referenced = true;
        esp_err_t ref_err = sg_store_file_referenced(name ? name + 1 : tx->files[i].canonical, &referenced);
        if (ref_err != ESP_OK) return ref_err;
        if (referenced) { tx->graph_asset_conflict = true; return ESP_ERR_INVALID_STATE; }
        struct stat st;
        if (stat(tx->files[i].canonical, &st) == 0) {
            if (!S_ISREG(st.st_mode) || st.st_size < 0) return ESP_ERR_INVALID_STATE;
            tx->files[i].original = true;
            if ((uint64_t)st.st_size > UINT64_MAX - required) return ESP_ERR_NO_MEM;
            required += (uint64_t)st.st_size;
        } else if (errno != ENOENT) return ESP_FAIL;
    }
    uint64_t free_bytes = 0;
    if (storage_get_free_bytes(&free_bytes) != ESP_OK || free_bytes < required) return ESP_ERR_NO_MEM;
    uint8_t *buffer = malloc(8192);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < tx->file_count && err == ESP_OK; ++i) {
        wav_file_tx_t *file = &tx->files[i];
        if (!file->original) continue;
        char rel[128];
        FILE *src = fopen(file->canonical, "rb");
        FILE *dst = create_private_file(file->backup, sizeof(file->backup), rel, sizeof(rel), "bak");
        if (src == NULL || dst == NULL) err = ESP_FAIL;
        if (src != NULL && dst != NULL) {
            size_t n;
            while ((n = fread(buffer, 1, 8192, src)) > 0U) {
                if (transfer_expired() || WEB_FWRITE(buffer, 1, n, dst) != n) { err = ESP_FAIL; break; }
            }
            if (ferror(src)) err = ESP_FAIL;
        }
        if (src != NULL && fclose(src) != 0) err = ESP_FAIL;
        if (dst != NULL) {
            if (WEB_FFLUSH(dst) != 0 || fsync(fileno(dst)) != 0) err = ESP_FAIL;
            if (WEB_FCLOSE(dst) != 0) err = ESP_FAIL;
        }
    }
    free(buffer);
    if (err != ESP_OK) return err;
    char rel[128];
    FILE *journal = create_private_file(tx->journal, sizeof(tx->journal), rel, sizeof(rel), "bak");
    if (journal == NULL) return ESP_FAIL;
    /* Versioned recovery evidence, not an automatically replayed commit:
     * magic, counts, old config/tracks, then canonical-to-backup path records. */
    const char magic[8] = "WAVTXN1";
    uint32_t counts[2] = {(uint32_t)tx->previous_count, (uint32_t)tx->file_count};
    bool ok = WEB_FWRITE(magic, 1, sizeof(magic), journal) == sizeof(magic) &&
        WEB_FWRITE(counts, 1, sizeof(counts), journal) == sizeof(counts) &&
        WEB_FWRITE(&tx->previous_cfg, 1, sizeof(tx->previous_cfg), journal) == sizeof(tx->previous_cfg) &&
        WEB_FWRITE(tx->previous, sizeof(tx->previous[0]), tx->previous_count, journal) == tx->previous_count &&
        WEB_FWRITE(tx->files, sizeof(tx->files[0]), tx->file_count, journal) == tx->file_count;
    if (WEB_FFLUSH(journal) != 0 || fsync(fileno(journal)) != 0) ok = false;
    if (WEB_FCLOSE(journal) != 0) ok = false;
    return ok && !transfer_expired() ? ESP_OK : ESP_FAIL;
}

static esp_err_t wav_files_publish(wav_tx_t *tx)
{
    /* Preflight every destination before publishing even the first file. */
    for (size_t i = 0; i < tx->file_count; ++i) {
        const char *name = strrchr(tx->files[i].canonical, '/');
        bool referenced = true;
        esp_err_t err = sg_store_file_referenced(name ? name + 1 : tx->files[i].canonical, &referenced);
        if (err != ESP_OK) return err;
        if (referenced) { tx->graph_asset_conflict = true; return ESP_ERR_INVALID_STATE; }
    }
    for (size_t i = 0; i < tx->file_count; ++i) {
        if (transfer_expired() || rename(tx->files[i].staged, tx->files[i].canonical) != 0) return ESP_FAIL;
        tx->files[i].staged[0] = '\0';
        tx->files[i].published = true;
    }
    return ESP_OK;
}

static esp_err_t wav_tx_abort(httpd_req_t *req, wav_tx_t *tx, bool metadata_attempted,
                              bool boot_uncertain, const char *reason)
{
    bool metadata_restored = true;
    if (tx != NULL && metadata_attempted) {
        metadata_restored = settings_tracks_save(tx->previous, tx->previous_count) == ESP_OK;
        if (metadata_restored && settings_manifest_sync() != ESP_OK) metadata_restored = false;
    }
    bool restored = wav_files_restore(tx, !metadata_restored);
    bool clean = restored && metadata_restored && !boot_uncertain && wav_artifacts_clean(tx);
    char json[512];
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"error\":\"%s\",\"partial\":%s,\"metadata_uncertain\":%s,\"boot_selection_uncertain\":%s,\"recovery\":\"%s\"}",
             reason, clean ? "false" : "true", metadata_restored ? "false" : "true",
             boot_uncertain ? "true" : "false",
             tx != NULL ? tx->journal : "");
    if (!clean && tx != NULL) ESP_LOGE(TAG, "WAV recovery record retained: %s", tx->journal);
    free(tx);
    httpd_resp_set_status(req, strncmp(reason, "graph asset", 11) == 0 ? "409 Conflict" : "500 Internal Server Error");
    return send_json(req, json);
}

static esp_err_t audio_upload_post(httpd_req_t *req)
{
    if (!storage_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    }
    if (req->content_len <= 0 || req->content_len > UPLOAD_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }

    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0;
    if ((strstr(query, "slot=") != NULL && !parse_u8(query, "slot", &slot)) ||
        slot > SETTINGS_MAX_TRACKS)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");

    char hdr[160] = { 0 };
    esp_err_t header_err = httpd_req_get_hdr_value_str(req, "X-File-Name", hdr, sizeof(hdr));
    if (header_err != ESP_OK && header_err != ESP_ERR_NOT_FOUND)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename header");

    char decoded[128] = { 0 };
    char encoded_name[sizeof(hdr) + 5];
    snprintf(encoded_name, sizeof(encoded_name), "n=%s", hdr);
    if (!parse_query(encoded_name, "n", decoded, sizeof(decoded)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename encoding");

    char name[64] = { 0 };
    if (strlen(decoded) >= sizeof(name))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename too long");
    if (decoded[0] != '\0') {
        sanitize_name(decoded, name, sizeof(name));
    }

    /* Load the track list first: it is also used to auto-assign a slot when the
     * request did not carry a valid one (slot 0 is not displayable/deletable). */
    wav_tx_t *tx = calloc(1, sizeof(*tx));
    if (tx == NULL) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no transaction memory");
    settings_track_t *tracks = tx->tracks;
    size_t count = 0;
    esp_err_t load_err = settings_tracks_load(tracks, &count);
    if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) {
        free(tx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "tracks load failed");
    }
    bool have_tracks = load_err == ESP_OK;
    if (!have_tracks) count = 0;
    if (slot == 0U) {
        for (uint16_t cand = 1U; cand <= SETTINGS_MAX_TRACKS && slot == 0U; ++cand) {
            bool used = false;
            for (size_t i = 0; have_tracks && i < count; ++i) {
                if (tracks[i].slot == cand) { used = true; break; }
            }
            if (!used) { slot = cand; }
        }
        if (slot == 0U) {
            free(tx);
            return send_json(req, "{\"ok\":false,\"error\":\"track slots full\"}");
        }
    }
    if (name[0] == '\0') {
        snprintf(name, sizeof(name), "slot%u.wav", (unsigned)slot);
    }

    tx->file_count = 1;
    tx->previous_count = count;
    memcpy(tx->previous, tracks, sizeof(tx->previous));
    tx->previous_cfg = s_cfg;
    char *path = tx->files[0].staged;
    /* Relative path (no /userdata prefix): matches the V0 convention and the
     * web UI which strips the "audio/" prefix for display. */
    char rel_file[128];
    char staging_rel[128];
    snprintf(rel_file, sizeof(rel_file), "audio/%s", name);
    snprintf(tx->files[0].canonical, sizeof(tx->files[0].canonical), WEB_USERDATA_DIR "/%s", rel_file);
    FILE *f = create_staged_wav(path, sizeof(tx->files[0].staged), staging_rel, sizeof(staging_rel));
    if (f == NULL) { free(tx); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); }

    ESP_LOGI(TAG, "UP: enter slot=%u name=%s len=%d", (unsigned)slot, name, (int)req->content_len);

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

    /* Always close the stream before removing the file: a short-circuit
     * would leave the handle open and make remove() fail on the host and on
     * LittleFS. */
    int close_rc = fflush(f);
    close_rc |= fsync(fileno(f));
    close_rc |= fclose(f);
    if (close_rc != 0) {
        ESP_LOGE(TAG, "UP: flush/close failed total=%d/%d", total, (int)req->content_len);
        (void)remove(path);
        free(tx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }

    if (perr != ESP_OK || transfer_expired()) {
        ESP_LOGE(TAG, "UP: fail total=%d/%d err=%s", total, (int)req->content_len, esp_err_to_name(perr));
        (void)remove(path);
        free(tx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }
    ESP_LOGI(TAG, "UP: done total=%d", total);

    if (audio_validate_wav(path) != ESP_OK || transfer_expired()) {
        (void)remove(path);
        free(tx);
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
        free(tx);
        return send_json(req, "{\"ok\":false,\"error\":\"track slots full\"}");
    }
    count = w;
    tx->count = count;

    esp_err_t file_err = wav_files_prepare(tx);
    if (file_err != ESP_OK)
        return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project" : "backup/space preflight failed");
    file_err = wav_files_publish(tx);
    if (file_err != ESP_OK)
        return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project" : "canonical publication failed");

    esp_err_t save_err = settings_tracks_save(tracks, count);
    ESP_LOGI(TAG, "UP: track bound slot=%u count=%u save=%s",
             (unsigned)slot, (unsigned)count, esp_err_to_name(save_err));
    if (save_err != ESP_OK || transfer_expired()) {
        ESP_LOGE(TAG, "UP: tracks save failed, keeping old track");
        return wav_tx_abort(req, tx, true, false, "tracks save failed");
    }
    if (settings_manifest_sync() != ESP_OK || transfer_expired())
        return wav_tx_abort(req, tx, true, false, "manifest save failed");
    bool cleanup_pending = !wav_artifacts_clean(tx);
    char recovery[160];
    snprintf(recovery, sizeof(recovery), "%s", tx->journal);
    free(tx);

    /* Only this canonical asset was explicitly replaced. Never unlink an old
     * different slot path: saved projects and other slots may still use it. */

    char esc_file[320];
    json_escape(rel_file, esc_file, sizeof(esc_file));
    char esc_label[128];
    json_escape(name, esc_label, sizeof(esc_label));
    char json[768];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"slot\":%u,\"file\":\"%s\",\"label\":\"%s\",\"enabled\":true,\"bytes\":%d,\"recovery_cleanup_pending\":%s,\"recovery\":\"%s\"}",
             (unsigned)slot, esc_file, esc_label, (int)req->content_len, cleanup_pending ? "true" : "false", recovery);
    return send_json(req, json);
}

static esp_err_t audio_track_delete_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0;
    parse_u8(query, "slot", &slot);

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    if (!parse_u8(query, "slot", &slot) || slot < 1U || slot > SETTINGS_MAX_TRACKS)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
    if (settings_tracks_load(tracks, &count) == ESP_OK) {
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
    } else {
        return send_json(req, "{\"ok\":false,\"error\":\"tracks load failed\"}");
    }

    esp_err_t save_err = settings_tracks_save(tracks, count);
    if (save_err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"tracks save failed\"}");
    }

    /* Unbind, do not unlink a path potentially owned by a sound project. */

    if (s_cfg.active_slot == slot) {
        s_cfg.active_slot = 0;
        (void)settings_save(&s_cfg);
        audio_voice_release_owned(s_preview_voice);
        memset(&s_preview_voice, 0, sizeof(s_preview_voice));
    }

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"slot\":%u}", (unsigned)slot);
    return send_json(req, json);
}

static esp_err_t track_category_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
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
    char query[WEB_QUERY_MAX] = { 0 };
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
    char query[WEB_QUERY_MAX] = { 0 };
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
    s_func_bind_present = true;
    func_apply_output_locked(0);
    if (n == 0U) {
        for (size_t f = 0; f < SETTINGS_FUNC_MAP_COUNT; ++f) {
            audio_voice_release_owned(s_legacy_voice[f][0]);
            audio_voice_release_owned(s_legacy_voice[f][1]);
        }
        memset(s_legacy_voice, 0, sizeof(s_legacy_voice));
    }
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
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
        if (sound_graph_active() && (s_func_bind[idx].target_type == FUNC_TARGET_SOUND || s_func_bind[idx].target_type == FUNC_TARGET_SLOT))
            return graph_error(req, "409 Conflict", "graph mode: use /sound-editor/blocks.html");
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
    if (sound_graph_active() && (type == FUNC_TARGET_SOUND || type == FUNC_TARGET_SLOT))
        return graph_error(req, "409 Conflict", "graph mode: use /sound-editor/blocks.html");
    if (!parse_u8(query, "id", &id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
    }
    /* Reject a target id outside its type's domain up front (defence in depth:
     * func_eval already bounds OUTPUT, but a bad persisted record should never
     * be created in the first place). */
    if ((type == FUNC_TARGET_OUTPUT && id > 8U) ||
        (type == FUNC_TARGET_LOGIC && (id < 1U || id > 8U)) ||
        (type == FUNC_TARGET_SLOT && (id < 1U || id > SETTINGS_MAX_TRACKS)) ||
        (type == FUNC_TARGET_SOUND && id >= SG_MAX_EFFECTS)) {
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
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char view[16] = { 0 };
    if (parse_query(query, "view", view, sizeof(view)) &&
        (strcmp(view, "bind") == 0 || strcmp(view, "matrix") == 0)) {
        return func_bind_get(req);
    }

    char json[2048];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"map\":[");
    for (uint8_t f = 0; f < WEB_FN_COUNT; ++f) {
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
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (parse_bool(query, "bind", false)) {
        return func_map_post_binding(req, query);
    }
    if (sound_graph_active()) return graph_error(req, "409 Conflict", "graph mode: use /sound-editor/blocks.html");
    uint8_t fn = 0, sa = 0, sb = 0, dir = 0, spd = 0;
    uint16_t aux = 0;
    if (!parse_u8(query, "fn", &fn) || fn >= WEB_FN_COUNT) {
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
    char query[WEB_QUERY_MAX] = { 0 };
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
    char query[WEB_QUERY_MAX] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query");
    if (!query_encoding_valid(query))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query encoding");

    /* Parse into a local copy: on a validation error nothing may be applied,
     * otherwise a later deferred save would persist the partial change. */
    settings_config_t next = s_cfg;
    next.wifi_mode = 1; /* access point only */
    char buf[SETTINGS_PASS_MAX] = { 0 };
    if (query_has_key(query, "ap_ssid") && !parse_query(query, "ap_ssid", buf, sizeof(buf)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid SSID");
    if (parse_query(query, "ap_ssid", buf, sizeof(buf)) && buf[0]) {
        if (strlen(buf) > 32U)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID exceeds 32 bytes");
        strncpy(next.ap_ssid, buf, sizeof(next.ap_ssid) - 1);
        next.ap_ssid[sizeof(next.ap_ssid) - 1] = '\0';
    }
    memset(buf, 0, sizeof(buf));
    if (query_has_key(query, "ap_password") && !parse_query(query, "ap_password", buf, sizeof(buf)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid password");
    if (parse_query(query, "ap_password", buf, sizeof(buf)) && buf[0]) {
        if (strlen(buf) < 8U) {
            return send_json(req, "{\"ok\":false,\"error\":\"ap password must be >=8 chars\"}");
        }
        if (strlen(buf) == 64U) {
            for (size_t i = 0; i < 64U; ++i)
                if (!isxdigit((unsigned char)buf[i]))
                    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "64 byte PSK must be hex");
        }
        strncpy(next.ap_password, buf, sizeof(next.ap_password) - 1);
        next.ap_password[sizeof(next.ap_password) - 1] = '\0';
    }
    if (parse_bool(query, "ap_password_clear", false)) {
        next.ap_password[0] = '\0';
    }
    /* Configurable access point address (default 192.168.100.1). */
    memset(buf, 0, sizeof(buf));
    if (query_has_key(query, "ap_ip") && !parse_query(query, "ap_ip", buf, sizeof(buf)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid IP");
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

    if (web_maintenance_begin(false) != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}");
    if (settings_save(&next) != ESP_OK) {
        web_maintenance_end();
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    s_cfg = next;
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
    if (web_maintenance_begin(false) != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}");
    settings_config_t def = { 0 };
    def.wifi_mode = 1;
    def.port = 80;
    def.hold = 0;
    strncpy(def.ap_ip, AP_IP_DEFAULT, sizeof(def.ap_ip) - 1);
    strncpy(def.ap_ssid, "ADDITIPUS AURA-X", sizeof(def.ap_ssid) - 1);
    def.master_volume = 20;
    def.engine_volume = 20;
    def.effects_volume = 20;
    if (settings_save(&def) != ESP_OK) {
        web_maintenance_end();
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
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
    if (web_maintenance_begin(false) != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}");
    web_log_event("Сброс", "заводские настройки, перезагрузка");
    if (settings_factory_reset() != ESP_OK) {
        portENTER_CRITICAL(&s_control_state_mux);
        s_actuation_ready = false;
        portEXIT_CRITICAL(&s_control_state_mux);
        web_maintenance_end();
        return graph_error(req, "500 Internal Server Error", "reset failed; actuation disabled; no reboot");
    }
    /* NVS and the external selector cannot form one atomic reset transaction. */
    if ((storage_is_mounted() || sound_graph_active()) && sg_store_select_legacy() != ESP_OK) {
        portENTER_CRITICAL(&s_control_state_mux);
        s_actuation_ready = false;
        portEXIT_CRITICAL(&s_control_state_mux);
        web_maintenance_end();
        return graph_error(req, "500 Internal Server Error", "settings reset but graph selector reset failed; actuation disabled; no reboot");
    }
    sound_graph_deactivate();
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
    char query[WEB_QUERY_MAX] = { 0 };
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
        if (s_transfer_deadline != 0 && esp_timer_get_time() >= s_transfer_deadline) return -1;
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

/* POST /api/audio/pack — unpack an AURA Sound Pack straight from the request
 * body: validate the index, delete the whole old library and stream every PCM16
 * payload into /userdata/audio/<name>.wav with a generated WAV header. */
static esp_err_t audio_pack_upload_post(httpd_req_t *req)
{
    const char *errmsg = "invalid sound pack";
    if (!storage_is_mounted())
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    if (req->content_len < AUDIO_PACK_HDR_LEN || req->content_len > AUDIO_PACK_MAX_BYTES)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    /* A pack can be far larger than a single WAV, so give the transfer more
     * than the generic 120 s window (the UI XHR timeout matches). */
    s_transfer_deadline = esp_timer_get_time() + 600000000;

    ota_stream_t *st = calloc(1, sizeof(*st));
    uint8_t *buf = malloc(8192);
    audio_pack_entry_t *idx = NULL;
    if (st == NULL || buf == NULL) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    st->req = req;
    st->remaining = (int)req->content_len;

    uint8_t hdr[AUDIO_PACK_HDR_LEN];
    audio_pack_header_t ph;
    if (ota_stream_read(st, hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        !audio_pack_header_parse(hdr, sizeof(hdr), &ph)) {
        errmsg = "invalid pack header";
        goto fail;
    }
    idx = calloc(ph.count, sizeof(*idx));
    if (idx == NULL) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    uint32_t index_end = AUDIO_PACK_HDR_LEN;
    uint32_t payload_off = ph.data_off;
    uint64_t total_data = 0;
    for (uint32_t i = 0; i < ph.count; ++i) {
        uint8_t eh[AUDIO_PACK_ENTRY_HDR_LEN];
        if (ota_stream_read(st, eh, sizeof(eh)) != (int)sizeof(eh)) { errmsg = "truncated index"; goto fail; }
        index_end += AUDIO_PACK_ENTRY_HDR_LEN;
        uint8_t nlen = 0;
        if (!audio_pack_entry_head(eh, req->content_len, &idx[i], &nlen)) { errmsg = "bad entry"; goto fail; }
        uint8_t nm[AUDIO_PACK_NAME_MAX + 1];
        if (ota_stream_read(st, nm, nlen) != (int)nlen) { errmsg = "truncated name"; goto fail; }
        index_end += nlen;
        if (!audio_pack_entry_name(&idx[i], nm, nlen)) { errmsg = "bad name"; goto fail; }
        char base[WEB_LIB_NAME_MAX];
        if (!web_lib_base_name(idx[i].name, base, sizeof(base))) { errmsg = "bad name"; goto fail; }
        for (uint32_t j = 0; j < i; ++j) {
            if (strcasecmp(idx[j].name, base) == 0) { errmsg = "duplicate sound name"; goto fail; }
        }
        snprintf(idx[i].name, sizeof(idx[i].name), "%s", base);
        if (idx[i].data_off != payload_off) { errmsg = "non-contiguous payload"; goto fail; }
        payload_off += idx[i].data_len;
        if ((uint64_t)payload_off > (uint64_t)req->content_len) { errmsg = "payload past end"; goto fail; }
        total_data += idx[i].data_len;
    }
    if (index_end != ph.data_off) { errmsg = "index length mismatch"; goto fail; }
    if ((uint64_t)payload_off != (uint64_t)req->content_len) { errmsg = "size mismatch"; goto fail; }

    uint64_t freeb = 0;
    (void)storage_get_free_bytes(&freeb);
    uint64_t need = total_data + (uint64_t)ph.count * 4096ULL + 262144ULL;
    if (freeb + audio_library_bytes() < need) {
        free(idx);
        free(st); free(buf);
        return send_json(req, "{\"ok\":false,\"error\":\"not enough space\"}");
    }

    /* A pack replaces the whole library: stop the preview, delete every old
     * file, then stream the new ones in. */
    audio_voice_release_owned(s_preview_voice);
    memset(&s_preview_voice, 0, sizeof(s_preview_voice));
    audio_library_clear();

    s_up_slot = 0;
    size_t done = 0;
    for (uint32_t i = 0; i < ph.count; ++i) {
        char path[192];
        snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s.wav", idx[i].name);
        FILE *f = fopen(path, "wb");
        bool ok = f != NULL;
        if (ok) (void)setvbuf(f, NULL, _IOFBF, 8192);
        uint8_t wh[AUDIO_PACK_WAV_HDR_MAX];
        size_t whl = audio_pack_wav_header(wh, sizeof(wh), idx[i].data_len,
                                           idx[i].sample_rate, idx[i].channels, idx[i].bits);
        if (ok) ok = whl > 0U && fwrite(wh, 1, whl, f) == whl;
        uint32_t left = idx[i].data_len;
        while (ok && left > 0U) {
            size_t want = left > 8192U ? 8192U : (size_t)left;
            int r = ota_stream_read(st, buf, want);
            if (r <= 0) { ok = false; break; }
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { ok = false; break; }
            left -= (uint32_t)r;
            s_up_received = (int)req->content_len - st->remaining - (int)(st->len - st->pos);
        }
        if (ok && fflush(f) != 0) ok = false;
        if (f != NULL && fsync(fileno(f)) != 0) ok = false;
        if (f != NULL && fclose(f) != 0) ok = false;
        if (!ok) { (void)remove(path); errmsg = "write failed"; break; }
        done++;
    }

    if (done != ph.count || st->remaining != 0 || st->pos != st->len ||
        (s_transfer_deadline != 0 && esp_timer_get_time() >= s_transfer_deadline)) {
        for (uint32_t i = 0; i < done; ++i) {
            char path[192];
            snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s.wav", idx[i].name);
            (void)remove(path);
        }
        goto fail;
    }

    {
        web_vol_t *v = malloc(sizeof(*v) * ph.count);
        if (v != NULL) {
            for (uint32_t i = 0; i < ph.count; ++i) {
                snprintf(v[i].name, sizeof(v[i].name), "%s", idx[i].name);
                v[i].vol = idx[i].volume;
            }
            (void)web_vol_save(v, ph.count);
            free(v);
        }
    }

    ESP_LOGI(TAG, "PACK: %lu file(s), %lu bytes",
             (unsigned long)ph.count, (unsigned long)total_data);
    web_log_event("Звук", "пакет: %lu звук(ов)", (unsigned long)ph.count);
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"files\":%lu,\"bytes\":%lu}",
             (unsigned long)ph.count, (unsigned long)total_data);
    free(idx);
    free(st);
    free(buf);
    return send_json(req, json);

fail:
    free(idx);
    free(st);
    free(buf);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errmsg);
}

/* POST /api/ota/update
 * - plain ESP image          -> written to the next OTA partition;
 * - "AURAOTA2" container      -> firmware to the OTA partition and each sound
 *                               file to /userdata, then reboot. Lets one
 *                               upload carry both a firmware and its sounds. */
static esp_err_t ota_update_post(httpd_req_t *req)
{
    s_transfer_reboot = false;
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
    if (!combined && memcmp(hdr, OTA_CONTAINER_MAGIC, OTA_CONTAINER_MAGIC_LEN) == 0) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid container header");
    }
    uint32_t fw_len;
    uint32_t n_files = 0;
    if (combined) {
        fw_len = ch.fw_len;
        n_files = ch.n_files;
        if (n_files > SETTINGS_MAX_TRACKS ||
            fw_len > (uint32_t)req->content_len - OTA_CONTAINER_HDR_LEN ||
            (n_files > 0U && !storage_is_mounted())) {
            free(st); free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid container size/storage");
        }
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
    if (!combined && hdr[0] != 0xE9U) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid firmware header");
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
             update->label, (int)req->content_len, combined ? 1 : 0,
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
    uint32_t files_ok = 0;
    wav_tx_t *tx = n_files > 0U ? calloc(1, sizeof(*tx)) : NULL;
    if (n_files > 0U && tx == NULL) {
        s_up_active = false; free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no transaction memory; boot unchanged");
    }
    settings_track_t *tracks = tx != NULL ? tx->tracks : NULL;
    size_t tn = 0;
    bool metadata_attempted = false;
    uint32_t incoming_slots = 0U;
    if (n_files > 0U) {
        esp_err_t load_err = settings_tracks_load(tracks, &tn);
        if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) {
            s_up_active = false; free(st); free(buf); free(tx);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "tracks load failed; boot unchanged");
        }
        if (load_err != ESP_OK) tn = 0;
        tx->file_count = n_files;
        memcpy(tx->previous, tracks, sizeof(tx->previous));
        tx->previous_count = tn;
        tx->previous_cfg = s_cfg;
    }
    if (combined && n_files > 0U && storage_is_mounted()) {
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
            if (memchr(raw, '\0', f.name_len) != NULL ||
                memchr(label, '\0', f.label_len) != NULL || f.data_len > UPLOAD_MAX ||
                f.data_len > (uint32_t)(st->remaining + (int)(st->len - st->pos))) break;

            char safe[80];
            ota_safe_name(raw, safe, sizeof(safe));
            snprintf(tx->files[i].canonical, sizeof(tx->files[i].canonical), WEB_AUDIO_DIR "/%s", safe);
            for (uint32_t j = 0; j < i; ++j) {
                if (strcmp(tx->files[j].canonical, tx->files[i].canonical) == 0) fail = true;
            }
            if (fail) break;
            uint64_t free_bytes = 0;
            if (storage_get_free_bytes(&free_bytes) != ESP_OK || free_bytes < (uint64_t)f.data_len + 65536U) {
                err = ESP_ERR_NO_MEM; fail = true; break;
            }
            char *path = tx->files[i].staged;
            char rel[SETTINGS_TRACK_FILE_MAX];
            char staging_rel[128];
            snprintf(rel, sizeof(rel), "audio/%s", safe);
            FILE *fp = create_staged_wav(path, sizeof(tx->files[i].staged), staging_rel, sizeof(staging_rel));
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
                if (fsync(fileno(fp)) != 0) write_ok = false;
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
                break;
            }
            if (audio_validate_wav(path) != ESP_OK) {
                ESP_LOGW(TAG, "OTA: file '%s' invalid wav", safe);
                (void)remove(path);
                break;
            }
            files_ok++;

            {
                int slot = ota_slot_from_name(safe);
                if (slot == 0) {
                    slot = 1;
                    while (slot <= SETTINGS_MAX_TRACKS) {
                        bool used = (incoming_slots & (1UL << (slot - 1))) != 0U;
                        if (!used) {
                            break;
                        }
                        slot++;
                    }
                }
                if (slot >= 1 && slot <= SETTINGS_MAX_TRACKS) {
                    if ((incoming_slots & (1UL << (slot - 1))) != 0U) { fail = true; break; }
                    incoming_slots |= 1UL << (slot - 1);
                    size_t index = 0;
                    while (index < tn && tracks[index].slot != (uint8_t)slot) ++index;
                    if (index >= SETTINGS_MAX_TRACKS) { fail = true; break; }
                    if (index == tn) ++tn;
                    tracks[index].slot = (uint8_t)slot;
                    snprintf(tracks[index].file, sizeof(tracks[index].file), "%s", rel);
                    if (label[0] != '\0') {
                        snprintf(tracks[index].label, sizeof(tracks[index].label), "%.63s", label);
                    } else {
                        snprintf(tracks[index].label, sizeof(tracks[index].label), "%.63s", safe);
                        char *dot = strrchr(tracks[index].label, '.');
                        if (dot != NULL) {
                            *dot = '\0';
                        }
                    }
                    tracks[index].enabled = true;
                } else {
                    fail = true;
                    break;
                }
            }
        }
    }

    if (fail || transfer_expired() || files_ok != n_files || st->remaining != 0 || st->pos != st->len) {
        (void)wav_artifacts_clean(tx);
        free(tx);
        s_up_active = false; free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete/invalid container; boot unchanged");
    }
    if (n_files > 0U) {
        tx->count = tn;
        err = wav_files_prepare(tx);
        if (err != ESP_OK) {
            s_up_active = false; free(st); free(buf);
            return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project; boot unchanged" : "backup/space preflight failed; boot unchanged");
        }
        err = wav_files_publish(tx);
        if (err != ESP_OK) {
            s_up_active = false; free(st); free(buf);
            return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project; boot unchanged" : "canonical publication failed; boot unchanged");
        }
        metadata_attempted = true;
        err = settings_tracks_save(tracks, tn);
        if (err == ESP_OK) err = settings_manifest_sync();
        if (err != ESP_OK || transfer_expired()) {
            s_up_active = false; free(st); free(buf);
            return wav_tx_abort(req, tx, true, false, "metadata incomplete; boot unchanged");
        }
    }
    bool boot_attempted = !transfer_expired();
    err = boot_attempted ? esp_ota_set_boot_partition(update) : ESP_ERR_TIMEOUT;
    if (err != ESP_OK) {
        s_up_active = false; free(st); free(buf);
        return wav_tx_abort(req, tx, metadata_attempted, boot_attempted, "ota boot failed; restoration attempted");
    }
    bool cleanup_pending = !wav_artifacts_clean(tx);
    char recovery[160] = {0};
    if (tx != NULL) snprintf(recovery, sizeof(recovery), "%s", tx->journal);
    free(tx);

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
    char json[384];
    snprintf(json, sizeof(json), "{\"ok\":true,\"bytes\":%d,\"files\":%lu,\"recovery_cleanup_pending\":%s,\"recovery\":\"%s\"}",
             (int)req->content_len, (unsigned long)files_ok, cleanup_pending ? "true" : "false", recovery);
    send_json(req, json);
    s_transfer_reboot = true;
    return ESP_OK;
}

static esp_err_t task_inputs_get(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"actions\":[1,2,3]}");
}

/* GET /api/log?since=N — control events newer than sequence N. */
static esp_err_t log_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    uint32_t since = 0;
    if (parse_query(query, "since", buf, sizeof(buf)) && buf[0]) {
        since = (uint32_t)strtoul(buf, NULL, 10);
    }

    char json[4096];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"entries\":[");
    if (s_evlog_mutex != NULL) {
        (void)xSemaphoreTake(s_evlog_mutex, portMAX_DELAY);
    }
    int emitted = 0;
    uint32_t transmitted = since;
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
        transmitted = e->seq;
    }
    if (s_evlog_mutex != NULL) {
        xSemaphoreGive(s_evlog_mutex);
    }
    buf_appendf(json, sizeof(json), &used, "],\"seq\":%lu}", (unsigned long)transmitted);
    return send_json(req, json);
}

/* GET /api/clientlog?m=... — browser JS errors forwarded to the serial log. */
static esp_err_t clientlog_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char msg[224] = { 0 };
    if (parse_query(query, "m", msg, sizeof(msg)) && msg[0]) {
        ESP_LOGE(TAG, "CLIENT ERROR: %s", msg);
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------- server ---------- */

/* GET /progress (port 81): read-only received-byte progress, CORS-enabled for
 * the cross-port fetch from the UI. No transfer or mutation admission here. */
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

typedef struct {
    httpd_req_t *req;
    esp_err_t (*handler)(httpd_req_t *);
} transfer_job_t;

static void transfer_worker(void *arg)
{
    transfer_job_t *job = arg;
    portENTER_CRITICAL(&s_control_state_mux);
    s_maintenance_owner = xTaskGetCurrentTaskHandle();
    portEXIT_CRITICAL(&s_control_state_mux);
    s_transfer_deadline = esp_timer_get_time() + 120000000;
    s_transfer_reboot = false;
    esp_err_t err = maintenance_quiesce(false);
    bool lease = false;
    if (err == ESP_OK && (job->handler == audio_upload_post || storage_is_mounted())) {
        err = storage_access_begin();
        lease = err == ESP_OK;
    }
    if (err == ESP_OK) {
        httpd_resp_set_hdr(job->req, "Connection", "close");
        (void)job->handler(job->req);
    } else {
        (void)httpd_resp_send_err(job->req, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer quiescence/admission failed");
    }
    if (lease) storage_access_end();
    s_up_active = false;
    s_transfer_deadline = 0;
    int fd = httpd_req_to_sockfd(job->req);
    if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
    /* Complete wakes httpd to observe EOF and close the session. Do not queue
     * a later close by fd: the server could already have reused that number. */
    (void)httpd_req_async_handler_complete(job->req);
    free(job);
    if (s_transfer_reboot) {
        /* Keep inhibit/admission closed across reboot. Filesystem and boot
         * selection cannot be one power-loss-atomic transaction. */
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else {
        web_maintenance_end();
    }
    vTaskDelete(NULL);
}

static esp_err_t transfer_start(httpd_req_t *req, esp_err_t (*handler)(httpd_req_t *))
{
    transfer_job_t *job = malloc(sizeof(*job));
    if (job == NULL) return close_rejected_body(req,
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no worker memory"));
    esp_err_t err = maintenance_reserve();
    if (err != ESP_OK) { free(job); return close_rejected_body(req,
        send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}")); }
    err = httpd_req_async_handler_begin(req, &job->req);
    if (err != ESP_OK) {
        free(job); web_maintenance_end();
        return close_rejected_body(req,
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async admission failed"));
    }
    job->handler = handler;
    s_up_active = true;
    s_up_total = (int)req->content_len;
    s_up_received = 0;
    s_up_slot = 0;
    s_up_ota = handler == ota_update_post;
    if (xTaskCreate(transfer_worker, "web_transfer", 16384, job, 6, NULL) != pdPASS) {
        (void)httpd_resp_send_err(job->req, HTTPD_500_INTERNAL_SERVER_ERROR, "worker creation failed");
        int fd = httpd_req_to_sockfd(job->req);
        if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
        (void)httpd_req_async_handler_complete(job->req);
        free(job); s_up_active = false; web_maintenance_end();
    }
    return ESP_OK;
}

static esp_err_t mutation_dispatch(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_req_get_url_query_len(req) >= sizeof(query) ||
        (query_err != ESP_OK && query_err != ESP_ERR_NOT_FOUND) || !query_encoding_valid(query))
        return close_rejected_body(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query"));
    esp_err_t (*handler)(httpd_req_t *) = *(esp_err_t (**)(httpd_req_t *))req->user_ctx;
    if (handler == audio_upload_post || handler == ota_update_post ||
        handler == audio_pack_upload_post)
        return transfer_start(req, handler);
    if (req->content_len != 0U && handler != graph_validate_post && handler != graph_save_post)
        return close_rejected_body(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unexpected body"));
    /* Cancellation only closes its own calibration, even before actuation admission. */
    if (handler == bemf_cal_post) {
        int action = bemf_cal_action(query);
        if (action == 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid calibration action");
        if (action == 3) return handler(req);
    }
    if (handler == bemf_use_post) {
        bool enabled;
        if (!bemf_bool_query(query, "enabled", &enabled))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid enabled");
    }
    portENTER_CRITICAL(&s_control_state_mux);
    bool admitted = s_maintenance_owner == NULL;
    if (admitted) { ++s_mutation_count; s_mutation_owner = xTaskGetCurrentTaskHandle(); }
    bool ready = s_actuation_ready;
    portEXIT_CRITICAL(&s_control_state_mux);
    if (!admitted) return close_rejected_body(req, graph_error(req, "503 Service Unavailable", "maintenance"));
    bool actuation = handler == motor_post || handler == function_post || handler == aux_effect_post ||
                     handler == audio_play_post || handler == bemf_cal_post || handler == bemf_use_post;
    esp_err_t err;
    if (actuation && !ready) err = send_json(req, "{\"ok\":false,\"error\":\"actuation not ready\"}");
    else if (handler == audio_track_delete_post) {
        err = web_maintenance_begin(false);
        if (err == ESP_OK) {
            err = storage_access_begin();
            if (err == ESP_OK) { err = handler(req); storage_access_end(); }
            web_maintenance_end();
        }
        if (err != ESP_OK) err = send_json(req, "{\"ok\":false,\"error\":\"delete admission failed\"}");
    } else if (handler == graph_save_post || handler == graph_validate_post) {
        err = storage_access_begin();
        if (err == ESP_OK) {
            err = handler(req);
            storage_access_end();
        } else err = close_rejected_body(req, graph_error(req, "503 Service Unavailable", "storage unavailable or maintenance"));
    } else err = handler(req);
    portENTER_CRITICAL(&s_control_state_mux);
    --s_mutation_count;
    if (s_mutation_count == 0U) s_mutation_owner = NULL;
    portEXIT_CRITICAL(&s_control_state_mux);
    return err;
}

static void register_route(const char *uri, httpd_method_t method,
                           esp_err_t (*handler)(httpd_req_t *))
{
    if (s_route_count >= 64U) return;
    s_route_handlers[s_route_count] = handler;
    httpd_uri_t u = { .uri = uri, .method = method,
        .handler = method == HTTP_POST ? mutation_dispatch : handler,
        .user_ctx = &s_route_handlers[s_route_count++] };
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
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    register_route("/", HTTP_GET, root_handler);
    register_route("/sound-editor", HTTP_GET, sound_editor_get);
    register_route("/sound-editor/*", HTTP_GET, sound_editor_get);
    register_route("/api/sound/graph/capabilities", HTTP_GET, graph_capabilities_get);
    register_route("/api/sound/graph/projects", HTTP_GET, graph_projects_get);
    register_route("/api/sound/graph/project", HTTP_GET, graph_project_get);
    register_route("/api/sound/graph/state", HTTP_GET, graph_state_get);
    register_route("/api/sound/graph/asset", HTTP_GET, graph_asset_get);
    register_route("/api/sound/graph/validate", HTTP_POST, graph_validate_post);
    register_route("/api/sound/graph/save", HTTP_POST, graph_save_post);
    register_route("/api/sound/graph/apply", HTTP_POST, graph_apply_post);
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
    register_route("/api/emergency", HTTP_POST, emergency_post);
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
    register_route("/api/audio/library", HTTP_GET, audio_library_get);
    register_route("/api/audio/library/volume", HTTP_POST, audio_library_volume_post);
    register_route("/api/audio/pack", HTTP_POST, audio_pack_upload_post);
    register_route("/api/sound-files", HTTP_GET, sound_files_list_get);
    register_route("/sound-files/*", HTTP_GET, sound_file_get);
    register_route("/api/track/category", HTTP_POST, track_category_post);
    register_route("/api/func-map", HTTP_GET, func_map_get);
    register_route("/api/func-map", HTTP_POST, func_map_post);
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
    esp_err_t control_err = control_mutex_init();
    s_evlog_mutex = xSemaphoreCreateMutex();
    if (s_func_mutex == NULL || control_err != ESP_OK || s_evlog_mutex == NULL)
        return ESP_ERR_NO_MEM;
    esp_err_t load_err = settings_load(&s_cfg);
    if (load_err != ESP_OK) return load_err;
    (void)audio_set_volume(s_cfg.master_volume);
    {
        size_t cat_count = 0;
        load_err = settings_track_cats_load(s_track_cat, &cat_count);
        if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) return load_err;
        s_track_cat_loaded = true;
    }
    {
        size_t fmap_count = 0;
        load_err = settings_func_map_load(s_func_map, &fmap_count);
        if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) return load_err;
    }
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind_count = 0;
    load_err = func_bind_load();
    if (load_err != ESP_OK) return load_err;

    /* Bring up the AUX outputs before restoring the saved per-channel level/
     * effect: auxio_config() only programmes channels that auxio_init() has
     * created, otherwise it fails and the values are overwritten with defaults
     * (REV-W1). */
    load_err = outputs_init();
    if (load_err != ESP_OK) return load_err;

    {
        size_t aux_count = 0;
        load_err = settings_aux_cfg_load(s_aux_cfg, &aux_count);
        if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) return load_err;
        for (uint8_t i = 0; i < SETTINGS_AUX_COUNT; ++i) {
            /* Clamp values from a stale/corrupt blob to the valid ranges. */
            if (s_aux_cfg[i].level > 100U) {
                s_aux_cfg[i].level = 100U;
            }
            if ((uint32_t)s_aux_cfg[i].effect >= AUXIO_EFFECT_COUNT) {
                s_aux_cfg[i].effect = AUXIO_EFFECT_STEADY;
            }
            uint8_t pwm = (uint8_t)((uint16_t)s_aux_cfg[i].level * 255U / 100U);
            load_err = auxio_config(i, pwm, 0, (auxio_effect_t)s_aux_cfg[i].effect,
                                    aux_effect_period(s_aux_cfg[i].effect));
            if (load_err != ESP_OK) return load_err;
        }
    }

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
