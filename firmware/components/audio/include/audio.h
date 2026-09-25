#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_MAX_VOICES 20

esp_err_t audio_init(void);
esp_err_t audio_validate_wav(const char *path);

/* Start a mixer voice playing a WAV file (loop repeats). volume 0..100. */
esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume);
esp_err_t audio_voice_stop(uint8_t voice);
void audio_stop_all(void);

void audio_set_volume(uint8_t vol);
uint8_t audio_get_volume(void);
bool audio_is_playing(void);

/* Convenience wrappers used by the web UI (single one-shot voice). */
esp_err_t audio_play(const char *path);
esp_err_t audio_stop(void);

#endif
