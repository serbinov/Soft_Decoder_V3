#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sound_types.h"

/* Sound-scheme engine (SOUND_ENGINE_ROADMAP.md R3). Owns the active scheme,
 * the engine state machine, the Init/Loop/End table sequencer and the playback
 * rate control. Real-time inputs come from the motor (applied speed) and from
 * function keys (DCC / web). */

esp_err_t sound_init(void);

/* Emergency stop: silence every sound and reset the engine state. */
void sound_stop_all(void);
bool sound_engine_is_on(void);

/* Scheme / table / extra access.
 *
 * sound_scheme_get() copies the whole (~26 KB) scheme: the caller MUST provide
 * a heap/static buffer, never a task-stack local. The fine-grained accessors
 * below exist so the REST layer never has to copy the whole struct. */
esp_err_t sound_scheme_get(sound_scheme_t *out);
esp_err_t sound_scheme_set(const sound_scheme_t *in);
uint8_t sound_type_get(void);
esp_err_t sound_type_set(uint8_t type);
esp_err_t sound_engine_get(sound_engine_t *out);
esp_err_t sound_engine_set(const sound_engine_t *in);
esp_err_t sound_table_get(uint8_t idx, sound_table_t *out);
esp_err_t sound_table_set(uint8_t idx, const sound_table_t *t);
esp_err_t sound_extra_get(uint8_t idx, sound_extra_t *out);
esp_err_t sound_extra_set(uint8_t idx, const sound_extra_t *e);
esp_err_t sound_brake_get(sound_brake_t *out);
esp_err_t sound_brake_set(const sound_brake_t *in);

/* Runtime inputs. */
void sound_set_speed(uint8_t speed, bool forward);
void sound_function(uint8_t fn, bool state);

/* True when a real scheme (not NONE/LEGACY) is active, i.e. the engine owns
 * the function-key routing and the reserved voices 18/19. */
bool sound_scheme_enabled(void);
/* Directly start/stop the prime mover (HIL / diagnostics). */
void sound_engine_power(bool on);

/* Re-read the canonical function bindings from NVS after the settings store
 * changes (legacy migration / REST binding add/remove). */
void sound_reload_bindings(void);

/* Active scheme file name (empty string when none is selected). */
esp_err_t sound_active_name_get(char *out, size_t cap);
/* Load <root>/projects/<name>.mds, make it the active scheme and persist the
 * pointer in NVS. */
esp_err_t sound_load_scheme(const char *name);

/* Validate the active scheme graph. Returns the number of problems found and,
 * when `out` is non-NULL, a short human-readable report. */
int sound_lint(char *out, size_t cap);

/* Live engine status for the REST/UI layer. */
typedef struct {
    uint8_t type;      /* sound_scheme_type_t */
    bool    enabled;   /* a real scheme (not NONE/LEGACY) is active */
    bool    engine;    /* prime mover on */
    uint8_t table;     /* current engine table */
    uint8_t phase;     /* tb_phase_t */
    uint8_t speed;
    bool    forward;
    char    name[SOUND_FILE_MAX];
} sound_status_t;

void sound_status_get(sound_status_t *out);

/* Persist the current scheme to its active .mds file. */
esp_err_t sound_scheme_save(void);

/* Scheme "project" files (<root>/projects/<name>.mds). `name` must be a safe
 * file name (see sound_store_name_ok): 1..63 chars of [A-Za-z0-9_-]. */

/* Create a new scheme file with a default body of `type` and make it active. */
esp_err_t sound_scheme_create(const char *name, uint8_t type);
/* Delete a scheme file; if it was active the engine falls back to no scheme. */
esp_err_t sound_scheme_delete(const char *name);
/* Copy the raw `.mds` bytes of a scheme into `buf` (cap >= SOUND_STORE_MAX_BYTES). */
esp_err_t sound_scheme_export(const char *name, uint8_t *buf, size_t cap, size_t *out_len);
/* Validate raw `.mds` bytes and install them as <name>.mds; optionally activate. */
esp_err_t sound_scheme_import(const char *name, const uint8_t *buf, size_t len, bool activate);
/* Enumerate the stored schemes into `names` (up to `max` rows of SOUND_FILE_MAX). */
esp_err_t sound_scheme_list(char names[][SOUND_FILE_MAX], size_t max, size_t *count);

#endif
