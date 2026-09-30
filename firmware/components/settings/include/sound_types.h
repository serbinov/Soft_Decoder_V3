#ifndef SOUND_TYPES_H
#define SOUND_TYPES_H

/* Shared data model for the sound-scheme engine (see
 * SOUND_ENGINE_IMPLEMENTATION.md sections 5/8.4 and SOUND_ENGINE_ROADMAP.md R1).
 *
 * Dependency-free on purpose: included by `settings` (function bindings),
 * `sound` (scheme engine) and `web` (REST/UI). Only fixed-width types. Track
 * cells hold storage-relative WAV *file names* so the engine can resolve the
 * absolute path through storage_get_root(); the filename model follows the
 * roadmap (R1.1, resolution of the model/storage conflict in
 * SOUND_ENGINE_IMPLEMENTATION.md section 18.2). */

#include <stdbool.h>
#include <stdint.h>

#define SOUND_MAX_TABLES            32
#define SOUND_MAX_TRACKS_PER_TABLE  12
#define SOUND_MAX_GROUPS            4   /* cylinders / sub-groups (steam) */
#define SOUND_ENGINE_STEPS          5   /* D1..D5 / A1..A5 / CX1..CX5 */
#define SOUND_MAX_EXTRAS            24
#define SOUND_FILE_MAX              64
#define SOUND_NAME_MAX              24

/* Index 0 is always "unset". */
#define SOUND_TABLE_NONE            0

/* A "no function key" marker for state/random extra sounds. */
#define SOUND_FN_NONE               0xFF

typedef enum {
    SOUND_SCHEME_NONE = 0,   /* engine off: legacy slot behaviour */
    SOUND_SCHEME_LEGACY,     /* explicit legacy: F1..F20 = slots */
    SOUND_SCHEME_DIESEL,
    SOUND_SCHEME_STEAM,
    SOUND_SCHEME_ELECTRIC
} sound_scheme_type_t;

/* One track cell of a sound table. */
typedef struct {
    char file[SOUND_FILE_MAX];
} sound_track_ref_t;

typedef struct {
    bool     used;
    char     name[SOUND_NAME_MAX];
    sound_track_ref_t init[SOUND_MAX_GROUPS];
    sound_track_ref_t loop[SOUND_MAX_GROUPS];
    sound_track_ref_t end[SOUND_MAX_GROUPS];

    /* Conditions that break the Loop and move on. */
    uint8_t  max_speed;                   /* 0 = unset */
    uint8_t  min_speed;                   /* 0 = unset */

    /* Playback behaviour. */
    uint8_t  rate_scale;                  /* 0..127 "sound acceleration" */
    uint8_t  min_plays;
    uint8_t  max_plays;

    /* Graph (engine tables only). */
    uint8_t  end_table;
    uint8_t  next_accel;
    uint8_t  next_decel;
} sound_table_t;

/* Scheme option bits (CV30). */
#define SOUND_ENG_SKIP_STOD1 (1u << 0)  /* Skip Stop -> D1 */
#define SOUND_ENG_SKIP_D1TOS (1u << 1)  /* Skip D1 -> Stop */

typedef struct {
    uint8_t start_table;                  /* M to S */
    uint8_t stop_table;                   /* Stop (idle) */
    uint8_t shutdown_table;               /* S to M */
    uint8_t drive[SOUND_ENGINE_STEPS];    /* D1..D5 */
    uint8_t accel[SOUND_ENGINE_STEPS];    /* A1..A5 */
    uint8_t coast[SOUND_ENGINE_STEPS];    /* CX1..CX5 */
    uint8_t engine_start_fn;              /* F-key that starts the engine */
    uint8_t cyl_shift_min;                /* steam chuff phase shift */
    uint8_t cyl_shift_max;
    uint8_t cyl_shift_inc;
    uint8_t flags;                        /* SOUND_ENG_* */
    bool    sync_motion;                  /* "sync motion with sound" */
} sound_engine_t;

typedef enum {
    SOUND_MODE_ONE_SHOT = 0,   /* single sample on the 0->1 edge */
    SOUND_MODE_LOOP_HELD,      /* loops while held */
    SOUND_MODE_SHORT_LONG,     /* short tap -> short_table, else loop+fade */
    SOUND_MODE_LATCHED,        /* toggles on each press */
    SOUND_MODE_TRIGGER,        /* one-shot with a cooldown */
    SOUND_MODE_RANDOM,         /* fired at random intervals */
    SOUND_MODE_STATE           /* triggered by state/direction */
} sound_mode_t;

typedef struct {
    uint8_t  table;
    uint8_t  fn;               /* 0..28 or SOUND_FN_NONE */
    uint8_t  dir;              /* FUNC_DIR_* */
    uint8_t  state;            /* FUNC_STATE_* */
    uint8_t  mode;             /* SOUND_MODE_* */
    uint16_t random_min_ms;
    uint16_t random_max_ms;
    uint8_t  volume;
} sound_extra_t;

typedef struct {
    uint8_t max_on_speed;      /* lower bound: squeal only above this speed     */
    uint8_t min_brake_speed;   /* upper bound: squeal only at/below this speed  */
} sound_brake_t;

typedef struct {
    uint8_t        type;       /* sound_scheme_type_t */
    uint8_t        table_count;
    sound_table_t  tables[SOUND_MAX_TABLES];
    sound_engine_t engine;
    uint8_t        extra_count;
    sound_extra_t  extras[SOUND_MAX_EXTRAS];
    sound_brake_t  brake;
} sound_scheme_t;

/* ---- Function bindings (SOUND_ENGINE_IMPLEMENTATION.md section 8.4) ---- */

#define FUNC_TARGET_NONE    0
#define FUNC_TARGET_OUTPUT  1  /* target_id = output 0..8 (F0F,F0R,AUX1..7) */
#define FUNC_TARGET_SOUND   2  /* target_id = sound table index */
#define FUNC_TARGET_SLOT    3  /* target_id = audio slot 1..20 (legacy) */
#define FUNC_TARGET_LOGIC   4  /* target_id = FUNC_LOGIC_* */

#define FUNC_DIR_ANY    0
#define FUNC_DIR_FWD    1
#define FUNC_DIR_REV    2

#define FUNC_STATE_ANY     0
#define FUNC_STATE_MOVING  1
#define FUNC_STATE_STOPPED 2

#define FUNC_FLAG_DUCK   (1u << 0)  /* duck the engine while active */
#define FUNC_FLAG_INVERT (1u << 1)

/* Logic targets. */
#define FUNC_LOGIC_MUTE_STOP        1
#define FUNC_LOGIC_MUTE_MOVE        2
#define FUNC_LOGIC_MUTE_LIGHT       3
#define FUNC_LOGIC_DRIVE_HOLD       4
#define FUNC_LOGIC_COAST            5
#define FUNC_LOGIC_DYNAMIC_BRAKE    6
#define FUNC_LOGIC_NOTCH_UP         7
#define FUNC_LOGIC_NOTCH_DOWN       8

#define FUNC_BIND_MAX 64

typedef struct {
    uint8_t  used;         /* 0 = empty record */
    uint8_t  fn;           /* F0..F28 */
    uint8_t  target_type;  /* FUNC_TARGET_* */
    uint8_t  target_id;    /* output / table / slot / logic */
    uint8_t  dir;          /* FUNC_DIR_* */
    uint8_t  state;        /* FUNC_STATE_* */
    uint8_t  mode;         /* SOUND_MODE_* */
    uint8_t  flags;        /* FUNC_FLAG_* */
    uint8_t  short_table;  /* short-tap sound (0 = none) */
    uint16_t short_ms;     /* short-press threshold, e.g. 400 ms */
    uint16_t min_ms;       /* minimum playback, e.g. 150 ms */
    uint16_t fade_ms;      /* release fade, e.g. 80 ms */
} func_binding_t;

#endif
