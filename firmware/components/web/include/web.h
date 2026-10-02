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

/* True only when rails control is ready and maintenance is inactive. */
bool web_control_is_rails(void);
/* Source lease serializes a rail command with web/source handover. */
bool web_control_rails_begin(uint32_t *generation);
void web_control_rails_end(void);
/* Default false until outputs, maps and safety consumer are ready. */
esp_err_t web_set_actuation_ready(bool ready);
esp_err_t web_maintenance_begin(bool exclusive_fs);
void web_maintenance_end(void);
bool web_maintenance_active(void);

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

/* Apply a new master volume (0..100) from the CV63 alias: updates the cached
 * config, the live audio volume and persists it. Called from the CV path so a
 * CV63 write does not desync from /api/audio/volume. */
void web_master_volume_changed(uint8_t vol0_100);

/* Includes maintenance admission, not just the body-receive interval. */
bool web_fs_busy(void);

#endif
