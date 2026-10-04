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
static portMUX_TYPE s_cv_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_pending;
static int64_t s_pending_us;
/* A CV was written but not yet committed to flash (service-mode programming
 * and DCC CV writes must not block the real-time task on a flash commit). */
static bool s_cv_pending;
static uint32_t s_cv_generation;
static settings_config_t s_cfg;
static bool s_cfg_valid;
static uint8_t s_master_volume = 20;
static bool s_manifest_pending;
static esp_err_t s_last_error;
static uint32_t s_retries;
static bool s_ready;

#define CV_RECORD_HEADER 6U
#define CV_RECORD_SIZE (CV_RECORD_HEADER + SETTINGS_CV_COUNT + 1U + 4U)

static uint32_t record_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

static esp_err_t cv_store_locked(void)
{
    uint8_t record[CV_RECORD_SIZE] = { 'C', 'V', 1, 0, 1, 2 };
    uint32_t generation;
    portENTER_CRITICAL(&s_cv_mux);
    memcpy(record + CV_RECORD_HEADER, s_cv, sizeof(s_cv));
    generation = s_cv_generation;
    portEXIT_CRITICAL(&s_cv_mux);
    uint32_t crc = record_crc32(record, sizeof(record) - 4U);
    for (unsigned i = 0; i < 4; ++i) record[sizeof(record) - 4U + i] = (uint8_t)(crc >> (8U * i));
    esp_err_t err = nvs_set_blob(s_h, "cv_record", record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(s_h);
    portENTER_CRITICAL(&s_cv_mux);
    if (err == ESP_OK && generation == s_cv_generation) s_cv_pending = false;
    else s_cv_pending = true;
    portEXIT_CRITICAL(&s_cv_mux);
    return err;
}

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
#define SETTINGS_CV7_VERSION 10
#define SETTINGS_CV29_DEFAULT 0x02U

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
    s_cv[29] = SETTINGS_CV29_DEFAULT; /* 28 steps, analog disabled */
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
    s_cv_pending = true;
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
    s_cv_pending = true;
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
        if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH) return err;
        strncpy(out, def, len - 1);
        out[len - 1] = '\0';
    }
    return ESP_OK;
}

esp_err_t settings_init(void)
{
    s_ready = false;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Losing NVS wipes every setting, including the sound track list that
         * points at the WAV files on the external storage. Make the reason
         * visible in the boot log instead of resetting silently. */
        ESP_LOGE(TAG, "NVS unusable (%s): erasing NVS - saved settings and the "
                      "track list will be reset", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
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

    s_pending = false;
    s_cv_pending = false;
    s_cfg_valid = false;
    s_last_error = ESP_OK;
    s_retries = 0;
    /* One NVS blob is the atomic unit in IDF6. Never pair a new CV blob with a
     * separately updated checksum. Keep shipped legacy records untouched. */
    uint8_t record[CV_RECORD_SIZE];
    size_t record_len = sizeof(record);
    err = nvs_get_blob(s_h, "cv_record", record, &record_len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_NVS_INVALID_LENGTH) return err;
    bool valid_record = false;
    if (err == ESP_OK && record_len == sizeof(record) &&
        memcmp(record, "CV\1\0\1\2", CV_RECORD_HEADER) == 0) {
        uint32_t crc = 0;
        for (unsigned i = 0; i < 4; ++i) crc |= (uint32_t)record[sizeof(record) - 4U + i] << (8U * i);
        valid_record = crc == record_crc32(record, sizeof(record) - 4U);
    }
    uint8_t blob[SETTINGS_CV_COUNT + 1];
    size_t len = sizeof(blob);
    esp_err_t legacy_err = nvs_get_blob(s_h, "cv", blob, &len);
    uint32_t stored_crc = 0;
    esp_err_t crc_err = nvs_get_u32(s_h, "cv_crc", &stored_crc);
    if (err == ESP_ERR_NVS_NOT_FOUND && legacy_err != ESP_OK &&
        legacy_err != ESP_ERR_NVS_NOT_FOUND && legacy_err != ESP_ERR_NVS_INVALID_LENGTH) return legacy_err;
    if (err == ESP_ERR_NVS_NOT_FOUND && legacy_err == ESP_OK && crc_err != ESP_OK &&
        crc_err != ESP_ERR_NVS_NOT_FOUND) return crc_err;
    if (valid_record || (err == ESP_ERR_NVS_NOT_FOUND && legacy_err == ESP_OK &&
        crc_err == ESP_OK && len == sizeof(blob) && stored_crc == cv_crc32(blob, len))) {
        memcpy(s_cv, valid_record ? record + CV_RECORD_HEADER : blob, sizeof(s_cv));
        s_cv_pending = !valid_record;
        ESP_LOGI(TAG, "CV store loaded from NVS");
        cv_migrate_curve();
        cv_migrate_version();
    } else {
        cv_set_defaults();
        s_cv_pending = true;
        ESP_LOGW(TAG, "CV defaults staged (missing or invalid record)");
    }
    err = s_cv_pending ? cv_store_locked() : ESP_OK;
    s_last_error = err;
    if (err != ESP_OK) return err;
    settings_config_t cfg;
    err = settings_load(&cfg);
    bool recovering = false;
    if (err == ESP_OK) err = settings_recovery_pending(&recovering);
    portENTER_CRITICAL(&s_cv_mux);
    s_ready = err == ESP_OK;
    s_manifest_pending = recovering;
    s_pending_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_cv_mux);
    return err;
}

esp_err_t settings_load(settings_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_cfg_valid) {
        *cfg = s_cfg;
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    memset(cfg, 0, sizeof(*cfg));

    esp_err_t err;
#define LOAD(call) do { err = (call); if (err != ESP_OK) { xSemaphoreGive(s_lock); return err; } } while (0)
    LOAD(nvs_read_u8("wifi_mode", &cfg->wifi_mode, 1));
    LOAD(nvs_read_str("ap_ssid", cfg->ap_ssid, sizeof(cfg->ap_ssid), "ADDITIPUS AURA-X"));
    LOAD(nvs_read_str("ap_pass", cfg->ap_password, sizeof(cfg->ap_password), ""));
    LOAD(nvs_read_str("ap_ip", cfg->ap_ip, sizeof(cfg->ap_ip), "192.168.100.1"));
    LOAD(nvs_read_str("sta_ssid", cfg->sta_ssid, sizeof(cfg->sta_ssid), ""));
    LOAD(nvs_read_str("sta_pass", cfg->sta_password, sizeof(cfg->sta_password), ""));
    LOAD(nvs_read_u8("hold", &cfg->hold, 1));
    LOAD(nvs_read_u8("auto_off", &cfg->auto_off_min, 0));
    LOAD(nvs_read_u8("mvol", &cfg->master_volume, 20));
    LOAD(nvs_read_u8("evol", &cfg->engine_volume, 20));
    LOAD(nvs_read_u8("fvol", &cfg->effects_volume, 20));
    LOAD(nvs_read_u8("slot", &cfg->active_slot, 0));
    LOAD(nvs_read_u8("csrc", &cfg->control_source, 0));
    LOAD(nvs_read_str("dev_name", cfg->device_name, sizeof(cfg->device_name), "DECODER"));
#undef LOAD

    uint16_t port = 80;
    err = nvs_get_u16(s_h, "port", &port);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) { xSemaphoreGive(s_lock); return err; }
    cfg->port = port;
    s_cfg = *cfg;
    s_cfg_valid = true;
    portENTER_CRITICAL(&s_cv_mux);
    s_master_volume = cfg->master_volume;
    portEXIT_CRITICAL(&s_cv_mux);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* IDF6 setters write flash immediately. Called only by synchronous save or the
 * persistence worker, never by RAM-only deferred save. Caller holds s_lock. */
static esp_err_t settings_store_locked(const settings_config_t *cfg)
{
    esp_err_t err;
#define STORE(call) do { err = (call); if (err != ESP_OK) return err; } while (0)
    STORE(nvs_set_u8(s_h, "wifi_mode", cfg->wifi_mode));
    STORE(nvs_set_str(s_h, "ap_ssid", cfg->ap_ssid));
    STORE(nvs_set_str(s_h, "ap_pass", cfg->ap_password));
    STORE(nvs_set_str(s_h, "ap_ip", cfg->ap_ip));
    STORE(nvs_set_str(s_h, "sta_ssid", cfg->sta_ssid));
    STORE(nvs_set_str(s_h, "sta_pass", cfg->sta_password));
    STORE(nvs_set_u16(s_h, "port", cfg->port));
    STORE(nvs_set_u8(s_h, "hold", cfg->hold));
    STORE(nvs_set_u8(s_h, "auto_off", cfg->auto_off_min));
    STORE(nvs_set_u8(s_h, "mvol", cfg->master_volume));
    STORE(nvs_set_u8(s_h, "evol", cfg->engine_volume));
    STORE(nvs_set_u8(s_h, "fvol", cfg->effects_volume));
    STORE(nvs_set_u8(s_h, "slot", cfg->active_slot));
    STORE(nvs_set_u8(s_h, "csrc", cfg->control_source));
    STORE(nvs_set_str(s_h, "dev_name", cfg->device_name));
#undef STORE
    return nvs_commit(s_h);
}

esp_err_t settings_save(const settings_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_cfg = *cfg;
    s_cfg_valid = true;
    esp_err_t err = settings_store_locked(cfg);
    portENTER_CRITICAL(&s_cv_mux);
    s_master_volume = cfg->master_volume;
    s_pending = err != ESP_OK;
    s_pending_us = esp_timer_get_time();
    s_last_error = err;
    portEXIT_CRITICAL(&s_cv_mux);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t settings_save_deferred(const settings_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_cfg = *cfg;
    s_cfg_valid = true;
    portENTER_CRITICAL(&s_cv_mux);
    s_master_volume = cfg->master_volume;
    s_pending_us = esp_timer_get_time();
    s_pending = true;
    portEXIT_CRITICAL(&s_cv_mux);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void settings_flush_status(settings_flush_status_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_cv_mux);
    *out = (settings_flush_status_t){ s_pending, s_cv_pending, s_manifest_pending,
                                    s_ready, s_last_error, s_retries };
    portEXIT_CRITICAL(&s_cv_mux);
}

esp_err_t settings_pending_flush(void)
{
    portENTER_CRITICAL(&s_cv_mux);
    bool due = (s_pending || s_cv_pending || s_manifest_pending) &&
               esp_timer_get_time() - s_pending_us >= SETTINGS_FLUSH_DELAY_US;
    portEXIT_CRITICAL(&s_cv_mux);
    if (!due) return ESP_OK;
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&s_cv_mux);
    bool config = s_pending, cv = s_cv_pending, manifest = s_manifest_pending;
    s_pending_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_cv_mux);
    esp_err_t err = config ? settings_store_locked(&s_cfg) : ESP_OK;
    portENTER_CRITICAL(&s_cv_mux);
    if (config && err == ESP_OK) s_pending = false;
    portEXIT_CRITICAL(&s_cv_mux);
    if (err == ESP_OK && cv) err = cv_store_locked();
    xSemaphoreGive(s_lock);
    if (err == ESP_OK && manifest) {
        bool recovering = false;
        err = settings_recovery_pending(&recovering);
        if (err == ESP_OK) err = recovering ? settings_manifest_load() : settings_manifest_sync();
    }
    portENTER_CRITICAL(&s_cv_mux);
    s_last_error = err;
    if (err != ESP_OK && s_retries != UINT32_MAX) ++s_retries;
    portEXIT_CRITICAL(&s_cv_mux);
    return err;
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
        portENTER_CRITICAL(&s_cv_mux);
        uint8_t vol = s_master_volume;
        if (vol > 100U) {
            vol = 100U;
        }
        *out = (uint8_t)((uint16_t)vol * 255U / 100U);
        portEXIT_CRITICAL(&s_cv_mux);
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_cv_mux);
    *out = s_cv[idx];
    portEXIT_CRITICAL(&s_cv_mux);
    return ESP_OK;
}

esp_err_t settings_cv_snapshot(uint8_t out[SETTINGS_CV_COUNT + 1])
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_cv_mux);
    memcpy(out, s_cv, sizeof(s_cv));
    uint8_t vol = s_master_volume > 100U ? 100U : s_master_volume;
    out[63] = (uint8_t)((uint16_t)vol * 255U / 100U);
    portEXIT_CRITICAL(&s_cv_mux);
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
        if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        if (!s_cfg_valid) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
        s_cfg.master_volume = vol;
        portENTER_CRITICAL(&s_cv_mux);
        s_cv[63] = value;
        s_master_volume = vol;
        s_pending = true;
        s_cv_pending = true;
        ++s_cv_generation;
        s_pending_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_cv_mux);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_cv_mux);
    s_cv[idx] = value;
    if (idx == 1U) { s_cv[29] &= (uint8_t)~0x20U; s_cv[19] = 0; }
    ++s_cv_generation;
    s_cv_pending = true;
    s_pending_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_cv_mux);
    return ESP_OK;
}

esp_err_t settings_cv_hard_reset(void)
{
    portENTER_CRITICAL(&s_cv_mux);
    s_cv[19] = 0;
    s_cv[29] = SETTINGS_CV29_DEFAULT;
    s_cv[31] = 0;
    s_cv[32] = 0;
    ++s_cv_generation;
    s_cv_pending = true;
    s_pending_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_cv_mux);
    return ESP_OK;
}

esp_err_t settings_cv_reset_to_factory(void)
{
    /* Reset publication is indivisible to readers; flash is worker-only. */
    portENTER_CRITICAL(&s_cv_mux);
    cv_set_defaults();
    ++s_cv_generation;
    s_cv_pending = true;
    s_pending_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_cv_mux);
    return ESP_OK;
}

esp_err_t settings_factory_reset(void)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = nvs_erase_all(s_h);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_cv_mux);
        cv_set_defaults();
        ++s_cv_generation;
        s_cv_pending = true;
        s_pending = false;
        s_manifest_pending = false;
        s_master_volume = 20U;
        s_cfg_valid = false;
        portEXIT_CRITICAL(&s_cv_mux);
        err = cv_store_locked();
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
    esp_err_t err = cv_store_locked();
    portENTER_CRITICAL(&s_cv_mux);
    s_last_error = err;
    portEXIT_CRITICAL(&s_cv_mux);
    xSemaphoreGive(s_lock);
    return err;
}

/* Stage a CV commit: the actual blob write + commit happens later from
 * settings_pending_flush(), so a DCC/service-mode CV write never blocks the
 * real-time task inside a flash program/erase. */
void settings_cv_commit_deferred(void)
{
    portENTER_CRITICAL(&s_cv_mux);
    s_pending_us = esp_timer_get_time();
    s_cv_pending = true;
    portEXIT_CRITICAL(&s_cv_mux);
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
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
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
    if (!settings_manifest_write_allowed()) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = nvs_set_blob(s_h, "tracks", tracks,
                                 sizeof(settings_track_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    if (err == ESP_OK) settings_manifest_dirty();
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        err = settings_manifest_sync();
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
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
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
    if (!settings_manifest_write_allowed()) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = nvs_set_blob(s_h, "track_cat", cats, sizeof(uint8_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    if (err == ESP_OK) settings_manifest_dirty();
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        err = settings_manifest_sync();
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
        return err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_SIZE ||
               err == ESP_ERR_NVS_INVALID_LENGTH ? ESP_ERR_NOT_FOUND : err;
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
    if (!settings_manifest_write_allowed()) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = nvs_set_blob(s_h, "func_map", map,
                                 sizeof(settings_func_map_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    if (err == ESP_OK) settings_manifest_dirty();
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        err = settings_manifest_sync();
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
    if (err == ESP_OK && (len % sizeof(func_binding_t)) != 0U) {
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
    if (!settings_manifest_write_allowed()) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = nvs_set_blob(s_h, "func_bind", bind, sizeof(func_binding_t) * count);
    if (err == ESP_OK) {
        err = nvs_commit(s_h);
    }
    if (err == ESP_OK) settings_manifest_dirty();
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        err = settings_manifest_sync();
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
                               FUNC_MODE_LATCHED);
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
    memset(cal, 0, sizeof(*cal));
    settings_bemf_cal_t candidate;
    size_t len = sizeof(candidate);
    esp_err_t err = nvs_get_blob(s_h, "bemf_cal", &candidate, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(candidate) || candidate.count > SETTINGS_BEMF_CAL_MAX_POINTS) return ESP_ERR_INVALID_SIZE;
    if (!settings_bemf_cal_validate(&candidate)) return ESP_ERR_INVALID_ARG;
    *cal = candidate;
    return ESP_OK;
}

bool settings_bemf_cal_validate(const settings_bemf_cal_t *cal)
{
    if (cal == NULL || cal->count < 2U || cal->count > SETTINGS_BEMF_CAL_MAX_POINTS) return false;
    for (size_t i = 0; i < cal->count; ++i) {
        if (cal->speed[i] > 126U || cal->frac[i] > 1024U) return false;
        if (i > 0U && (cal->speed[i] <= cal->speed[i - 1U] || cal->frac[i] < cal->frac[i - 1U])) return false;
    }
    return cal->frac[cal->count - 1U] >= SETTINGS_BEMF_CAL_MIN_END_FRAC;
}

esp_err_t settings_bemf_cal_save(const settings_bemf_cal_t *cal)
{
    return settings_bemf_cal_save_guarded(cal, NULL, NULL);
}

esp_err_t settings_bemf_cal_save_guarded(const settings_bemf_cal_t *cal,
                                       settings_bemf_cal_authorize_t authorize, void *context)
{
    if (!settings_bemf_cal_validate(cal)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Authorization is the linearization point, not the pre-lock validation.
     * The callback releases any output mux before the blocking NVS operation. */
    if (authorize != NULL && !authorize(context)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
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

esp_err_t settings_metadata_lock(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void settings_metadata_unlock(void) { xSemaphoreGive(s_lock); }

void settings_manifest_dirty(void)
{
    portENTER_CRITICAL(&s_cv_mux);
    s_manifest_pending = true;
    s_pending_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_cv_mux);
}

/* Called under the metadata lock so no newer save can be cleared. */
void settings_manifest_result(esp_err_t error)
{
    portENTER_CRITICAL(&s_cv_mux);
    s_manifest_pending = error != ESP_OK;
    s_last_error = error;
    portEXIT_CRITICAL(&s_cv_mux);
}

esp_err_t settings_recovery_pending(bool *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    if (settings_metadata_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(s_h, "recover", &value);
    *out = value != 0U;
    settings_metadata_unlock();
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

esp_err_t settings_recovery_set_pending(bool pending)
{
    if (settings_metadata_lock() != ESP_OK) return ESP_ERR_TIMEOUT;
    esp_err_t err = nvs_set_u8(s_h, "recover", pending ? 1U : 0U);
    if (err == ESP_OK) err = nvs_commit(s_h);
    settings_metadata_unlock();
    return err;
}
