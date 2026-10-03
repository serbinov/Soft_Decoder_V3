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

static const char *TAG = "audio";

/* Multi-voice mixer: one I2S channel at a fixed mono rate; all active voices
 * are resampled (linear) and summed. File open/close happens only in the mixer
 * task, so a voice can be (re)started safely from any other task. */
#define MIX_RATE  22050
#define MIX_BLOCK 256
#define AUDIO_PATH_MAX 160
/* Per-voice stdio buffer. Kept small on purpose: with AUDIO_MAX_VOICES voices a
 * large buffer would consume a lot of internal DRAM (allocations below
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL never go to PSRAM). */
#define VOICE_IO_BUF 2048
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
    double pos;
    uint32_t cur_idx;
    int16_t s_cur;
    int16_t s_next;
    uint16_t rate_permille; /* target rate, 1000 = nominal */
    double rate_cur;        /* smoothed rate actually used by the resampler */
    uint32_t played;        /* output samples produced since the last start */
    uint16_t env;           /* anti-click envelope, 0..AUDIO_ENV_ONE */
    bool stopping;          /* fading out towards silence, then close */
    bool storage_lease;
    bool io_failed;
} voice_state_t;

typedef struct {
    voice_state_t st;
    /* Pending request, protected by s_req_mutex. */
    bool req_play;
    bool req_stop;
    bool req_loop;
    uint8_t req_volume;
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
static bool s_mixer_work;
static unsigned s_validate_work;
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

static void voice_close(voice_state_t *st)
{
    if (st->f != NULL) { fclose(st->f); st->f = NULL; }
    if (st->storage_lease) { storage_access_end(); st->storage_lease = false; }
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
    if (s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_req_mutex = xSemaphoreCreateMutex();
    if (s_req_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        s_busy[v] = false;
        s_voice[v].req_rate = 1000;
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
    s_initialized = true;
    return ESP_OK;
fail:
    (void)gpio_set_level((gpio_num_t)PIN_AUDIO_SD_MODE, 0);
    if (s_tx != NULL) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    vSemaphoreDelete(s_req_mutex);
    s_req_mutex = NULL;
    return err;
}

/* Validate the complete RIFF chunk walk, including payload bounds and padding,
 * before either accepting an upload or opening a mixer voice. */
static bool wav_parse(FILE *f, wav_fmt_t *fmt, uint32_t *data_start, uint32_t *data_len)
{
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
            if (has_fmt || ch.size < sizeof(*fmt) ||
                fread(fmt, 1, sizeof(*fmt), f) != sizeof(*fmt)) { return false; }
            if (fmt->format != 1 || fmt->bits_per_sample != 16 ||
                fmt->channels < 1 || fmt->channels > 2 ||
                fmt->sample_rate == 0 || fmt->sample_rate > 192000 ||
                fmt->block_align != 2U * fmt->channels ||
                fmt->byte_rate != fmt->sample_rate * fmt->block_align) { return false; }
            has_fmt = true;
        } else if (!memcmp(ch.id, "data", 4)) {
            if (has_data) { return false; }
            *data_start = (uint32_t)pos;
            *data_len = ch.size;
            has_data = true;
        }
        if (fseek(f, (long)next, SEEK_SET) != 0) { return false; }
        pos = next;
    }
    return has_fmt && has_data && *data_len >= 2U * fmt->block_align &&
           *data_len % fmt->block_align == 0;
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
    wav_fmt_t fmt;
    uint32_t start, len;
    bool valid = f != NULL && wav_parse(f, &fmt, &start, &len);
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

/* Read the next mono source sample (downmixed), honouring loop/EOF. */
static bool voice_next_sample(voice_state_t *st, int16_t *out)
{
    for (;;) {
        if (st->samples_left == 0U) {
            if (st->loop && st->samples_total > 0U) {
                if (fseek(st->f, (long)st->data_start, SEEK_SET) != 0) { st->io_failed = true; return false; }
                st->samples_left = st->samples_total;
                continue;
            }
            return false;
        }
        st->samples_left--;
        if (st->channels == 1U) {
            int16_t l;
            if (fread(&l, 2, 1, st->f) != 1) {
                st->io_failed = true;
                return false;
            }
            *out = l;
            return true;
        }
        int16_t l, r;
        if (fread(&l, 2, 1, st->f) != 1 || fread(&r, 2, 1, st->f) != 1) {
            st->io_failed = true;
            return false;
        }
        *out = (int16_t)(((int32_t)l + (int32_t)r) / 2);
        return true;
    }
}

/* Produce up to `count` mono output samples, adding them into mix[]. */
static int voice_fill(voice_state_t *st, int16_t *mix, int count)
{
    /* Smooth the playback rate towards its target once per block so a rate step
     * does not produce an audible click (miniaudio/SoLoud rule: keep the
     * resampler active even at 1.0). */
    double target = (double)st->rate_permille / 1000.0;
    st->rate_cur += (target - st->rate_cur) * 0.25;
    double step = ((double)st->sample_rate / (double)MIX_RATE) * st->rate_cur;

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
        double frac = st->pos;
        int32_t s = (int32_t)st->s_cur +
                    (int32_t)((double)(st->s_next - st->s_cur) * frac);
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
        while (st->pos >= 1.0) {
            st->pos -= 1.0; /* bounded fractional phase, independent of index wrap */
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

    wav_fmt_t fmt;
    uint32_t data_start = 0;
    uint32_t data_len = 0;
    if (!wav_parse(f, &fmt, &data_start, &data_len)) {
        voice_close(st);
        return ESP_FAIL;
    }

    uint32_t samples_total = data_len / fmt.block_align;
    if (samples_total < 2U) {
        voice_close(st);
        return ESP_FAIL;
    }

    uint16_t rate = st->rate_permille;
    if (rate < AUDIO_RATE_MIN) {
        rate = 1000; /* unset/invalid (e.g. zero-initialised state) -> nominal */
    }

    if (fseek(f, (long)data_start, SEEK_SET) != 0) { voice_close(st); return ESP_FAIL; }
    st->f = f;
    st->sample_rate = fmt.sample_rate;
    st->channels = fmt.channels;
    st->data_start = data_start;
    st->samples_total = samples_total;
    st->samples_left = samples_total;
    st->loop = loop;
    st->volume = volume > 100U ? 100U : volume;
    st->pos = 0.0;
    st->cur_idx = 0;
    st->rate_permille = rate;
    st->rate_cur = (double)rate / 1000.0;
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
            } else if (vo->st.active) {
                vo->st.rate_permille = rate; /* live rate updates */
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
        (void)i2s_channel_write(s_tx, mix, sizeof(mix), &written, pdMS_TO_TICKS(1000));
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
    if (s_inhibited) { return ESP_ERR_INVALID_STATE; }
    voice_t *vo = &s_voice[voice];
    memcpy(vo->req_path, path, strlen(path) + 1);
    vo->req_loop = loop;
    vo->req_volume = volume > 100U ? 100U : volume;
    vo->req_play = true;
    vo->req_stop = false;
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

esp_err_t audio_voice_set_rate(uint8_t voice, uint16_t permille)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return ESP_ERR_INVALID_ARG;
    }
    voice_t *vo = &s_voice[voice];
    if (s_req_mutex != NULL) {
        xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    }
    vo->req_rate = rate_clamp(permille);
    if (s_req_mutex != NULL) {
        xSemaphoreGive(s_req_mutex);
    }
    return ESP_OK;
}

bool audio_voice_is_active(uint8_t voice)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return false;
    }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    bool active = s_voice[voice].published_active;
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return active;
}

uint32_t audio_voice_position(uint8_t voice)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return 0U;
    }
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    uint32_t played = s_voice[voice].published_position;
    if (s_req_mutex != NULL) { xSemaphoreGive(s_req_mutex); }
    return played;
}

uint8_t audio_voice_alloc(void)
{
    uint8_t found = AUDIO_VOICE_NONE;
    if (s_req_mutex != NULL) {
        xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    }
    for (int v = 0; !s_inhibited && v < AUDIO_DYNAMIC_VOICES; ++v) {
        if (!s_busy[v] && !s_voice[v].file_open && !s_voice[v].req_play &&
            !s_voice[v].req_stop) {
            s_busy[v] = true;
            next_generation(&s_voice[v]);
            found = (uint8_t)v;
            break;
        }
    }
    if (s_req_mutex != NULL) {
        xSemaphoreGive(s_req_mutex);
    }
    return found;
}

void audio_voice_release(uint8_t voice)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return;
    }
    if (s_req_mutex != NULL) {
        xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    }
    s_busy[voice] = false;
    s_voice[voice].req_play = false;
    s_voice[voice].req_stop = true;
    s_voice[voice].state = AUDIO_VOICE_FINISHED;
    next_generation(&s_voice[voice]);
    if (s_req_mutex != NULL) {
        xSemaphoreGive(s_req_mutex);
    }
}

esp_err_t audio_voice_alloc_owned(audio_voice_handle_t *out)
{
    if (out == NULL) { return ESP_ERR_INVALID_ARG; }
    *out = (audio_voice_handle_t){AUDIO_VOICE_NONE, 0};
    if (s_req_mutex != NULL) { xSemaphoreTake(s_req_mutex, portMAX_DELAY); }
    esp_err_t err = s_inhibited ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    for (int v = 0; !s_inhibited && v < AUDIO_DYNAMIC_VOICES; ++v) {
        voice_t *vo = &s_voice[v];
        if (!s_busy[v] && !vo->file_open && !vo->req_play && !vo->req_stop) {
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
        vo->req_play = false;
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

esp_err_t audio_play(const char *path)
{
    return audio_voice_play(0, path, false, 100);
}

esp_err_t audio_stop(void)
{
    audio_stop_all();
    return ESP_OK;
}
