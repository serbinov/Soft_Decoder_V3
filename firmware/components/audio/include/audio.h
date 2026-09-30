#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_MAX_VOICES 20

/* "No free voice" result of audio_voice_alloc(). */
#define AUDIO_VOICE_NONE 0xFF

/* Playback-rate limits, per mille of the original sample rate (1000 = nominal). */
#define AUDIO_RATE_MIN 500
#define AUDIO_RATE_MAX 3000

esp_err_t audio_init(void);
esp_err_t audio_validate_wav(const char *path);

/* Start a mixer voice playing a WAV file (loop repeats). volume 0..100. */
esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume);
esp_err_t audio_voice_stop(uint8_t voice);
void audio_stop_all(void);

/* Per-voice playback rate (resampling speed), clipped to AUDIO_RATE_MIN..MAX.
 * Applied live to a playing voice; 1000 keeps the original pitch. */
esp_err_t audio_voice_set_rate(uint8_t voice, uint16_t permille);

/* Voice state queries, safe to call from any task. */
bool audio_voice_is_active(uint8_t voice);
uint32_t audio_voice_position(uint8_t voice); /* output samples produced */

/* Simple voice allocator: returns a free voice index or AUDIO_VOICE_NONE. */
uint8_t audio_voice_alloc(void);
void audio_voice_release(uint8_t voice);

void audio_set_volume(uint8_t vol);
uint8_t audio_get_volume(void);
bool audio_is_playing(void);

/* Convenience wrappers used by the web UI (single one-shot voice). */
esp_err_t audio_play(const char *path);
esp_err_t audio_stop(void);

#endif
