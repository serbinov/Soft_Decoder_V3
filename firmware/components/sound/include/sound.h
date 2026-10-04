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
    uint32_t failed_channels;
} sound_graph_status_t;

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
bool sound_graph_file_used(const char *file);

esp_err_t sound_init(void);

/* Emergency stop: silence every sound and reset the engine state. */
void sound_stop_all(void);
/* Nonblocking safety-path admission close; sound task performs owned cleanup. */
void sound_request_stop(void);
/* Reset/stop old runtime and reject new runtime requests while inhibited.
 * Release does not resurrect engine/keys; sync-motion needs a new speed edge.
 * Pair with audio inhibition and bounded audio_is_quiescent() before format. */
esp_err_t sound_set_inhibited(bool inhibited);
bool sound_engine_is_on(void);

/* Runtime inputs. */
void sound_set_speed(uint8_t speed, bool forward);
void sound_function(uint8_t fn, bool state);

/* True when a graph owns the function-key routing and the reserved voices. */
bool sound_enabled(void);
/* Directly start/stop the prime mover (HIL / diagnostics). */
void sound_engine_power(bool on);

#endif
