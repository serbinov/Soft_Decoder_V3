/* Sound-scheme engine (SOUND_ENGINE_ROADMAP.md R3).
 *
 * Owns the active scheme, the engine state machine, the Init/Loop/End table
 * sequencer and the playback-rate control. The real-time inputs are the
 * applied motor speed (R4) and the function keys (DCC / web). All logic that
 * decides *what* to play is kept in small pure helpers so it is host-testable;
 * the actual voice I/O goes through the `audio` component. */

#include "sound.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "motor.h"
#include "settings.h"
#include "sound_store.h"
#include "storage.h"

static const char *TAG = "sound";

#define SOUND_TICK_MS          20
#define SOUND_VOICE_ENGINE     18
#define SOUND_VOICE_ENGINE_X   19
#define SOUND_FN_COUNT         SETTINGS_FUNC_MAP_COUNT
#define SOUND_PATH_MAX         192

/* Hysteresis dead-bands for the speed/accel thresholds (roadmap R3.4). */
#define SOUND_ACCEL_HYST       2

typedef enum { TB_STOP = 0, TB_INIT, TB_LOOP, TB_END } tb_phase_t;

static SemaphoreHandle_t s_lock;
static sound_scheme_t s_scheme;
static char s_active[SOUND_FILE_MAX];

/* Staging copy for file I/O done *outside* s_lock so a scheme load/save (which
 * touches LittleFS) never blocks the real-time DCC path (REV-RT1). The ~26 KB
 * scheme must never live on a task stack; there is a single httpd caller, so
 * one shared slot is enough. */
static sound_scheme_t s_io_scheme;

/* Engine runtime. */
static bool s_engine_key;          /* latched engine-start key state */
static uint8_t s_speed;
static bool s_forward = true;
static uint8_t s_prev_speed;
static int32_t s_accel;            /* EMA of d(speed)/tick */
static uint8_t s_table = SOUND_TABLE_NONE;
static uint8_t s_phase = TB_STOP;
static uint8_t s_group;
static uint8_t s_plays;
static bool s_brake_done;
/* Mute logic bindings (R6.4): silence the engine when idle / while moving, and
 * silence effect voices. */
static bool s_mute_stop;
static bool s_mute_move;
static bool s_mute_light;
static int64_t s_extra_next[SOUND_MAX_EXTRAS];
static uint8_t s_extra_voice[SOUND_MAX_EXTRAS];
static audio_voice_handle_t s_extra_handle[SOUND_MAX_EXTRAS];
static uint8_t s_extra_on[SOUND_MAX_EXTRAS];
/* Test hook: limit the task loop so the body can run on the host (0 = run
 * forever, as on target). Mirrors audio's s_mix_iter_cap. */
static uint32_t s_task_iter_cap;

/* Function bindings + per-function playback state. */
static func_binding_t s_binds[FUNC_BIND_MAX];
static size_t s_bind_count;
static bool s_fn_state[SOUND_FN_COUNT];
static uint8_t s_fn_voice[SOUND_FN_COUNT];
static audio_voice_handle_t s_fn_handle[SOUND_FN_COUNT];
static audio_voice_handle_t s_engine_handle;
static bool s_inhibited;
static bool s_control_armed = true;
static int32_t s_accel_q; /* Q10 retains fractional decay toward zero */
static int64_t s_fn_time[SOUND_FN_COUNT];
static sg_graph_t *s_graph;
static sg_runner_t *s_graph_runner;
static char s_graph_id[SG_ID_CAP];
static uint32_t s_graph_revision;
static bool s_graph_fault;
static atomic_bool s_stop_requested;
static atomic_uint s_motion_epoch;
static bool s_motion_ready = true;
struct sound_graph_prepared {
    sg_graph_t *graph;
    sg_runner_t *runner;
    char id[SG_ID_CAP];
    uint32_t revision;
};

/* Forward declarations (effect playback is defined further down). */
static uint8_t fx_play_table(uint8_t table, bool loop, uint8_t *slot);
static void fx_voice_stop(uint8_t *slot);
static void runtime_reset_locked(void);

static void graph_audio_release(void *ctx, sg_handle_t h)
{
    (void)ctx; audio_voice_release_owned((audio_voice_handle_t){h.voice,h.generation});
}
static esp_err_t graph_audio_play(void *ctx, bool engine, const sg_state_t *state, sg_handle_t *out)
{
    (void)ctx;
    if (s_inhibited || atomic_load(&s_stop_requested)) { return ESP_ERR_INVALID_STATE; }
    char path[SOUND_PATH_MAX];
    int path_len = snprintf(path,sizeof(path),"%s/audio/%s",storage_get_root(),state->file);
    if (path_len < 0 || (size_t)path_len >= sizeof(path)) { return ESP_ERR_INVALID_SIZE; }
    audio_voice_handle_t h = {AUDIO_VOICE_NONE,0}; esp_err_t err;
    if (engine) { err = audio_voice_play_generation(SOUND_VOICE_ENGINE,path,false,state->volume,&h); }
    else {
        err = audio_voice_alloc_owned(&h);
        if (err == ESP_OK) { err = audio_voice_play_owned(&h,path,false,state->volume); }
    }
    if (err == ESP_OK) { err = audio_voice_set_rate_owned(h,state->rate); }
    if (err != ESP_OK) { audio_voice_release_owned(h); return err; }
    *out = (sg_handle_t){h.voice,h.generation}; return ESP_OK;
}
static sg_audio_state_t graph_audio_poll(void *ctx, sg_handle_t h)
{
    (void)ctx; audio_voice_handle_t handle = {h.voice,h.generation};
    audio_completion_t done = audio_voice_completion(handle);
    if (done == AUDIO_COMPLETION_EOF) { return SG_AUDIO_DONE; }
    if (done != AUDIO_COMPLETION_NONE) { return SG_AUDIO_FAILED; }
    audio_voice_state_t state = audio_voice_get_state(handle);
    if (state == AUDIO_VOICE_PENDING) { return SG_AUDIO_PENDING; }
    if (state == AUDIO_VOICE_PLAYING) { return SG_AUDIO_PLAYING; }
    /* EOF may have been published between the two generation-scoped queries. */
    done = audio_voice_completion(handle);
    return done == AUDIO_COMPLETION_EOF ? SG_AUDIO_DONE : SG_AUDIO_FAILED;
}

/* Serialize the engine's shared state: the 20 ms task, the DCC/web function
 * path and the REST mutators all touch s_scheme/s_engine_key/s_binds. */
static void sound_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void sound_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static void build_path(const char *name, char *buf, size_t cap)
{
    if (name[0] == '/') {
        snprintf(buf, cap, "%s", name);
    } else {
        snprintf(buf, cap, "%s/%s", storage_get_root(), name);
    }
}

static audio_voice_handle_t *fx_handle(uint8_t *slot)
{
    for (size_t i = 0; i < SOUND_FN_COUNT; ++i) {
        if (slot == &s_fn_voice[i]) { return &s_fn_handle[i]; }
    }
    for (size_t i = 0; i < SOUND_MAX_EXTRAS; ++i) {
        if (slot == &s_extra_voice[i]) { return &s_extra_handle[i]; }
    }
    return NULL;
}

static bool fx_running(uint8_t *slot)
{
    audio_voice_handle_t *h = fx_handle(slot);
    return h != NULL && *slot != AUDIO_VOICE_NONE &&
           audio_voice_get_state(*h) != AUDIO_VOICE_FINISHED;
}

static void fx_voice_stop(uint8_t *slot)
{
    if (*slot != AUDIO_VOICE_NONE) {
        audio_voice_handle_t *h = fx_handle(slot);
        if (h != NULL) { audio_voice_release_owned(*h); }
        *slot = AUDIO_VOICE_NONE;
    }
}

/* First playable file of a table (prefers Loop, then Init, then End). */
static const char *table_first_file(const sound_table_t *t, char *buf, size_t cap)
{
    for (uint8_t g = 0; g < SOUND_MAX_GROUPS; ++g) {
        if (t->loop[g].file[0] != '\0') {
            build_path(t->loop[g].file, buf, cap);
            return buf;
        }
    }
    for (uint8_t g = 0; g < SOUND_MAX_GROUPS; ++g) {
        if (t->init[g].file[0] != '\0') {
            build_path(t->init[g].file, buf, cap);
            return buf;
        }
    }
    for (uint8_t g = 0; g < SOUND_MAX_GROUPS; ++g) {
        if (t->end[g].file[0] != '\0') {
            build_path(t->end[g].file, buf, cap);
            return buf;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* pure logic                                                         */
/* ------------------------------------------------------------------ */

/* Speed (0..255) -> drive step index 0..SOUND_ENGINE_STEPS-1. */
static uint8_t sound_step_for_speed(uint8_t speed)
{
    if (speed == 0U) {
        return 0U;
    }
    /* speed <= 255 rounds down to at most SOUND_ENGINE_STEPS-1. */
    return (uint8_t)(((uint16_t)speed * SOUND_ENGINE_STEPS) / 256U);
}

/* T-03 rate scaling: base * (1 + v/255 * accel/64), clipped 0.5x..3x. */
static uint16_t sound_rate_for(uint8_t rate_scale, uint8_t speed, uint16_t base_permille)
{
    uint32_t p = base_permille;
    p += (uint32_t)base_permille * (uint32_t)speed * (uint32_t)rate_scale / (255U * 64U);
    if (p < AUDIO_RATE_MIN) {
        p = AUDIO_RATE_MIN;
    }
    if (p > AUDIO_RATE_MAX) {
        p = AUDIO_RATE_MAX;
    }
    return (uint16_t)p;
}

/* Read a CV with a fallback default (sound R6.1). */
static uint8_t cv_u8(uint16_t idx, uint8_t def)
{
    uint8_t v = def;
    if (settings_cv_read(idx, &v) != ESP_OK) {
        v = def;
    }
    return v;
}

/* Sound-rate CV (R6.1) -> playback-rate permille: 0.5x (cv 0) .. 3.0x
 * (cv 255), with cv 51 = 1.0x nominal. CV114 = chuff/exhaust, CV115 = bell,
 * CV116 = dynamic brake. */
static uint16_t cv_rate_permille(uint16_t idx, uint8_t fallback)
{
    uint8_t cv = cv_u8(idx, fallback);
    return (uint16_t)(500U + ((uint32_t)cv * 2500U) / 255U);
}

static uint32_t s_rng = 0x12345678u;

static uint32_t rng_next(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 16) & 0x7FFFu;
}

/* Choose the engine table for the current speed/accel. `flags` are the CV30
 * option bits; returns SOUND_TABLE_NONE when the engine is off or idle. */
static uint8_t sound_engine_pick(uint8_t speed, int32_t accel, uint8_t flags)
{
    const sound_engine_t *e = &s_scheme.engine;
    if (!s_engine_key) {
        return SOUND_TABLE_NONE;
    }
    /* Mute logic bindings silence the engine for the matching motion state. */
    if ((speed == 0U && s_mute_stop) || (speed != 0U && s_mute_move)) {
        return SOUND_TABLE_NONE;
    }
    if (speed == 0U) {
        if ((flags & SOUND_ENG_SKIP_D1TOS) != 0U) {
            return SOUND_TABLE_NONE;
        }
        return e->stop_table;
    }
    uint8_t step = sound_step_for_speed(speed);
    if (step == 0U && (flags & SOUND_ENG_SKIP_STOD1) != 0U) {
        return SOUND_TABLE_NONE;
    }
    if (accel >= SOUND_ACCEL_HYST && e->accel[step] != SOUND_TABLE_NONE) {
        return e->accel[step];
    }
    if (accel <= -SOUND_ACCEL_HYST && e->coast[step] != SOUND_TABLE_NONE) {
        return e->coast[step];
    }
    if (e->drive[step] != SOUND_TABLE_NONE) {
        return e->drive[step];
    }
    return e->stop_table;
}

static const sound_table_t *table_at(uint8_t idx)
{
    if (idx == SOUND_TABLE_NONE || idx >= SOUND_MAX_TABLES || !s_scheme.tables[idx].used) {
        return NULL;
    }
    return &s_scheme.tables[idx];
}

/* ------------------------------------------------------------------ */
/* table sequencer (R3.3)                                             */
/* ------------------------------------------------------------------ */

static bool tb_play_init(void)
{
    const sound_table_t *t = table_at(s_table);
    if (t == NULL) {
        return false;
    }
    char path[SOUND_PATH_MAX];
    while (s_group < SOUND_MAX_GROUPS) {
        if (t->init[s_group].file[0] != '\0') {
            build_path(t->init[s_group].file, path, sizeof(path));
            return audio_voice_play_generation(SOUND_VOICE_ENGINE, path, false, 100,
                                                &s_engine_handle) == ESP_OK;
        }
        s_group++;
    }
    return false;
}

static bool tb_play_loop(void)
{
    const sound_table_t *t = table_at(s_table);
    if (t == NULL || t->loop[0].file[0] == '\0') {
        return false;
    }
    char path[SOUND_PATH_MAX];
    build_path(t->loop[0].file, path, sizeof(path));
    return audio_voice_play_generation(SOUND_VOICE_ENGINE, path, false, 100,
                                        &s_engine_handle) == ESP_OK;
}

static void tb_play_end(void)
{
    s_engine_handle = (audio_voice_handle_t){AUDIO_VOICE_NONE, 0};
    const sound_table_t *t = table_at(s_table);
    if (t == NULL || t->end[0].file[0] == '\0') {
        return;
    }
    char path[SOUND_PATH_MAX];
    build_path(t->end[0].file, path, sizeof(path));
    (void)audio_voice_play_generation(SOUND_VOICE_ENGINE, path, false, 100,
                                     &s_engine_handle);
}

static void tb_start(uint8_t idx)
{
    s_engine_handle = (audio_voice_handle_t){AUDIO_VOICE_NONE, 0};
    (void)audio_voice_stop(SOUND_VOICE_ENGINE);
    (void)audio_voice_set_rate(SOUND_VOICE_ENGINE, 1000);
    s_group = 0;
    s_plays = 0;
    s_table = idx;
    if (table_at(idx) == NULL) {
        s_table = SOUND_TABLE_NONE;
        s_phase = TB_STOP;
        return;
    }
    s_phase = TB_INIT;
    if (!tb_play_init()) {
        s_phase = TB_LOOP;
        if (!tb_play_loop()) {
            s_phase = TB_END;
            tb_play_end();
        }
    }
}

static void tb_advance(void)
{
    if (table_at(s_table) == NULL || s_phase == TB_STOP) {
        return;
    }
    if (audio_voice_get_state(s_engine_handle) != AUDIO_VOICE_FINISHED) {
        return;
    }
    switch (s_phase) {
        case TB_INIT:
            /* Advance to the next cylinder group; tb_play_init() itself skips
             * any empty groups, so a gap (init[1] empty, init[2] set) does not
             * cut the steam sequence short. */
            s_group++;
            if (!tb_play_init()) {
                s_phase = TB_LOOP;
                if (!tb_play_loop()) {
                    s_phase = TB_END;
                    tb_play_end();
                }
            }
            break;
        case TB_LOOP: {
            const sound_table_t *t = table_at(s_table);
            uint8_t maxp = t->max_plays;
            uint8_t minp = t->min_plays;
            bool under_max = (maxp == 0U) || (s_plays + 1U < maxp);
            /* min_plays is a floor: keep looping even once max_plays is hit. */
            bool under_min = (s_plays < minp);
            if (t->loop[0].file[0] != '\0' && (under_max || under_min)) {
                s_plays++;
                (void)tb_play_loop();
            } else {
                s_phase = TB_END;
                tb_play_end();
            }
            break;
        }
        case TB_END:
        default:
            s_phase = TB_STOP;
            break;
    }
}

/* ------------------------------------------------------------------ */
/* scheme / storage accessors (R3.2)                                  */
/* ------------------------------------------------------------------ */

esp_err_t sound_scheme_get(sound_scheme_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    *out = s_scheme;
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_scheme_set(const sound_scheme_t *in)
{
    if (in == NULL || sound_store_validate(in) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    runtime_reset_locked();
    s_scheme = *in;
    sound_unlock();
    return ESP_OK;
}

uint8_t sound_type_get(void)
{
    sound_lock();
    uint8_t type = s_scheme.type;
    sound_unlock();
    return type;
}

esp_err_t sound_type_set(uint8_t type)
{
    if (type > SOUND_SCHEME_ELECTRIC) { return ESP_ERR_INVALID_ARG; }
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    if (s_scheme.type != type) { runtime_reset_locked(); }
    s_scheme.type = type;
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_engine_get(sound_engine_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    *out = s_scheme.engine;
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_engine_set(const sound_engine_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    s_scheme.engine = *in;
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_table_get(uint8_t idx, sound_table_t *out)
{
    if (out == NULL || idx >= SOUND_MAX_TABLES) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    *out = s_scheme.tables[idx];
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_table_set(uint8_t idx, const sound_table_t *t)
{
    if (t == NULL || idx >= SOUND_MAX_TABLES) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    s_scheme.tables[idx] = *t;
    if (idx + 1U > s_scheme.table_count) {
        s_scheme.table_count = (uint8_t)(idx + 1U);
    }
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_extra_get(uint8_t idx, sound_extra_t *out)
{
    if (out == NULL || idx >= SOUND_MAX_EXTRAS) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    *out = s_scheme.extras[idx];
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_extra_set(uint8_t idx, const sound_extra_t *e)
{
    if (e == NULL || idx >= SOUND_MAX_EXTRAS) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    s_scheme.extras[idx] = *e;
    if (idx + 1U > s_scheme.extra_count) {
        s_scheme.extra_count = (uint8_t)(idx + 1U);
    }
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_brake_get(sound_brake_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    *out = s_scheme.brake;
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_brake_set(const sound_brake_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    s_scheme.brake = *in;
    sound_unlock();
    return ESP_OK;
}

void sound_status_get(sound_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    sound_lock();
    out->type = s_scheme.type;
    out->enabled = s_graph || s_graph_fault || (s_scheme.type != SOUND_SCHEME_NONE && s_scheme.type != SOUND_SCHEME_LEGACY);
    out->engine = s_graph_runner ? s_graph_runner->engine_on : s_engine_key;
    out->table = s_table;
    out->phase = s_phase;
    out->speed = s_speed;
    out->forward = s_forward;
    snprintf(out->name, sizeof(out->name), "%s", s_active);
    sound_unlock();
}

esp_err_t sound_scheme_save(void)
{
    char name[SOUND_FILE_MAX];
    /* Snapshot the name + scheme under the lock, then do the flash I/O with the
     * lock released (the DCC callback must not wait on a LittleFS fsync). */
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    snprintf(name, sizeof(name), "%s", s_active);
    s_io_scheme = s_scheme;
    sound_unlock();
    if (name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    char path[SOUND_PATH_MAX];
    esp_err_t err = sound_store_path(path, sizeof(path), name);
    if (err == ESP_OK) {
        err = sound_store_save(path, &s_io_scheme);
    }
    return err;
}

esp_err_t sound_active_name_get(char *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    sound_lock();
    snprintf(out, cap, "%s", s_active);
    sound_unlock();
    return ESP_OK;
}

esp_err_t sound_load_scheme(const char *name)
{
    if (sound_graph_active()) { return ESP_ERR_INVALID_STATE; }
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Reject path separators / traversal before building the .mds path. The
     * empty name is the explicit "no scheme" reset below. */
    if (name[0] != '\0' && !sound_store_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (name[0] == '\0') {
        sound_lock();
        if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
        runtime_reset_locked();
        (void)sound_store_default(&s_scheme);
        s_active[0] = '\0';
        sound_unlock();
        (void)settings_active_scheme_set("");
        return ESP_OK;
    }
    /* Read the file into the staging buffer *outside* the lock (the ~26 KB
     * scheme is a module static, never a stack local), then install it. */
    char path[SOUND_PATH_MAX];
    esp_err_t err = sound_store_path(path, sizeof(path), name);
    if (err == ESP_OK) {
        err = sound_store_load(path, &s_io_scheme);
    }
    if (err != ESP_OK) {
        (void)sound_store_default(&s_io_scheme);
    }
    /* Persist the name only when the file loaded, or when the failure was just
     * "storage not mounted" (keep the pointer so it loads once storage is up).
     * A missing/corrupt file must not leave a dangling active_scheme that would
     * be retried on every boot (REV-S8). */
    bool keep_name = (err == ESP_OK || err == ESP_ERR_INVALID_STATE);
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    runtime_reset_locked();
    s_scheme = s_io_scheme;
    if (keep_name) {
        snprintf(s_active, sizeof(s_active), "%s", name);
    } else {
        s_active[0] = '\0';
    }
    sound_unlock();
    (void)settings_active_scheme_set(keep_name ? name : "");
    return err;
}

esp_err_t sound_scheme_create(const char *name, uint8_t type)
{
    if (sound_graph_active()) { return ESP_ERR_INVALID_STATE; }
    if (!sound_store_name_ok(name) || type > SOUND_SCHEME_ELECTRIC) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[SOUND_PATH_MAX];
    esp_err_t err = sound_store_path(path, sizeof(path), name);
    if (err != ESP_OK) {
        return err;
    }
    (void)sound_store_default(&s_io_scheme);
    s_io_scheme.type = type;
    err = sound_store_save(path, &s_io_scheme);
    if (err != ESP_OK) {
        return err;
    }
    sound_lock();
    if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
    runtime_reset_locked();
    s_scheme = s_io_scheme;
    snprintf(s_active, sizeof(s_active), "%s", name);
    sound_unlock();
    (void)settings_active_scheme_set(name);
    return ESP_OK;
}

esp_err_t sound_scheme_delete(const char *name)
{
    if (!sound_store_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[SOUND_PATH_MAX];
    esp_err_t err = sound_store_path(path, sizeof(path), name);
    if (err != ESP_OK) {
        return err;
    }
    err = storage_access_begin();
    if (err != ESP_OK) { return err; }
    int removed = remove(path);
    storage_access_end();
    if (removed != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    bool was_active = false;
    sound_lock();
    if (!s_graph && !s_graph_fault && strcmp(s_active, name) == 0) {
        runtime_reset_locked();
        s_active[0] = '\0';
        (void)sound_store_default(&s_scheme);
        was_active = true;
    }
    sound_unlock();
    if (was_active) {
        (void)settings_active_scheme_set("");
    }
    return ESP_OK;
}

esp_err_t sound_scheme_export(const char *name, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (!sound_store_name_ok(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[SOUND_PATH_MAX];
    esp_err_t err = sound_store_path(path, sizeof(path), name);
    if (err != ESP_OK) {
        return err;
    }
    return sound_store_read(path, buf, cap, out_len);
}

esp_err_t sound_scheme_import(const char *name, const uint8_t *buf, size_t len, bool activate)
{
    if (activate && sound_graph_active()) { return ESP_ERR_INVALID_STATE; }
    if (!sound_store_name_ok(name) || buf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = sound_store_verify(buf, len, &s_io_scheme);
    if (err != ESP_OK) {
        return err;
    }
    char path[SOUND_PATH_MAX];
    err = sound_store_path(path, sizeof(path), name);
    if (err == ESP_OK) {
        err = sound_store_save(path, &s_io_scheme);
    }
    if (err == ESP_OK && activate) {
        sound_lock();
        if (s_graph || s_graph_fault) { sound_unlock(); return ESP_ERR_INVALID_STATE; }
        runtime_reset_locked();
        s_scheme = s_io_scheme;
        snprintf(s_active, sizeof(s_active), "%s", name);
        sound_unlock();
        (void)settings_active_scheme_set(name);
    }
    return err;
}

esp_err_t sound_scheme_list(char names[][SOUND_FILE_MAX], size_t max, size_t *count)
{
    if (names == NULL || count == NULL || max == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0U;
    if (!storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char dir[SOUND_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%s", storage_get_root(), SOUND_PROJECTS_DIR);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t admission = storage_access_begin();
    if (admission != ESP_OK) { return admission; }
    DIR *d = opendir(dir);
    if (d == NULL) {
        storage_access_end();
        return ESP_ERR_NOT_FOUND;
    }
    const size_t ext = strlen(SOUND_STORE_EXT);
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *count < max) {
        const char *nm = ent->d_name;
        size_t len = strlen(nm);
        if (len <= ext || strcmp(nm + len - ext, SOUND_STORE_EXT) != 0) {
            continue;
        }
        len -= ext;
        if (len == 0U || len >= SOUND_FILE_MAX) {
            continue;
        }
        memcpy(names[*count], nm, len);
        names[*count][len] = '\0';
        (*count)++;
    }
    closedir(d);
    storage_access_end();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* lint (R3.7)                                                        */
/* ------------------------------------------------------------------ */

int sound_lint(char *out, size_t cap)
{
    if (out != NULL && cap != 0U) {
        out[0] = '\0';
    }
    int problems = 0;
    sound_lock();
    if (s_scheme.type == SOUND_SCHEME_NONE || s_scheme.type == SOUND_SCHEME_LEGACY) {
        sound_unlock();
        return 0;
    }
    uint8_t start = s_scheme.engine.start_table;
    if (table_at(start) == NULL) {
        problems++;
        if (out != NULL) {
            snprintf(out, cap, "no start table");
        }
    }
    for (uint8_t i = 1U; i < SOUND_MAX_TABLES; ++i) {
        const sound_table_t *t = &s_scheme.tables[i];
        if (!t->used) {
            continue;
        }
        if (t->min_speed != 0U && t->max_speed != 0U && t->min_speed > t->max_speed) {
            problems++;
        }
    }
    /* A random extra must target a finite table (R6.4). */
    for (uint8_t j = 0; j < s_scheme.extra_count && j < SOUND_MAX_EXTRAS; ++j) {
        const sound_extra_t *e = &s_scheme.extras[j];
        if (e->mode == SOUND_MODE_RANDOM && e->table != SOUND_TABLE_NONE) {
            const sound_table_t *t = table_at(e->table);
            if (t != NULL && t->max_plays == 0U) {
                problems++;
            }
        }
    }
    sound_unlock();
    return problems;
}

/* ------------------------------------------------------------------ */
/* runtime inputs                                                     */
/* ------------------------------------------------------------------ */

bool sound_engine_is_on(void)
{
    sound_lock();
    bool on = s_graph_runner ? s_graph_runner->engine_on : s_engine_key;
    sound_unlock();
    return on;
}

bool sound_scheme_enabled(void)
{
    sound_lock();
    bool on = s_graph || s_graph_fault || (s_scheme.type != SOUND_SCHEME_NONE && s_scheme.type != SOUND_SCHEME_LEGACY);
    sound_unlock();
    return on;
}

void sound_engine_power(bool on)
{
    sound_lock();
    if (!s_graph_fault && !s_inhibited && !atomic_load(&s_stop_requested)) {
        if (s_graph_runner) { sg_runner_power(s_graph_runner,on); }
        else { s_engine_key = on; }
        s_control_armed = true;
    }
    sound_unlock();
}

static void sound_speed_locked(uint8_t speed, bool forward, bool applied)
{
    bool armed = s_graph_runner ? s_graph_runner->armed : s_control_armed;
    /* A stop-induced motor ramp is not a new command. Disarmed motion must
     * first settle at zero before a later nonzero sample can rearm playback. */
    bool fresh_motion = !applied || armed || (s_motion_ready && speed != 0);
    if (!s_inhibited && !atomic_load(&s_stop_requested) && fresh_motion && (s_speed != speed || s_forward != forward)) {
        s_control_armed = true;
        if (s_graph_runner) { s_graph_runner->armed = true; }
    }
    s_speed = speed;
    s_forward = forward;
    if (speed == 0 || !applied) { s_motion_ready = true; }
}
void sound_set_speed(uint8_t speed, bool forward)
{
    sound_lock(); sound_speed_locked(speed,forward,false);
    sound_unlock();
}
static void sound_applied_speed(uint8_t speed, bool forward, unsigned epoch)
{
    sound_lock();
    if (epoch == atomic_load(&s_motion_epoch) && !atomic_load(&s_stop_requested)) {
        sound_speed_locked(speed,forward,true);
    }
    sound_unlock();
}

/* Re-read the canonical function bindings from NVS. Called after the settings
 * store changes (legacy migration, REST binding add/remove) so the engine does
 * not keep a stale copy until reboot. */
void sound_reload_bindings(void)
{
    sound_lock();
    size_t n = 0;
    s_bind_count = 0;
    if (settings_func_bind_load(s_binds, &n) == ESP_OK) {
        s_bind_count = n;
    }
    sound_unlock();
}

/* Play a table on a (possibly new) effect voice; returns the voice used. */
static uint8_t fx_play_table(uint8_t table, bool loop, uint8_t *slot)
{
    if (s_inhibited || s_mute_light) { return AUDIO_VOICE_NONE; }
    const sound_table_t *t = table_at(table);
    if (t == NULL) {
        return AUDIO_VOICE_NONE;
    }
    char path[SOUND_PATH_MAX];
    if (table_first_file(t, path, sizeof(path)) == NULL) {
        return AUDIO_VOICE_NONE;
    }
    audio_voice_handle_t *h = fx_handle(slot);
    if (h == NULL) { return AUDIO_VOICE_NONE; }
    if (!fx_running(slot)) {
        *slot = AUDIO_VOICE_NONE;
        if (audio_voice_alloc_owned(h) != ESP_OK) { return AUDIO_VOICE_NONE; }
        *slot = h->voice;
    }
    if (audio_voice_play_owned(h, path, loop, 100) != ESP_OK) {
        fx_voice_stop(slot);
        return AUDIO_VOICE_NONE;
    }
    return *slot;
}

/* Direction/state gate, mirroring func_eval (§8.5). */
static bool binding_gate_ok(const func_binding_t *b)
{
    uint8_t state = (s_speed != 0U) ? FUNC_STATE_MOVING : FUNC_STATE_STOPPED;
    uint8_t dir = s_forward ? FUNC_DIR_FWD : FUNC_DIR_REV;
    if (b->dir != FUNC_DIR_ANY && b->dir != dir) {
        return false;
    }
    if (b->state != FUNC_STATE_ANY && b->state != state) {
        return false;
    }
    return true;
}

static void binding_apply(const func_binding_t *b, bool state)
{
    uint8_t fn = b->fn;
    if (fn >= SOUND_FN_COUNT) {
        return;
    }
    if (b->target_type == FUNC_TARGET_LOGIC) {
        if (state && !binding_gate_ok(b)) {
            return;
        }
        switch (b->target_id) {
            case FUNC_LOGIC_MUTE_STOP:  s_mute_stop = state; break;
            case FUNC_LOGIC_MUTE_MOVE:  s_mute_move = state; break;
            case FUNC_LOGIC_MUTE_LIGHT:
                s_mute_light = state;
                if (state) {
                    for (size_t i = 0; i < SOUND_FN_COUNT; ++i) {
                        fx_voice_stop(&s_fn_voice[i]);
                    }
                    for (size_t i = 0; i < SOUND_MAX_EXTRAS; ++i) {
                        fx_voice_stop(&s_extra_voice[i]);
                        s_extra_on[i] = 0;
                    }
                    (void)audio_voice_stop(SOUND_VOICE_ENGINE_X);
                }
                break;
            default: break;
        }
        return;
    }
    if (b->target_type != FUNC_TARGET_SOUND) {
        return; /* SLOT/OUTPUT stay on the legacy/web path */
    }
    if (state && s_mute_light) {
        return; /* light-mute silences effect sounds */
    }
    int64_t now = esp_timer_get_time();
    if (state) {
        if (!binding_gate_ok(b)) {
            return;
        }
        s_fn_time[fn] = now;
        switch (b->mode) {
            case SOUND_MODE_ONE_SHOT:
                (void)fx_play_table(b->target_id, false, &s_fn_voice[fn]);
                break;
            case SOUND_MODE_TRIGGER:
                /* One run to completion: ignore a press while it is sounding. */
                if (s_fn_voice[fn] == AUDIO_VOICE_NONE ||
                    !fx_running(&s_fn_voice[fn])) {
                    fx_voice_stop(&s_fn_voice[fn]);
                    (void)fx_play_table(b->target_id, false, &s_fn_voice[fn]);
                }
                break;
            case SOUND_MODE_LOOP_HELD:
            case SOUND_MODE_SHORT_LONG:
                (void)fx_play_table(b->target_id, true, &s_fn_voice[fn]);
                break;
            case SOUND_MODE_LATCHED:
                if (!fx_running(&s_fn_voice[fn])) {
                    (void)fx_play_table(b->target_id, true, &s_fn_voice[fn]);
                } else {
                    fx_voice_stop(&s_fn_voice[fn]);
                }
                break;
            default:
                break;
        }
        return;
    }
    /* Release: always release the voice; the short-tap sound keeps its gate. */
    int64_t dt_ms = (now - s_fn_time[fn]) / 1000;
    if (b->mode == SOUND_MODE_SHORT_LONG && b->short_table != SOUND_TABLE_NONE &&
        dt_ms < (int64_t)b->short_ms && binding_gate_ok(b) && !s_mute_light) {
        fx_voice_stop(&s_fn_voice[fn]);
        (void)fx_play_table(b->short_table, false, &s_fn_voice[fn]);
    } else if (b->mode == SOUND_MODE_LOOP_HELD || b->mode == SOUND_MODE_SHORT_LONG) {
        fx_voice_stop(&s_fn_voice[fn]);
    }
}

/* Key-driven extras: an extra sound whose `fn` names a function key. ONE_SHOT/
 * TRIGGER fire on press, LOOP_HELD loops while held and LATCHED toggles. */
static void extras_function(uint8_t fn, bool state)
{
    for (uint8_t i = 0; i < s_scheme.extra_count && i < SOUND_MAX_EXTRAS; ++i) {
        const sound_extra_t *e = &s_scheme.extras[i];
        if (e->fn != fn || e->table == SOUND_TABLE_NONE || (state && s_mute_light)) {
            continue;
        }
        switch (e->mode) {
            case SOUND_MODE_LOOP_HELD:
                if (state && s_extra_on[i] == 0U) {
                    (void)fx_play_table(e->table, true, &s_extra_voice[i]);
                    s_extra_on[i] = 1U;
                } else if (!state && s_extra_on[i] != 0U) {
                    fx_voice_stop(&s_extra_voice[i]);
                    s_extra_on[i] = 0U;
                }
                break;
            case SOUND_MODE_LATCHED:
                if (state) {
                    if (s_extra_on[i] == 0U) {
                        (void)fx_play_table(e->table, true, &s_extra_voice[i]);
                        s_extra_on[i] = 1U;
                    } else {
                        fx_voice_stop(&s_extra_voice[i]);
                        s_extra_on[i] = 0U;
                    }
                }
                break;
            case SOUND_MODE_TRIGGER:
                if (state && (s_extra_voice[i] == AUDIO_VOICE_NONE ||
                              !fx_running(&s_extra_voice[i]))) {
                    (void)fx_play_table(e->table, false, &s_extra_voice[i]);
                }
                break;
            default: /* ONE_SHOT / SHORT_LONG / RANDOM / STATE on a key edge */
                if (state) {
                    (void)fx_play_table(e->table, false, &s_extra_voice[i]);
                }
                break;
        }
    }
}

void sound_function(uint8_t fn, bool state)
{
    if (fn >= SOUND_FN_COUNT) {
        return;
    }
    sound_lock();
    if (s_graph_fault) { s_fn_state[fn] = state; sound_unlock(); return; }
    if (s_graph_runner) {
        s_fn_state[fn] = state;
        if (s_inhibited || atomic_load(&s_stop_requested)) {
            uint32_t bit = UINT32_C(1) << fn;
            if (state) { s_graph_runner->fn_levels |= bit; }
            else { s_graph_runner->fn_levels &= ~bit; }
        } else { sg_runner_function(s_graph_runner,fn,state); }
        sound_unlock(); return;
    }
    if (s_inhibited || atomic_load(&s_stop_requested) || s_scheme.type == SOUND_SCHEME_NONE ||
        s_scheme.type == SOUND_SCHEME_LEGACY) { sound_unlock(); return; }
    bool changed = (s_fn_state[fn] != state);
    s_fn_state[fn] = state;
    if (changed) {
        s_control_armed = true;
        if (s_scheme.type != SOUND_SCHEME_NONE && s_scheme.type != SOUND_SCHEME_LEGACY &&
            fn == s_scheme.engine.engine_start_fn) {
            if (state) {
                s_engine_key = !s_engine_key; /* start / stop the prime mover */
            }
        } else {
            for (size_t i = 0; i < s_bind_count; ++i) {
                if (s_binds[i].used != 0U && s_binds[i].fn == fn) {
                    binding_apply(&s_binds[i], state);
                }
            }
            extras_function(fn, state);
        }
    }
    sound_unlock();
}

static void runtime_reset_locked(void)
{
    atomic_fetch_add(&s_motion_epoch,1);
    s_motion_ready = (s_speed == 0);
    if (s_graph_runner) {
        sg_runner_reset(s_graph_runner);
        s_engine_key = false; s_accel = 0; s_accel_q = 0;
        s_prev_speed = s_speed; s_control_armed = false;
        return;
    }
    for (uint8_t i = 0; i < SOUND_FN_COUNT; ++i) {
        fx_voice_stop(&s_fn_voice[i]);
        s_fn_state[i] = false;
    }
    (void)audio_voice_stop(SOUND_VOICE_ENGINE);
    (void)audio_voice_stop(SOUND_VOICE_ENGINE_X);
    (void)audio_voice_set_rate(SOUND_VOICE_ENGINE, 1000);
    for (uint8_t i = 0; i < SOUND_MAX_EXTRAS; ++i) {
        fx_voice_stop(&s_extra_voice[i]);
        s_extra_next[i] = 0;
        s_extra_on[i] = 0;
    }
    s_brake_done = false;
    s_mute_stop = false;
    s_mute_move = false;
    s_mute_light = false;
    s_engine_key = false;
    s_table = SOUND_TABLE_NONE;
    s_phase = TB_STOP;
    s_accel = 0;
    s_accel_q = 0;
    s_prev_speed = s_speed;
    s_group = 0;
    s_plays = 0;
    s_engine_handle = (audio_voice_handle_t){AUDIO_VOICE_NONE, 0};
    s_control_armed = false;
}

void sound_stop_all(void)
{
    sound_lock();
    runtime_reset_locked();
    sound_unlock();
}

void sound_request_stop(void)
{
    atomic_store(&s_stop_requested,true);
    atomic_fetch_add(&s_motion_epoch,1);
}

bool sound_graph_active(void)
{
    sound_lock(); bool active = s_graph != NULL || s_graph_fault; sound_unlock(); return active;
}
void sound_graph_fail_closed(void)
{
    sound_lock(); runtime_reset_locked();
    sg_graph_t *old = s_graph; sg_runner_t *runner = s_graph_runner;
    s_graph = NULL; s_graph_runner = NULL; s_graph_fault = true;
    s_graph_id[0] = 0; s_graph_revision = 0;
    sound_unlock(); free(runner); free(old);
}
esp_err_t sound_graph_install(const sg_graph_t *graph, const char *id, uint32_t revision)
{
    sound_graph_prepared_t *prepared = NULL;
    esp_err_t err = sound_graph_prepare(graph,id,revision,&prepared);
    if (err == ESP_OK) { sound_graph_commit(prepared); } return err;
}
esp_err_t sound_graph_prepare(const sg_graph_t *graph, const char *id, uint32_t revision,
                             sound_graph_prepared_t **out)
{
    if (!out) { return ESP_ERR_INVALID_ARG; } *out = NULL;
    if (!graph || !sg_id_valid(id) || !sg_id_valid(graph->id) || strcmp(graph->id,id)) { return ESP_ERR_INVALID_ARG; }
    sound_graph_prepared_t *prepared = malloc(sizeof(*prepared));
    sg_graph_t *copy = malloc(sizeof(*copy)); sg_runner_t *runner = malloc(sizeof(*runner));
    if (!copy || !runner || !prepared) { free(copy); free(runner); free(prepared); return ESP_ERR_NO_MEM; }
    *copy = *graph; esp_err_t err = sg_validate(copy,NULL);
    if (err != ESP_OK) { free(copy); free(runner); free(prepared); return err; }
    sg_io_t io = {graph_audio_play,graph_audio_release,graph_audio_poll,NULL};
    sg_runner_init(runner,copy,&io);
    prepared->graph = copy; prepared->runner = runner; prepared->revision = revision;
    snprintf(prepared->id,sizeof(prepared->id),"%s",id); *out = prepared; return ESP_OK;
}
void sound_graph_prepared_free(sound_graph_prepared_t *prepared)
{
    if (!prepared) { return; } free(prepared->runner); free(prepared->graph); free(prepared);
}
void sound_graph_commit(sound_graph_prepared_t *prepared)
{
    if (!prepared) { return; }
    sound_lock();
    uint32_t levels = s_graph_runner ? s_graph_runner->fn_levels : 0;
    if (!s_graph_runner) { for (unsigned i = 0; i < SOUND_FN_COUNT && i <= 28; ++i) { if (s_fn_state[i]) { levels |= UINT32_C(1) << i; } } }
    runtime_reset_locked();
    sg_graph_t *old = s_graph; sg_runner_t *old_runner = s_graph_runner;
    s_graph = prepared->graph; s_graph_runner = prepared->runner; s_graph_fault = false; s_graph_runner->fn_levels = levels;
    snprintf(s_graph_id,sizeof(s_graph_id),"%s",prepared->id); s_graph_revision = prepared->revision;
    sound_unlock(); free(old_runner); free(old); free(prepared);
}
void sound_graph_prime_functions(uint32_t levels)
{
    sound_lock();
    for (unsigned i = 0; i < SOUND_FN_COUNT && i <= 28; ++i) { s_fn_state[i] = (levels & (UINT32_C(1) << i)) != 0; }
    if (s_graph_runner) { s_graph_runner->fn_levels = levels & UINT32_C(0x1fffffff); s_graph_runner->fn_press = 0; s_graph_runner->fn_release = 0; }
    sound_unlock();
}
void sound_legacy_commit(const sound_scheme_t *scheme, const char *name)
{
    if (!scheme || !name) { return; }
    sound_lock(); runtime_reset_locked();
    sg_graph_t *old = s_graph; sg_runner_t *runner = s_graph_runner;
    s_graph = NULL; s_graph_runner = NULL; s_graph_fault = false; s_graph_id[0] = 0; s_graph_revision = 0;
    s_scheme = *scheme; snprintf(s_active,sizeof(s_active),"%s",name);
    s_control_armed = false; s_engine_key = false;
    sound_unlock(); free(runner); free(old);
}
void sound_graph_deactivate(void)
{
    sound_lock(); runtime_reset_locked();
    sg_graph_t *old = s_graph; sg_runner_t *runner = s_graph_runner;
    s_graph = NULL; s_graph_runner = NULL; s_graph_fault = false; s_graph_id[0] = 0; s_graph_revision = 0;
    (void)sound_store_default(&s_scheme); s_active[0] = 0;
    sound_unlock(); free(runner); free(old);
}
void sound_graph_status_get(sound_graph_status_t *out)
{
    if (!out) { return; } memset(out,0,sizeof(*out)); memset(out->states,SG_NONE,sizeof(out->states));
    sound_lock(); out->active = s_graph != NULL || s_graph_fault; out->fault = s_graph_fault;
    out->speed = s_speed;
    if (s_graph_runner) {
        out->engine = s_graph_runner->engine_on; out->armed = s_graph_runner->armed;
        snprintf(out->id,sizeof(out->id),"%s",s_graph_id); out->revision = s_graph_revision;
        for (unsigned i = 0; i <= s_graph->effect_count; ++i) {
            out->states[i] = s_graph_runner->channels[i].state;
            if (s_graph_runner->channels[i].failed) { out->failed_channels |= UINT32_C(1) << i; }
        }
    }
    sound_unlock();
}
bool sound_graph_file_used(const char *file)
{
    sound_lock(); bool used = sg_file_used(s_graph,file); sound_unlock(); return used;
}

esp_err_t sound_set_inhibited(bool inhibited)
{
    if (s_lock == NULL) { return ESP_ERR_INVALID_STATE; }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return ESP_ERR_TIMEOUT; }
    s_inhibited = inhibited;
    if (inhibited) { runtime_reset_locked(); }
    sound_unlock();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* tick                                                               */
/* ------------------------------------------------------------------ */

static void random_schedule(uint8_t i)
{
    const sound_extra_t *e = &s_scheme.extras[i];
    uint32_t span = (e->random_max_ms > e->random_min_ms)
                        ? (uint32_t)(e->random_max_ms - e->random_min_ms)
                        : 0U;
    uint32_t add = (span != 0U) ? (rng_next() % (span + 1U)) : 0U;
    s_extra_next[i] = esp_timer_get_time() + (int64_t)((uint32_t)e->random_min_ms + add) * 1000;
}

/* Random and state-driven extra sounds (R6.4). */
static void extras_tick(void)
{
    if (s_mute_light) {
        for (size_t i = 0; i < SOUND_MAX_EXTRAS; ++i) {
            fx_voice_stop(&s_extra_voice[i]);
            s_extra_on[i] = 0;
            s_extra_next[i] = 0;
        }
        return;
    }
    int64_t now = esp_timer_get_time();
    uint8_t state = (s_speed != 0U) ? FUNC_STATE_MOVING : FUNC_STATE_STOPPED;
    uint8_t dir = s_forward ? FUNC_DIR_FWD : FUNC_DIR_REV;
    for (uint8_t i = 0; i < s_scheme.extra_count && i < SOUND_MAX_EXTRAS; ++i) {
        const sound_extra_t *e = &s_scheme.extras[i];
        if (e->table == SOUND_TABLE_NONE || e->fn != SOUND_FN_NONE) {
            continue; /* key-driven extras go through sound_function() */
        }
        if (e->mode == SOUND_MODE_RANDOM) {
            if (e->random_max_ms == 0U) {
                continue;
            }
            if (s_extra_next[i] == 0) {
                random_schedule(i);
            }
            if (now >= s_extra_next[i]) {
                char path[SOUND_PATH_MAX];
                const sound_table_t *t = table_at(e->table);
                /* A random sound must be finite (mandatory max_plays, R6.4). */
                if (t != NULL && t->max_plays != 0U &&
                    table_first_file(t, path, sizeof(path)) != NULL) {
                    (void)audio_voice_play(SOUND_VOICE_ENGINE_X, path, false, 100);
                    /* CV115 = bell ring rate. */
                    (void)audio_voice_set_rate(SOUND_VOICE_ENGINE_X, cv_rate_permille(115, 5));
                }
                random_schedule(i);
            }
        } else if (e->mode == SOUND_MODE_STATE) {
            bool match = (e->state == FUNC_STATE_ANY || e->state == state) &&
                         (e->dir == FUNC_DIR_ANY || e->dir == dir);
            if (match && s_extra_on[i] == 0U) {
                (void)fx_play_table(e->table, true, &s_extra_voice[i]);
                s_extra_on[i] = 1U;
            } else if (!match && s_extra_on[i] != 0U) {
                fx_voice_stop(&s_extra_voice[i]);
                s_extra_on[i] = 0U;
            }
        }
    }
}

/* Brake squeal (R6.3): a hard deceleration below min_brake_speed triggers the
 * Stop table early (reserved secondary voice) as the brake script. */
static void brake_tick(void)
{
    const sound_brake_t *b = &s_scheme.brake;
    if (s_mute_light || !s_engine_key || s_speed == 0U || b->min_brake_speed == 0U) {
        s_brake_done = false;
        return;
    }
    if (s_speed > b->min_brake_speed || s_speed <= b->max_on_speed) {
        s_brake_done = false;
        return;
    }
    if (s_accel > -SOUND_ACCEL_HYST || s_brake_done) {
        return;
    }
    const sound_table_t *t = table_at(s_scheme.engine.stop_table);
    if (t == NULL) {
        return;
    }
    char path[SOUND_PATH_MAX];
    if (table_first_file(t, path, sizeof(path)) != NULL) {
        (void)audio_voice_play(SOUND_VOICE_ENGINE_X, path, false, 100);
        /* CV116 = dynamic brake rate. */
        (void)audio_voice_set_rate(SOUND_VOICE_ENGINE_X, cv_rate_permille(116, 30));
        s_brake_done = true;
    }
}

static void sound_tick(void)
{
    sound_lock();
    if (atomic_exchange(&s_stop_requested,false)) { runtime_reset_locked(); sound_unlock(); return; }
    if (s_graph_fault) { sound_unlock(); return; }
    if (s_graph_runner) {
        if (!s_inhibited && s_graph_runner->armed) {
            int32_t delta = (int32_t)s_speed - s_prev_speed;
            s_accel_q = (s_accel_q * 7 + delta * 1024 * 3) / 10;
            s_accel = s_accel_q / 1024; s_prev_speed = s_speed;
            sg_runner_tick(s_graph_runner,s_speed,s_accel);
        }
        sound_unlock(); return;
    }
    if (s_inhibited || !s_control_armed || s_scheme.type == SOUND_SCHEME_NONE ||
        s_scheme.type == SOUND_SCHEME_LEGACY) {
        sound_unlock();
        return;
    }
    int32_t d = (int32_t)s_speed - (int32_t)s_prev_speed;
    s_accel_q = (s_accel_q * 7 + d * 1024 * 3) / 10;
    s_accel = s_accel_q / 1024;
    s_prev_speed = s_speed;

    /* sync_motion: the prime mover follows the wheels automatically. */
    if (s_scheme.engine.sync_motion) {
        s_engine_key = (s_speed != 0U);
    }
    uint8_t flags = cv_u8(30, s_scheme.engine.flags);
    uint8_t want = sound_engine_pick(s_speed, s_accel, flags);
    if (want != s_table) {
        tb_start(want);
    }
    tb_advance();
    const sound_table_t *t = table_at(s_table);
    if (t != NULL && s_phase != TB_STOP) {
        /* CV114 = chuff/exhaust rate; the table's rate_scale adds per-table
         * acceleration on top. */
        (void)audio_voice_set_rate(SOUND_VOICE_ENGINE,
                                   sound_rate_for(t->rate_scale, s_speed,
                                                  cv_rate_permille(114, 57)));
    }
    brake_tick();
    extras_tick();
    sound_unlock();
}

static void sound_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(SOUND_TICK_MS);
    TickType_t last = xTaskGetTickCount();
    uint32_t iters = 0;
    for (;;) {
        /* Follow the *applied* (ramped) motor output, not the target, so the
         * sound transitions match the real motion. */
        uint8_t spd = 0;
        bool fwd = true;
        unsigned epoch = atomic_load(&s_motion_epoch);
        (void)motor_get_applied_speed(&spd, &fwd);
        /* The motor reports the DCC 128-step scale (0..126); the engine works
         * on a normalized 0..255 scale. */
        if (spd > 126U) {
            spd = 126U;
        }
        sound_applied_speed((uint8_t)(((uint16_t)spd * 255U) / 126U), fwd,epoch);
        sound_tick();
        iters++;
        if (s_task_iter_cap != 0U && iters >= s_task_iter_cap) {
            break;
        }
        vTaskDelayUntil(&last, period);
    }
}

esp_err_t sound_init(void)
{
    if (s_lock != NULL) { return ESP_ERR_INVALID_STATE; }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_fn_voice, 0xFF, sizeof(s_fn_voice));
    memset(s_fn_state, 0, sizeof(s_fn_state));
    memset(s_fn_time, 0, sizeof(s_fn_time));
    memset(s_extra_voice, 0xFF, sizeof(s_extra_voice));
    memset(s_extra_on, 0, sizeof(s_extra_on));
    memset(s_extra_next, 0, sizeof(s_extra_next));
    s_mute_stop = false;
    s_mute_move = false;
    s_mute_light = false;

    char name[SOUND_FILE_MAX] = { 0 };
    if (settings_active_scheme_get(name, sizeof(name)) != ESP_OK || name[0] == '\0') {
        (void)sound_store_default(&s_scheme);
        s_active[0] = '\0';
    } else {
        (void)sound_load_scheme(name);
    }
    (void)sound_store_ensure_dir();

    s_bind_count = 0;
    sound_reload_bindings();

    if (xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 6, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "sound task create failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "sound engine ready (%s)", s_active[0] ? s_active : "no scheme");
    return ESP_OK;
}
