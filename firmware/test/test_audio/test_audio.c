#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <math.h>

#include "../../components/sound/src/sound_graph.c"
#include "../../components/sound/src/sound_graph_runner.c"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pinmap.h"
#include "storage.h"
#include "sound.h"
#include "settings.h"
#include "motor.h"
#ifdef _WIN32
#include <windows.h>
#endif

/* White-box: expose voice_start / voice_next_sample / voice_fill / mixer state. */
static size_t audio_test_fread(void *ptr, size_t size, size_t count, FILE *file);
static FILE *audio_test_fopen(const char *path, const char *mode);
static int audio_test_fclose(FILE *file);
static int audio_test_fseek(FILE *file, long offset, int origin);
static BaseType_t audio_test_take(SemaphoreHandle_t mutex, TickType_t ticks);
static BaseType_t audio_test_give(SemaphoreHandle_t mutex);
static esp_err_t audio_test_i2s_write(i2s_chan_handle_t channel, const void *data,
                                     size_t size, size_t *written, uint32_t timeout);
static esp_err_t audio_test_gpio_config(const gpio_config_t *cfg);
static esp_err_t audio_test_gpio_level(gpio_num_t pin, uint32_t level);
static esp_err_t audio_test_i2s_disable(i2s_chan_handle_t channel);
static esp_err_t audio_test_i2s_delete(i2s_chan_handle_t channel);
static esp_err_t audio_test_i2s_enable(i2s_chan_handle_t channel);
#define i2s_channel_disable audio_test_i2s_disable
#define i2s_del_channel audio_test_i2s_delete
#define i2s_channel_enable audio_test_i2s_enable
#define fread audio_test_fread
#define fopen audio_test_fopen
#define fclose audio_test_fclose
#define fseek audio_test_fseek
#define xSemaphoreTake audio_test_take
#define xSemaphoreGive audio_test_give
#define i2s_channel_write audio_test_i2s_write
#define gpio_config audio_test_gpio_config
#define gpio_set_level audio_test_gpio_level
#define static
#include "../../components/audio/src/audio.c"
#undef i2s_channel_disable
#undef i2s_del_channel
#undef i2s_channel_enable
#undef fread
#undef fopen
#undef fclose
#undef fseek
#undef xSemaphoreTake
#undef xSemaphoreGive
#undef i2s_channel_write
#undef gpio_config
#undef gpio_set_level
#define TAG sound_test_tag
#define s_inhibited sound_test_inhibited
#include "../../components/sound/src/sound.c"
#undef s_inhibited
#undef TAG
#undef static

#include "../../test_libs/teststubs/stubs.c"

static void (*s_read_interleave)(void);
static unsigned s_request_lock_depth;
static esp_err_t s_gpio_config_err, s_gpio_enable_err;
static uint32_t s_amp_level;
static int s_i2s_delete_calls, s_i2s_disable_calls;
static esp_err_t s_i2s_enable_err;
static BaseType_t audio_test_take(SemaphoreHandle_t mutex, TickType_t ticks)
{
    BaseType_t result = xSemaphoreTake(mutex, ticks);
    if (result == pdTRUE) { ++s_request_lock_depth; }
    return result;
}
static BaseType_t audio_test_give(SemaphoreHandle_t mutex)
{
    TEST_ASSERT_TRUE(s_request_lock_depth > 0);
    --s_request_lock_depth;
    return xSemaphoreGive(mutex);
}
static FILE *audio_test_fopen(const char *path, const char *mode)
{
    TEST_ASSERT_EQUAL(0, s_request_lock_depth);
    return fopen(path, mode);
}
static int audio_test_fclose(FILE *file)
{
    TEST_ASSERT_EQUAL(0, s_request_lock_depth);
    return fclose(file);
}
static int audio_test_fseek(FILE *file, long offset, int origin)
{
    TEST_ASSERT_EQUAL(0, s_request_lock_depth);
    return fseek(file, offset, origin);
}
static esp_err_t audio_test_i2s_write(i2s_chan_handle_t channel, const void *data,
                                     size_t size, size_t *written, uint32_t timeout)
{
    TEST_ASSERT_EQUAL(0, s_request_lock_depth);
    return i2s_channel_write(channel, data, size, written, timeout);
}
static esp_err_t audio_test_gpio_config(const gpio_config_t *cfg)
{
    return s_gpio_config_err != ESP_OK ? s_gpio_config_err : gpio_config(cfg);
}
static esp_err_t audio_test_gpio_level(gpio_num_t pin, uint32_t level)
{
    if (level != 0 && s_gpio_enable_err != ESP_OK) { return s_gpio_enable_err; }
    s_amp_level = level;
    return gpio_set_level(pin, level);
}
static esp_err_t audio_test_i2s_disable(i2s_chan_handle_t channel)
{
    (void)channel;
    ++s_i2s_disable_calls;
    return ESP_OK;
}
static esp_err_t audio_test_i2s_delete(i2s_chan_handle_t channel)
{
    (void)channel;
    ++s_i2s_delete_calls;
    return ESP_OK;
}
static esp_err_t audio_test_i2s_enable(i2s_chan_handle_t channel)
{
    return s_i2s_enable_err != ESP_OK ? s_i2s_enable_err : i2s_channel_enable(channel);
}
static size_t audio_test_fread(void *ptr, size_t size, size_t count, FILE *file)
{
    TEST_ASSERT_EQUAL(0, s_request_lock_depth);
    size_t n = fread(ptr, size, count, file);
    if (s_read_interleave != NULL && s_voice[0].st.active && file == s_voice[0].st.f) {
        void (*hook)(void) = s_read_interleave;
        s_read_interleave = NULL;
        hook();
    }
    return n;
}

static int s_storage_leases;
static bool s_storage_blocked;
esp_err_t storage_access_begin(void)
{
    if (s_storage_blocked) { return ESP_ERR_INVALID_STATE; }
    ++s_storage_leases;
    return ESP_OK;
}
void storage_access_end(void) { --s_storage_leases; }
bool storage_is_mounted(void) { return true; }
const char *storage_get_root(void) { return getenv("TEMP") != NULL ? getenv("TEMP") : "."; }
esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    (void)idx;
    *out = 51;
    return ESP_OK;
}
esp_err_t settings_active_scheme_get(char *out, size_t cap)
{
    (void)cap;
    out[0] = 0;
    return ESP_ERR_NOT_FOUND;
}
esp_err_t settings_active_scheme_set(const char *name) { (void)name; return ESP_OK; }
esp_err_t settings_func_bind_load(func_binding_t *out, size_t *count)
{
    (void)out;
    *count = 0;
    return ESP_OK;
}
void motor_get_applied_speed(uint8_t *speed, bool *forward) { *speed = 0; *forward = true; }

#define TMP_MONO   "dcc_test_mono.wav"
#define TMP_STEREO "dcc_test_stereo.wav"
#define TMP_BAD    "dcc_test_bad.wav"

static char s_path_mono[512];
static char s_path_stereo[512];
static char s_path_bad[512];

static void build_path(char *out, size_t n, const char *name)
{
    const char *dir = getenv("TEMP");
    if (dir == NULL) {
        dir = ".";
    }
    snprintf(out, n, "%s/%s", dir, name);
}

static void put_u16(FILE *f, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    fwrite(b, 1, sizeof(b), f);
}

static void put_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, sizeof(b), f);
}

/* Write a minimal PCM WAV. Each frame's samples are 1000 + 1000*frame for the
 * left channel and +1000 more for the right (so downmixing is observable). */
static void make_wav(const char *path, uint16_t channels, uint32_t rate,
                     uint16_t fmt, uint16_t bits, size_t frames, bool extra_chunk)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        TEST_FAIL_MESSAGE("cannot create temp wav");
        return;
    }
    uint32_t data_bytes = (uint32_t)(frames * channels * (bits / 8U));

    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36U + data_bytes + (extra_chunk ? 12U : 0U));
    fwrite("WAVE", 1, 4, f);

    if (extra_chunk) {
        fwrite("LIST", 1, 4, f);
        put_u32(f, 4);
        put_u32(f, 0);
    }

    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, fmt);
    put_u16(f, channels);
    put_u32(f, rate);
    put_u32(f, rate * channels * (bits / 8U));
    put_u16(f, (uint16_t)(channels * (bits / 8U)));
    put_u16(f, bits);

    fwrite("data", 1, 4, f);
    put_u32(f, data_bytes);
    for (size_t i = 0; i < frames; ++i) {
        for (uint16_t c = 0; c < channels; ++c) {
            if (bits == 16U) {
                put_u16(f, (uint16_t)(1000 + 1000 * (int)i + 1000 * (int)c));
            } else {
                uint8_t b = (uint8_t)(i & 0xFFU);
                fwrite(&b, 1, 1, f);
            }
        }
    }
    fclose(f);
}

/* Constant-amplitude PCM WAV, for envelope/rate tests that need many frames. */
static void make_wav_const(const char *path, uint32_t rate, size_t frames, int16_t value)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) { TEST_FAIL_MESSAGE("cannot create temp wav"); return; }
    uint32_t data_bytes = (uint32_t)(frames * 2U);
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36U + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, 1);
    put_u32(f, rate);
    put_u32(f, rate * 2U);
    put_u16(f, 2);
    put_u16(f, 16);
    fwrite("data", 1, 4, f);
    put_u32(f, data_bytes);
    for (size_t i = 0; i < frames; ++i) {
        put_u16(f, (uint16_t)value);
    }
    fclose(f);
}

void setUp(void)
{
    build_path(s_path_mono, sizeof(s_path_mono), TMP_MONO);
    build_path(s_path_stereo, sizeof(s_path_stereo), TMP_STEREO);
    build_path(s_path_bad, sizeof(s_path_bad), TMP_BAD);
    remove(s_path_mono);
    remove(s_path_stereo);
    remove(s_path_bad);

    memset(s_voice, 0, sizeof(s_voice));
    memset(s_busy, 0, sizeof(s_busy));
    s_initialized = false;
    s_inhibited = false;
    s_tx = NULL;
    s_req_mutex = NULL;
    s_storage_leases = 0;
    s_storage_blocked = false;
    s_mixer_work = false;
    s_validate_work = 0;
    s_read_interleave = NULL;
    s_request_lock_depth = 0;
    s_gpio_config_err = ESP_OK;
    s_gpio_enable_err = ESP_OK;
    s_amp_level = 0;
    s_i2s_delete_calls = 0;
    s_i2s_disable_calls = 0;
    s_i2s_enable_err = ESP_OK;
    s_lock = NULL;
    sound_test_inhibited = false;
    s_control_armed = true;
    mock_mutex_create_fail = 0;
    s_volume = 100;
    /* The anti-click envelope is exercised by dedicated tests; keep the
     * amplitude/step expectations of the legacy tests exact by default. */
    s_fade_blocks = 0;
    mock_i2s_write_count = 0;
    mock_i2s_new_channel_err = 0;
    mock_i2s_init_std_err = 0;
    mock_i2s_enable_err = 0;
    mock_sem_take_fail = 0;
    mock_task_create_ok = 1;
    s_mix_iter_cap = 0;
    TEST_ASSERT_EQUAL(ESP_OK, audio_init());
}

void tearDown(void)
{
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) { voice_close(&s_voice[v].st); }
    remove(s_path_mono);
    remove(s_path_stereo);
    remove(s_path_bad);
}

/* ---- audio_validate_wav ---- */

static void test_validate_wav_accepts_pcm16(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, false);
    TEST_ASSERT_EQUAL(ESP_OK, audio_validate_wav(s_path_mono));

    make_wav(s_path_stereo, 2, 44100, 1, 16, 8, false);
    TEST_ASSERT_EQUAL(ESP_OK, audio_validate_wav(s_path_stereo));
}

static void test_validate_wav_skips_extra_chunks(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, true);
    TEST_ASSERT_EQUAL(ESP_OK, audio_validate_wav(s_path_mono));
}

static void test_validate_wav_rejects_non_pcm16(void)
{
    make_wav(s_path_bad, 1, 22050, 1, 8, 8, false); /* 8-bit */
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));

    make_wav(s_path_bad, 1, 22050, 3, 16, 8, false); /* IEEE float */
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));
}

static void test_validate_wav_rejects_missing(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, audio_validate_wav(s_path_bad));
}

static void test_validate_wav_rejects_non_riff(void)
{
    FILE *f = fopen(s_path_bad, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("NOPEnopeNOPE", 1, 12, f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));
}

/* ---- voice_start parsing ---- */

static void test_voice_start_parses_mono(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, true, 200));
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_EQUAL_UINT16(1, st.channels);
    TEST_ASSERT_EQUAL_UINT32(22050, st.sample_rate);
    TEST_ASSERT_EQUAL_UINT32(8, st.samples_total);
    TEST_ASSERT_TRUE(st.loop);
    TEST_ASSERT_EQUAL_UINT8(100, st.volume); /* clamped from 200 */
    voice_close(&st);
}

static void test_voice_start_parses_stereo(void)
{
    make_wav(s_path_stereo, 2, 44100, 1, 16, 10, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_stereo, false, 50));
    TEST_ASSERT_EQUAL_UINT16(2, st.channels);
    TEST_ASSERT_EQUAL_UINT32(10, st.samples_total);
    TEST_ASSERT_EQUAL_UINT8(50, st.volume);
    voice_close(&st);
}

static void test_voice_start_rejects_unusable(void)
{
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, voice_start(&st, s_path_bad, false, 50));

    /* Too few samples (< 2). */
    make_wav(s_path_bad, 1, 22050, 1, 16, 1, false);
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 50));

    /* Unsupported channel count. */
    make_wav(s_path_bad, 3, 22050, 1, 16, 8, false);
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 50));
}

/* ---- sample reading / mixing ---- */

static void test_voice_next_sample_downmix(void)
{
    make_wav(s_path_stereo, 2, 22050, 1, 16, 6, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_stereo, false, 100));
    /* Frame 0: L=1000, R=2000 -> downmixed to 1500; frame 1 -> 2500. */
    TEST_ASSERT_EQUAL_INT16(1500, st.s_cur);
    TEST_ASSERT_EQUAL_INT16(2500, st.s_next);
    voice_close(&st);
}

static void test_voice_fill_passes_samples(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));

    int16_t mix[2] = { 0, 0 };
    (void)voice_fill(&st, mix, 2);
    TEST_ASSERT_EQUAL_INT16(1000, mix[0]);
    TEST_ASSERT_EQUAL_INT16(2000, mix[1]);
    voice_close(&st);
}

static void test_voice_fill_clips(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));

    int16_t mix[1] = { 32000 };
    (void)voice_fill(&st, mix, 1);
    TEST_ASSERT_EQUAL_INT16(32767, mix[0]); /* 32000 + 1000 saturates */
    voice_close(&st);
}

static void test_voice_fill_applies_volume(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 50));

    int16_t mix[1] = { 0 };
    (void)voice_fill(&st, mix, 1);
    TEST_ASSERT_EQUAL_INT16(500, mix[0]); /* 1000 * (50 * 100 / 100) / 100 */
    voice_close(&st);
}

/* ---- request / volume API ---- */

static void test_voice_play_stop_requests(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play(0, "audio/slot1.wav", true, 50));
    TEST_ASSERT_TRUE(s_voice[0].req_play);
    TEST_ASSERT_FALSE(s_voice[0].req_stop);
    TEST_ASSERT_TRUE(s_voice[0].req_loop);
    TEST_ASSERT_EQUAL_UINT8(50, s_voice[0].req_volume);
    TEST_ASSERT_EQUAL_STRING("audio/slot1.wav", s_voice[0].req_path);

    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_stop(0));
    TEST_ASSERT_FALSE(s_voice[0].req_play);
    TEST_ASSERT_TRUE(s_voice[0].req_stop);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_voice_play(AUDIO_MAX_VOICES, "x", false, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_voice_stop(AUDIO_MAX_VOICES));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_voice_play(0, NULL, false, 1));

    audio_stop_all();
    for (int v = 0; v < AUDIO_MAX_VOICES; ++v) {
        TEST_ASSERT_TRUE(s_voice[v].req_stop);
    }
}

static void test_volume_clamp_and_playing(void)
{
    audio_set_volume(200);
    TEST_ASSERT_EQUAL_UINT8(100, audio_get_volume());
    audio_set_volume(30);
    TEST_ASSERT_EQUAL_UINT8(30, audio_get_volume());

    TEST_ASSERT_FALSE(audio_is_playing());
    s_voice[3].published_active = true;
    TEST_ASSERT_TRUE(audio_is_playing());
    s_voice[3].published_active = false;
    TEST_ASSERT_FALSE(audio_is_playing());
}

/* ---- error / edge paths ---- */

static void test_audio_init_error_paths(void)
{
    s_initialized = false;
    s_tx = NULL;
    mock_mutex_create_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, audio_init());
    TEST_ASSERT_NULL(s_req_mutex);
    mock_mutex_create_fail = 0;
    s_gpio_config_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_init());
    TEST_ASSERT_NULL(s_req_mutex);
    TEST_ASSERT_NULL(s_tx);
    TEST_ASSERT_EQUAL(0, s_amp_level);
    s_gpio_config_err = ESP_OK;
    mock_i2s_new_channel_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_init());
    mock_i2s_new_channel_err = 0;

    mock_i2s_init_std_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_init());
    TEST_ASSERT_NULL(s_req_mutex);
    TEST_ASSERT_NULL(s_tx);
    TEST_ASSERT_EQUAL(1, s_i2s_delete_calls);
    mock_i2s_init_std_err = 0;

    s_i2s_enable_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_init());
    TEST_ASSERT_NULL(s_req_mutex);
    TEST_ASSERT_NULL(s_tx);
    TEST_ASSERT_EQUAL(2, s_i2s_delete_calls);
    s_i2s_enable_err = ESP_OK;

    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, audio_init());
    TEST_ASSERT_NULL(s_req_mutex);
    TEST_ASSERT_NULL(s_tx);
    TEST_ASSERT_EQUAL(3, s_i2s_delete_calls);
    TEST_ASSERT_EQUAL(3, s_i2s_disable_calls);
    TEST_ASSERT_EQUAL(0, s_amp_level);
    mock_task_create_ok = 1;
    s_gpio_enable_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_init());
    TEST_ASSERT_NULL(s_req_mutex);
    TEST_ASSERT_NULL(s_tx);
    TEST_ASSERT_EQUAL(4, s_i2s_delete_calls);
    TEST_ASSERT_EQUAL(4, s_i2s_disable_calls);
    TEST_ASSERT_EQUAL(0, s_amp_level);
    s_gpio_enable_err = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, audio_init()); /* restore */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_init()); /* no duplicate task/resources */
}

static void test_validate_wav_short_and_empty(void)
{
    FILE *f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f); /* shorter than the RIFF header */
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));

    f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 4);
    fwrite("WAVE", 1, 4, f); /* valid header, no chunks */
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));
}

/* Write a WAV whose fmt chunk is 18 bytes (extended), with a data chunk. */
static void make_wav_ext_fmt(const char *path, uint16_t channels, size_t frames)
{
    FILE *f = fopen(path, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, (uint32_t)(38U + frames * channels * 2U));
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 18);
    put_u16(f, 1);
    put_u16(f, channels);
    put_u32(f, 22050);
    put_u32(f, 44100);
    put_u16(f, (uint16_t)(channels * 2));
    put_u16(f, 16);
    put_u16(f, 0); /* one extra fmt byte pair -> 18-byte fmt chunk */
    fwrite("data", 1, 4, f);
    put_u32(f, (uint32_t)(frames * channels * 2));
    for (size_t i = 0; i < frames * channels; ++i) {
        put_u16(f, (uint16_t)(1000 + i * 10));
    }
    fclose(f);
}

static void test_validate_wav_extended_fmt(void)
{
    make_wav_ext_fmt(s_path_bad, 1, 8);
    TEST_ASSERT_EQUAL(ESP_OK, audio_validate_wav(s_path_bad));
}

static void test_voice_start_reopens_and_skips_chunks(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, true); /* LIST before fmt */
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    /* Second start closes the previous FILE first. */
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    voice_close(&st);
}

static void test_voice_start_bad_riff(void)
{
    FILE *f = fopen(s_path_bad, "wb");
    fwrite("NOPEnopeNOPEdata", 1, 16, f);
    fclose(f);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
    TEST_ASSERT_NULL(st.f);
}

static void test_voice_start_extended_fmt(void)
{
    make_wav_ext_fmt(s_path_bad, 1, 8);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_bad, false, 100));
    voice_close(&st);
}

static void test_voice_start_truncated_data(void)
{
    /* Mono: data claims 2 samples but no bytes follow -> first read fails. */
    FILE *f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 100);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, 1);
    put_u32(f, 22050);
    put_u32(f, 44100);
    put_u16(f, 2);
    put_u16(f, 16);
    fwrite("data", 1, 4, f);
    put_u32(f, 4);
    fclose(f);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));

    /* Mono: data claims 4 samples but only one is present -> second read fails. */
    f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 100);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, 1);
    put_u32(f, 22050);
    put_u32(f, 44100);
    put_u16(f, 2);
    put_u16(f, 16);
    fwrite("data", 1, 4, f);
    put_u32(f, 8);
    put_u16(f, 1000);
    fclose(f);
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
    TEST_ASSERT_NULL(st.f);

    /* Stereo: data claims 2 frames but the right channel is missing. */
    f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 100);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, 2);
    put_u32(f, 22050);
    put_u32(f, 88200);
    put_u16(f, 4);
    put_u16(f, 16);
    fwrite("data", 1, 4, f);
    put_u32(f, 8);
    put_u16(f, 1000);
    fclose(f);
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
}

static void test_voice_fill_negative_clip(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    st.s_cur = -30000;
    st.s_next = -30000;
    st.volume = 100;
    s_volume = 100;
    int16_t mix[1] = { -30000 };
    (void)voice_fill(&st, mix, 1);
    TEST_ASSERT_EQUAL_INT16(-32768, mix[0]);
    voice_close(&st);
}

static void test_voice_fill_ends_midway(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 2, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    int16_t mix[64] = { 0 };
    int produced = voice_fill(&st, mix, 64);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_TRUE(produced > 0 && produced < 64);
    TEST_ASSERT_NULL(st.f);
}

static void test_mixer_task_processes_requests(void)
{
    /* Longer than one MIX_BLOCK so the voice is still active after a block. */
    make_wav(s_path_mono, 1, 22050, 1, 16, 5000, false);
    strncpy(s_voice[0].req_path, s_path_mono, sizeof(s_voice[0].req_path) - 1);
    s_voice[0].req_play = true;
    s_voice[0].req_loop = false;
    s_voice[0].req_volume = 100;

    s_mix_iter_cap = 1;
    mixer_task(NULL); /* one iteration: start voice, mix, i2s write */
    TEST_ASSERT_TRUE(s_voice[0].st.active);
    TEST_ASSERT_TRUE(mock_i2s_write_count >= 1);

    s_voice[0].req_stop = true;
    s_mix_iter_cap = 1;
    mixer_task(NULL); /* stop request closes the file */
    TEST_ASSERT_FALSE(s_voice[0].st.active);
    TEST_ASSERT_NULL(s_voice[0].st.f);
}

static void test_mixer_task_releases_voice_on_failure(void)
{
    /* Unusable WAV: voice_start fails, so the allocator slot must be freed
     * instead of leaking a voice forever (REV-A1). */
    make_wav(s_path_bad, 1, 22050, 1, 8, 8, false);
    s_busy[0] = true;
    strncpy(s_voice[0].req_path, s_path_bad, sizeof(s_voice[0].req_path) - 1);
    s_voice[0].req_play = true;
    s_voice[0].req_loop = false;
    s_voice[0].req_volume = 100;
    s_mix_iter_cap = 1;
    mixer_task(NULL);
    TEST_ASSERT_FALSE(s_voice[0].st.active);
    TEST_ASSERT_FALSE(s_busy[0]);
}

static void test_audio_play_stop_wrappers(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, audio_play("audio/slot1.wav"));
    TEST_ASSERT_TRUE(s_voice[0].req_play);
    TEST_ASSERT_EQUAL(ESP_OK, audio_stop());
}

static void test_voice_next_sample_loops(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 2, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, true, 100));
    int16_t out = 0;
    /* samples_left is 0 after the two startup reads -> wraps around. */
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_TRUE(voice_next_sample(&st, &out));
    }
    voice_close(&st);
}

static void test_voice_start_no_data_chunk(void)
{
    FILE *f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 100);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, 1);
    put_u32(f, 22050);
    put_u32(f, 44100);
    put_u16(f, 2);
    put_u16(f, 16);
    fclose(f);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
}

/* A JUNK chunk of odd size is followed by a pad byte; the parser must honour
 * RIFF word alignment instead of desyncing. */
static void make_wav_odd_chunk(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) { TEST_FAIL_MESSAGE("cannot create temp wav"); return; }
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 54);
    fwrite("WAVE", 1, 4, f);
    fwrite("JUNK", 1, 4, f);
    put_u32(f, 5); /* odd size -> one pad byte follows */
    for (int i = 0; i < 6; ++i) { fputc(0, f); } /* 5 data + 1 pad */
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 16);
    put_u16(f, 1);
    put_u16(f, 1);
    put_u32(f, 22050);
    put_u32(f, 44100);
    put_u16(f, 2);
    put_u16(f, 16);
    fwrite("data", 1, 4, f);
    put_u32(f, 4);
    put_u16(f, 1000);
    put_u16(f, 1000);
    fclose(f);
}

static void test_wav_odd_chunk_padding(void)
{
    make_wav_odd_chunk(s_path_bad);
    TEST_ASSERT_EQUAL(ESP_OK, audio_validate_wav(s_path_bad));
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_bad, false, 100));
    voice_close(&st);
}

/* A bogus (implausibly large) chunk size must abort the walk, not fseek
 * backwards through a negative offset. */
static void test_wav_oversized_chunk_rejected(void)
{
    FILE *f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 0);
    fwrite("WAVE", 1, 4, f);
    fwrite("JUNK", 1, 4, f);
    put_u32(f, 0xFFFFFFFFu);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
}

/* voice_start must reject non-PCM16, sample_rate 0 and out-of-range rate. */
static void test_voice_start_rejects_bad_fmt(void)
{
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    make_wav(s_path_bad, 1, 22050, 1, 8, 8, false);
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
    make_wav(s_path_bad, 1, 0, 1, 16, 8, false);
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
    make_wav(s_path_bad, 1, 200000, 1, 16, 8, false);
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
}

/* A one-shot that reaches EOF must close its descriptor. */
static void test_voice_fill_eof_closes_file(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 2, false);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    TEST_ASSERT_NOT_NULL(st.f);
    int16_t mix[64];
    memset(mix, 0, sizeof(mix));
    (void)voice_fill(&st, mix, 64);
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_NULL(st.f);
}

/* An implausibly large fmt chunk size must abort the walk. */
static void test_wav_oversized_fmt_chunk_rejected(void)
{
    FILE *f = fopen(s_path_bad, "wb");
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 0);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put_u32(f, 0xFFFFFFFFu);
    put_u16(f, 1);
    put_u16(f, 1);
    put_u32(f, 22050);
    put_u32(f, 44100);
    put_u16(f, 2);
    put_u16(f, 16);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
}

/* ---- R0: playback rate ---- */

static void test_voice_start_rate_default_and_explicit(void)
{
    make_wav(s_path_mono, 1, 22050, 1, 16, 8, false);

    /* Zero-initialised state (rate unset) falls back to nominal. */
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    TEST_ASSERT_EQUAL_UINT16(1000, st.rate_permille);
    TEST_ASSERT_TRUE(st.rate_cur > 0.99 && st.rate_cur < 1.01);
    voice_close(&st);

    /* Explicit valid rate is honoured on start. */
    memset(&st, 0, sizeof(st));
    st.rate_permille = 2000;
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    TEST_ASSERT_EQUAL_UINT16(2000, st.rate_permille);
    TEST_ASSERT_TRUE(st.rate_cur > 1.99 && st.rate_cur < 2.01);
    voice_close(&st);
}

static void test_voice_set_rate_clips_and_validates(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, audio_voice_set_rate(AUDIO_MAX_VOICES, 1000));

    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_set_rate(2, 100));
    TEST_ASSERT_EQUAL_UINT16(AUDIO_RATE_MIN, s_voice[2].req_rate);

    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_set_rate(2, 9999));
    TEST_ASSERT_EQUAL_UINT16(AUDIO_RATE_MAX, s_voice[2].req_rate);

    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_set_rate(2, 1234));
    TEST_ASSERT_EQUAL_UINT16(1234, s_voice[2].req_rate);
}

static void test_voice_is_active_and_position(void)
{
    TEST_ASSERT_FALSE(audio_voice_is_active(AUDIO_MAX_VOICES));
    TEST_ASSERT_EQUAL_UINT32(0, audio_voice_position(AUDIO_MAX_VOICES));

    s_voice[0].published_active = true;
    s_voice[0].published_position = 42;
    TEST_ASSERT_TRUE(audio_voice_is_active(0));
    TEST_ASSERT_EQUAL_UINT32(42, audio_voice_position(0));

    /* voice_fill counts produced output samples. */
    make_wav_const(s_path_mono, 22050, 64, 1000);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    int16_t mix[10] = { 0 };
    (void)voice_fill(&st, mix, 10);
    TEST_ASSERT_EQUAL_UINT32(10, st.played);
    voice_close(&st);
}

/* ---- R0: voice allocator ---- */

static void test_voice_alloc_release(void)
{
    for (uint8_t i = 0; i < AUDIO_DYNAMIC_VOICES; ++i) {
        TEST_ASSERT_EQUAL_UINT8(i, audio_voice_alloc());
    }
    TEST_ASSERT_EQUAL_UINT8(AUDIO_VOICE_NONE, audio_voice_alloc());

    audio_voice_release(7);
    s_mix_iter_cap = 1;
    mixer_task(NULL);
    TEST_ASSERT_EQUAL_UINT8(7, audio_voice_alloc());

    audio_voice_release(AUDIO_MAX_VOICES); /* invalid: no-op */
    TEST_ASSERT_EQUAL_UINT8(AUDIO_VOICE_NONE, audio_voice_alloc());
}

/* ---- R0: live rate update through the mixer ---- */

static void test_mixer_live_rate_update(void)
{
    make_wav_const(s_path_mono, 22050, 2000, 1000);
    strncpy(s_voice[0].req_path, s_path_mono, sizeof(s_voice[0].req_path) - 1);
    s_voice[0].req_play = true;
    s_voice[0].req_loop = true;
    s_voice[0].req_volume = 100;
    s_voice[0].req_rate = 1000;

    s_mix_iter_cap = 1;
    mixer_task(NULL);
    TEST_ASSERT_TRUE(s_voice[0].st.active);
    TEST_ASSERT_EQUAL_UINT16(1000, s_voice[0].st.rate_permille);

    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_set_rate(0, 2000));
    s_mix_iter_cap = 1;
    mixer_task(NULL);
    TEST_ASSERT_EQUAL_UINT16(2000, s_voice[0].st.rate_permille);
    TEST_ASSERT_TRUE(s_voice[0].st.rate_cur > 1.0); /* smoothing moved towards 2.0 */
}

/* ---- R0: anti-click envelope ---- */

static void test_voice_fade_in_ramps(void)
{
    s_fade_blocks = 2; /* 512-sample ramp at MIX_BLOCK=256 */
    make_wav_const(s_path_mono, 22050, 600, 1000);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    TEST_ASSERT_EQUAL_UINT16(0, st.env);

    int16_t mix[512];
    memset(mix, 0, sizeof(mix));
    (void)voice_fill(&st, mix, 512);
    TEST_ASSERT_TRUE(mix[0] < 10);        /* first samples are faded in */
    TEST_ASSERT_EQUAL_INT16(1000, mix[511]); /* ramp complete after 2 blocks */
    TEST_ASSERT_EQUAL_UINT16(AUDIO_ENV_ONE, st.env);
    voice_close(&st);
}

static void test_voice_fade_out_on_stop(void)
{
    s_fade_blocks = 2;
    make_wav_const(s_path_mono, 22050, 2000, 1000);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));

    int16_t mix[512];
    memset(mix, 0, sizeof(mix));
    (void)voice_fill(&st, mix, 512); /* reach full envelope */
    TEST_ASSERT_EQUAL_UINT16(AUDIO_ENV_ONE, st.env);

    st.stopping = true;
    memset(mix, 0, sizeof(mix));
    (void)voice_fill(&st, mix, 512); /* ramp down and close */
    TEST_ASSERT_FALSE(st.active);
    TEST_ASSERT_NULL(st.f);
}

static void test_voice_fade_step_clamped(void)
{
    s_fade_blocks = 5; /* AUDIO_ENV_ONE/(5*256) == 0 -> clamped to 1 */
    make_wav_const(s_path_mono, 22050, 64, 1000);
    voice_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));

    int16_t mix[1] = { 0 };
    (void)voice_fill(&st, mix, 1);
    TEST_ASSERT_EQUAL_UINT16(1, st.env);
    voice_close(&st);
}

static void test_mixer_stop_fades_when_enabled(void)
{
    s_fade_blocks = 2;
    make_wav_const(s_path_mono, 22050, 2000, 1000);
    strncpy(s_voice[0].req_path, s_path_mono, sizeof(s_voice[0].req_path) - 1);
    s_voice[0].req_play = true;
    s_voice[0].req_loop = true;
    s_voice[0].req_volume = 100;
    s_voice[0].req_rate = 1000;

    s_mix_iter_cap = 1;
    mixer_task(NULL);
    TEST_ASSERT_TRUE(s_voice[0].st.active);

    s_voice[0].req_stop = true;
    s_mix_iter_cap = 1;
    mixer_task(NULL);
    /* Took the fade-out branch (the immediate-close branch resets `stopping`),
     * and closed the file only after the envelope reached silence. */
    TEST_ASSERT_TRUE(s_voice[0].st.stopping);
    TEST_ASSERT_FALSE(s_voice[0].st.active);
    TEST_ASSERT_NULL(s_voice[0].st.f);
}

static void mixer_step(void)
{
    s_mix_iter_cap = 1;
    mixer_task(NULL);
}

static audio_voice_handle_t s_race_handle;
static void replace_during_read(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_owned(&s_race_handle, s_path_stereo, true, 100));
}

static void test_generation_eof_interleave_and_stale_release(void)
{
    make_wav_const(s_path_mono, 22050, 4, 1000);
    make_wav_const(s_path_stereo, 22050, 2000, 2000);
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&s_race_handle));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_owned(&s_race_handle, s_path_mono, false, 100));
    audio_voice_handle_t old = s_race_handle;
    TEST_ASSERT_EQUAL(AUDIO_VOICE_PENDING, audio_voice_get_state(old));
    s_read_interleave = replace_during_read;
    mixer_step(); /* old EOF must not clear replacement's allocator/state */
    TEST_ASSERT_TRUE(s_busy[0]);
    TEST_ASSERT_TRUE(s_voice[0].req_play);
    TEST_ASSERT_EQUAL(AUDIO_VOICE_PENDING, audio_voice_get_state(s_race_handle));
    TEST_ASSERT_EQUAL(AUDIO_VOICE_FINISHED, audio_voice_get_state(old));
    audio_voice_release_owned(old);
    TEST_ASSERT_FALSE(s_voice[0].req_stop);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      audio_voice_play_owned(&old, s_path_mono, false, 100));
    TEST_ASSERT_EQUAL_STRING(s_path_stereo, s_voice[0].req_path);
    mixer_step();
    TEST_ASSERT_EQUAL(AUDIO_VOICE_PLAYING, audio_voice_get_state(s_race_handle));
    TEST_ASSERT_EQUAL(1, s_storage_leases);
    audio_voice_release_owned(s_race_handle);
    audio_voice_handle_t next;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&next));
    TEST_ASSERT_NOT_EQUAL(0, next.voice); /* old FILE still owned by mixer */
    mixer_step();
    TEST_ASSERT_EQUAL(0, s_storage_leases);
    audio_voice_handle_t reused;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&reused));
    TEST_ASSERT_EQUAL(0, reused.voice);
    audio_voice_release_owned(s_race_handle);
    TEST_ASSERT_TRUE(s_busy[reused.voice]);
    TEST_ASSERT_FALSE(s_voice[reused.voice].req_stop);
}

static void test_inhibit_confirmed_quiescence(void)
{
    SemaphoreHandle_t mutex = s_req_mutex;
    s_req_mutex = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_set_inhibited(true));
    TEST_ASSERT_FALSE(audio_is_quiescent());
    s_req_mutex = mutex;
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, audio_set_inhibited(true));
    TEST_ASSERT_FALSE(audio_is_quiescent());
    TEST_ASSERT_FALSE(s_inhibited);
    mock_sem_take_fail = 0;
    make_wav_const(s_path_mono, 22050, 2000, 1000);
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play(18, s_path_mono, true, 100));
    TEST_ASSERT_FALSE(audio_is_quiescent());
    mixer_step();
    TEST_ASSERT_EQUAL(1, s_storage_leases);
    TEST_ASSERT_EQUAL(ESP_OK, audio_set_inhibited(true));
    TEST_ASSERT_FALSE(audio_is_quiescent());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_play(s_path_mono));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_validate_wav(s_path_mono));
    TEST_ASSERT_EQUAL(ESP_OK, audio_inspect_wav(s_path_mono));
    TEST_ASSERT_EQUAL(AUDIO_VOICE_NONE, audio_voice_alloc());
    s_fade_blocks = 255; /* maintenance close must not wait for any fade */
    mixer_step();
    TEST_ASSERT_TRUE(audio_is_quiescent());
    TEST_ASSERT_EQUAL(0, s_storage_leases);
    TEST_ASSERT_EQUAL(ESP_OK, audio_set_inhibited(false));
    mixer_step();
    TEST_ASSERT_FALSE(audio_is_playing());
    s_mixer_work = true;
    TEST_ASSERT_FALSE(audio_is_quiescent());
    s_mixer_work = false;
    s_validate_work = 1;
    TEST_ASSERT_FALSE(audio_is_quiescent());
    s_validate_work = 0;
    s_storage_blocked = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_validate_wav(s_path_mono));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_inspect_wav(s_path_mono));
    TEST_ASSERT_EQUAL(0, s_validate_work);
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

static void test_reserved_voices_survive_fx_saturation(void)
{
    make_wav_const(s_path_mono, 22050, 2000, 1000);
    audio_voice_handle_t engine, secondary;
    TEST_ASSERT_EQUAL(ESP_OK,
                      audio_voice_play_generation(18, s_path_mono, true, 100, &engine));
    TEST_ASSERT_EQUAL(ESP_OK,
                      audio_voice_play_generation(19, s_path_mono, true, 100, &secondary));
    mixer_step();
    audio_voice_handle_t effects[AUDIO_DYNAMIC_VOICES];
    for (size_t i = 0; i < AUDIO_DYNAMIC_VOICES; ++i) {
        TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&effects[i]));
        TEST_ASSERT_EQUAL(i, effects[i].voice);
    }
    audio_voice_handle_t overflow;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, audio_voice_alloc_owned(&overflow));
    TEST_ASSERT_EQUAL(AUDIO_VOICE_NONE, overflow.voice);
    mixer_step();
    TEST_ASSERT_EQUAL(AUDIO_VOICE_PLAYING, audio_voice_get_state(engine));
    TEST_ASSERT_EQUAL(AUDIO_VOICE_PLAYING, audio_voice_get_state(secondary));
    TEST_ASSERT_EQUAL(2, s_storage_leases);
    audio_stop_all();
    mixer_step();
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

static void test_generation_wrap_skips_zero(void)
{
    s_voice[0].generation = UINT32_MAX;
    audio_voice_handle_t owned;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&owned));
    TEST_ASSERT_EQUAL(1, owned.generation);
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_owned(&owned, s_path_bad, false, 100));
    TEST_ASSERT_EQUAL(2, owned.generation);
    mixer_step(); /* Failed open completes the queued generation and frees slot. */
    TEST_ASSERT_EQUAL(AUDIO_VOICE_FINISHED, audio_voice_get_state(owned));
    TEST_ASSERT_FALSE(s_busy[0]);
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

static void test_resampler_accelerated_index_wrap(void)
{
    make_wav_const(s_path_mono, 192000, 64, 1000);
    voice_state_t st = {0};
    st.rate_permille = 3000;
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, true, 100));
    st.cur_idx = UINT32_MAX;
    st.pos = 0.25;
    int16_t mix[8] = {0};
    TEST_ASSERT_EQUAL(8, voice_fill(&st, mix, 8));
    TEST_ASSERT_TRUE(st.active);
    TEST_ASSERT_TRUE(st.pos >= 0 && st.pos < 1);
    TEST_ASSERT_TRUE(st.cur_idx < 256);
    for (size_t i = 0; i < 8; ++i) { TEST_ASSERT_EQUAL(1000, mix[i]); }
    voice_close(&st);
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

static void test_wav_malformed_corpus_common_parser(void)
{
    const struct { long offset; uint32_t value; } cases[] = {
        {4, 3}, {4, 10000}, {4, 20}, {16, 0}, {16, 15}, {16, UINT32_MAX},
        {28, 123}, {32, 0x00100001}, {40, 15}, {40, 18}, {40, 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        make_wav(s_path_bad, 1, 22050, 1, 16, 8, false);
        FILE *f = fopen(s_path_bad, "r+b");
        TEST_ASSERT_NOT_NULL(f);
        fseek(f, cases[i].offset, SEEK_SET);
        put_u32(f, cases[i].value);
        fclose(f);
        TEST_ASSERT_EQUAL(ESP_FAIL, audio_validate_wav(s_path_bad));
        voice_state_t st = {0};
        TEST_ASSERT_EQUAL(ESP_FAIL, voice_start(&st, s_path_bad, false, 100));
        TEST_ASSERT_NULL(st.f);
        TEST_ASSERT_EQUAL(0, s_storage_leases);
    }
}

static void test_generation_scoped_completion_and_rate(void)
{
    make_wav_const(s_path_mono, 22050, 4, 1000);
    audio_voice_handle_t handle;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&handle));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_owned(&handle, s_path_mono, false, 100));
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_NONE, audio_voice_completion(handle));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_set_rate_owned(handle, 1500));
    mixer_step();
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_EOF, audio_voice_completion(handle));
    audio_voice_handle_t next;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&next));
    TEST_ASSERT_EQUAL(handle.voice, next.voice);
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_STALE, audio_voice_completion(handle));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, audio_voice_set_rate_owned(handle, 2000));
    TEST_ASSERT_EQUAL(1500, s_voice[next.voice].req_rate);
    audio_voice_release_owned(next);
    mixer_step();
}

static void test_async_failure_is_not_successful_eof(void)
{
    audio_voice_handle_t handle;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&handle));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_owned(&handle, "no_such_graph_sample_1.wav", false, 100));
    mixer_step();
    TEST_ASSERT_EQUAL(AUDIO_VOICE_FINISHED, audio_voice_get_state(handle));
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_ERROR, audio_voice_completion(handle));
    mixer_step();
    mixer_step();
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_ERROR, audio_voice_completion(handle));
    make_wav_const(s_path_mono, 22050, 2000, 1000);
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_generation(18, s_path_mono, false, 100, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_set_rate_owned(handle, 700));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_stop(handle.voice));
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_STOPPED, audio_voice_completion(handle));
    mixer_step();
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_STOPPED, audio_voice_completion(handle));
    mixer_step();
    TEST_ASSERT_EQUAL(AUDIO_COMPLETION_STOPPED, audio_voice_completion(handle));
}

static void test_reserved_generation_release_stops_only_current_owner(void)
{
    make_wav_const(s_path_mono, 22050, 2000, 1000);
    audio_voice_handle_t first, second;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_generation(18, s_path_mono, true, 100, &first));
    mixer_step();
    TEST_ASSERT_TRUE(audio_voice_is_active(18));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_generation(18, s_path_mono, true, 100, &second));
    audio_voice_release_owned(first);
    TEST_ASSERT_FALSE(s_voice[18].req_stop);
    mixer_step();
    audio_voice_release_owned(second);
    TEST_ASSERT_TRUE(s_voice[18].req_stop);
    s_fade_blocks = 0;
    mixer_step();
    TEST_ASSERT_FALSE(audio_voice_is_active(18));
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_validate_wav_accepts_pcm16);
    RUN_TEST(test_validate_wav_skips_extra_chunks);
    RUN_TEST(test_validate_wav_rejects_non_pcm16);
    RUN_TEST(test_validate_wav_rejects_missing);
    RUN_TEST(test_validate_wav_rejects_non_riff);
    RUN_TEST(test_voice_start_parses_mono);
    RUN_TEST(test_voice_start_parses_stereo);
    RUN_TEST(test_voice_start_rejects_unusable);
    RUN_TEST(test_voice_next_sample_downmix);
    RUN_TEST(test_voice_fill_passes_samples);
    RUN_TEST(test_voice_fill_clips);
    RUN_TEST(test_voice_fill_applies_volume);
    RUN_TEST(test_voice_play_stop_requests);
    RUN_TEST(test_volume_clamp_and_playing);
    RUN_TEST(test_audio_init_error_paths);
    RUN_TEST(test_validate_wav_short_and_empty);
    RUN_TEST(test_validate_wav_extended_fmt);
    RUN_TEST(test_voice_start_reopens_and_skips_chunks);
    RUN_TEST(test_voice_start_bad_riff);
    RUN_TEST(test_voice_start_extended_fmt);
    RUN_TEST(test_voice_start_truncated_data);
    RUN_TEST(test_voice_fill_negative_clip);
    RUN_TEST(test_voice_fill_ends_midway);
    RUN_TEST(test_mixer_task_processes_requests);
    RUN_TEST(test_mixer_task_releases_voice_on_failure);
    RUN_TEST(test_audio_play_stop_wrappers);
    RUN_TEST(test_voice_next_sample_loops);
    RUN_TEST(test_voice_start_no_data_chunk);
    RUN_TEST(test_wav_odd_chunk_padding);
    RUN_TEST(test_wav_oversized_chunk_rejected);
    RUN_TEST(test_wav_oversized_fmt_chunk_rejected);
    RUN_TEST(test_voice_start_rejects_bad_fmt);
    RUN_TEST(test_voice_fill_eof_closes_file);
    RUN_TEST(test_voice_start_rate_default_and_explicit);
    RUN_TEST(test_voice_set_rate_clips_and_validates);
    RUN_TEST(test_voice_is_active_and_position);
    RUN_TEST(test_voice_alloc_release);
    RUN_TEST(test_mixer_live_rate_update);
    RUN_TEST(test_voice_fade_in_ramps);
    RUN_TEST(test_voice_fade_out_on_stop);
    RUN_TEST(test_voice_fade_step_clamped);
    RUN_TEST(test_mixer_stop_fades_when_enabled);
    RUN_TEST(test_generation_eof_interleave_and_stale_release);
    RUN_TEST(test_inhibit_confirmed_quiescence);
    RUN_TEST(test_reserved_voices_survive_fx_saturation);
    RUN_TEST(test_generation_wrap_skips_zero);
    RUN_TEST(test_resampler_accelerated_index_wrap);
    RUN_TEST(test_wav_malformed_corpus_common_parser);
    RUN_TEST(test_generation_scoped_completion_and_rate);
    RUN_TEST(test_async_failure_is_not_successful_eof);
    RUN_TEST(test_reserved_generation_release_stops_only_current_owner);
    return UNITY_END();
}
