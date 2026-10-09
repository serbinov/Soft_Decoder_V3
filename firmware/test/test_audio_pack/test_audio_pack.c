#include <unity.h>

#include <stdint.h>
#include <string.h>

#include "../../components/web/src/audio_pack.c"

void setUp(void) {}
void tearDown(void) {}

/* Build a tiny pack in memory: header + two entries + payloads. */
static size_t build_pack(uint8_t *out, size_t cap, bool bad_magic, bool bad_entry)
{
    const char *n0 = "horn";
    const char *n1 = "bell";
    uint32_t d0 = 8, d1 = 4;
    uint32_t index_len = (uint32_t)(16 + strlen(n0)) + (uint32_t)(16 + strlen(n1));
    uint32_t data_off = 16U + index_len;
    uint32_t off0 = data_off;
    uint32_t off1 = data_off + d0;

    size_t w = 0;
    memcpy(out + w, bad_magic ? "BADMAGIC" : AUDIO_PACK_MAGIC, 8); w += 8;
    out[w++] = AUDIO_PACK_VERSION & 0xFF; out[w++] = 0;
    out[w++] = 2; out[w++] = 0; /* count */
    out[w++] = data_off & 0xFF; out[w++] = (data_off >> 8) & 0xFF;
    out[w++] = (data_off >> 16) & 0xFF; out[w++] = (data_off >> 24) & 0xFF;

    uint8_t e0[16] = { (uint8_t)strlen(n0), 70, 1, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    e0[4] = 22050 & 0xFF; e0[5] = (22050 >> 8) & 0xFF; e0[6] = (22050 >> 16) & 0xFF; e0[7] = 0;
    e0[8] = off0 & 0xFF; e0[9] = (off0 >> 8) & 0xFF; e0[10] = (off0 >> 16) & 0xFF; e0[11] = 0;
    e0[12] = d0 & 0xFF; e0[13] = 0; e0[14] = 0; e0[15] = 0;
    if (bad_entry) e0[2] = 3; /* channels = 3 invalid */
    memcpy(out + w, e0, 16); w += 16;
    memcpy(out + w, n0, strlen(n0)); w += strlen(n0);

    uint8_t e1[16] = { (uint8_t)strlen(n1), 100, 1, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    e1[4] = 11025 & 0xFF; e1[5] = (11025 >> 8) & 0xFF; e1[6] = 0; e1[7] = 0;
    e1[8] = off1 & 0xFF; e1[9] = (off1 >> 8) & 0xFF; e1[10] = 0; e1[11] = 0;
    e1[12] = d1 & 0xFF; e1[13] = 0; e1[14] = 0; e1[15] = 0;
    memcpy(out + w, e1, 16); w += 16;
    memcpy(out + w, n1, strlen(n1)); w += strlen(n1);

    for (uint32_t i = 0; i < d0 + d1; ++i) out[w++] = (uint8_t)i;
    TEST_ASSERT_TRUE(w <= cap);
    return w;
}

static void test_header_ok(void)
{
    uint8_t buf[256];
    size_t len = build_pack(buf, sizeof(buf), false, false);
    audio_pack_header_t h;
    TEST_ASSERT_TRUE(audio_pack_header_parse(buf, len, &h));
    TEST_ASSERT_EQUAL_UINT32(2, h.count);
    TEST_ASSERT_EQUAL_UINT32(16U + 2U * 16U + 4U + 4U, h.data_off);
}

static void test_header_rejects(void)
{
    uint8_t buf[256];
    size_t len = build_pack(buf, sizeof(buf), false, false);
    audio_pack_header_t h;
    TEST_ASSERT_FALSE(audio_pack_header_parse(buf, 15, &h)); /* too short */
    TEST_ASSERT_FALSE(audio_pack_header_parse(NULL, len, &h));

    uint8_t bad[16];
    memcpy(bad, buf, 16);
    memcpy(bad, "BADMAGIC", 8);
    TEST_ASSERT_FALSE(audio_pack_header_parse(bad, 16, &h));
    memcpy(bad, buf, 16);
    bad[8] = 9; /* version */
    TEST_ASSERT_FALSE(audio_pack_header_parse(bad, 16, &h));
    memcpy(bad, buf, 16);
    bad[10] = 0; bad[11] = 0; /* count 0 */
    TEST_ASSERT_FALSE(audio_pack_header_parse(bad, 16, &h));
    memcpy(bad, buf, 16);
    bad[12] = 0; bad[13] = 0; bad[14] = 0; bad[15] = 0; /* data_off too small */
    TEST_ASSERT_FALSE(audio_pack_header_parse(bad, 16, &h));
}

static void test_entry_roundtrip(void)
{
    uint8_t buf[256];
    size_t len = build_pack(buf, sizeof(buf), false, false);
    audio_pack_header_t h;
    TEST_ASSERT_TRUE(audio_pack_header_parse(buf, len, &h));

    size_t pos = AUDIO_PACK_HDR_LEN;
    audio_pack_entry_t e;
    uint8_t nlen = 0;
    TEST_ASSERT_TRUE(audio_pack_entry_head(buf + pos, len, &e, &nlen));
    pos += 16;
    TEST_ASSERT_EQUAL_UINT8(4, nlen);
    TEST_ASSERT_TRUE(audio_pack_entry_name(&e, buf + pos, nlen));
    pos += nlen;
    TEST_ASSERT_EQUAL_STRING("horn", e.name);
    TEST_ASSERT_EQUAL_UINT8(70, e.volume);
    TEST_ASSERT_EQUAL_UINT8(1, e.channels);
    TEST_ASSERT_EQUAL_UINT32(22050, e.sample_rate);
    TEST_ASSERT_EQUAL_UINT32(h.data_off, e.data_off);
    TEST_ASSERT_EQUAL_UINT32(8, e.data_len);

    TEST_ASSERT_TRUE(audio_pack_entry_head(buf + pos, len, &e, &nlen));
    pos += 16;
    TEST_ASSERT_TRUE(audio_pack_entry_name(&e, buf + pos, nlen));
    pos += nlen;
    TEST_ASSERT_EQUAL_STRING("bell", e.name);
    TEST_ASSERT_EQUAL_UINT32(11025, e.sample_rate);
    TEST_ASSERT_EQUAL_UINT32(h.data_off + 8U, e.data_off);
}

static void test_entry_bounds(void)
{
    uint8_t buf[256];
    size_t len = build_pack(buf, sizeof(buf), false, false);
    audio_pack_entry_t e;
    uint8_t nlen = 0;
    /* chunk_end before the payload end */
    TEST_ASSERT_FALSE(audio_pack_entry_head(buf + 16, 16U + 16U + 4U + 8U + 4U - 1U, &e, &nlen));
    /* invalid channels */
    uint8_t eh[16];
    memcpy(eh, buf + 16, 16);
    eh[2] = 3;
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, len, &e, &nlen));
    memcpy(eh, buf + 16, 16);
    eh[3] = 8; /* bits */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, len, &e, &nlen));
    memcpy(eh, buf + 16, 16);
    eh[0] = 0; /* name_len */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, len, &e, &nlen));
    memcpy(eh, buf + 16, 16);
    eh[0] = 64; /* name too long */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, len, &e, &nlen));
    memcpy(eh, buf + 16, 16);
    eh[1] = 101; /* volume */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, len, &e, &nlen));
    memcpy(eh, buf + 16, 16);
    eh[12] = 3; eh[13] = 0; eh[14] = 0; eh[15] = 0; /* odd data_len for mono */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, len, &e, &nlen));
    memcpy(eh, buf + 16, 16);
    memset(eh + 4, 0, 4); /* sample_rate = 0 */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, len, &e, &nlen));
}

static void test_entry_adpcm(void)
{
    uint8_t eh[16] = { 4, 100, 1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0 };
    eh[4] = 22050 & 0xFF; eh[5] = (22050 >> 8) & 0xFF;
    audio_pack_entry_t e;
    uint8_t nlen = 0;
    TEST_ASSERT_TRUE(audio_pack_entry_head(eh, 1000000, &e, &nlen));
    TEST_ASSERT_EQUAL_UINT8(4, e.bits);
    TEST_ASSERT_EQUAL_UINT32(256, e.data_len);
    eh[2] = 2; /* ADPCM must be mono */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, 1000000, &e, &nlen));
    eh[2] = 1; eh[12] = 1; eh[13] = 0; /* data_len = 1, not a whole block */
    TEST_ASSERT_FALSE(audio_pack_entry_head(eh, 1000000, &e, &nlen));
}

static void test_entry_name_rejects(void)
{
    audio_pack_entry_t e;
    TEST_ASSERT_TRUE(audio_pack_entry_name(&e, (const uint8_t *)"ok", 2));
    TEST_ASSERT_EQUAL_STRING("ok", e.name);
    TEST_ASSERT_FALSE(audio_pack_entry_name(&e, (const uint8_t *)"a/b", 3));
    TEST_ASSERT_FALSE(audio_pack_entry_name(&e, (const uint8_t *)"a\\b", 3));
    TEST_ASSERT_FALSE(audio_pack_entry_name(&e, (const uint8_t *)"a\0b", 3));
    TEST_ASSERT_FALSE(audio_pack_entry_name(&e, (const uint8_t *)".", 1));
    TEST_ASSERT_FALSE(audio_pack_entry_name(&e, (const uint8_t *)"..", 2));
    TEST_ASSERT_FALSE(audio_pack_entry_name(&e, NULL, 2));
    TEST_ASSERT_FALSE(audio_pack_entry_name(&e, (const uint8_t *)"x", 0));
}

static void test_wav_header(void)
{
    uint8_t hdr[AUDIO_PACK_WAV_HDR_MAX];
    TEST_ASSERT_EQUAL_UINT32(AUDIO_PACK_WAV_HDR_LEN,
        audio_pack_wav_header(hdr, sizeof(hdr), 200, 22050, 1, 16));
    TEST_ASSERT_EQUAL_MEMORY("RIFF", hdr, 4);
    TEST_ASSERT_EQUAL_MEMORY("WAVE", hdr + 8, 4);
    TEST_ASSERT_EQUAL_MEMORY("fmt ", hdr + 12, 4);
    TEST_ASSERT_EQUAL_MEMORY("data", hdr + 36, 4);
    /* riff size = 36 + data, data size = 200 */
    TEST_ASSERT_EQUAL_UINT8(36 + 200, hdr[4]);
    TEST_ASSERT_EQUAL_UINT8(200, hdr[40]);
    TEST_ASSERT_EQUAL_UINT8(1, hdr[22]); /* channels */
    TEST_ASSERT_EQUAL_UINT8(22050 & 0xFF, hdr[24]);
    TEST_ASSERT_EQUAL_UINT8(16, hdr[34]);
    /* stereo header doubles the block align */
    TEST_ASSERT_EQUAL_UINT32(AUDIO_PACK_WAV_HDR_LEN,
        audio_pack_wav_header(hdr, sizeof(hdr), 400, 8000, 2, 16));
    TEST_ASSERT_EQUAL_UINT8(4, hdr[32]);
    TEST_ASSERT_EQUAL_UINT8(2, hdr[22]);
}

static void test_wav_header_adpcm(void)
{
    uint8_t hdr[AUDIO_PACK_WAV_HDR_MAX];
    /* 2 blocks of 256 bytes -> 1010 samples. */
    TEST_ASSERT_EQUAL_UINT32(AUDIO_PACK_WAV_HDR_MAX,
        audio_pack_wav_header(hdr, sizeof(hdr), 512, 22050, 1, 4));
    TEST_ASSERT_EQUAL_MEMORY("RIFF", hdr, 4);
    TEST_ASSERT_EQUAL_MEMORY("fmt ", hdr + 12, 4);
    TEST_ASSERT_EQUAL_MEMORY("fact", hdr + 40, 4);
    TEST_ASSERT_EQUAL_MEMORY("data", hdr + 52, 4);
    TEST_ASSERT_EQUAL_UINT8(0x11, hdr[20]); /* WAVE_FORMAT_IMA_ADPCM */
    TEST_ASSERT_EQUAL_UINT8(1, hdr[22]);
    TEST_ASSERT_EQUAL_UINT8(4, hdr[34]);    /* bits */
    TEST_ASSERT_EQUAL_UINT8(0x00, hdr[32]); /* block_align = 256 */
    TEST_ASSERT_EQUAL_UINT8(0x01, hdr[33]);
    TEST_ASSERT_EQUAL_UINT8(505 & 0xFF, hdr[38]);
    TEST_ASSERT_EQUAL_UINT8(512, hdr[56]);
    /* a single channel is required */
    TEST_ASSERT_EQUAL_UINT32(0, audio_pack_wav_header(hdr, sizeof(hdr), 512, 22050, 2, 4));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_ok);
    RUN_TEST(test_header_rejects);
    RUN_TEST(test_entry_roundtrip);
    RUN_TEST(test_entry_bounds);
    RUN_TEST(test_entry_adpcm);
    RUN_TEST(test_entry_name_rejects);
    RUN_TEST(test_wav_header);
    RUN_TEST(test_wav_header_adpcm);
    return UNITY_END();
}
