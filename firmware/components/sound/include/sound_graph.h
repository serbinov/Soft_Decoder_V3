#ifndef SOUND_GRAPH_H
#define SOUND_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SG_MAX_STATES 57
#define SG_MAX_TRANSITIONS 128
#define SG_MAX_EFFECTS 24
#define SG_MAX_ASSETS 31
#define SG_ID_CAP 32
#define SG_NAME_CAP 64
#define SG_FILE_CAP 64
#define SG_MAX_JSON (128U * 1024U)
#define SG_NONE 255U

typedef enum {
    SG_FN_PRESS, SG_FN_RELEASE, SG_FN_ON, SG_FN_OFF, SG_ENGINE_ON,
    SG_ENGINE_OFF, SG_SPEED, SG_ACCEL, SG_DECEL, SG_SAMPLE_DONE,
    SG_DIR_FWD, SG_DIR_REV, SG_RANDOM
} sg_condition_type_t;
typedef enum { SG_IMMEDIATE, SG_AFTER_SAMPLE } sg_timing_t;
typedef struct {
    sg_condition_type_t type;
    uint8_t fn, min, max;
    bool has_fn, has_min, has_max;
} sg_condition_t;
typedef struct {
    char id[SG_ID_CAP], name[SG_NAME_CAP], file[SG_FILE_CAP];
    bool loop;
    uint8_t volume;
    uint16_t rate;
} sg_state_t;
typedef struct {
    char id[SG_ID_CAP], source[SG_ID_CAP], target[SG_ID_CAP];
    uint8_t source_index, target_index, priority;
    sg_timing_t timing;
    sg_condition_t condition;
} sg_transition_t;
typedef struct {
    char id[SG_ID_CAP], entry[SG_ID_CAP];
    uint8_t entry_index, fn;
} sg_effect_t;
typedef struct {
    char file[SG_FILE_CAP], crc32[9];
    uint32_t size, sampleRate, channels, bits, durationMs;
} sg_asset_t;
typedef struct {
    char id[SG_ID_CAP], name[SG_NAME_CAP], engine_entry[SG_ID_CAP];
    uint8_t engine_entry_index, engine_fn, hysteresis;
    uint8_t state_count, transition_count, effect_count, asset_count;
    sg_state_t states[SG_MAX_STATES];
    sg_transition_t transitions[SG_MAX_TRANSITIONS];
    sg_effect_t effects[SG_MAX_EFFECTS];
    sg_asset_t assets[SG_MAX_ASSETS];
} sg_graph_t;
typedef struct {
    size_t offset;
    char message[128], code[32], id[SG_ID_CAP], field[32];
} sg_diagnostic_t;

/* Caller allocates graph on heap. Editor JSON is checked, not retained here.
 * Store callers preserve original bytes. Validation resolves index fields. */
esp_err_t sg_parse(const char *json, size_t len, sg_graph_t *out, sg_diagnostic_t *diag);
esp_err_t sg_validate(sg_graph_t *graph, sg_diagnostic_t *diag);
bool sg_file_used(const sg_graph_t *graph, const char *file);
bool sg_id_valid(const char *id);
bool sg_filename_valid(const char *file);

/* Runtime I/O keeps the semantic runner dependency-free and host-testable. */
typedef enum { SG_AUDIO_PENDING, SG_AUDIO_PLAYING, SG_AUDIO_DONE, SG_AUDIO_FAILED } sg_audio_state_t;
typedef struct { uint8_t voice; uint32_t generation; } sg_handle_t;
typedef struct {
    esp_err_t (*play)(void *ctx, bool engine, const sg_state_t *state, sg_handle_t *handle);
    void (*release)(void *ctx, sg_handle_t handle);
    sg_audio_state_t (*poll)(void *ctx, sg_handle_t handle);
    void *ctx;
} sg_io_t;
typedef struct {
    uint8_t state, pending;
    bool playing, done, failed;
    sg_handle_t handle;
    bool speed_match[SG_MAX_TRANSITIONS];
} sg_channel_t;
typedef struct {
    const sg_graph_t *graph;
    sg_io_t io;
    sg_channel_t channels[SG_MAX_EFFECTS + 1];
    uint32_t fn_levels, fn_press, fn_release;
    bool engine_on, armed;
    bool forward;   /* travel direction, from the motor/DCC input */
    uint32_t rng;   /* LCG state for SG_RANDOM conditions */
} sg_runner_t;
void sg_runner_init(sg_runner_t *runner, const sg_graph_t *graph, const sg_io_t *io);
void sg_runner_reset(sg_runner_t *runner);
void sg_runner_function(sg_runner_t *runner, uint8_t fn, bool on);
void sg_runner_power(sg_runner_t *runner, bool on);
void sg_runner_tick(sg_runner_t *runner, uint8_t speed, int32_t accel, bool forward);

#endif
