#ifndef WEB_H
#define WEB_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t web_init(void);

/* Append a control event to the journal shown in the web UI (exposed via
 * /api/log). Safe to call from any task; text is truncated if too long. */
void web_log_event(const char *tag, const char *fmt, ...);

/* Function output states (F0..F28), shared with the DCC decoder. */
bool web_get_function_state(uint8_t fn);

/* True when rails (DCC) control is active; web throttle is ignored then. */
bool web_control_is_rails(void);

/* Per-voice volume (0..100) for a function/slot sound. Slot 1 is the engine
 * (uses engine volume), all other slots are effects (uses effects volume);
 * the category can be changed per slot via /api/track/category. */
uint8_t web_get_voice_volume(uint8_t fn);

/* Function (F) mapping, configured via /api/func-map.
 * web_apply_function() applies the mapped AUX/light outputs and the mapped
 * sound slots for a function change and records the state. Both the DCC
 * decoder and the web UI call it, so the reaction is identical whichever
 * control source is active; web_motion_changed() re-evaluates the
 * direction/speed gates whenever the throttle state changes;
 * web_func_audio_slots() returns the (up to two) mapped sound slots (0=none). */
void web_apply_function(uint8_t fn, bool state);
void web_motion_changed(uint8_t speed, bool forward);
void web_func_audio_slots(uint8_t fn, uint8_t *slot_a, uint8_t *slot_b);
bool web_func_map_get(uint8_t fn, uint8_t *slot_a, uint8_t *slot_b, uint16_t *aux,
                      uint8_t *dir, uint8_t *speed);
bool web_func_map_set(uint8_t fn, uint8_t slot_a, uint8_t slot_b, uint16_t aux,
                      uint8_t dir, uint8_t speed);

#endif
