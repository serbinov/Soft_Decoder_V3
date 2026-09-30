#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "decoder"

/* A deferred save is committed only after the configuration has been stable
 * for this long, so a slider drag does not trigger one flash write per move. */
#define SETTINGS_FLUSH_DELAY_US (2000LL * 1000LL)

static nvs_handle_t s_h;
static uint8_t s_cv[SETTINGS_CV_COUNT + 1];
static SemaphoreHandle_t s_lock;
static volatile bool s_pending;
static volatile int64_t s_pending_us;
/* A CV was written but not yet committed to flash (service-mode programming
 * and DCC CV writes must not block the real-time task on a flash commit). */
static volatile bool s_cv_pending;

static uint32_t cv_crc32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

/* Decoder version (minor) — single source of truth is version.txt; CV7 mirrors
 * its minor. Bump both together. */
#define SETTINGS_CV7_VERSION 9

/* Minimal NMRA baseline + motor PID defaults. */
static void cv_set_defaults(void)
{
    memset(s_cv, 0, sizeof(s_cv));
    s_cv[1] = 3;      /* short address */
    s_cv[2] = 0;      /* Vstart: 0 at step 1 so the lowest steps creep slowly */
    s_cv[5] = 255;    /* Vhigh: full PWM at step 126 */
    s_cv[6] = 128;    /* Vmid: half PWM at step 63 (linear 0..max curve) */
    s_cv[7] = SETTINGS_CV7_VERSION; /* decoder version (matches version.txt) */
    s_cv[8] = 0;      /* manufacturer (read-only) */
    s_cv[29] = 0x02;  /* 28 speed steps, DCC (analog DC off by default) */
    s_cv[54] = 128;   /* BEMF Kp (~1.05) */
    s_cv[55] = 60;    /* BEMF Ki (~0.07) */
    s_cv[56] = 32;    /* BEMF Kd (~0.10) */
    /* Sound rates (SOUND_ENGINE_ROADMAP.md R6.1): chuff/exhaust, bell and
     * dynamic-brake playback rate, 0..255 (51 = 1.0x, see sound_cv_rate). */
    s_cv[114] = 57;   /* chuff / exhaust rate */
    s_cv[115] = 5;    /* bell ring rate */
    s_cv[116] = 30;   /* dynamic brake rate */
    for (uint16_t i = 0; i < 28; ++i) {
        s_cv[67 + i] = (uint8_t)((i * 255U) / 27U); /* speed table */
    }
}

/* After an OTA the stored CV blob may still carry the previous firmware
 * version in CV7. Rewrite it once so the reported decoder version matches the
 * running firmware (CV7 is read-only through the CV API). */
static void cv_migrate_version(void)
{
    if (s_cv[7] == SETTINGS_CV7_VERSION) {
        return;
    }
    s_cv[7] = SETTINGS_CV7_VERSION;
    (void)nvs_set_blob(s_h, "cv", s_cv, sizeof(s_cv));
    (void)nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv)));
    (void)nvs_commit(s_h);
}

/* One-time migration: the old factory speed curve (Vstart=24 with Vmid/Vhigh
 * left at 0) made the lowest speed steps run fast. Move an untouched store to
 * the new linear curve (0 / 128 / 255). A store whose curve was tuned by the
 * user does not match and is left alone. Safe to call with any s_cv content. */
static void cv_migrate_curve(void)
{
    if (s_cv[2] != 24U || s_cv[5] != 0U || s_cv[6] != 0U) {
        return;
    }
    s_cv[2] = 0U;
    s_cv[5] = 255U;
    s_cv[6] = 128U;
    (void)nvs_set_blob(s_h, "cv", s_cv, sizeof(s_cv));
    (void)nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv)));
    (void)nvs_commit(s_h);
    ESP_LOGI(TAG, "CV speed curve migrated: Vstart 24->0, Vmid=128, Vhigh=255");
}

static esp_err_t nvs_read_u8(const char *key, uint8_t *out, uint8_t def)
{
    uint8_t v = def;
    esp_err_t err = nvs_get_u8(s_h, key, &v);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        *out = v;
        return ESP_OK;
    }
    return err;
}

static esp_err_t nvs_read_str(const char *key, char *out, size_t len, const char *def)
{
    size_t rl = len;
    esp_err_t err = nvs_get_str(s_h, key, out, &rl);
    if (err != ESP_OK) {
        /* Missing, truncated or corrupt: always fall back to the documented
         * default instead of leaving the field empty. */
        strncpy(out, def, len - 1);
        out[len - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Losing NVS wipes every setting, including the sound track list that
         * points at the WAV files on the external storage. Make the reason
         * visible in the boot log instead of resetting silently. */
        ESP_LOGE(TAG, "NVS unusable (%s): erasing NVS - saved settings and the "
                      "track list will be reset", esp_err_to_name(err));
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_h);
    if (err != ESP_OK) {
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Load CVs from NVS (or defaults on first boot / corrupted store). */
    uint8_t blob[SETTINGS_CV_COUNT + 1];
    size_t len = sizeof(blob);
    err = nvs_get_blob(s_h, "cv", blob, &len);
    uint32_t stored_crc = 0;
    (void)nvs_get_u32(s_h, "cv_crc", &stored_crc);
    if (err == ESP_OK && len == sizeof(blob) && stored_crc == cv_crc32(blob, len)) {
        memcpy(s_cv, blob, len);
        ESP_LOGI(TAG, "CV store loaded from NVS");
        cv_migrate_curve();
        cv_migrate_version();
    } else {
        cv_set_defaults();
        (void)nvs_set_blob(s_h, "cv", s_cv, sizeof(s_cv));
        (void)nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv)));
        (void)nvs_commit(s_h);
        ESP_LOGI(TAG, "CV defaults written to NVS");
    }

    return ESP_OK;
}

esp_err_t settings_load(settings_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(cfg, 0, sizeof(*cfg));

    nvs_read_u8("wifi_mode", &cfg->wifi_mode, 1);
    nvs_read_str("ap_ssid", cfg->ap_ssid, sizeof(cfg->ap_ssid), "ADDITIPUS AURA-X");
    nvs_read_str("ap_pass", cfg->ap_password, sizeof(cfg->ap_password), "");
    nvs_read_str("ap_ip", cfg->ap_ip, sizeof(cfg->ap_ip), "192.168.100.1");
    nvs_read_str("sta_ssid", cfg->sta_ssid, sizeof(cfg->sta_ssid), "");
    nvs_read_str("sta_pass", cfg->sta_password, sizeof(cfg->sta_password), "");
    nvs_read_u8("hold", &cfg->hold, 1);
    nvs_read_u8("auto_off", &cfg->auto_off_min, 0);
    nvs_read_u8("mvol", &cfg->master_volume, 20);
    nvs_read_u8("evol", &cfg->engine_volume, 20);
    nvs_read_u8("fvol", &cfg->effects_volume, 20);
    nvs_read_u8("slot", &cfg->active_slot, 0);
    nvs_read_u8("csrc", &cfg->control_source, 0);
    nvs_read_str("dev_name", cfg->device_name, sizeof(cfg->device_name), "DECODER");

    uint16_t port = 80;
    (void)nvs_get_u16(s_h, "port", &port);
    cfg->port = port;

    return ESP_OK;
}

/* Stage every config key in the NVS cache (no flash write yet). Caller holds
 * s_lock. */
static void settings_store_locked(const settings_config_t *cfg)
{
    (void)nvs_set_u8(s_h, "wifi_mode", cfg->wifi_mode);
    (void)nvs_set_str(s_h, "ap_ssid", cfg->ap_ssid);
    (void)nvs_set_str(s_h, "ap_pass", cfg->ap_password);
    (void)nvs_set_str(s_h, "ap_ip", cfg->ap_ip);
    (void)nvs_set_str(s_h, "sta_ssid", cfg->sta_ssid);
    (void)nvs_set_str(s_h, "sta_pass", cfg->sta_password);
    (void)nvs_set_u16(s_h, "port", cfg->port);
    (void)nvs_set_u8(s_h, "hold", cfg->hold);
    (void)nvs_set_u8(s_h, "auto_off", cfg->auto_off_min);
    (void)nvs_set_u8(s_h, "mvol", cfg->master_volume);
    (void)nvs_set_u8(s_h, "evol", cfg->engine_volume);
    (void)nvs_set_u8(s_h, "fvol", cfg->effects_volume);
    (void)nvs_set_u8(s_h, "slot", cfg->active_slot);
    (void)nvs_set_u8(s_h, "csrc", cfg->control_source);
    (void)nvs_set_str(s_h, "dev_name", cfg->device_name);
}

esp_err_t settings_save(const settings_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    settings_store_locked(cfg);
    esp_err_t err = nvs_commit(s_h);
    s_pending = false;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_save_deferred(const settings_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    settings_store_locked(cfg);
    s_pending_us = esp_timer_get_time();
    s_pending = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void settings_pending_flush(void)
{
    if (!s_pending && !s_cv_pending) {
        return;
    }
    if (esp_timer_get_time() - s_pending_us < SETTINGS_FLUSH_DELAY_US) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    bool commit = s_pending;
    if (s_cv_pending) {
        esp_err_t err = nvs_set_blob(s_h, "cv", s_cv, sizeof(s_cv));
        if (err == ESP_OK) {
            (void)nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv)));
        }
        s_cv_pending = false;
        commit = true;
    }
    if (commit) {
        (void)nvs_commit(s_h);
        s_pending = false;
    }
    xSemaphoreGive(s_lock);
}

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (out == NULL || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (idx == 63U) {
        /* CV63 aliases the persisted master volume so the sound UI can drive it
         * through the normal CV path (SOUND_ENGINE_ROADMAP.md R6.1). "mvol" is
         * stored as 0..100 %; report it on the CV 0..255 scale. */
        uint8_t vol = 20U;
        (void)nvs_get_u8(s_h, "mvol", &vol);
        if (vol > 100U) {
            vol = 100U;
        }
        *out = (uint8_t)((uint16_t)vol * 255U / 100U);
        return ESP_OK;
    }
    *out = s_cv[idx];
    return ESP_OK;
}

esp_err_t settings_cv_write(uint16_t idx, uint8_t value)
{
    if (idx < 1 || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (idx == 7) {
        return ESP_ERR_INVALID_ARG; /* read-only */
    }
    if (idx == 8) {
        /* NMRA: writing 8 to CV8 (manufacturer id) triggers factory reset. */
        if (value == 8) {
            return settings_cv_reset_to_factory();
        }
        return ESP_ERR_INVALID_ARG;
    }
    if (idx == 63U) {
        /* CV63 aliases the master volume (SOUND_ENGINE_ROADMAP.md R6.1). The
         * value is 0..255; "mvol" is stored as 0..100 %. The live volume and the
         * cached config are updated by the caller (web_master_volume_changed). */
        uint8_t vol = (uint8_t)((uint16_t)value * 100U / 255U);
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        s_cv[63] = value;
        esp_err_t verr = nvs_set_u8(s_h, "mvol", vol);
        xSemaphoreGive(s_lock);
        return verr;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_cv[idx] = value;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_cv_reset_to_factory(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Keep cv_set_defaults() and the commit under the same lock as the readers
     * so the motor task never observes a half-reset CV table. */
    cv_set_defaults();
    esp_err_t err = nvs_set_blob(s_h, "cv", s_cv, sizeof(s_cv));
    if (err == ESP_OK) {
        err = nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv)));
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "CV factory reset%s", err == ESP_OK ? "" : " (commit failed)");
    return err;
}

esp_err_t settings_factory_reset(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_erase_all(s_h);
    if (err == ESP_OK) {
        cv_set_defaults();
        err = nvs_set_blob(s_h, "cv", s_cv, sizeof(s_cv));
        if (err == ESP_OK) {
            err = nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv)));
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Full factory reset%s", err == ESP_OK ? "" : " (failed)");
    return err;
}

esp_err_t settings_cv_commit(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "cv", s_cv, sizeof(s_cv));
    if (err == ESP_OK) {
        err = nvs_set_u32(s_h, "cv_crc", cv_crc32(s_cv, sizeof(s_cv)));
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* Stage a CV commit: the actual blob write + commit happens later from
 * settings_pending_flush(), so a DCC/service-mode CV write never blocks the
 * real-time task inside a flash program/erase. */
void settings_cv_commit_deferred(void)
{
    s_pending_us = esp_timer_get_time();
    s_cv_pending = true;
}

esp_err_t settings_tracks_load(settings_track_t *tracks, size_t *count)
{
    if (tracks == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = sizeof(settings_track_t) * SETTINGS_MAX_TRACKS;
    esp_err_t err = nvs_get_blob(s_h, "tracks", tracks, &len);
    if (err != ESP_OK) {
        *count = 0;
        return ESP_ERR_NOT_FOUND;
    }
    if ((len % sizeof(settings_track_t)) != 0U) { *count = 0; return ESP_ERR_INVALID_SIZE; }
    *count = len / sizeof(settings_track_t);
    for (size_t i = 0; i < *count; ++i) {
        /* Defensive: a legacy/corrupt blob might not be NUL-terminated. */
        tracks[i].file[sizeof(tracks[i].file) - 1U] = '\0';
        tracks[i].label[sizeof(tracks[i].label) - 1U] = '\0';
    }
    return ESP_OK;
}

esp_err_t settings_tracks_save(const settings_track_t *tracks, size_t count)
{
    if (tracks == NULL && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > SETTINGS_MAX_TRACKS) { return ESP_ERR_INVALID_ARG; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "tracks", tracks,
                                 sizeof(settings_track_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        (void)settings_manifest_sync();
    }
    return err;
}

esp_err_t settings_track_cats_load(uint8_t *cats, size_t *count)
{
    if (cats == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = sizeof(uint8_t) * SETTINGS_MAX_TRACKS;
    esp_err_t err = nvs_get_blob(s_h, "track_cat", cats, &len);
    if (err != ESP_OK) {
        for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
            cats[i] = SETTINGS_TRACK_CAT_DEFAULT_SLOT((uint8_t)(i + 1U));
        }
        *count = SETTINGS_MAX_TRACKS;
        return ESP_ERR_NOT_FOUND;
    }
    *count = len / sizeof(uint8_t);
    return ESP_OK;
}

esp_err_t settings_track_cats_save(const uint8_t *cats, size_t count)
{
    if (cats == NULL && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > SETTINGS_MAX_TRACKS) { return ESP_ERR_INVALID_ARG; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "track_cat", cats, sizeof(uint8_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        (void)settings_manifest_sync();
    }
    return err;
}

esp_err_t settings_func_map_load(settings_func_map_t *map, size_t *count)
{
    if (map == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = sizeof(settings_func_map_t) * SETTINGS_FUNC_MAP_COUNT;
    esp_err_t err = nvs_get_blob(s_h, "func_map", map, &len);
    /* Ignore a blob written by an older firmware (different struct size). */
    if (err == ESP_OK && len != sizeof(settings_func_map_t) * SETTINGS_FUNC_MAP_COUNT) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        for (size_t i = 0; i < SETTINGS_FUNC_MAP_COUNT; ++i) {
            /* Hobby default: F0 = head light (front forward / rear reverse),
             * F1..F20 = the matching sound slot, everything else off. */
            map[i].slot_a = (i >= 1U && i <= SETTINGS_MAX_TRACKS) ? (uint8_t)i : 0U;
            map[i].slot_b = 0U;
            map[i].aux_mask = (i == 0U)
                                  ? (uint16_t)(SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R)
                                  : 0U;
            map[i].dir = SETTINGS_FUNC_DIR_NONE;
            map[i].speed = SETTINGS_FUNC_SPD_NONE;
        }
        *count = SETTINGS_FUNC_MAP_COUNT;
        return ESP_ERR_NOT_FOUND;
    }
    *count = len / sizeof(settings_func_map_t);
    return ESP_OK;
}

esp_err_t settings_func_map_save(const settings_func_map_t *map, size_t count)
{
    if (map == NULL && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > SETTINGS_FUNC_MAP_COUNT) { return ESP_ERR_INVALID_ARG; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "func_map", map,
                                 sizeof(settings_func_map_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        (void)settings_manifest_sync();
    }
    return err;
}

esp_err_t settings_func_bind_load(func_binding_t *bind, size_t *count)
{
    if (bind == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = sizeof(func_binding_t) * FUNC_BIND_MAX;
    esp_err_t err = nvs_get_blob(s_h, "func_bind", bind, &len);
    if (err == ESP_OK && (len == 0U || (len % sizeof(func_binding_t)) != 0U)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        memset(bind, 0, sizeof(func_binding_t) * FUNC_BIND_MAX);
        *count = 0;
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }
    *count = len / sizeof(func_binding_t);
    return ESP_OK;
}

esp_err_t settings_func_bind_save(const func_binding_t *bind, size_t count)
{
    if (bind == NULL && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > FUNC_BIND_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "func_bind", bind, sizeof(func_binding_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        (void)settings_manifest_sync();
    }
    return err;
}

static void func_bind_fill(func_binding_t *b, uint8_t fn, uint8_t type, uint8_t id,
                           uint8_t dir, uint8_t state, uint8_t mode)
{
    memset(b, 0, sizeof(*b));
    b->used = 1;
    b->fn = fn;
    b->target_type = type;
    b->target_id = id;
    b->dir = dir;
    b->state = state;
    b->mode = mode;
}

esp_err_t settings_func_bind_legacy_convert(const settings_func_map_t *map, size_t map_count,
                                            func_binding_t *out, size_t *out_count)
{
    if (map == NULL || out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = 0;
    for (size_t f = 0; f < map_count && f < SETTINGS_FUNC_MAP_COUNT; ++f) {
        uint8_t dir = FUNC_DIR_ANY;
        if (map[f].dir == SETTINGS_FUNC_DIR_FWD) {
            dir = FUNC_DIR_FWD;
        } else if (map[f].dir == SETTINGS_FUNC_DIR_REV) {
            dir = FUNC_DIR_REV;
        }
        uint8_t state = FUNC_STATE_ANY;
        if (map[f].speed == SETTINGS_FUNC_SPD_MOVING) {
            state = FUNC_STATE_MOVING;
        } else if (map[f].speed == SETTINGS_FUNC_SPD_STOP) {
            state = FUNC_STATE_STOPPED;
        }
        const uint16_t light = SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R;
        for (uint8_t bit = 0; bit < 9U && n < FUNC_BIND_MAX; ++bit) {
            if ((map[f].aux_mask & (uint16_t)(1U << bit)) == 0U) {
                continue;
            }
            /* F0F+F0R together are the directional head light (see
             * web_util_func_desired): forward drives F0F, reverse F0R. A
             * function-level dir narrows it to just that side. */
            if ((map[f].aux_mask & light) == light && bit <= 1U) {
                if (map[f].dir == SETTINGS_FUNC_DIR_FWD && bit != 0U) {
                    continue;
                }
                if (map[f].dir == SETTINGS_FUNC_DIR_REV && bit != 1U) {
                    continue;
                }
                uint8_t ldir = (bit == 0U) ? FUNC_DIR_FWD : FUNC_DIR_REV;
                func_bind_fill(&out[n], (uint8_t)f, FUNC_TARGET_OUTPUT, bit, ldir, state, 0);
                n++;
                continue;
            }
            func_bind_fill(&out[n], (uint8_t)f, FUNC_TARGET_OUTPUT, bit, dir, state, 0);
            n++;
        }
        const uint8_t slots[2] = { map[f].slot_a, map[f].slot_b };
        for (int s = 0; s < 2 && n < FUNC_BIND_MAX; ++s) {
            if (slots[s] != 0U) {
                func_bind_fill(&out[n], (uint8_t)f, FUNC_TARGET_SLOT, slots[s], dir, state,
                               SOUND_MODE_LATCHED);
                n++;
            }
        }
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t settings_func_bind_add(func_binding_t *bind, size_t *count, const func_binding_t *b)
{
    if (bind == NULL || count == NULL || b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*count >= FUNC_BIND_MAX) {
        return ESP_ERR_NO_MEM;
    }
    bind[*count] = *b;
    bind[*count].used = 1;
    (*count)++;
    return ESP_OK;
}

esp_err_t settings_func_bind_remove(func_binding_t *bind, size_t *count, size_t idx)
{
    if (bind == NULL || count == NULL || idx >= *count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = idx; i + 1U < *count; ++i) {
        bind[i] = bind[i + 1U];
    }
    (*count)--;
    memset(&bind[*count], 0, sizeof(bind[0]));
    return ESP_OK;
}

static bool func_bind_same(const func_binding_t *a, const func_binding_t *b)
{
    return a->used == b->used && a->fn == b->fn && a->target_type == b->target_type &&
           a->target_id == b->target_id && a->dir == b->dir && a->state == b->state &&
           a->mode == b->mode && a->flags == b->flags &&
           a->short_table == b->short_table && a->short_ms == b->short_ms &&
           a->min_ms == b->min_ms && a->fade_ms == b->fade_ms;
}

int settings_func_bind_find(const func_binding_t *bind, size_t count, const func_binding_t *b)
{
    if (bind == NULL || b == NULL) {
        return -1;
    }
    for (size_t i = 0; i < count && i < FUNC_BIND_MAX; ++i) {
        if (func_bind_same(&bind[i], b)) {
            return (int)i;
        }
    }
    return -1;
}

esp_err_t settings_active_scheme_get(char *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = cap;
    esp_err_t err = nvs_get_str(s_h, "active_scheme", out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }
    return ESP_OK;
}

esp_err_t settings_active_scheme_set(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_str(s_h, "active_scheme", name);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_aux_cfg_load(settings_aux_cfg_t *cfg, size_t *count)
{
    if (cfg == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = sizeof(settings_aux_cfg_t) * SETTINGS_AUX_COUNT;
    esp_err_t err = nvs_get_blob(s_h, "aux_cfg", cfg, &len);
    /* Ignore a blob written by an older firmware (different struct size). */
    if (err == ESP_OK && len != sizeof(settings_aux_cfg_t) * SETTINGS_AUX_COUNT) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        for (size_t i = 0; i < SETTINGS_AUX_COUNT; ++i) {
            cfg[i].level = 100U; /* full on */
            cfg[i].effect = 0U;  /* plain output */
        }
        *count = SETTINGS_AUX_COUNT;
        return ESP_ERR_NOT_FOUND;
    }
    *count = len / sizeof(settings_aux_cfg_t);
    return ESP_OK;
}

esp_err_t settings_aux_cfg_save(const settings_aux_cfg_t *cfg, size_t count)
{
    if (cfg == NULL && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > SETTINGS_AUX_COUNT) { return ESP_ERR_INVALID_ARG; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "aux_cfg", cfg,
                                 sizeof(settings_aux_cfg_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_bemf_cal_load(settings_bemf_cal_t *cal)
{
    if (cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = sizeof(settings_bemf_cal_t);
    esp_err_t err = nvs_get_blob(s_h, "bemf_cal", cal, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(settings_bemf_cal_t) || cal->count > SETTINGS_BEMF_CAL_MAX_POINTS) { return ESP_ERR_INVALID_SIZE; }
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save(const settings_bemf_cal_t *cal)
{
    if (cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "bemf_cal", cal, sizeof(*cal));
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_bemf_cal_clear(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_erase_key(s_h, "bemf_cal");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* Closed-loop BEMF regulation flag. Default is enabled so behaviour is
 * unchanged for existing installs where the key is absent. */
esp_err_t settings_bemf_use_load(bool *out_enabled)
{
    if (out_enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t v = 1;
    esp_err_t err = nvs_get_u8(s_h, "bemf_use", &v);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    *out_enabled = (v != 0U);
    return err;
}

esp_err_t settings_bemf_use_save(bool enabled)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_u8(s_h, "bemf_use", enabled ? 1U : 0U);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
    return err;
}
