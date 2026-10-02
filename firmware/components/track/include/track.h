#ifndef TRACK_H
#define TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* CV29 bit 2 permits analog driving; it does NOT select the detected mode.
 * DC requires absent digital packets/edges plus stable polarity and fresh ADC.
 * Source ownership is checked atomically before any motor command. */

esp_err_t track_init(void);

bool track_is_dc_mode(void);
/* Call for an accepted DCC speed/stop/reset command while holding the same
 * web_control_rails_begin/end lease as the motor write. Function/CV packets
 * must not claim motor ownership. */
void track_note_dcc_motor_command(void);

/* Result of track_recover_from_storage(). */
typedef struct {
    size_t count;       /* slots available after recovery */
    size_t files_found; /* .wav files seen (only when rebuilding from files) */
    bool had_nvs;       /* NVS already held a list (nothing was changed) */
    bool from_manifest; /* metadata restored from the manifest */
    bool rebuilt;       /* list rebuilt from the file names */
    esp_err_t error;     /* load/save/scan failure, ESP_OK when complete */
    bool retry_pending;  /* metadata recovery needs a later retry */
} track_recover_result_t;

/* Rebuild the track list from storage when NVS has none: first from the
 * metadata manifest, else by scanning `audio_dir` (then `root_dir`) for .wav. */
void track_recover_from_storage(const char *audio_dir, const char *root_dir,
                                track_recover_result_t *out);

#endif
