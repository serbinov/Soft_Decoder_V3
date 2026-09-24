#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SETTINGS_CV_COUNT   512
#define SETTINGS_SSID_MAX   33
#define SETTINGS_PASS_MAX   65
#define SETTINGS_IP_MAX     16
#define SETTINGS_NAME_MAX   33
#define SETTINGS_MAX_TRACKS 20
#define SETTINGS_TRACK_FILE_MAX 128
#define SETTINGS_TRACK_LABEL_MAX 64
#define SETTINGS_BEMF_CAL_MAX_POINTS 16

/* Track slot categories: a slot is either an "engine" (prime mover, looping
 * motor sound) or an "effects" (horn, bell, ...) sound. The category selects
 * which volume slider (engine_volume vs effects_volume) scales that slot. */
#define SETTINGS_TRACK_CAT_ENGINE   0
#define SETTINGS_TRACK_CAT_EFFECTS  1
/* Default category layout: slot 1 = engine (F1, the conventional prime mover),
 * all other slots = effects. */
#define SETTINGS_TRACK_CAT_DEFAULT_SLOT(slot) \
    (((slot) == 1U) ? SETTINGS_TRACK_CAT_ENGINE : SETTINGS_TRACK_CAT_EFFECTS)

/* Function (F) mapping: how each DCC function key drives sound slots and/or
 * AUX/light outputs, gated by the driving direction and the speed state. */
#define SETTINGS_FUNC_MAP_COUNT 29  /* F0..F28 */
#define SETTINGS_FUNC_SLOTS     2   /* up to two sound slots per function */

/* Output bits within settings_func_map_t.aux_mask. */
#define SETTINGS_FUNC_OUT_F0F   (1u << 0)
#define SETTINGS_FUNC_OUT_F0R   (1u << 1)
#define SETTINGS_FUNC_OUT_AUX1  (1u << 2)  /* AUX1..AUX7 = bits 2..8 */
#define SETTINGS_FUNC_OUT_AUX7  (1u << 8)
#define SETTINGS_FUNC_OUT_ALL   0x01FFu

#define SETTINGS_FUNC_DIR_NONE   0  /* "-" (not set / any direction) */
#define SETTINGS_FUNC_DIR_FWD    1  /* forward only */
#define SETTINGS_FUNC_DIR_REV    2  /* reverse only */

#define SETTINGS_FUNC_SPD_NONE   0  /* "-" (not set / any speed) */
#define SETTINGS_FUNC_SPD_MOVING 1  /* only while moving */
#define SETTINGS_FUNC_SPD_STOP   2  /* only while stopped */

typedef struct {
    uint8_t slot_a;     /* 0 = none, 1..SETTINGS_MAX_TRACKS = audio slot */
    uint8_t slot_b;     /* 0 = none, 1..SETTINGS_MAX_TRACKS = audio slot */
    uint16_t aux_mask;  /* SETTINGS_FUNC_OUT_* bits */
    uint8_t dir;        /* SETTINGS_FUNC_DIR_* */
    uint8_t speed;      /* SETTINGS_FUNC_SPD_* */
} settings_func_map_t;

typedef struct {
    uint8_t wifi_mode;          /* 0=off, 1=AP, 2=STA, 3=APSTA */
    char ap_ssid[SETTINGS_SSID_MAX];
    char ap_password[SETTINGS_PASS_MAX];
    char ap_ip[SETTINGS_IP_MAX]; /* access point address, default 192.168.100.1 */
    char sta_ssid[SETTINGS_SSID_MAX];
    char sta_password[SETTINGS_PASS_MAX];
    uint16_t port;
    uint8_t hold;               /* answer captive probes with 204 */
    uint8_t auto_off_min;       /* wifi auto-off timeout (0=off) */
    uint8_t master_volume;
    uint8_t engine_volume;
    uint8_t effects_volume;
    uint8_t active_slot;
    uint8_t control_source;     /* 0=rails, 1=web */
    char device_name[SETTINGS_NAME_MAX];
} settings_config_t;

typedef struct {
    uint8_t slot;
    char file[SETTINGS_TRACK_FILE_MAX];
    char label[SETTINGS_TRACK_LABEL_MAX];
    bool enabled;
} settings_track_t;

/* BEMF calibration: measured back-EMF (as a fraction of the rail voltage,
 * scaled by 1024) at a set of applied speed steps, recorded while the motor
 * runs without load. Used by the PID to hold the no-load RPM under load. */
typedef struct {
    uint8_t count;
    uint8_t speed[SETTINGS_BEMF_CAL_MAX_POINTS];
    uint16_t frac[SETTINGS_BEMF_CAL_MAX_POINTS];
} settings_bemf_cal_t;

esp_err_t settings_init(void);

esp_err_t settings_load(settings_config_t *cfg);
esp_err_t settings_save(const settings_config_t *cfg);

/* Same as settings_save(), but the NVS commit is deferred: values are staged
 * in the NVS cache and flushed by settings_pending_flush() after a short idle
 * period. Use for high-frequency UI actions (volume slider, slot category,
 * active slot) so a flash program/erase cycle cannot stall the Wi-Fi driver. */
esp_err_t settings_save_deferred(const settings_config_t *cfg);

/* Flush a staged settings_save_deferred() write once it has been idle for a
 * short guard interval. Cheap to call periodically from a low-rate task. */
void settings_pending_flush(void);

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out);
esp_err_t settings_cv_write(uint16_t idx, uint8_t value);
esp_err_t settings_cv_commit(void);
esp_err_t settings_cv_reset_to_factory(void);
/* Erase the whole settings namespace and restore CV defaults. The WiFi config,
 * volumes, name, control source, sound tracks and BEMF calibration are cleared
 * and return to their factory defaults on the next load. */
esp_err_t settings_factory_reset(void);

esp_err_t settings_tracks_load(settings_track_t *tracks, size_t *count);
esp_err_t settings_tracks_save(const settings_track_t *tracks, size_t count);

/* Per-slot category (engine/effects), indexed by slot-1 (0..SETTINGS_MAX_TRACKS-1).
 * Loaded from a separate NVS key so the tracks blob format stays unchanged.
 * On a missing key the default layout is returned and *count = SETTINGS_MAX_TRACKS. */
esp_err_t settings_track_cats_load(uint8_t *cats, size_t *count);
esp_err_t settings_track_cats_save(const uint8_t *cats, size_t count);

/* Function (F) map. Stored in its own NVS key; on a missing key the hobby
 * defaults are returned (F0 = head light, F1..F20 = matching sound slot). */
esp_err_t settings_func_map_load(settings_func_map_t *map, size_t *count);
esp_err_t settings_func_map_save(const settings_func_map_t *map, size_t count);

/* Metadata manifest stored next to the sounds on the external storage
 * (/userdata/audio/tracks.txt). It survives a full NVS reset, so the track
 * names, categories and function map come back even after a chip erase.
 * settings_manifest_sync() rewrites it whenever that metadata changes;
 * settings_manifest_load() restores it (ESP_OK when tracks were restored). */
void settings_manifest_sync(void);
esp_err_t settings_manifest_load(void);

/* Per-output PWM level and light effect (F0F, F0R, AUX1..AUX7). */
#define SETTINGS_AUX_COUNT 9
typedef struct {
    uint8_t level;   /* PWM level 0..100 % */
    uint8_t effect;  /* 0 = steady plain output, else a light-effect id */
} settings_aux_cfg_t;

esp_err_t settings_aux_cfg_load(settings_aux_cfg_t *cfg, size_t *count);
esp_err_t settings_aux_cfg_save(const settings_aux_cfg_t *cfg, size_t count);

esp_err_t settings_bemf_cal_load(settings_bemf_cal_t *cal);
esp_err_t settings_bemf_cal_save(const settings_bemf_cal_t *cal);
esp_err_t settings_bemf_cal_clear(void);

/* Motor regulation mode: true = BEMF-PID closed loop (default), false = the
 * motor runs open-loop on the speed-curve PWM only (no coast sampling, no
 * feedback). Stored in its own NVS key so the flag survives independently of
 * the calibration blob. */
esp_err_t settings_bemf_use_load(bool *out_enabled);
esp_err_t settings_bemf_use_save(bool enabled);

#endif
