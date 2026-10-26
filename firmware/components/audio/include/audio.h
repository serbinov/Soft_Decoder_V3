#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_MAX_VOICES 20

/* Sentinel for an unassigned voice handle. */
#define AUDIO_VOICE_NONE 0xFF
/* Voices 0..17 are dynamically allocated; 18 is reserved for SELFTEST/HIL and
 * 19 belongs exclusively to the sound engine. */
#define AUDIO_DYNAMIC_VOICES 18

typedef struct {
    uint8_t voice;
    uint32_t generation;
} audio_voice_handle_t;

typedef enum {
    AUDIO_VOICE_FINISHED = 0, /* also returned for an invalid/stale handle */
    AUDIO_VOICE_PENDING,
    AUDIO_VOICE_PLAYING
} audio_voice_state_t;

typedef enum {
    AUDIO_COMPLETION_NONE = 0,
    AUDIO_COMPLETION_EOF,
    AUDIO_COMPLETION_ERROR,
    AUDIO_COMPLETION_STOPPED,
    AUDIO_COMPLETION_STALE
} audio_completion_t;

/* Playback-rate limits, per mille of the original sample rate (1000 = nominal). */
#define AUDIO_RATE_MIN 500
#define AUDIO_RATE_MAX 3000

esp_err_t audio_init(void);
esp_err_t audio_validate_wav(const char *path);
/* Read-only inspection during graph Apply, after playback is quiescent.
 * Still requires a storage lease; exclusive filesystem maintenance rejects it. */
esp_err_t audio_inspect_wav(const char *path);
/* Admission barrier, not a synchronous stop. Poll quiescence with a bounded
 * deadline before touching the filesystem. Uninhibit never restores old plays. */
esp_err_t audio_set_inhibited(bool inhibited);
/* Quiescence is also true before initialization/after complete init rollback;
 * mutex contention on an initialized mixer is never treated as quiet. */
bool audio_is_quiescent(void);

/* Atomic ownership API. alloc excludes 18 (selftest) and 19 (engine). play
 * updates the handle to the new playback generation; stale play/release cannot
 * affect another owner.
 * EOF frees the allocator slot, so callers must query/reallocate before replay. */
esp_err_t audio_voice_alloc_owned(audio_voice_handle_t *out);
esp_err_t audio_voice_play_owned(audio_voice_handle_t *handle, const char *path,
                                bool loop, uint8_t volume);
void audio_voice_release_owned(audio_voice_handle_t handle);
audio_voice_state_t audio_voice_get_state(audio_voice_handle_t handle);
/* Completion and rate updates retain the playback generation, including the
 * reserved engine voices. A stale caller cannot affect a new owner. */
audio_completion_t audio_voice_completion(audio_voice_handle_t handle);
esp_err_t audio_voice_set_rate_owned(audio_voice_handle_t handle, uint16_t permille);
/* Reserved/index caller: queue a new generation and return its exact identity. */
esp_err_t audio_voice_play_generation(uint8_t voice, const char *path, bool loop,
                                      uint8_t volume, audio_voice_handle_t *out);

/* Start a mixer voice playing a WAV file (loop repeats). volume 0..100. */
esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume);
esp_err_t audio_voice_stop(uint8_t voice);
void audio_stop_all(void);

/* Live per-voice volume override (0..100) applied to the current playback,
 * including a play request still pending in the same mixer block. Cleared on
 * the next play request, so it never leaks into a new playback. Used by the
 * sound engine to duck the engine voice under effects. */
esp_err_t audio_voice_set_volume_live(uint8_t voice, uint8_t volume);

void audio_set_volume(uint8_t vol);
uint8_t audio_get_volume(void);
bool audio_is_playing(void);

#endif
