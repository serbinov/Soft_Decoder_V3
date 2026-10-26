#include "audio.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pinmap.h"
#include "storage.h"
#include "ima_adpcm.h"

static const char *TAG = "audio";

/* Multi-voice mixer: one I2S channel at a fixed mono rate; all active voices
 * are resampled (linear) and summed. File open/close happens only in the mixer
 * task, so a voice can be (re)started safely from any other task. */
#define MIX_RATE  22050
#define MIX_BLOCK 256
/* I2S recovery backoff: start small, double up to the cap. */
#define I2S_BACKOFF_MIN_MS 10U
#define I2S_BACKOFF_MAX_MS 500U
#define AUDIO_PATH_MAX 160
/* Per-voice stdio buffer. Kept small on purpose: with AUDIO_MAX_VOICES voices a
 * large buffer would consume a lot of internal DRAM (allocations below
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL never go to PSRAM). */
#define VOICE_IO_BUF 1024
/* PCM voices read this many source frames per stdio call instead of one
 * fread() per output sample. The cache is allocated only while a voice is
 * active, so idle voices cost no sample memory. */
#define PCM_CACHE_FRAMES 256
/* Envelope full-scale (fixed point); the anti-click fade ramps 0..AUDIO_ENV_ONE. */
#define AUDIO_ENV_ONE 1024

typedef struct {
    char riff[4];
    uint32_t size;
    char wave[4];
} wav_riff_t;

typedef struct {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

typedef struct {
    char id[4];
    uint32_t size;
} wav_chunk_t;

/* Base WAV fmt (16 bytes). ADPCM keeps this struct 16 bytes so a single read
 * matches the format chunk; the IMA extras live beside it. */
typedef struct {
    wav_fmt_t fmt;
    bool adpcm;
    uint16_t samples_per_block;
} wav_info_t;

typedef struct {
    bool active;
    FILE *f;
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t data_start;
    uint32_t samples_total;
    uint32_t samples_left;
    bool loop;
    uint8_t volume;
    float pos;
    uint32_t cur_idx;
    int16_t s_cur;
    int16_t s_next;
    uint16_t rate_permille; /* target rate, 1000 = nominal */
    float rate_cur;         /* smoothed rate actually used by the resampler */
    uint32_t played;        /* output samples produced since the last start */
    uint16_t env;           /* anti-click envelope, 0..AUDIO_ENV_ONE */
    bool stopping;          /* fading out towards silence, then close */
    bool storage_lease;
    bool io_failed;
    /* Mono sample cache: PCM reads blocks here; ADPCM decodes a block here.
     * Allocated only for an active voice and freed on close. */
    bool adpcm;
    uint32_t cache_len;
    uint32_t cache_pos;
    int16_t *cache;
} voice_state_t;

typedef struct {
    voice_state_t st;
    /* Pending request, protected by s_req_mutex. */
    bool req_play;
    bool req_stop;
    bool req_loop;
    uint8_t req_volume;
    bool req_volume_live_set; /* live volume override for an active voice */
    uint8_t req_volume_live;
    uint16_t req_rate; /* per-voice playback rate, 1000 = nominal */
    char req_path[AUDIO_PATH_MAX];
    uint32_t generation;
    audio_voice_state_t state;
    audio_completion_t completion;
    bool file_open;
    bool published_active;
    uint32_t published_position;
    uint32_t mix_generation; /* mixer-only identity of the open playback */
} voice_t;

static i2s_chan_handle_t s_tx;
static volatile uint8_t s_volume = 20;
static voice_t s_voice[AUDIO_MAX_VOICES];
static SemaphoreHandle_t s_req_mutex;
/* Voice allocator flags, protected by s_req_mutex (owner: alloc/release and
 * the mixer task when a voice ends on its own). */
static bool s_busy[AUDIO_MAX_VOICES];
static bool s_inhibited;
static bool s_initialized;
static bool s_mixer_ready;
static bool s_mixer_work;
static unsigned s_validate_work;
/* Consecutive failed/short I2S writes and the current bounded backoff. A
 * persistent DMA/clock error must not turn the priority-7 mixer into a busy
 * loop; every failure delays the next block and success clears the ratchet. */
static uint32_t s_i2s_error_streak;
static uint32_t s_i2s_backoff_ms;
/* Anti-click fade length in mix blocks; test hook (0 disables the envelope). */
static uint8_t s_fade_blocks = 2;

static uint16_t rate_clamp(uint16_t permille)
{
    if (permille < AUDIO_RATE_MIN) {
        return AUDIO_RATE_MIN;
    }
    if (permille > AUDIO_RATE_MAX) {
        return AUDIO_RATE_MAX;
    }
    return permille;
}

static void mixer_task(void *arg);

/* Clear every per-play request so a reused voice cannot inherit the previous
 * owner's path, rate, volume or one-shot live override. The playback state is
 * reset by voice_start(); only the rate is carried in from a request. */
static void voice_requests_reset(voice_t *vo)
{
    vo->req_play = false;
    vo->req_stop = false;
    vo->req_loop = false;
    vo->req_volume = 100;
    vo->req_volume_live_set = false;
    vo->req_volume_live = 100;
    vo->req_rate = 1000;
    vo->req_path[0] = '\0';
}

static void voice_close(voice_state_t *st)
{
    if (st->f != NULL) { fclose(st->f); st->f = NULL; }
    if (st->storage_lease) { storage_access_end(); st->storage_lease = false; }
    free(st->cache);
    st->cache = NULL;
    st->cache_len = 0;
    st->cache_pos = 0;
    st->active = false;
}

static esp_err_t i2s_setup(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 512;
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIX_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_AUDIO_BCLK,
            .ws = PIN_AUDIO_WS,
            .dout = PIN_AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(s_tx);
}

esp_err_t audio_init(void)
{
    if (s_initialized || s_mixer_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Until the mixer task exists the public play APIs must reject work
     * instead of queuing voices that no task will ever consume. */
    s_initialized = false;
    s_mixer_ready = false;
    s_req_mutex = xSemaphoreCreateMutex();
    if (s_req_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        s_busy[v] = false;
        s_voice[v].generation = 0;
        s_voice[v].state = AUDIO_VOICE_FINISHED;
        s_voice[v].file_open = false;
        s_voice[v].published_active = false;
        s_voice[v].published_position = 0;
        s_voice[v].mix_generation = 0;
        voice_requests_reset(&s_voice[v]);
    }

    gpio_config_t sd = {
        .pin_bit_mask = 1ULL << PIN_AUDIO_SD_MODE,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&sd);
    if (err != ESP_OK) { goto fail; }
    err = gpio_set_level((gpio_num_t)PIN_AUDIO_SD_MODE, 0);
    if (err != ESP_OK) { goto fail; }
    err = i2s_setup();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S setup failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = gpio_set_level((gpio_num_t)PIN_AUDIO_SD_MODE, 1);
    if (err != ESP_OK) { goto fail; }
    if (xTaskCreatePinnedToCore(mixer_task, "audio_mix", 4096, NULL, 7, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "mixer task create failed");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    ESP_LOGI(TAG, "Audio mixer initialized (%u Hz mono, %u voices)", MIX_RATE, AUDIO_MAX_VOICES);
    s_inhibited = false;
    s_i2s_error_streak = 0;
    s_i2s_backoff_ms = 0;
    s_mixer_ready = true;
    s_initialized = true;
    return ESP_OK;
fail:
    (void)gpio_set_level((gpio_num_t)PIN_AUDIO_SD_MODE, 0);
    if (s_tx != NULL) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_req_mutex != NULL) {
        vSemaphoreDelete(s_req_mutex);
        s_req_mutex = NULL;
    }
    s_mixer_ready = false;
    s_initialized = false;
    s_inhibited = false;
    s_i2s_error_streak = 0;
    s_i2s_backoff_ms = 0;
    return err;
}

/* Validate the complete RIFF chunk walk, including payload bounds and padding,
 * before either accepting an upload or opening a mixer voice. Accepts PCM16
 * (1..2 ch) and mono IMA ADPCM (WAVE_FORMAT_IMA_ADPCM). */
static bool wav_parse(FILE *f, wav_info_t *info, uint32_t *data_start, uint32_t *data_len)
{
    if (info == NULL || data_start == NULL || data_len == NULL) { return false; }
    memset(info, 0, sizeof(*info));
    if (fseek(f, 0, SEEK_END) != 0) { return false; }
    long file_len = ftell(f);
    if (file_len < 12 || fseek(f, 0, SEEK_SET) != 0) { return false; }
    wav_riff_t riff;
    if (fread(&riff, 1, sizeof(riff), f) != sizeof(riff) ||
        memcmp(riff.riff, "RIFF", 4) || memcmp(riff.wave, "WAVE", 4) ||
        riff.size < 4 || (uint64_t)riff.size + 8U > (uint64_t)file_len ||
        (uint64_t)riff.size + 8U > LONG_MAX) { return false; }
    uint64_t end = (uint64_t)riff.size + 8U;
    uint64_t pos = 12;
    bool has_fmt = false, has_data = false;
    while (pos < end) {
        wav_chunk_t ch;
        if (end - pos < sizeof(ch) || fread(&ch, 1, sizeof(ch), f) != sizeof(ch)) {
            return false;
        }
        pos += sizeof(ch);
        uint64_t next = pos + ch.size + (ch.size & 1U);
        if (next > end) { return false; }
        if (!memcmp(ch.id, "fmt ", 4)) {
            wav_fmt_t *fmt = &info->fmt;
            if (has_fmt || ch.size < sizeof(*fmt) ||
                fread(fmt, 1, sizeof(*fmt), f) != sizeof(*fmt)) { return false; }
            if (fmt->format == 1) {
                if (fmt->bits_per_sample != 16 ||
                    fmt->channels < 1 || fmt->channels > 2 ||
                    fmt->sample_rate == 0 || fmt->sample_rate > 192000 ||
                    fmt->block_align != 2U * fmt->channels ||
                    fmt->byte_rate != fmt->sample_rate * fmt->block_align) { return false; }
            } else if (fmt->format == 0x0011U) { /* WAVE_FORMAT_IMA_ADPCM */
                if (ch.size < 20U) { return false; }
                uint8_t ext[4];
                if (fread(ext, 1, sizeof(ext), f) != sizeof(ext)) { return false; }
                uint16_t cb_size = (uint16_t)(ext[0] | (ext[1] << 8));
                uint16_t spb = (uint16_t)(ext[2] | (ext[3] << 8));
                if (cb_size < 2U || fmt->channels != 1U || fmt->bits_per_sample != IMA_ADPCM_BITS ||
                    fmt->block_align != IMA_ADPCM_BLOCK_ALIGN ||
                    spb != IMA_ADPCM_SAMPLES_PER_BLOCK ||
                    fmt->sample_rate == 0 || fmt->sample_rate > 192000) { return false; }
                info->adpcm = true;
                info->samples_per_block = spb;
            } else {
                return false;
            }
            has_fmt = true;
        } else if (!memcmp(ch.id, "data", 4)) {
            /* WAV requires fmt before data. Rejecting here also guarantees the
             * frame size below is derived from a validated format header. */
            if (has_data || !has_fmt) { return false; }
            *data_start = (uint32_t)pos;
            *data_len = ch.size;
            has_data = true;
        }
        if (fseek(f, (long)next, SEEK_SET) != 0) { return false; }
        pos = next;
    }
    if (!has_fmt || !has_data) { return false; }
    uint32_t frame = info->adpcm ? (uint32_t)IMA_ADPCM_BLOCK_ALIGN : info->fmt.block_align;
    if (frame == 0U || *data_len < frame || *data_len % frame != 0U) { return false; }
    return true;
}

static esp_err_t validate_wav_file(const char *path, bool allow_inhibited)
{
    if (path == NULL) { return ESP_ERR_INVALID_ARG; }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    if (s_inhibited && !allow_inhibited) {
        if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t admission = storage_access_begin();
    if (admission != ESP_OK) {
        if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
        return admission;
    }
    ++s_validate_work;
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    FILE *f = fopen(path, "rb");
    wav_info_t info;
    uint32_t start, len;
    bool valid = f != NULL && wav_parse(f, &info, &start, &len);
    if (f != NULL) { fclose(f); }
    storage_access_end();
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    --s_validate_work;
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return f == NULL ? ESP_ERR_NOT_FOUND : (valid ? ESP_OK : ESP_FAIL);
}

esp_err_t audio_validate_wav(const char *path)
{
    return validate_wav_file(path, false);
}

esp_err_t audio_inspect_wav(const char *path)
{
    return validate_wav_file(path, true);
}

/* Allocate the per-voice mono sample cache on first use. */
static bool voice_ensure_cache(voice_state_t *st, size_t samples)
{
    if (st->cache != NULL) { return true; }
    st->cache = malloc(samples * sizeof(*st->cache));
    if (st->cache == NULL) {
        st->io_failed = true;
        return false;
    }
    return true;
}

static bool voice_rewind(voice_state_t *st)
{
    if (!st->loop || st->samples_total == 0U) { return false; }
    if (fseek(st->f, (long)st->data_start, SEEK_SET) != 0) {
        st->io_failed = true;
        return false;
    }
    st->samples_left = st->samples_total;
    st->cache_len = 0;
    st->cache_pos = 0;
    return true;
}

/* Decode one ADPCM block into the voice's sample cache. */
static bool voice_read_block(voice_state_t *st)
{
    if (!voice_ensure_cache(st, IMA_ADPCM_SAMPLES_PER_BLOCK)) { return false; }
    uint8_t raw[IMA_ADPCM_BLOCK_ALIGN];
    if (fread(raw, 1, sizeof(raw), st->f) != sizeof(raw)) {
        st->io_failed = true;
        return false;
    }
    int cnt = ima_adpcm_decode_block(raw, sizeof(raw), st->cache, IMA_ADPCM_SAMPLES_PER_BLOCK);
    if (cnt <= 0) {
        st->io_failed = true;
        return false;
    }
    st->cache_len = (uint32_t)cnt;
    st->cache_pos = 0;
    return true;
}

/* Fill the PCM cache with the next block of downmixed source frames. */
static bool voice_fill_pcm_cache(voice_state_t *st)
{
    if (!voice_ensure_cache(st, PCM_CACHE_FRAMES)) { return false; }
    uint32_t frames = st->samples_left < PCM_CACHE_FRAMES
                          ? st->samples_left
                          : (uint32_t)PCM_CACHE_FRAMES;
    if (frames == 0U) { return false; }

    if (st->channels == 1U) {
        if (fread(st->cache, sizeof(*st->cache), frames, st->f) != frames) {
            st->io_failed = true;
            return false;
        }
    } else {
        /* Read stereo in small chunks so the mixer stack stays small. */
        uint32_t done = 0;
        while (done < frames) {
            uint32_t chunk = (frames - done) > 128U ? 128U : (frames - done);
            int16_t raw[256];
            if (fread(raw, sizeof(*raw), chunk * 2U, st->f) != chunk * 2U) {
                st->io_failed = true;
                return false;
            }
            for (uint32_t i = 0; i < chunk; ++i) {
                st->cache[done + i] = (int16_t)(((int32_t)raw[i * 2U] +
                                                 (int32_t)raw[i * 2U + 1U]) / 2);
            }
            done += chunk;
        }
    }
    st->cache_len = frames;
    st->cache_pos = 0;
    return true;
}

/* Read the next mono source sample (downmixed), honouring loop/EOF. */
static bool voice_next_sample(voice_state_t *st, int16_t *out)
{
    for (;;) {
        if (st->cache_pos < st->cache_len) {
            st->samples_left--;
            *out = st->cache[st->cache_pos++];
            return true;
        }
        if (st->samples_left == 0U) {
            if (voice_rewind(st)) { continue; }
            return false;
        }
        if (st->adpcm) {
            if (!voice_read_block(st)) { return false; }
        } else if (!voice_fill_pcm_cache(st)) {
            return false;
        }
    }
}

/* Produce up to `count` mono output samples, adding them into mix[]. */
static int voice_fill(voice_state_t *st, int16_t *mix, int count)
{
    /* Smooth the playback rate towards its target once per block so a rate step
     * does not produce an audible click (miniaudio/SoLoud rule: keep the
     * resampler active even at 1.0). */
    float target = (float)st->rate_permille / 1000.0f;
    st->rate_cur += (target - st->rate_cur) * 0.25f;
    float step = ((float)st->sample_rate / (float)MIX_RATE) * st->rate_cur;

    /* Anti-click envelope: ramp in on start / out on stop over s_fade_blocks. */
    int env_step = 0;
    if (s_fade_blocks != 0U) {
        env_step = AUDIO_ENV_ONE / ((int)s_fade_blocks * MIX_BLOCK);
        if (env_step < 1) {
            env_step = 1;
        }
    }
    int i;
    for (i = 0; i < count; ++i) {
        if (env_step != 0) {
            if (st->stopping) {
                st->env = ((int)st->env > env_step) ? (uint16_t)(st->env - env_step) : 0U;
            } else if (st->env < AUDIO_ENV_ONE) {
                int e = (int)st->env + env_step;
                st->env = (uint16_t)(e > AUDIO_ENV_ONE ? AUDIO_ENV_ONE : e);
            }
        }
        float frac = st->pos;
        int32_t s = (int32_t)st->s_cur +
                    (int32_t)((float)(st->s_next - st->s_cur) * frac);
        int32_t gain = (int32_t)st->volume * (int32_t)s_volume / 100;
        int32_t out = s * gain / 100;
        if (env_step != 0) {
            out = out * (int32_t)st->env / AUDIO_ENV_ONE;
        }
        int32_t acc = (int32_t)mix[i] + out;
        if (acc > 32767) {
            acc = 32767;
        } else if (acc < -32768) {
            acc = -32768;
        }
        mix[i] = (int16_t)acc;
        st->played++;

        /* A stop request fades the voice out; close only once silent. */
        if (env_step != 0 && st->stopping && st->env == 0U) {
            voice_close(st);
            return i + 1;
        }

        st->pos += step;
        while (st->pos >= 1.0f) {
            st->pos -= 1.0f; /* bounded fractional phase, independent of index wrap */
            st->cur_idx++;
            st->s_cur = st->s_next;
            if (!voice_next_sample(st, &st->s_next)) {
                /* Close the file here: a one-shot that ends on its own would
                 * otherwise keep its descriptor until the voice is reused. */
                voice_close(st);
                return i + 1;
            }
        }
    }
    return i;
}

/* Open and initialise a voice (runs only in the mixer task). */
static esp_err_t voice_start(voice_state_t *st, const char *path, bool loop, uint8_t volume)
{
    voice_close(st);
    st->io_failed = false;
    esp_err_t admission = storage_access_begin();
    if (admission != ESP_OK) { return admission; }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        storage_access_end();
        return ESP_ERR_NOT_FOUND;
    }
    st->f = f;
    st->storage_lease = true;
    if (setvbuf(f, NULL, _IOFBF, VOICE_IO_BUF) != 0) { (void)setvbuf(f, NULL, _IONBF, 0); }

    wav_info_t info = {0};
    uint32_t data_start = 0;
    uint32_t data_len = 0;
    if (!wav_parse(f, &info, &data_start, &data_len)) {
        voice_close(st);
        return ESP_FAIL;
    }

    uint32_t samples_total = info.adpcm
        ? (data_len / IMA_ADPCM_BLOCK_ALIGN) * IMA_ADPCM_SAMPLES_PER_BLOCK
        : data_len / info.fmt.block_align;
    if (samples_total < 2U) {
        voice_close(st);
        return ESP_FAIL;
    }

    uint16_t rate = st->rate_permille;
    if (rate < AUDIO_RATE_MIN || rate > AUDIO_RATE_MAX) {
        rate = 1000; /* unset/invalid (e.g. zero-initialised state) -> nominal */
    }

    if (fseek(f, (long)data_start, SEEK_SET) != 0) { voice_close(st); return ESP_FAIL; }
    st->f = f;
    st->sample_rate = info.fmt.sample_rate;
    st->channels = info.fmt.channels;
    st->data_start = data_start;
    st->samples_total = samples_total;
    st->samples_left = samples_total;
    st->adpcm = info.adpcm;
    st->cache_len = 0;
    st->cache_pos = 0;
    st->loop = loop;
    st->volume = volume > 100U ? 100U : volume;
    st->pos = 0.0f;
    st->cur_idx = 0;
    st->rate_permille = rate;
    st->rate_cur = (float)rate / 1000.0f;
    st->played = 0;
    st->stopping = false;
    st->env = (s_fade_blocks == 0U) ? AUDIO_ENV_ONE : 0U;

    if (!voice_next_sample(st, &st->s_cur)) {
        voice_close(st);
        return ESP_FAIL;
    }
    if (!voice_next_sample(st, &st->s_next)) {
        st->s_next = st->s_cur;
    }
    st->active = true;
    return ESP_OK;
}

/* Test hook: 0 runs forever (production); host tests set a small cap. */
static uint32_t s_mix_iter_cap;

static void mixer_task(void *arg)
{
    (void)arg;
    int16_t mix[MIX_BLOCK];
    uint32_t iters = 0;
    while (s_mix_iter_cap == 0U || iters < s_mix_iter_cap) {
        /* Publish in-flight filesystem work before dropping the short request
         * lock. Slow FILE/I2S operations never block admission or sound locks. */
        if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
        s_mixer_work = true;
        if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
        memset(mix, 0, sizeof(mix));
        /* Apply pending play/stop requests, then mix one block. */
        for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
            voice_t *vo = &s_voice[v];
            bool play = false, stop = false, loop = false, start_failed = false;
            uint8_t volume = 100;
            bool vol_live = false; uint8_t vol_live_val = 100;
            uint16_t rate = 1000;
            char path[AUDIO_PATH_MAX];
            if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
            uint32_t generation = vo->generation;
            bool inhibited = s_inhibited;
            play = vo->req_play;
            stop = vo->req_stop;
            loop = vo->req_loop;
            volume = vo->req_volume;
            rate = rate_clamp(vo->req_rate);
            vol_live = vo->req_volume_live_set; vol_live_val = vo->req_volume_live; vo->req_volume_live_set = false;
            memcpy(path, vo->req_path, sizeof(path));
            path[sizeof(path) - 1] = '\0';
            vo->req_play = false;
            vo->req_stop = false;
            if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }

            if (stop) {
                if (!inhibited && s_fade_blocks != 0U && vo->st.active) {
                    vo->st.stopping = true; /* fade out, then close in voice_fill */
                } else {
                    vo->st.stopping = false;
                    voice_close(&vo->st);
                }
            }
            if (play && !inhibited) {
                vo->mix_generation = generation;
                vo->st.stopping = false;
                vo->st.rate_permille = rate;
                start_failed = voice_start(&vo->st, path, loop, volume) != ESP_OK;
                /* The sound engine queues play and its first live volume in the
                 * same tick. Apply the override after start so ducking/muting is
                 * honoured for the first block instead of being dropped. */
                if (!start_failed && vol_live) {
                    vo->st.volume = vol_live_val > 100U ? 100U : vol_live_val;
                }
            } else if (vo->st.active) {
                vo->st.rate_permille = rate; /* live rate updates */
                if (vol_live) { vo->st.volume = vol_live_val > 100U ? 100U : vol_live_val; }
            }
            if (vo->st.active) { (void)voice_fill(&vo->st, mix, MIX_BLOCK); }
            if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
            vo->file_open = vo->st.f != NULL;
            vo->published_active = vo->st.active;
            vo->published_position = vo->st.played;
            /* EOF/failed open must not free or publish over a newer request. */
            if (vo->generation == generation && !vo->req_play && !vo->req_stop &&
                (play || stop || vo->mix_generation == generation)) {
                if (!vo->st.active) {
                    s_busy[v] = false;
                    vo->state = AUDIO_VOICE_FINISHED;
                    if (vo->completion == AUDIO_COMPLETION_NONE) {
                        vo->completion = start_failed || vo->st.io_failed ? AUDIO_COMPLETION_ERROR :
                            (stop || inhibited || vo->st.stopping ? AUDIO_COMPLETION_STOPPED : AUDIO_COMPLETION_EOF);
                    }
                } else if (!stop) {
                    vo->state = AUDIO_VOICE_PLAYING;
                }
            }
            if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
        }
        if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
        s_mixer_work = false;
        if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
        size_t written = 0;
        esp_err_t werr = i2s_channel_write(s_tx, mix, sizeof(mix), &written,
                                           pdMS_TO_TICKS(1000));
        if (werr != ESP_OK || written != sizeof(mix)) {
            if (s_i2s_error_streak < UINT32_MAX) { ++s_i2s_error_streak; }
            uint32_t backoff = s_i2s_backoff_ms;
            if (backoff == 0U) {
                backoff = I2S_BACKOFF_MIN_MS;
            } else if (backoff < I2S_BACKOFF_MAX_MS) {
                backoff = backoff > I2S_BACKOFF_MAX_MS / 2U
                              ? I2S_BACKOFF_MAX_MS
                              : backoff * 2U;
            }
            s_i2s_backoff_ms = backoff;
            /* Log the entry into the error state and then only occasionally, so
             * a dead I2S link cannot flood the UART either. */
            if (s_i2s_error_streak == 1U || (s_i2s_error_streak % 50U) == 0U) {
                ESP_LOGW(TAG, "I2S write failed (%s, %u/%u bytes), backing off %u ms",
                         esp_err_to_name(werr), (unsigned)written,
                         (unsigned)sizeof(mix), (unsigned)backoff);
            }
            vTaskDelay(pdMS_TO_TICKS(backoff));
        } else if (s_i2s_error_streak != 0U) {
            ESP_LOGI(TAG, "I2S recovered after %u failed block(s)",
                     (unsigned)s_i2s_error_streak);
            s_i2s_error_streak = 0;
            s_i2s_backoff_ms = 0;
        }
        iters++;
    }
}

static uint32_t next_generation(voice_t *vo)
{
    if (++vo->generation == 0) { ++vo->generation; }
    vo->completion = AUDIO_COMPLETION_NONE;
    return vo->generation;
}

static esp_err_t queue_play(uint8_t voice, const char *path, bool loop, uint8_t volume,
                           audio_voice_handle_t *out)
{
    /* A failed/partial init (or a missing mixer task) leaves no consumer for a
     * queued request, so refuse it instead of publishing AUDIO_VOICE_PENDING. */
    if (!s_initialized || !s_mixer_ready || s_req_mutex == NULL || s_inhibited) {
        return ESP_ERR_INVALID_STATE;
    }
    voice_t *vo = &s_voice[voice];
    /* Each play is a new owner: never let a previous file's rate or live
     * volume carry over (the caller sets the new rate after this request). */
    voice_requests_reset(vo);
    memcpy(vo->req_path, path, strlen(path) + 1);
    vo->req_loop = loop;
    vo->req_volume = volume > 100U ? 100U : volume;
    vo->req_play = true;
    s_busy[voice] = true;
    vo->state = AUDIO_VOICE_PENDING;
    vo->completion = AUDIO_COMPLETION_NONE;
    next_generation(vo);
    if (out != NULL) { *out = (audio_voice_handle_t){voice, vo->generation}; }
    return ESP_OK;
}

esp_err_t audio_voice_play_generation(uint8_t voice, const char *path, bool loop,
                                      uint8_t volume, audio_voice_handle_t *out)
{
    if (voice >= AUDIO_MAX_VOICES || path == NULL || strlen(path) >= AUDIO_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out != NULL) { *out = (audio_voice_handle_t){AUDIO_VOICE_NONE, 0}; }
    if (s_req_mutex != NULL) {
        xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    }
    esp_err_t err = queue_play(voice, path, loop, volume, out);
    if (s_req_mutex != NULL) {
        xSemaphoreGive(s_req_mutex);
    }
    return err;
}

esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume)
{
    return audio_voice_play_generation(voice, path, loop, volume, NULL);
}

esp_err_t audio_voice_stop(uint8_t voice)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return ESP_ERR_INVALID_ARG;
    }
    voice_t *vo = &s_voice[voice];
    if (s_req_mutex != NULL) {
        xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    }
    vo->req_play = false;
    vo->req_stop = true;
    vo->state = AUDIO_VOICE_FINISHED;
    vo->completion = AUDIO_COMPLETION_STOPPED;
    if (s_req_mutex != NULL) {
        xSemaphoreGive(s_req_mutex);
    }
    return ESP_OK;
}

void audio_stop_all(void)
{
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        (void)audio_voice_stop(v);
    }
}

esp_err_t audio_voice_set_volume_live(uint8_t voice, uint8_t volume)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return ESP_ERR_INVALID_ARG;
    }
    voice_t *vo = &s_voice[voice];
    if (s_req_mutex != NULL) {
        xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    }
    vo->req_volume_live = volume > 100U ? 100U : volume;
    vo->req_volume_live_set = true;
    if (s_req_mutex != NULL) {
        xSemaphoreGive(s_req_mutex);
    }
    return ESP_OK;
}

esp_err_t audio_voice_alloc_owned(audio_voice_handle_t *out)
{
    if (out == NULL) { return ESP_ERR_INVALID_ARG; }
    *out = (audio_voice_handle_t){AUDIO_VOICE_NONE, 0};
    if (!s_initialized || !s_mixer_ready || s_req_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    esp_err_t err = s_inhibited ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    for (int v = 0; !s_inhibited && v < AUDIO_DYNAMIC_VOICES; ++v) {
        voice_t *vo = &s_voice[v];
        if (!s_busy[v] && !vo->file_open && !vo->req_play && !vo->req_stop) {
            /* Reuse starts from a clean request table; otherwise a stale rate
             * from the previous file would be applied to the next playback. */
            voice_requests_reset(vo);
            if (!vo->st.active) {
                vo->st.rate_permille = 1000;
                vo->st.rate_cur = 1.0f;
            }
            s_busy[v] = true;
            *out = (audio_voice_handle_t){(uint8_t)v, next_generation(vo)};
            err = ESP_OK;
            break;
        }
    }
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return err;
}

esp_err_t audio_voice_play_owned(audio_voice_handle_t *handle, const char *path,
                                bool loop, uint8_t volume)
{
    if (handle == NULL || handle->voice >= AUDIO_DYNAMIC_VOICES || path == NULL ||
        strlen(path) >= AUDIO_PATH_MAX) { return ESP_ERR_INVALID_ARG; }
    if (!s_initialized || !s_mixer_ready || s_req_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    uint8_t v = handle->voice;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_busy[v] && s_voice[v].generation == handle->generation) {
        err = queue_play(v, path, loop, volume, handle);
    }
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return err;
}

void audio_voice_release_owned(audio_voice_handle_t handle)
{
    if (handle.voice >= AUDIO_MAX_VOICES) { return; }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    voice_t *vo = &s_voice[handle.voice];
    if (vo->generation == handle.generation) {
        s_busy[handle.voice] = false;
        voice_requests_reset(vo);
        vo->req_stop = true;
        vo->state = AUDIO_VOICE_FINISHED;
        next_generation(vo);
    }
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
}

audio_voice_state_t audio_voice_get_state(audio_voice_handle_t handle)
{
    if (handle.voice >= AUDIO_MAX_VOICES) { return AUDIO_VOICE_FINISHED; }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    voice_t *vo = &s_voice[handle.voice];
    audio_voice_state_t state = vo->generation == handle.generation ? vo->state :
                               AUDIO_VOICE_FINISHED;
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return state;
}

audio_completion_t audio_voice_completion(audio_voice_handle_t handle)
{
    if (handle.voice >= AUDIO_MAX_VOICES) { return AUDIO_COMPLETION_STALE; }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    voice_t *vo = &s_voice[handle.voice];
    audio_completion_t completion = vo->generation == handle.generation ? vo->completion :
                                    AUDIO_COMPLETION_STALE;
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return completion;
}

esp_err_t audio_voice_set_rate_owned(audio_voice_handle_t handle, uint16_t permille)
{
    if (handle.voice >= AUDIO_MAX_VOICES) { return ESP_ERR_INVALID_ARG; }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    voice_t *vo = &s_voice[handle.voice];
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_inhibited && s_busy[handle.voice] && vo->generation == handle.generation) {
        vo->req_rate = rate_clamp(permille);
        err = ESP_OK;
    }
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return err;
}

esp_err_t audio_set_inhibited(bool inhibited)
{
    if (s_req_mutex == NULL) { return ESP_ERR_INVALID_STATE; }
    /* Never let maintenance wait unboundedly for slow filesystem I/O. */
    if (xSemaphoreTake(s_req_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_inhibited = inhibited;
    if (inhibited) {
        for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
            s_voice[v].req_play = false;
            s_voice[v].req_stop = true;
            s_voice[v].state = AUDIO_VOICE_FINISHED;
            s_busy[v] = false;
            next_generation(&s_voice[v]);
        }
    }
    xSemaphoreGive(s_req_mutex);
    return ESP_OK;
}

bool audio_is_quiescent(void)
{
    if (s_req_mutex == NULL) {
        return !s_initialized && !s_mixer_work && s_validate_work == 0;
    }
    if (xSemaphoreTake(s_req_mutex, 0) != pdTRUE) { return false; }
    bool quiet = !s_mixer_work && s_validate_work == 0;
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        voice_t *vo = &s_voice[v];
        if (vo->file_open || vo->req_play || vo->req_stop) { quiet = false; break; }
    }
    xSemaphoreGive(s_req_mutex);
    return quiet;
}

void audio_set_volume(uint8_t vol)
{
    if (vol > 100) {
        vol = 100;
    }
    s_volume = vol;
}

uint8_t audio_get_volume(void)
{
    return s_volume;
}

bool audio_is_playing(void)
{
    bool playing = false;
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        if (s_voice[v].published_active) {
            playing = true;
            break;
        }
    }
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return playing;
}
