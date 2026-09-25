#ifndef TRACK_H
#define TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* DC (analog) mode support. Enabled by CV29 bit 2 (NMRA analog bit);
 * default is DCC-only. The rail ADC task drives the motor from the rail
 * voltage only while DC mode is active and rail control is enabled. */

esp_err_t track_init(void);

bool track_is_dc_mode(void);

/* Result of track_recover_from_storage(). */
typedef struct {
    size_t count;       /* slots available after recovery */
    size_t files_found; /* .wav files seen (only when rebuilding from files) */
    bool had_nvs;       /* NVS already held a list (nothing was changed) */
    bool from_manifest; /* metadata restored from the manifest */
    bool rebuilt;       /* list rebuilt from the file names */
} track_recover_result_t;

/* Rebuild the track list from storage when NVS has none: first from the
 * metadata manifest, else by scanning `audio_dir` (then `root_dir`) for .wav. */
void track_recover_from_storage(const char *audio_dir, const char *root_dir,
                                track_recover_result_t *out);

#endif

