/* Function (F) mappings, AUX configuration and light/logic application.
 * Owns the function-apply lock and the cached bindings, track categories,
 * AUX configuration and motion/logic state. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "auxio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"
#include "sound.h"

static const char *TAG_FUNC = "web";

static bool s_fn[WEB_FN_COUNT];
static uint8_t s_track_cat[SETTINGS_MAX_TRACKS];
static bool s_track_cat_loaded;
static settings_func_map_t s_func_map[SETTINGS_FUNC_MAP_COUNT];
static func_binding_t s_func_bind[FUNC_BIND_MAX];
static size_t s_func_bind_count;
static settings_aux_cfg_t s_aux_cfg[SETTINGS_AUX_COUNT];
static uint16_t s_func_last_mask[SETTINGS_FUNC_MAP_COUNT];
static SemaphoreHandle_t s_func_mutex;
static bool s_func_bind_present;
static uint16_t s_outputs_applied;
static audio_voice_handle_t s_legacy_voice[SETTINGS_FUNC_MAP_COUNT][2];
static settings_track_t s_func_tracks[SETTINGS_MAX_TRACKS];
static size_t s_func_tracks_count;
static bool s_func_tracks_valid;
static bool s_motion_forward = true;
static uint8_t s_motion_speed;
static bool s_motion_initialized;


/* Heap size of the /api/audio/tracks JSON frame (was a 8 KB stack buffer). */
#define WEB_TRACKS_JSON_MAX 8192

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


/* ---------- function outputs ---------- */


uint8_t web_get_voice_volume(uint8_t fn)
{
    uint8_t engine_vol;
    uint8_t effects_vol;
    /* Called from the DCC audio path: never block behind an HTTP NVS save. */
    if (web_cfg_lock(0)) {
        engine_vol = web_cfg()->engine_volume;
        effects_vol = web_cfg()->effects_volume;
        web_cfg_unlock();
    } else {
        engine_vol = web_cfg()->engine_volume;
        effects_vol = web_cfg()->effects_volume;
    }
    return web_util_voice_volume(fn, s_track_cat, s_track_cat_loaded,
                                 engine_vol, effects_vol);
}

void web_master_volume_changed(uint8_t vol0_100)
{
    if (vol0_100 > 100U) {
        vol0_100 = 100U;
    }
    if (web_cfg_lock(pdMS_TO_TICKS(100))) {
        web_cfg()->master_volume = vol0_100;
        (void)settings_save_deferred(web_cfg());
        web_cfg_unlock();
    }
    (void)audio_set_volume(vol0_100);
}

/* Effect-specific cycle period (ms): Mars ~1.75 Hz, beacon ~1.1 Hz, strobe
 * and ditch use their own cadence; others ignore it. */
static uint8_t aux_level_to_pwm(uint8_t level)
{
    if (level > 100U) level = 100U;
    return (uint8_t)((uint16_t)level * 255U / 100U);
}
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

static bool func_var_resolve(void *ctx, uint8_t index, int32_t *out)
{
    (void)ctx;
    return sound_variable_get(index, out);
}

/* Desired output mask for one function: canonical bindings when present,
 * otherwise the legacy per-function map (verbatim old behaviour). */
static uint16_t func_desired_locked(uint8_t fn, bool fn_on)
{
    if (s_func_bind_present || s_func_bind_count > 0U) {
        uint8_t dir = s_motion_forward ? FUNC_DIR_FWD : FUNC_DIR_REV;
        uint8_t st = (s_motion_speed != 0U) ? FUNC_STATE_MOVING : FUNC_STATE_STOPPED;
        return fn_on ? func_eval_ex(s_func_bind, s_func_bind_count, fn, st, dir, NULL, NULL,
                                    func_var_resolve, NULL) : 0U;
    }
    return web_util_func_desired(&s_func_map[fn], fn_on, s_motion_forward, s_motion_speed);
}

static uint16_t s_logic_active;
static int8_t s_logic_notch;


/* Force every light/AUX output off and drop the applied-output mirror. Used by
 * the emergency path and by an in-flight apply that lost the safety epoch. */
static void func_outputs_force_off_locked(void)
{
    for (uint8_t ch = 0; ch < AUXIO_CH_COUNT; ++ch) {
        (void)auxio_set_enabled(ch, false);
    }
    s_outputs_applied = 0;
    s_logic_active = 0;
}

static void logic_collect_cb(void *ctx, uint8_t target_type, uint8_t target_id, bool active)
{
    if (target_type != FUNC_TARGET_LOGIC || !active ||
        target_id < FUNC_LOGIC_MUTE_STOP || target_id > FUNC_LOGIC_NOTCH_DOWN) {
        return;
    }
    *(uint16_t *)ctx |= (uint16_t)(1U << (target_id - 1U));
}

/* Apply FUNC_TARGET_LOGIC actions. Mutes are evaluated against the current
 * motion/light state; notch up/down are edge-triggered; drive hold/coast/brake
 * are passed to the sound engine, which owns the runtime speed transform. */
static void logic_apply_locked(uint16_t mask)
{
    const uint16_t up = (uint16_t)(1U << (FUNC_LOGIC_NOTCH_UP - 1U));
    const uint16_t down = (uint16_t)(1U << (FUNC_LOGIC_NOTCH_DOWN - 1U));
    if ((mask & up) != 0U && (s_logic_active & up) == 0U && s_logic_notch < 24) {
        ++s_logic_notch;
    }
    if ((mask & down) != 0U && (s_logic_active & down) == 0U && s_logic_notch > -24) {
        --s_logic_notch;
    }
    bool light_fwd = false, light_rev = false;
    (void)auxio_get_enabled(func_out_channel(0), &light_fwd);
    (void)auxio_get_enabled(func_out_channel(1), &light_rev);
    bool light_on = light_fwd || light_rev;
    bool mute = ((mask & (1U << (FUNC_LOGIC_MUTE_STOP - 1U))) != 0U &&
                 s_motion_speed == 0U) ||
                ((mask & (1U << (FUNC_LOGIC_MUTE_MOVE - 1U))) != 0U &&
                 s_motion_speed != 0U) ||
                ((mask & (1U << (FUNC_LOGIC_MUTE_LIGHT - 1U))) != 0U && light_on);
    sound_set_logic(mute,
                    (mask & (1U << (FUNC_LOGIC_DRIVE_HOLD - 1U))) != 0U,
                    (mask & (1U << (FUNC_LOGIC_COAST - 1U))) != 0U,
                    (mask & (1U << (FUNC_LOGIC_DYNAMIC_BRAKE - 1U))) != 0U,
                    s_logic_notch);
    s_logic_active = mask;
}

/* Caller must hold s_func_mutex (or be single-threaded at init). */
static void func_apply_all_locked(void)
{
    if (!web_actuation_ready() || web_maintenance_active()) return;
    uint16_t desired = 0U;
    uint16_t logic = 0U;
    uint8_t dir = s_motion_forward ? FUNC_DIR_FWD : FUNC_DIR_REV;
    uint8_t st = (s_motion_speed != 0U) ? FUNC_STATE_MOVING : FUNC_STATE_STOPPED;
    for (uint8_t f = 0; f < SETTINGS_FUNC_MAP_COUNT; ++f) {
        if (s_func_bind_present || s_func_bind_count > 0U) {
            s_func_last_mask[f] = s_fn[f]
                ? func_eval_ex(s_func_bind, s_func_bind_count, f, st, dir,
                               logic_collect_cb, &logic, func_var_resolve, NULL)
                : 0U;
        } else {
            s_func_last_mask[f] = web_util_func_desired(&s_func_map[f], s_fn[f],
                                                        s_motion_forward, s_motion_speed);
        }
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
    logic_apply_locked(logic);
}

/* Per-function entry point used by the REST/DCC paths. All bindings are
 * evaluated because direction/motion conditions can affect several F-keys. */
static void func_apply_output_locked(uint8_t fn)
{
    if (fn >= SETTINGS_FUNC_MAP_COUNT) return;
    func_apply_all_locked();
}

void web_motion_changed(uint8_t speed, bool forward)
{
    if (!web_actuation_ready() || web_maintenance_active()) return;
    if (s_motion_initialized && speed == s_motion_speed && forward == s_motion_forward) {
        return;
    }
    /* Sample before waiting: an emergency reset that runs while this task is
     * blocked on the function lock must invalidate this update. */
    uint32_t epoch = web_control_safety_epoch_get();
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    if (web_control_safety_epoch_get() != epoch) {
        s_motion_speed = 0;
        s_motion_forward = true;
        s_motion_initialized = true;
        func_outputs_force_off_locked();
        sound_set_logic(false, false, false, false, s_logic_notch);
        if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
        return;
    }
    s_motion_initialized = true;
    s_motion_speed = speed;
    s_motion_forward = forward;
    func_apply_all_locked();
    /* An emergency reset may have run while the function lock was contended.
     * Its outputs must win over this in-flight update. */
    if (web_control_safety_epoch_get() != epoch) {
        /* Discard the whole in-flight update, including its motion mirror, so a
         * repeat of the same command cannot be swallowed by the early return. */
        s_motion_speed = 0;
        s_motion_forward = true;
        s_motion_initialized = true;
        func_outputs_force_off_locked();
        sound_set_logic(false, false, false, false, s_logic_notch);
    }
    if (s_func_mutex != NULL) {
        xSemaphoreGive(s_func_mutex);
    }
    web_log_event("Ход", "скор %u, напр %s", (unsigned)speed,
                  forward ? "вперёд" : "назад");
}

void web_emergency_reset(void)
{
    /* Invalidate every function/motion apply that is already in flight before
     * touching the shared state. This is lock-free and cannot be starved by a
     * handler that holds s_func_mutex across slow I/O. */
    web_control_safety_epoch_bump();
    bool locked = false;
    if (s_func_mutex != NULL) {
        /* Never block a DCC/safety callback behind an HTTP handler that holds
         * the function lock across storage/NVS. If the lock is busy, clear the
         * latched outputs and state directly; the epoch check in the apply
         * paths makes the reset win once that handler finishes. */
        locked = xSemaphoreTake(s_func_mutex, 0) == pdTRUE;
    }
    if (!locked) {
        ESP_LOGW(TAG_FUNC, "emergency reset without function lock");
    }
    memset(s_fn, 0, sizeof(s_fn));
    func_outputs_force_off_locked();
    sound_set_logic(false, false, false, false, s_logic_notch);
    s_motion_speed = 0;
    s_motion_initialized = true;
    if (locked) xSemaphoreGive(s_func_mutex);
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

/* Refresh the RAM snapshot used by the DCC/function hot path. NVS is read here
 * (init / upload / delete), never from web_apply_function(). */
static void func_tracks_cache_refresh(void)
{
    bool locked = s_func_mutex != NULL &&
        xSemaphoreTake(s_func_mutex, portMAX_DELAY) == pdTRUE;
    size_t count = 0;
    esp_err_t err = settings_tracks_load(s_func_tracks, &count);
    if (err == ESP_OK) {
        if (count > SETTINGS_MAX_TRACKS) count = SETTINGS_MAX_TRACKS;
        s_func_tracks_count = count;
        s_func_tracks_valid = true;
    } else if (err == ESP_ERR_NOT_FOUND) {
        s_func_tracks_count = 0;
        s_func_tracks_valid = true;
    } else {
        s_func_tracks_count = 0;
        s_func_tracks_valid = false;
    }
    if (locked) xSemaphoreGive(s_func_mutex);
}

/* Apply a function state exactly like the DCC decoder does: drive the mapped
 * AUX/light outputs and start/stop the mapped sound slots. Both the DCC
 * callback (rails mode) and POST /api/function (web mode) go through here, so
 * the reaction is identical regardless of the control source. Removed the old
 * hard-coded F2..F8 -> AUX1..AUX7 binding; everything now comes from the map. */
void web_apply_function(uint8_t fn, bool state)
{
    if (!web_actuation_ready() || web_maintenance_active()) return;
    if (fn >= SETTINGS_FUNC_MAP_COUNT) {
        return;
    }
    uint32_t epoch = web_control_safety_epoch_get();
    if (s_func_mutex != NULL) {
        (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    }
    /* An emergency reset may have completed while we were waiting for the
     * function lock. Do not apply anything in that case. */
    if (web_control_safety_epoch_get() != epoch) {
        if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
        return;
    }

    bool changed = (s_fn[fn] != state);
    s_fn[fn] = state;
    func_apply_output_locked(fn);
    if (web_control_safety_epoch_get() != epoch) {
        /* web_emergency_reset() ran while this apply was in flight. */
        s_fn[fn] = false;
        func_outputs_force_off_locked();
        sound_set_logic(false, false, false, false, s_logic_notch);
        if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
        return;
    }

    char sounds[48];
    size_t sounds_used = 0;
    sounds[0] = '\0';

    /* With an active sound scheme the engine owns the function-key routing and
     * the reserved engine voices, so the legacy slot mapping below is skipped. */
    bool scheme_on = sound_enabled();
    if (changed && scheme_on && web_control_safety_epoch_get() == epoch) {
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
            /* RAM snapshot only: web_apply_function() runs on the DCC task and
             * must never touch NVS or the filesystem. */
            size_t count = s_func_tracks_count;
            if (s_func_tracks_valid) {
                const uint8_t want[2] = { slot_a, use_b ? slot_b : 0U };
                for (uint8_t k = 0; k < 2U; ++k) {
                    bool started = false;
                    if (web_control_safety_epoch_get() != epoch) break;
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

    if (web_control_safety_epoch_get() != epoch) {
        /* The reset must win even if it arrived after the output/sound phase. */
        audio_voice_release_owned(s_legacy_voice[fn][0]);
        audio_voice_release_owned(s_legacy_voice[fn][1]);
        memset(s_legacy_voice[fn], 0, sizeof(s_legacy_voice[fn]));
        sound_request_stop();
        func_outputs_force_off_locked();
        sound_set_logic(false, false, false, false, s_logic_notch);
        s_fn[fn] = false;
        changed = false;
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
        ESP_LOGE(TAG_FUNC, "auxio init failed");
    }
    return err;
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

bool web_func_bind_add(const func_binding_t *b)
{
    func_binding_t tmp[FUNC_BIND_MAX];
    memcpy(tmp, s_func_bind, sizeof(tmp));
    size_t n = s_func_bind_count;
    if (settings_func_bind_add(tmp, &n, b) != ESP_OK) {
        return false;
    }
    return web_func_bind_commit(tmp, n);
}

bool web_func_bind_remove(size_t idx)
{
    func_binding_t tmp[FUNC_BIND_MAX];
    memcpy(tmp, s_func_bind, sizeof(tmp));
    size_t n = s_func_bind_count;
    if (settings_func_bind_remove(tmp, &n, idx) != ESP_OK) {
        return false;
    }
    return web_func_bind_commit(tmp, n);
}





esp_err_t web_func_init(void)
{
    memset(s_fn, 0, sizeof(s_fn));
    s_func_mutex = xSemaphoreCreateMutex();
    if (s_func_mutex == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err;
    {
        size_t cat_count = 0;
        err = settings_track_cats_load(s_track_cat, &cat_count);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
        s_track_cat_loaded = true;
    }
    {
        size_t fmap_count = 0;
        err = settings_func_map_load(s_func_map, &fmap_count);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    }
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind_count = 0;
    err = func_bind_load();
    if (err != ESP_OK) return err;
    func_tracks_cache_refresh();

    /* Bring up the AUX outputs before restoring the saved per-channel level/
     * effect: auxio_config() only programmes channels that auxio_init() has
     * created, otherwise it fails and the values are overwritten with defaults. */
    err = outputs_init();
    if (err != ESP_OK) return err;

    {
        size_t aux_count = 0;
        err = settings_aux_cfg_load(s_aux_cfg, &aux_count);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
        for (uint8_t i = 0; i < SETTINGS_AUX_COUNT; ++i) {
            if (s_aux_cfg[i].level > 100U) s_aux_cfg[i].level = 100U;
            if ((uint32_t)s_aux_cfg[i].effect >= AUXIO_EFFECT_COUNT)
                s_aux_cfg[i].effect = AUXIO_EFFECT_STEADY;
            uint8_t pwm = aux_level_to_pwm(s_aux_cfg[i].level);
            err = auxio_config(i, pwm, 0, (auxio_effect_t)s_aux_cfg[i].effect,
                               aux_effect_period(s_aux_cfg[i].effect));
            if (err != ESP_OK) return err;
        }
    }
    return ESP_OK;
}

void web_func_refresh_tracks(void)
{
    func_tracks_cache_refresh();
}

uint32_t web_func_levels(void)
{
    uint32_t levels = 0;
    if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) if (s_fn[fn]) levels |= 1UL << fn;
    if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
    return levels;
}

void web_func_io_state(uint16_t *outputs, uint16_t *logic, uint32_t *functions)
{
    if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    *outputs = s_outputs_applied;
    *logic = s_logic_active;
    uint32_t fn = 0;
    for (uint8_t i = 0; i < WEB_FN_COUNT; ++i) if (s_fn[i]) fn |= 1UL << i;
    *functions = fn;
    if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
}

uint8_t web_func_track_cat(size_t index)
{
    return index < SETTINGS_MAX_TRACKS ? s_track_cat[index] : 0;
}

void web_func_track_cat_set(size_t index, uint8_t cat)
{
    if (index >= SETTINGS_MAX_TRACKS) return;
    s_track_cat[index] = cat;
    (void)settings_track_cats_save(s_track_cat, SETTINGS_MAX_TRACKS);
}

esp_err_t web_func_reconcile_slots(const bool *slots)
{
    esp_err_t result = ESP_OK;
    bool func_locked = s_func_mutex != NULL &&
        xSemaphoreTake(s_func_mutex, portMAX_DELAY) == pdTRUE;
    bool map_changed = false;
    for (uint8_t fn = 0; fn < SETTINGS_FUNC_MAP_COUNT; ++fn) {
        if (s_func_map[fn].slot_a != 0U &&
            (s_func_map[fn].slot_a > SETTINGS_MAX_TRACKS ||
             !slots[s_func_map[fn].slot_a])) {
            s_func_map[fn].slot_a = 0;
            map_changed = true;
        }
        if (s_func_map[fn].slot_b != 0U &&
            (s_func_map[fn].slot_b > SETTINGS_MAX_TRACKS ||
             !slots[s_func_map[fn].slot_b])) {
            s_func_map[fn].slot_b = 0;
            map_changed = true;
        }
    }
    if (map_changed) {
        esp_err_t save = settings_func_map_save(s_func_map, SETTINGS_FUNC_MAP_COUNT);
        if (result == ESP_OK) result = save;
    }

    bool bind_changed = false;
    size_t kept_bind = 0;
    for (size_t i = 0; i < s_func_bind_count; ++i) {
        const func_binding_t *b = &s_func_bind[i];
        if (b->used && b->target_type == FUNC_TARGET_SLOT &&
            (b->target_id == 0U || b->target_id > SETTINGS_MAX_TRACKS ||
             !slots[b->target_id])) {
            bind_changed = true;
            continue;
        }
        if (kept_bind != i) s_func_bind[kept_bind] = *b;
        ++kept_bind;
    }
    if (bind_changed) {
        s_func_bind_count = kept_bind;
        esp_err_t save = settings_func_bind_save(s_func_bind, s_func_bind_count);
        if (result == ESP_OK) result = save;
    }
    if (func_locked) xSemaphoreGive(s_func_mutex);
    func_tracks_cache_refresh();
    return result;
}

bool web_func_slots_referenced(const bool *slot_used)
{
    bool binding_ref = false;
    bool func_locked = s_func_mutex != NULL &&
        xSemaphoreTake(s_func_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
    if (func_locked) {
        for (size_t i = 0; i < s_func_bind_count; ++i) {
            const func_binding_t *b = &s_func_bind[i];
            if (b->used && b->target_type == FUNC_TARGET_SLOT &&
                b->target_id >= 1U && b->target_id <= SETTINGS_MAX_TRACKS &&
                slot_used[b->target_id]) {
                binding_ref = true;
                break;
            }
        }
        for (uint8_t f = 0; f < SETTINGS_FUNC_MAP_COUNT && !binding_ref; ++f) {
            uint8_t a = s_func_map[f].slot_a;
            uint8_t b = s_func_map[f].slot_b;
            if ((a >= 1U && a <= SETTINGS_MAX_TRACKS && slot_used[a]) ||
                (b >= 1U && b <= SETTINGS_MAX_TRACKS && slot_used[b])) {
                binding_ref = true;
            }
        }
        xSemaphoreGive(s_func_mutex);
    }
    return binding_ref;
}


void web_func_states(bool out[WEB_FN_COUNT])
{
    if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    memcpy(out, s_fn, sizeof(s_fn));
    if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
}

const char *web_func_output_name(uint8_t ch)
{
    return ch < WEB_ARRAY_SIZE(WEB_OUT_NAMES) ? WEB_OUT_NAMES[ch] : "";
}

size_t web_func_bind_count_get(void)
{
    if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    size_t count = s_func_bind_count;
    if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
    return count;
}

bool web_func_bind_at(size_t idx, func_binding_t *out)
{
    if (out == NULL) return false;
    if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    bool ok = idx < s_func_bind_count && idx < FUNC_BIND_MAX;
    if (ok) *out = s_func_bind[idx];
    if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
    return ok;
}

void web_func_aux_snapshot(settings_aux_cfg_t out[SETTINGS_AUX_COUNT])
{
    if (s_func_mutex != NULL) (void)xSemaphoreTake(s_func_mutex, portMAX_DELAY);
    memcpy(out, s_aux_cfg, sizeof(s_aux_cfg));
    if (s_func_mutex != NULL) xSemaphoreGive(s_func_mutex);
}

web_aux_update_t web_func_aux_update(uint8_t ch, uint8_t level, uint8_t fx)
{
    if (ch >= SETTINGS_AUX_COUNT) return WEB_AUX_UPDATE_SAVE_FAIL;
    settings_aux_cfg_t previous = s_aux_cfg[ch];
    s_aux_cfg[ch].level = level;
    s_aux_cfg[ch].effect = fx;
    if (settings_aux_cfg_save(s_aux_cfg, SETTINGS_AUX_COUNT) != ESP_OK) {
        s_aux_cfg[ch] = previous;
        return WEB_AUX_UPDATE_SAVE_FAIL;
    }
    uint8_t pwm = aux_level_to_pwm(level);
    if (auxio_config(ch, pwm, 0, (auxio_effect_t)fx, aux_effect_period(fx)) != ESP_OK) {
        s_aux_cfg[ch] = previous;
        (void)settings_aux_cfg_save(s_aux_cfg, SETTINGS_AUX_COUNT);
        uint8_t prev_pwm = aux_level_to_pwm(previous.level);
        (void)auxio_config(ch, prev_pwm, 0, (auxio_effect_t)previous.effect,
                           aux_effect_period(previous.effect));
        return WEB_AUX_UPDATE_APPLY_FAIL;
    }
    return WEB_AUX_UPDATE_OK;
}