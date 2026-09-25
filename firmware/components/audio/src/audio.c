#include "audio.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pinmap.h"

static const char *TAG = "audio";

/* Multi-voice mixer: one I2S channel at a fixed mono rate; all active voices
 * are resampled (linear) and summed. File open/close happens only in the mixer
 * task, so a voice can be (re)started safely from any other task. */
#define MIX_RATE  22050
#define MIX_BLOCK 256
#define AUDIO_PATH_MAX 160

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
} voice_state_t;

typedef struct {
    voice_state_t st;
    /* Pending request, protected by s_req_mutex. */
    bool req_play;
    bool req_stop;
    bool req_loop;
    uint8_t req_volume;
    char req_path[AUDIO_PATH_MAX];
} voice_t;

static i2s_chan_handle_t s_tx;
static volatile uint8_t s_volume = 20;
static voice_t s_voice[AUDIO_MAX_VOICES];
static SemaphoreHandle_t s_req_mutex;

static void mixer_task(void *arg);

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
    gpio_config_t sd = {
        .pin_bit_mask = 1ULL << PIN_AUDIO_SD_MODE,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sd));
    gpio_set_level((gpio_num_t)PIN_AUDIO_SD_MODE, 1);

    esp_err_t err = i2s_setup();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2S setup failed: %s", esp_err_to_name(err));
        return err;
    }

    s_req_mutex = xSemaphoreCreateMutex();
    if (xTaskCreatePinnedToCore(mixer_task, "audio_mix", 4096, NULL, 7, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "mixer task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Audio mixer initialized (%u Hz mono, %u voices)", MIX_RATE, AUDIO_MAX_VOICES);
    return ESP_OK;
}

esp_err_t audio_validate_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    wav_riff_t riff;
    if (fread(&riff, 1, sizeof(riff), f) != sizeof(riff)) {
        fclose(f);
        return ESP_FAIL;
    }
    if (memcmp(riff.riff, "RIFF", 4) != 0 || memcmp(riff.wave, "WAVE", 4) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    bool has_fmt = false;
    bool has_data = false;
    while (1) {
        wav_chunk_t ch;
        if (fread(&ch, 1, sizeof(ch), f) != sizeof(ch)) {
            break;
        }
        if (memcmp(ch.id, "fmt ", 4) == 0) {
            wav_fmt_t fmt;
            if (fread(&fmt, 1, sizeof(fmt), f) == sizeof(fmt)) {
                has_fmt = (fmt.format == 1 && fmt.bits_per_sample == 16);
            }
            if (ch.size > sizeof(wav_fmt_t)) {
                fseek(f, ch.size - sizeof(wav_fmt_t), SEEK_CUR);
            }
        } else if (memcmp(ch.id, "data", 4) == 0) {
            has_data = true;
            break;
        } else {
            fseek(f, ch.size, SEEK_CUR);
        }
    }
    fclose(f);
    return (has_fmt && has_data) ? ESP_OK : ESP_FAIL;
}

/* Read the next mono source sample (downmixed), honouring loop/EOF. */
static bool voice_next_sample(voice_state_t *st, int16_t *out)
{
    for (;;) {
        if (st->samples_left == 0U) {
            if (st->loop && st->samples_total > 0U) {
                fseek(st->f, (long)st->data_start, SEEK_SET);
                st->samples_left = st->samples_total;
                continue;
            }
            return false;
        }
        st->samples_left--;
        if (st->channels == 1U) {
            int16_t l;
            if (fread(&l, 2, 1, st->f) != 1) {
                return false;
            }
            *out = l;
            return true;
        }
        int16_t l, r;
        if (fread(&l, 2, 1, st->f) != 1 || fread(&r, 2, 1, st->f) != 1) {
            return false;
        }
        *out = (int16_t)(((int32_t)l + (int32_t)r) / 2);
        return true;
    }
}

/* Produce up to `count` mono output samples, adding them into mix[]. */
static int voice_fill(voice_state_t *st, int16_t *mix, int count)
{
    double step = (double)st->sample_rate / (double)MIX_RATE;
    int i;
    for (i = 0; i < count; ++i) {
        double frac = st->pos - (double)st->cur_idx;
        int32_t s = (int32_t)st->s_cur +
                    (int32_t)((double)(st->s_next - st->s_cur) * frac);
        int32_t gain = (int32_t)st->volume * (int32_t)s_volume / 100;
        int32_t out = s * gain / 100;
        int32_t acc = (int32_t)mix[i] + out;
        if (acc > 32767) {
            acc = 32767;
        } else if (acc < -32768) {
            acc = -32768;
        }
        mix[i] = (int16_t)acc;

        st->pos += step;
        while (st->pos >= (double)(st->cur_idx + 1U)) {
            st->cur_idx++;
            st->s_cur = st->s_next;
            if (!voice_next_sample(st, &st->s_next)) {
                st->active = false;
                return i + 1;
            }
        }
    }
    return i;
}

/* Open and initialise a voice (runs only in the mixer task). */
static esp_err_t voice_start(voice_state_t *st, const char *path, bool loop, uint8_t volume)
{
    if (st->f != NULL) {
        fclose(st->f);
        st->f = NULL;
    }
    st->active = false;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    (void)setvbuf(f, NULL, _IOFBF, 8192);

    wav_riff_t riff;
    if (fread(&riff, 1, sizeof(riff), f) != sizeof(riff) ||
        memcmp(riff.riff, "RIFF", 4) != 0 || memcmp(riff.wave, "WAVE", 4) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    uint32_t sample_rate = MIX_RATE;
    uint16_t channels = 1;
    bool has_data = false;
    uint32_t data_start = 0;
    uint32_t data_len = 0;
    while (1) {
        wav_chunk_t ch;
        if (fread(&ch, 1, sizeof(ch), f) != sizeof(ch)) {
            break;
        }
        if (memcmp(ch.id, "fmt ", 4) == 0) {
            wav_fmt_t fmt;
            if (fread(&fmt, 1, sizeof(fmt), f) == sizeof(fmt)) {
                sample_rate = fmt.sample_rate;
                channels = fmt.channels;
            }
            if (ch.size > sizeof(wav_fmt_t)) {
                fseek(f, ch.size - sizeof(wav_fmt_t), SEEK_CUR);
            }
        } else if (memcmp(ch.id, "data", 4) == 0) {
            data_start = (uint32_t)ftell(f);
            data_len = ch.size;
            has_data = true;
            break;
        } else {
            fseek(f, ch.size, SEEK_CUR);
        }
    }

    if (!has_data || sample_rate == 0U || channels < 1U || channels > 2U) {
        fclose(f);
        return ESP_FAIL;
    }

    uint32_t samples_total = data_len / (2U * (uint32_t)channels);
    if (samples_total < 2U) {
        fclose(f);
        return ESP_FAIL;
    }

    fseek(f, (long)data_start, SEEK_SET);
    st->f = f;
    st->sample_rate = sample_rate;
    st->channels = channels;
    st->data_start = data_start;
    st->samples_total = samples_total;
    st->samples_left = samples_total;
    st->loop = loop;
    st->volume = volume > 100U ? 100U : volume;
    st->pos = 0.0;
    st->cur_idx = 0;

    if (!voice_next_sample(st, &st->s_cur)) {
        fclose(f);
        st->f = NULL;
        return ESP_FAIL;
    }
    if (!voice_next_sample(st, &st->s_next)) {
        st->s_next = st->s_cur;
    }
    st->active = true;
    return ESP_OK;
}

static void mixer_task(void *arg)
{
    (void)arg;
    int16_t mix[MIX_BLOCK];
    for (;;) {
        /* Apply pending play/stop requests, then mix one block. */
        for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
            voice_t *vo = &s_voice[v];
            bool play = false, stop = false, loop = false;
            uint8_t volume = 100;
            char path[AUDIO_PATH_MAX];

            if (s_req_mutex != NULL) {
                xSemaphoreTake(s_req_mutex, portMAX_DELAY);
            }
            play = vo->req_play;
            stop = vo->req_stop;
            loop = vo->req_loop;
            volume = vo->req_volume;
            memcpy(path, vo->req_path, sizeof(path));
            path[sizeof(path) - 1] = '\0';
            vo->req_play = false;
            vo->req_stop = false;
            if (s_req_mutex != NULL) {
                xSemaphoreGive(s_req_mutex);
            }

            if (stop) {
                vo->st.active = false;
                if (vo->st.f != NULL) {
                    fclose(vo->st.f);
                    vo->st.f = NULL;
                }
            }
            if (play) {
                (void)voice_start(&vo->st, path, loop, volume);
            }
        }

        memset(mix, 0, sizeof(mix));
        for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
            voice_state_t *st = &s_voice[v].st;
            if (!st->active) {
                continue;
            }
            (void)voice_fill(st, mix, MIX_BLOCK);
        }
        size_t written = 0;
        (void)i2s_channel_write(s_tx, mix, sizeof(mix), &written, pdMS_TO_TICKS(1000));
    }
}

esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume)
{
    if (voice >= AUDIO_MAX_VOICES || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    voice_t *vo = &s_voice[voice];
    if (s_req_mutex != NULL) {
        xSemaphoreTake(s_req_mutex, portMAX_DELAY);
    }
    strncpy(vo->req_path, path, sizeof(vo->req_path) - 1);
    vo->req_path[sizeof(vo->req_path) - 1] = '\0';
    vo->req_loop = loop;
    vo->req_volume = volume > 100U ? 100U : volume;
    vo->req_play = true;
    vo->req_stop = false;
    if (s_req_mutex != NULL) {
        xSemaphoreGive(s_req_mutex);
    }
    return ESP_OK;
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
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        if (s_voice[v].st.active) {
            return true;
        }
    }
    return false;
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
