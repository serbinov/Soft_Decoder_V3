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

static uint32_t cv_crc32(const uint8_t *data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

/* Minimal NMRA baseline + motor PID defaults. */
static void cv_set_defaults(void)
{
    memset(s_cv, 0, sizeof(s_cv));
    s_cv[1] = 3;      /* short address */
    s_cv[2] = 0;      /* Vstart: 0 at step 1 so the lowest steps creep slowly */
    s_cv[5] = 255;    /* Vhigh: full PWM at step 126 */
    s_cv[6] = 128;    /* Vmid: half PWM at step 63 (linear 0..max curve) */
    s_cv[7] = 7;      /* decoder version (minor, matches version.txt) */
    s_cv[8] = 0;      /* manufacturer (read-only) */
    s_cv[29] = 0x02;  /* 28 speed steps, DCC (analog DC off by default) */
    s_cv[54] = 128;   /* BEMF Kp (~1.05) */
    s_cv[55] = 60;    /* BEMF Ki (~0.07) */
    s_cv[56] = 32;    /* BEMF Kd (~0.10) */
    for (uint16_t i = 0; i < 28; ++i) {
        s_cv[67 + i] = (uint8_t)((i * 255U) / 27U); /* speed table */
    }
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
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(out, def, len - 1);
        out[len - 1] = '\0';
        return ESP_OK;
    }
    return err;
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
    if (!s_pending) {
        return;
    }
    if (esp_timer_get_time() - s_pending_us < SETTINGS_FLUSH_DELAY_US) {
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    if (s_pending) {
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
    s_cv[idx] = value;
    return ESP_OK;
}

esp_err_t settings_cv_reset_to_factory(void)
{
    cv_set_defaults();
    esp_err_t err = settings_cv_commit();
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
    *count = len / sizeof(settings_track_t);
    return ESP_OK;
}

esp_err_t settings_tracks_save(const settings_track_t *tracks, size_t count)
{
    if (tracks == NULL && count != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "tracks", tracks,
                                 sizeof(settings_track_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
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
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "track_cat", cats, sizeof(uint8_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    xSemaphoreGive(s_lock);
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
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_set_blob(s_h, "func_map", map,
                                 sizeof(settings_func_map_t) * count);
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
    if (cal->count > SETTINGS_BEMF_CAL_MAX_POINTS) {
        return ESP_ERR_INVALID_SIZE;
    }
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
