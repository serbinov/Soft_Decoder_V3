#ifndef SOUND_GRAPH_H
#define SOUND_GRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SG_MAX_STATES 192
#define SG_MAX_SOUND_STATES 144
#define SG_MAX_SILENT_STATES 96
#define SG_MAX_TRANSITIONS 250
#define SG_MAX_EFFECTS 32
#define SG_MAX_ASSETS 128
#define SG_MAX_VARIABLES 16
#define SG_VAR_NAME_CAP 24
#define SG_ID_CAP 32
#define SG_NAME_CAP 64
#define SG_FILE_CAP 64
#define SG_MAX_JSON (128U * 1024U)
#define SG_NONE 255U

typedef enum {
    SG_FN_PRESS, SG_FN_RELEASE, SG_FN_ON, SG_FN_OFF, SG_ENGINE_ON,
    SG_ENGINE_OFF, SG_SPEED, SG_ACCEL, SG_DECEL, SG_SAMPLE_DONE,
    SG_DIR_FWD, SG_DIR_REV, SG_RANDOM, SG_TIMEOUT, SG_VAR_CMP,
    SG_STATE_ENTER     /* true for the tick after a channel enters a state */
} sg_condition_type_t;
/* Built-in status variables resolved by the runner; SG_VAR_USER is stored. */
typedef enum { SG_VAR_USER, SG_VAR_SPEED, SG_VAR_FORWARD, SG_VAR_MOVING, SG_VAR_ENGINE } sg_var_kind_t;
typedef enum { SG_CMP_EQ, SG_CMP_NE, SG_CMP_LT, SG_CMP_LE, SG_CMP_GT, SG_CMP_GE } sg_compare_t;
typedef enum { SG_IMMEDIATE, SG_AFTER_SAMPLE } sg_timing_t;
typedef struct {
    char name[SG_VAR_NAME_CAP];
    uint8_t kind;           /* sg_var_kind_t */
    int32_t init;           /* user variables only; built-ins must use 0 */
} sg_variable_t;
typedef struct {
    sg_condition_type_t type;
    uint8_t fn, min, max;
    bool has_fn, has_min, has_max;
    uint8_t var, var2, op;  /* SG_VAR_CMP: lhs variable, rhs operator (sg_compare_t) */
    bool has_var, has_var2; /* var2 selects a second variable instead of the literal value */
    int16_t value;          /* SG_VAR_CMP rhs literal when has_var2 is false */
} sg_condition_t;
typedef struct {
    char id[SG_ID_CAP], name[SG_NAME_CAP], file[SG_FILE_CAP];
    bool loop;
    uint8_t volume;
    uint16_t rate;
    uint8_t loops;          /* repeat count for a looping state; 0 = endless */
    uint8_t set_var;        /* variable to assign when this state is entered */
    int16_t set_value;
    bool has_set;
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
    bool engine_hold;       /* input-linked F behaves as hold instead of toggle */
    bool observers;         /* global logic/var/delay blocks exist; tick from boot */
    uint8_t state_count, transition_count, effect_count, asset_count, variable_count;
    sg_variable_t variables[SG_MAX_VARIABLES];
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
/* Structural parse for editor drafts: no referential/semantic validation. */
esp_err_t sg_parse_draft(const char *json, size_t len, sg_graph_t *out, sg_diagnostic_t *diag);
esp_err_t sg_validate(sg_graph_t *graph, sg_diagnostic_t *diag);
bool sg_file_used(const sg_graph_t *graph, const char *file);
bool sg_id_valid(const char *id);
bool sg_filename_valid(const char *file);

/* Runtime I/O keeps the semantic runner dependency-free and host-testable. */
/* IDLE means the voice stopped without a decode/IO error (stop/abort/stale):
 * the runner releases it and retries the current state instead of latching a
 * permanent channel failure. */
typedef enum {
    SG_AUDIO_PENDING,
    SG_AUDIO_PLAYING,
    SG_AUDIO_DONE,
    SG_AUDIO_FAILED,
    SG_AUDIO_IDLE
} sg_audio_state_t;
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
    bool retry_play;        /* a voice was unavailable; retry playback next tick */
    bool entered, started; /* for SG_STATE_ENTER and first-tick setVar/timeout */
    sg_handle_t handle;
    uint32_t elapsed_ms;   /* time in the current state, for SG_TIMEOUT */
    uint16_t loops_done;   /* completed repeats of a finite looping state */
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
    int32_t vars[SG_MAX_VARIABLES];   /* user-defined variable values */
} sg_runner_t;
void sg_runner_init(sg_runner_t *runner, const sg_graph_t *graph, const sg_io_t *io);
void sg_runner_reset(sg_runner_t *runner);
void sg_runner_function(sg_runner_t *runner, uint8_t fn, bool on);
/* Restore a full function-key snapshot (e.g. after applying a graph while keys
 * are held) and recompute engine_on/armed from it, without generating press
 * events or requiring an extra engine-F toggle. */
void sg_runner_prime_levels(sg_runner_t *runner, uint32_t levels);
void sg_runner_power(sg_runner_t *runner, bool on);
void sg_runner_tick(sg_runner_t *runner, uint8_t speed, int32_t accel, bool forward, uint32_t dt_ms);

#endif
