#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sound_graph.h"

/* Graph sound engine: the graph is the single sound behaviour model. F-key,
 * motor and web inputs drive the semantic runner in sound_graph_runner.c; the
 * actual voice I/O goes through the `audio` component. Function bindings
 * (func_types.h) remain the runtime output/AUX routing handled by the web
 * layer. */

typedef struct {
    bool active, engine, armed, fault;
    char id[SG_ID_CAP];
    uint32_t revision;
    uint8_t speed;
    uint8_t states[SG_MAX_EFFECTS + 1];
    /* Channel masks for channels 0..31 (engine + effects 1..31). Effect channel
     * 32 cannot be represented in 32 bits, so it has explicit flags instead of
     * the undefined `1u << 32` shift. */
    uint32_t failed_channels;
    uint32_t retry_channels;
    bool failed_channel32;
    bool retry_channel32;
} sound_graph_status_t;

/* DCC 128-step motor scale (0..126) to the graph's 0..255 speed. Rounding
 * (not truncation) keeps the 28-step positions on the same percentage band
 * boundaries as the sound editor simulator. */
static inline uint8_t sound_speed_from_dcc128(uint8_t speed128)
{
    if (speed128 > 126U) speed128 = 126U;
    return (uint8_t)(((uint16_t)speed128 * 255U + 63U) / 126U);
}

bool sound_graph_active(void);
/* Invalid persisted graph retains routing ownership without playing anything. */
void sound_graph_fail_closed(void);
typedef struct sound_graph_prepared sound_graph_prepared_t;
esp_err_t sound_graph_prepare(const sg_graph_t *graph, const char *id, uint32_t revision,
                             sound_graph_prepared_t **out);
void sound_graph_prepared_free(sound_graph_prepared_t *prepared);
/* Consumes prepared after durable selector commit; caller holds admission inhibit. */
void sound_graph_commit(sound_graph_prepared_t *prepared);
void sound_graph_prime_functions(uint32_t levels);
esp_err_t sound_graph_install(const sg_graph_t *graph, const char *id, uint32_t revision);
void sound_graph_deactivate(void);
void sound_graph_status_get(sound_graph_status_t *out);

esp_err_t sound_init(void);

/* Emergency stop: silence every sound and reset the engine state. */
void sound_stop_all(void);
/* Nonblocking safety-path admission close; sound task performs owned cleanup. */
void sound_request_stop(void);
/* Reset/stop old runtime and reject new runtime requests while inhibited.
 * Release does not resurrect engine/keys; sync-motion needs a new speed edge.
 * Pair with audio inhibition and bounded audio_is_quiescent() before format. */
esp_err_t sound_set_inhibited(bool inhibited);

/* Current value of a graph variable by index (built-ins are resolved from the
 * live motion/engine state). Used by conditional output/logic bindings. */
bool sound_variable_get(uint8_t index, int32_t *out);

/* Runtime inputs. */
void sound_set_speed(uint8_t speed, bool forward);
void sound_function(uint8_t fn, bool state);

/* True when a graph owns the function-key routing and the reserved voices. */
bool sound_enabled(void);
/* Directly start/stop the prime mover (HIL / diagnostics). */
void sound_engine_power(bool on);
/* Per-category gain for graph states (0..100). Master volume is handled by
 * audio_set_volume(); these scale engine vs effect voices before playback. */
void sound_set_volumes(uint8_t engine, uint8_t effects);
/* Runtime logic actions compiled from FUNC_TARGET_LOGIC bindings.
 * mute silences the graph; drive_hold freezes the speed/notch seen by the
 * graph; coast/dynamic_brake force idle; notch_offset shifts the graph speed. */
void sound_set_logic(bool mute, bool drive_hold, bool coast, bool dynamic_brake,
                     int8_t notch_offset);

#endif
