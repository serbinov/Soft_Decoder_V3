#ifndef FUNC_TYPES_H
#define FUNC_TYPES_H

/* Canonical function (F-key) bindings: a many-to-many mapping from F0..F28 to
 * an output, a sound graph entry, a legacy audio slot or a logic target. Gated
 * by driving direction and motion state. The graph engine is the single sound
 * behaviour model; these bindings are the runtime routing compiled from it. */

#include <stdint.h>

#define FUNC_TARGET_NONE    0
#define FUNC_TARGET_OUTPUT  1  /* target_id = output 0..8 (F0F,F0R,AUX1..7) */
#define FUNC_TARGET_SOUND   2  /* target_id = sound graph effect index */
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

/* Playback mode for SOUND/SLOT bindings (legacy slot routing). */
#define FUNC_MODE_ONE_SHOT   0
#define FUNC_MODE_LOOP_HELD  1
#define FUNC_MODE_SHORT_LONG 2
#define FUNC_MODE_LATCHED    3
#define FUNC_MODE_TRIGGER    4
#define FUNC_MODE_RANDOM     5
#define FUNC_MODE_STATE      6

typedef struct {
    uint8_t  used;         /* 0 = empty record */
    uint8_t  fn;           /* F0..F28 */
    uint8_t  target_type;  /* FUNC_TARGET_* */
    uint8_t  target_id;    /* output / sound / slot / logic */
    uint8_t  dir;          /* FUNC_DIR_* */
    uint8_t  state;        /* FUNC_STATE_* */
    uint8_t  mode;         /* playback mode */
    uint8_t  flags;        /* FUNC_FLAG_* */
    uint8_t  short_table;  /* short-tap sound (0 = none) */
    uint16_t short_ms;     /* short-press threshold, e.g. 400 ms */
    uint16_t min_ms;       /* minimum playback, e.g. 150 ms */
    uint16_t fade_ms;      /* release fade, e.g. 80 ms */
} func_binding_t;

#endif
