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

void setUp(void)
{
    build_path(s_path_mono, sizeof(s_path_mono), TMP_MONO);
    build_path(s_path_stereo, sizeof(s_path_stereo), TMP_STEREO);
    build_path(s_path_bad, sizeof(s_path_bad), TMP_BAD);
    remove(s_path_mono);
    remove(s_path_stereo);
    remove(s_path_bad);

    memset(s_voice, 0, sizeof(s_voice));
    s_volume = 100;
    mock_i2s_write_count = 0;
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
    return UNITY_END();
}
