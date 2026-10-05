/* Graph sound engine (SOUND_ENGINE_ROADMAP.md).
 *
 * The graph is the single sound behaviour model: the semantic runner in
 * sound_graph_runner.c decides *what* to play from the active graph, and this
 * file owns the RTOS task, the runtime inputs (applied motor speed and function
 * keys) and the audio voice I/O. Function bindings (func_types.h) remain the
 * runtime output/AUX routing handled by the web layer. */

#include "sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>

#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "motor.h"
#include "storage.h"

static const char *TAG = "sound";

#define SOUND_TICK_MS          20
#define SOUND_VOICE_ENGINE     18
#define SOUND_FN_COUNT         29
#define SOUND_PATH_MAX         192
/* Engine gain while any effect voice is audible (ducking), percent. */
#define SOUND_DUCK_PCT         40

static SemaphoreHandle_t s_lock;

/* Engine runtime. */
static uint8_t s_speed;
static bool s_forward = true;
static uint8_t s_prev_speed;
static int32_t s_accel;            /* EMA of d(speed)/tick */
static int32_t s_accel_q;          /* Q10 retains fractional decay toward zero */
static bool s_control_armed = true;
static uint32_t s_task_iter_cap;   /* test hook: 0 = run forever */
static bool s_fn_state[SOUND_FN_COUNT];

static sg_graph_t *s_graph;
static sg_runner_t *s_graph_runner;
static char s_graph_id[SG_ID_CAP];
static uint32_t s_graph_revision;
static bool s_graph_fault;
static bool s_inhibited;
static atomic_bool s_stop_requested;
static atomic_uint s_motion_epoch;
static bool s_motion_ready = true;

struct sound_graph_prepared {
    sg_graph_t *graph;
    sg_runner_t *runner;
    char id[SG_ID_CAP];
    uint32_t revision;
};

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
 * path and the REST mutators all touch the runner/graph. */
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
/* runtime inputs                                                     */
/* ------------------------------------------------------------------ */

bool sound_engine_is_on(void)
{
    sound_lock();
    bool on = s_graph_runner ? s_graph_runner->engine_on : false;
    sound_unlock();
    return on;
}

bool sound_enabled(void)
{
    sound_lock();
    bool on = s_graph != NULL || s_graph_fault;
    sound_unlock();
    return on;
}

void sound_engine_power(bool on)
{
    sound_lock();
    if (!s_graph_fault && !s_inhibited && !atomic_load(&s_stop_requested)) {
        if (s_graph_runner) { sg_runner_power(s_graph_runner,on); }
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
    s_fn_state[fn] = state;
    sound_unlock();
}

static void runtime_reset_locked(void)
{
    atomic_fetch_add(&s_motion_epoch,1);
    s_motion_ready = (s_speed == 0);
    if (s_graph_runner) {
        sg_runner_reset(s_graph_runner);
    }
    s_accel = 0;
    s_accel_q = 0;
    s_prev_speed = s_speed;
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

/* ------------------------------------------------------------------ */
/* graph ownership                                                    */
/* ------------------------------------------------------------------ */

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
    if (!s_graph_runner) { for (unsigned i = 0; i < SOUND_FN_COUNT; ++i) { if (s_fn_state[i]) { levels |= UINT32_C(1) << i; } } }
    runtime_reset_locked();
    sg_graph_t *old = s_graph; sg_runner_t *old_runner = s_graph_runner;
    s_graph = prepared->graph; s_graph_runner = prepared->runner; s_graph_fault = false; s_graph_runner->fn_levels = levels;
    snprintf(s_graph_id,sizeof(s_graph_id),"%s",prepared->id); s_graph_revision = prepared->revision;
    sound_unlock(); free(old_runner); free(old); free(prepared);
}
void sound_graph_prime_functions(uint32_t levels)
{
    sound_lock();
    for (unsigned i = 0; i < SOUND_FN_COUNT; ++i) { s_fn_state[i] = (levels & (UINT32_C(1) << i)) != 0; }
    if (s_graph_runner) { s_graph_runner->fn_levels = levels & UINT32_C(0x1fffffff); s_graph_runner->fn_press = 0; s_graph_runner->fn_release = 0; }
    sound_unlock();
}
void sound_graph_deactivate(void)
{
    sound_lock(); runtime_reset_locked();
    sg_graph_t *old = s_graph; sg_runner_t *runner = s_graph_runner;
    s_graph = NULL; s_graph_runner = NULL; s_graph_fault = false; s_graph_id[0] = 0; s_graph_revision = 0;
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

/* Duck the engine voice while any effect channel is audible. Called with the
 * sound lock held, right after the runner tick. */
static void sound_apply_duck_locked(void)
{
    if (!s_graph_runner || !s_graph) { return; }
    bool duck = false;
    for (unsigned i = 1; i <= s_graph->effect_count; ++i) {
        const sg_channel_t *c = &s_graph_runner->channels[i];
        if (c->playing && s_graph->states[c->state].file[0]) { duck = true; break; }
    }
    const sg_channel_t *eng = &s_graph_runner->channels[0];
    uint8_t base = s_graph->states[eng->state].file[0] ? s_graph->states[eng->state].volume : 100U;
    uint8_t vol = duck ? (uint8_t)((uint16_t)base * SOUND_DUCK_PCT / 100U) : base;
    (void)audio_voice_set_volume_live(SOUND_VOICE_ENGINE, vol);
}

/* ------------------------------------------------------------------ */
/* tick                                                               */
/* ------------------------------------------------------------------ */

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
            sg_runner_tick(s_graph_runner,s_speed,s_accel,s_forward,SOUND_TICK_MS);
            sound_apply_duck_locked();
        }
    }
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
    memset(s_fn_state, 0, sizeof(s_fn_state));
    if (xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 6, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "sound task create failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "graph sound engine ready");
    return ESP_OK;
}
