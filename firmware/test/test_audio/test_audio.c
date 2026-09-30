#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pinmap.h"

/* White-box: expose voice_start / voice_next_sample / voice_fill / mixer state. */
#define static
#include "../../components/audio/src/audio.c"
#undef static

#include "../../test_libs/teststubs/stubs.c"

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
    s_volume = 100;
    /* The anti-click envelope is exercised by dedicated tests; keep the
     * amplitude/step expectations of the legacy tests exact by default. */
    s_fade_blocks = 0;
    mock_i2s_write_count = 0;
    mock_i2s_new_channel_err = 0;
    mock_i2s_init_std_err = 0;
    mock_task_create_ok = 1;
    s_mix_iter_cap = 0;
    TEST_ASSERT_EQUAL(ESP_OK, audio_init());
}

void tearDown(void)
{
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
    fclose(st.f);
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
    fclose(st.f);
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
    fclose(st.f);
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
    fclose(st.f);
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
    fclose(st.f);
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
    fclose(st.f);
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
    s_voice[3].st.active = true;
    TEST_ASSERT_TRUE(audio_is_playing());
    s_voice[3].st.active = false;
    TEST_ASSERT_FALSE(audio_is_playing());
}

/* ---- error / edge paths ---- */

static void test_audio_init_error_paths(void)
{
    mock_i2s_new_channel_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_init());
    mock_i2s_new_channel_err = 0;

    mock_i2s_init_std_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, audio_init());
    mock_i2s_init_std_err = 0;

    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, audio_init());
    mock_task_create_ok = 1;
    TEST_ASSERT_EQUAL(ESP_OK, audio_init()); /* restore */
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
    put_u32(f, 100);
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
    fclose(st.f);
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
    fclose(st.f);
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
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_bad, false, 100));
    TEST_ASSERT_EQUAL_INT16(1000, st.s_next); /* fell back to s_cur */
    fclose(st.f);

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
    fclose(st.f);
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
    fclose(st.f);
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
    fclose(st.f);
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
    put_u32(f, 0);
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
    if (st.f != NULL) { fclose(st.f); st.f = NULL; }
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
    fclose(st.f);

    /* Explicit valid rate is honoured on start. */
    memset(&st, 0, sizeof(st));
    st.rate_permille = 2000;
    TEST_ASSERT_EQUAL(ESP_OK, voice_start(&st, s_path_mono, false, 100));
    TEST_ASSERT_EQUAL_UINT16(2000, st.rate_permille);
    TEST_ASSERT_TRUE(st.rate_cur > 1.99 && st.rate_cur < 2.01);
    fclose(st.f);
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

    s_voice[0].st.active = true;
    s_voice[0].st.played = 42;
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
    fclose(st.f);
}

/* ---- R0: voice allocator ---- */

static void test_voice_alloc_release(void)
{
    for (uint8_t i = 0; i < AUDIO_MAX_VOICES; ++i) {
        TEST_ASSERT_EQUAL_UINT8(i, audio_voice_alloc());
    }
    TEST_ASSERT_EQUAL_UINT8(AUDIO_VOICE_NONE, audio_voice_alloc());

    audio_voice_release(7);
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
    fclose(st.f);
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
    (void)voice_fill(&st, mix, 520); /* ramp down and close */
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
    fclose(st.f);
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
    return UNITY_END();
}
