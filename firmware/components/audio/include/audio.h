#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_MAX_VOICES 20

/* "No free voice" result of audio_voice_alloc(). */
#define AUDIO_VOICE_NONE 0xFF
#define AUDIO_DYNAMIC_VOICES 18 /* 18/19 belong exclusively to the sound engine */

typedef struct {
    uint8_t voice;
    uint32_t generation;
} audio_voice_handle_t;

typedef enum {
    AUDIO_VOICE_FINISHED = 0, /* also returned for an invalid/stale handle */
    AUDIO_VOICE_PENDING,
    AUDIO_VOICE_PLAYING
} audio_voice_state_t;

/* Playback-rate limits, per mille of the original sample rate (1000 = nominal). */
#define AUDIO_RATE_MIN 500
#define AUDIO_RATE_MAX 3000

esp_err_t audio_init(void);
esp_err_t audio_validate_wav(const char *path);
/* Admission barrier, not a synchronous stop. Poll quiescence with a bounded
 * deadline before touching the filesystem. Uninhibit never restores old plays. */
esp_err_t audio_set_inhibited(bool inhibited);
/* Quiescence is also true before initialization/after complete init rollback;
 * mutex contention on an initialized mixer is never treated as quiet. */
bool audio_is_quiescent(void);

/* Atomic ownership API. alloc excludes 18/19. play updates the handle to the
 * new playback generation; stale play/release cannot affect another owner.
 * EOF frees the allocator slot, so callers must query/reallocate before replay.
 * Index APIs below remain for legacy callers, without ownership guarantees. */
esp_err_t audio_voice_alloc_owned(audio_voice_handle_t *out);
esp_err_t audio_voice_play_owned(audio_voice_handle_t *handle, const char *path,
                                bool loop, uint8_t volume);
void audio_voice_release_owned(audio_voice_handle_t handle);
audio_voice_state_t audio_voice_get_state(audio_voice_handle_t handle);
/* Reserved/index caller: queue a new generation and return its exact identity. */
esp_err_t audio_voice_play_generation(uint8_t voice, const char *path, bool loop,
                                      uint8_t volume, audio_voice_handle_t *out);

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
