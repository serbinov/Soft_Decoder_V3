#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* web_util.c is a pure translation unit: including it (its functions are not
 * static) is enough to exercise it without any ESP-IDF stub. */
#include "../../components/web/src/web_util.c"

void setUp(void)
{
}

void tearDown(void)
{
}

/* ---- parse_query ---- */

static void test_parse_query_basic(void)
{
    char out[32];
    TEST_ASSERT_TRUE(parse_query("a=1&b=2", "b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2", out);
    TEST_ASSERT_TRUE(parse_query("b=2", "b", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2", out);
    TEST_ASSERT_TRUE(parse_query("a=1&b=2&c=3", "c", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("3", out);
}

static void test_parse_query_percent_and_plus(void)
{
    char out[64];
    TEST_ASSERT_TRUE(parse_query("name=hello%20world", "name", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("hello world", out);
    TEST_ASSERT_TRUE(parse_query("q=a+b%2Bc", "q", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a b+c", out);
}

static void test_parse_query_missing_and_prefix(void)
{
    char out[16];
    TEST_ASSERT_FALSE(parse_query("a=1&b=2", "c", out, sizeof(out)));
    /* A key prefix must not match a longer key. */
    TEST_ASSERT_FALSE(parse_query("abc=1", "ab", out, sizeof(out)));
    /* No '=' separator -> not found. */
    TEST_ASSERT_FALSE(parse_query("abc", "abc", out, sizeof(out)));
    TEST_ASSERT_FALSE(parse_query(NULL, "a", out, sizeof(out)));
    TEST_ASSERT_FALSE(parse_query("a=1", NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(parse_query("a=1", "a", NULL, sizeof(out)));
    TEST_ASSERT_FALSE(parse_query("a=1", "a", out, 0));
}

static void test_parse_query_truncates(void)
{
    char out[4];
    TEST_ASSERT_TRUE(parse_query("k=abcdef", "k", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("abc", out);
}

/* ---- parse_u8 / parse_u16 / parse_bool ---- */

static void test_parse_u8(void)
{
    uint8_t v = 0;
    TEST_ASSERT_TRUE(parse_u8("v=0", "v", &v));
    TEST_ASSERT_EQUAL_UINT8(0, v);
    TEST_ASSERT_TRUE(parse_u8("v=255", "v", &v));
    TEST_ASSERT_EQUAL_UINT8(255, v);
    TEST_ASSERT_FALSE(parse_u8("v=256", "v", &v));
    TEST_ASSERT_FALSE(parse_u8("v=12x", "v", &v));
    TEST_ASSERT_FALSE(parse_u8("v=", "v", &v));
    TEST_ASSERT_FALSE(parse_u8("other=1", "v", &v));
}

static void test_parse_u16(void)
{
    uint16_t v = 0;
    TEST_ASSERT_TRUE(parse_u16("p=65535", "p", &v));
    TEST_ASSERT_EQUAL_UINT16(65535, v);
    TEST_ASSERT_FALSE(parse_u16("p=65536", "p", &v));
    TEST_ASSERT_FALSE(parse_u16("p=-1", "p", &v)); /* strtoul wraps to ULONG_MAX */
}

static void test_parse_bool(void)
{
    TEST_ASSERT_TRUE(parse_bool("x=1", "x", false));
    TEST_ASSERT_TRUE(parse_bool("x=true", "x", false));
    TEST_ASSERT_TRUE(parse_bool("x=YES", "x", false));
    TEST_ASSERT_FALSE(parse_bool("x=0", "x", true));
    TEST_ASSERT_FALSE(parse_bool("x=no", "x", true));
    /* Missing key -> default. */
    TEST_ASSERT_TRUE(parse_bool("y=1", "x", true));
    TEST_ASSERT_FALSE(parse_bool("y=1", "x", false));
}

/* ---- json_escape ---- */

static void test_json_escape(void)
{
    char out[64];
    json_escape("a\"b\\c\nd", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\nd", out);
    json_escape("plain", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("plain", out);
}

static void test_json_escape_bounds(void)
{
    char out[64];
    /* out_len too small: nothing more than out_len-1 chars plus NUL. */
    json_escape("abcdef", out, 4);
    TEST_ASSERT_EQUAL_STRING("ab", out);

    json_escape(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);

    /* out_len == 0 is a no-op; a 1-byte buffer only receives the NUL. */
    char tiny[1] = { 'X' };
    json_escape("abc", tiny, 0);
    TEST_ASSERT_EQUAL_CHAR('X', tiny[0]);
    json_escape("abc", tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);
}

/* ---- buf_appendf (regression for the size_t underflow) ---- */

static void test_buf_appendf_normal(void)
{
    char buf[32];
    size_t used = 0;
    buf_appendf(buf, sizeof(buf), &used, "%s", "hello");
    buf_appendf(buf, sizeof(buf), &used, "%s", " world");
    TEST_ASSERT_EQUAL_STRING("hello world", buf);
    TEST_ASSERT_EQUAL_UINT32(11, (uint32_t)used);
}

static void test_buf_appendf_overflow_is_safe(void)
{
    char buf[8];
    size_t used = 0;
    memset(buf, 0, sizeof(buf));
    buf_appendf(buf, sizeof(buf), &used, "%s", "1234567890");
    /* Truncated, NUL-terminated, and `used` never exceeds the capacity. */
    TEST_ASSERT_EQUAL_STRING("1234567", buf);
    TEST_ASSERT_EQUAL_UINT32(sizeof(buf), (uint32_t)used);

    /* Appending again once full must be a no-op (the old code underflowed
     * sizeof(buf) - used and wrote out of bounds). */
    buf_appendf(buf, sizeof(buf), &used, "%s", "X");
    TEST_ASSERT_EQUAL_STRING("1234567", buf);
    TEST_ASSERT_EQUAL_UINT32(sizeof(buf), (uint32_t)used);
}

/* ---- dns_build_response ---- */

static int make_dns_query(uint8_t *buf, const char *name)
{
    memset(buf, 0, 12);
    buf[0] = 0x12;
    buf[1] = 0x34;
    buf[5] = 0x01; /* RD */
    int o = 12;
    const char *p = name;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        buf[o++] = (uint8_t)l;
        memcpy(buf + o, p, l);
        o += (int)l;
        p = dot ? dot + 1 : "";
    }
    buf[o++] = 0x00; /* root label */
    buf[o++] = 0x00; /* QTYPE  A */
    buf[o++] = 0x01;
    buf[o++] = 0x00; /* QCLASS IN */
    buf[o++] = 0x01;
    return o;
}

static void test_dns_build_response_valid(void)
{
    uint8_t q[512];
    uint8_t r[512];
    int qlen = make_dns_query(q, "example.com");
    size_t rlen = dns_build_response(q, qlen, r, (const uint8_t[4]){192, 168, 1, 1});
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(qlen + 16), (uint32_t)rlen);
    TEST_ASSERT_EQUAL_UINT8(0x81, r[2]);
    TEST_ASSERT_EQUAL_UINT8(0x80, r[3]);
    TEST_ASSERT_EQUAL_UINT8(0x00, r[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01, r[7]); /* ANCOUNT */
    TEST_ASSERT_EQUAL_UINT8(0x00, r[9]); /* NSCOUNT */
    /* Answer points at the question name and returns 192.168.1.1. */
    TEST_ASSERT_EQUAL_UINT8(0xC0, r[rlen - 16]);
    TEST_ASSERT_EQUAL_UINT8(0x0C, r[rlen - 15]);
    TEST_ASSERT_EQUAL_UINT8(192, r[rlen - 4]);
    TEST_ASSERT_EQUAL_UINT8(168, r[rlen - 3]);
    TEST_ASSERT_EQUAL_UINT8(1, r[rlen - 2]);
    TEST_ASSERT_EQUAL_UINT8(1, r[rlen - 1]);
}

static void test_dns_build_response_malformed(void)
{
    uint8_t q[32];
    uint8_t r[512];
    memset(q, 0, sizeof(q));
    /* Label length runs past the end, no terminating zero. */
    q[12] = 200;
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)dns_build_response(
                                    q, 14, r, (const uint8_t[4]){192, 168, 1, 1}));
}

/* A near-512-byte query must be rejected instead of overflowing `r`. */
static void test_dns_build_response_too_large(void)
{
    uint8_t q[512];
    uint8_t r[512];
    memset(q, 0, sizeof(q));
    q[5] = 0x01;
    int o = 12;
    for (int k = 0; k < 2; ++k) {
        q[o++] = 240;
        o += 240;
    }
    q[o++] = 0x00;
    q[o++] = 0x00;
    q[o++] = 0x01;
    q[o++] = 0x00;
    q[o++] = 0x01;
    TEST_ASSERT_TRUE(o > 496); /* qend + 16 would exceed DNS_MSG_MAX */
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)dns_build_response(
                                    q, o, r, (const uint8_t[4]){192, 168, 1, 1}));
}

/* ---- url_decode ---- */

static void test_url_decode(void)
{
    char out[32];
    url_decode("a%20b+c", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a b c", out);
    url_decode("a%2B", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a+", out);
    /* Incomplete percent escape is copied verbatim. */
    url_decode("a%zz", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a%zz", out);
    url_decode(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- sanitize_name ---- */

static void test_sanitize_name(void)
{
    char out[64];
    sanitize_name("a/b\\c d!", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a_b_c_d_", out);

    /* Path separators cannot survive (defuses path traversal). */
    sanitize_name("../../etc/passwd", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING(".._.._etc_passwd", out);

    /* Allowed punctuation is kept. */
    sanitize_name("a-b_c.d", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a-b_c.d", out);

    /* UTF-8 continuation/lead bytes (>= 0x80) are preserved (Cyrillic). */
    sanitize_name("\xD0\x9C\xD0\xB0", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("\xD0\x9C\xD0\xB0", out);

    sanitize_name(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ---- function output mapping ---- */

static settings_func_map_t make_map(uint16_t aux_mask, uint8_t dir, uint8_t speed)
{
    settings_func_map_t m;
    memset(&m, 0, sizeof(m));
    m.aux_mask = aux_mask;
    m.dir = dir;
    m.speed = speed;
    return m;
}

static void test_func_desired_headlight_directional(void)
{
    uint16_t light = SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R;
    settings_func_map_t m = make_map(light, SETTINGS_FUNC_DIR_NONE, SETTINGS_FUNC_SPD_NONE);

    TEST_ASSERT_EQUAL_UINT16(SETTINGS_FUNC_OUT_F0F,
                             web_util_func_desired(&m, true, true, 50));
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_FUNC_OUT_F0R,
                             web_util_func_desired(&m, true, false, 50));
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(&m, false, true, 50));

    /* A single light bit is not directional. */
    settings_func_map_t single = make_map(SETTINGS_FUNC_OUT_F0F,
                                          SETTINGS_FUNC_DIR_NONE, SETTINGS_FUNC_SPD_NONE);
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_FUNC_OUT_F0F,
                             web_util_func_desired(&single, true, false, 50));
}

/* Selecting both head lights AND additional AUX outputs must still drive the
 * AUX outputs: only the light bit is directional. */
static void test_func_desired_headlight_keeps_other_aux(void)
{
    const uint16_t other = SETTINGS_FUNC_OUT_AUX1 | SETTINGS_FUNC_OUT_AUX7;
    uint16_t mask = (uint16_t)(SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R | other);
    settings_func_map_t m = make_map(mask, SETTINGS_FUNC_DIR_NONE, SETTINGS_FUNC_SPD_NONE);

    TEST_ASSERT_EQUAL_UINT16((uint16_t)(SETTINGS_FUNC_OUT_F0F | other),
                             web_util_func_desired(&m, true, true, 50));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(SETTINGS_FUNC_OUT_F0R | other),
                             web_util_func_desired(&m, true, false, 50));
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(&m, false, true, 50));
}

static void test_func_desired_plain_mask(void)
{
    uint16_t mask = SETTINGS_FUNC_OUT_AUX1 | SETTINGS_FUNC_OUT_AUX7;
    settings_func_map_t m = make_map(mask, SETTINGS_FUNC_DIR_NONE, SETTINGS_FUNC_SPD_NONE);
    TEST_ASSERT_EQUAL_UINT16(mask, web_util_func_desired(&m, true, true, 40));
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(&m, false, true, 40));
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(NULL, true, true, 40));
}

static void test_func_desired_direction_gate(void)
{
    uint16_t mask = SETTINGS_FUNC_OUT_AUX1;

    settings_func_map_t fwd = make_map(mask, SETTINGS_FUNC_DIR_FWD, SETTINGS_FUNC_SPD_NONE);
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(&fwd, true, false, 40));
    TEST_ASSERT_EQUAL_UINT16(mask, web_util_func_desired(&fwd, true, true, 40));

    settings_func_map_t rev = make_map(mask, SETTINGS_FUNC_DIR_REV, SETTINGS_FUNC_SPD_NONE);
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(&rev, true, true, 40));
    TEST_ASSERT_EQUAL_UINT16(mask, web_util_func_desired(&rev, true, false, 40));
}

static void test_func_desired_speed_gate(void)
{
    uint16_t mask = (uint16_t)(SETTINGS_FUNC_OUT_AUX1 << 1U); /* AUX2 */

    settings_func_map_t moving = make_map(mask, SETTINGS_FUNC_DIR_NONE,
                                          SETTINGS_FUNC_SPD_MOVING);
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(&moving, true, true, 0));
    TEST_ASSERT_EQUAL_UINT16(mask, web_util_func_desired(&moving, true, true, 1));

    settings_func_map_t stop = make_map(mask, SETTINGS_FUNC_DIR_NONE,
                                        SETTINGS_FUNC_SPD_STOP);
    TEST_ASSERT_EQUAL_UINT16(mask, web_util_func_desired(&stop, true, true, 0));
    TEST_ASSERT_EQUAL_UINT16(0, web_util_func_desired(&stop, true, true, 1));
}

/* ---- voice volume categories ---- */

static void test_voice_volume_default_layout(void)
{
    uint8_t cats[SETTINGS_MAX_TRACKS];
    memset(cats, 0, sizeof(cats));
    /* Slot 1 (F1) is the engine; every other slot is an effect. */
    TEST_ASSERT_EQUAL_UINT8(70, web_util_voice_volume(1, NULL, false, 70, 30));
    TEST_ASSERT_EQUAL_UINT8(30, web_util_voice_volume(2, NULL, false, 70, 30));
    TEST_ASSERT_EQUAL_UINT8(30, web_util_voice_volume(0, NULL, false, 70, 30));
}

static void test_voice_volume_categories_override(void)
{
    uint8_t cats[SETTINGS_MAX_TRACKS];
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        cats[i] = SETTINGS_TRACK_CAT_EFFECTS;
    }
    cats[4] = SETTINGS_TRACK_CAT_ENGINE; /* slot 5 */

    TEST_ASSERT_EQUAL_UINT8(70, web_util_voice_volume(5, cats, true, 70, 30));
    TEST_ASSERT_EQUAL_UINT8(30, web_util_voice_volume(1, cats, true, 70, 30));

    /* A NULL pointer falls back to the default layout instead of crashing. */
    TEST_ASSERT_EQUAL_UINT8(70, web_util_voice_volume(1, NULL, true, 70, 30));
}

static void test_ota_container_parse(void)
{
    uint8_t buf[OTA_CONTAINER_HDR_LEN];
    memcpy(buf, OTA_CONTAINER_MAGIC, OTA_CONTAINER_MAGIC_LEN);
    buf[8] = 0x00; buf[9] = 0x00; buf[10] = 0x02; buf[11] = 0x00; /* 131072 */
    buf[12] = 0x03; buf[13] = 0x00; buf[14] = 0x00; buf[15] = 0x00; /* 3 */

    ota_container_hdr_t h;
    TEST_ASSERT_TRUE(ota_container_parse(buf, sizeof(buf), &h));
    TEST_ASSERT_EQUAL_UINT32(131072, h.fw_len);
    TEST_ASSERT_EQUAL_UINT32(3, h.n_files);

    /* too short */
    TEST_ASSERT_FALSE(ota_container_parse(buf, OTA_CONTAINER_HDR_LEN - 1, &h));

    /* bad magic */
    buf[0] = 'X';
    TEST_ASSERT_FALSE(ota_container_parse(buf, sizeof(buf), &h));
    memcpy(buf, OTA_CONTAINER_MAGIC, OTA_CONTAINER_MAGIC_LEN);

    /* zero firmware length is rejected */
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    TEST_ASSERT_FALSE(ota_container_parse(buf, sizeof(buf), &h));
}

static void test_ota_file_hdr_parse(void)
{
    uint8_t buf[OTA_FILE_HDR_LEN];
    buf[0] = 9; buf[1] = 0;   /* name_len */
    buf[2] = 5; buf[3] = 0;   /* label_len */
    buf[4] = 0x40; buf[5] = 0x42; buf[6] = 0x0F; buf[7] = 0x00; /* 1000000 */

    ota_file_hdr_t f;
    TEST_ASSERT_TRUE(ota_file_hdr_parse(buf, sizeof(buf), &f));
    TEST_ASSERT_EQUAL_UINT16(9, f.name_len);
    TEST_ASSERT_EQUAL_UINT16(5, f.label_len);
    TEST_ASSERT_EQUAL_UINT32(1000000, f.data_len);

    TEST_ASSERT_FALSE(ota_file_hdr_parse(buf, OTA_FILE_HDR_LEN - 1, &f));

    buf[0] = 0; /* empty name */
    TEST_ASSERT_FALSE(ota_file_hdr_parse(buf, sizeof(buf), &f));
    buf[0] = (uint8_t)(OTA_FILE_NAME_MAX + 1); /* name too long */
    TEST_ASSERT_FALSE(ota_file_hdr_parse(buf, sizeof(buf), &f));

    buf[0] = 9;
    buf[2] = (uint8_t)(OTA_FILE_LABEL_MAX + 1); /* label too long */
    TEST_ASSERT_FALSE(ota_file_hdr_parse(buf, sizeof(buf), &f));

    buf[2] = 5;
    buf[4] = buf[5] = buf[6] = buf[7] = 0; /* zero data */
    TEST_ASSERT_FALSE(ota_file_hdr_parse(buf, sizeof(buf), &f));
}

/* Defensive guards: early return on bad arguments / missing keys. */
static void test_guards_and_bad_args(void)
{
    uint16_t v16 = 0;
    TEST_ASSERT_FALSE(parse_u16("a=1", "missing", &v16));

    char out[8] = "x";
    url_decode("a", NULL, sizeof(out));    /* output == NULL */
    url_decode("a", out, 0);               /* out_len == 0 */
    sanitize_name("a", NULL, sizeof(out)); /* out == NULL */
    sanitize_name("a", out, 0);            /* out_len == 0 */
    TEST_ASSERT_EQUAL_STRING("x", out);    /* unchanged by the guard paths */
}

/* Fuzz / robustness: random and truncated inputs must never crash. */
static void test_fuzz_random_inputs(void)
{
    uint32_t seed = 0x12345678u;
    for (int iter = 0; iter < 3000; ++iter) {
        char q[64];
        char out[64];
        seed = seed * 1103515245u + 12345u;
        size_t qlen = seed % sizeof(q);
        for (size_t i = 0; i < qlen; ++i) {
            seed = seed * 1103515245u + 12345u;
            q[i] = (char)(seed >> 16);
        }
        q[qlen] = '\0';

        char key[6];
        for (size_t i = 0; i < sizeof(key) - 1; ++i) {
            key[i] = (char)('a' + (seed % 26u));
        }
        key[sizeof(key) - 1] = '\0';

        uint8_t u8 = 0;
        uint16_t u16 = 0;
        (void)parse_query(q, key, out, sizeof(out));
        (void)parse_query(q, key, out, 0);
        (void)parse_u8(q, key, &u8);
        (void)parse_u16(q, key, &u16);
        (void)parse_bool(q, key, true);
        (void)json_escape(q, out, sizeof(out));
        (void)json_escape(q, out, 0);
        (void)url_decode(q, out, sizeof(out));
        (void)sanitize_name(q, out, sizeof(out));

        uint8_t bytes[64];
        size_t blen = seed % sizeof(bytes);
        for (size_t i = 0; i < blen; ++i) {
            seed = seed * 1103515245u + 12345u;
            bytes[i] = (uint8_t)(seed >> 16);
        }
        uint8_t resp[128];
        const uint8_t ip[4] = { 1, 2, 3, 4 };
        (void)dns_build_response(bytes, (int)blen, resp, ip);
        ota_container_hdr_t ch;
        ota_file_hdr_t fh;
        (void)ota_container_parse(bytes, blen, &ch);
        (void)ota_file_hdr_parse(bytes, blen, &fh);
    }
    TEST_ASSERT_TRUE(true);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_query_basic);
    RUN_TEST(test_parse_query_percent_and_plus);
    RUN_TEST(test_parse_query_missing_and_prefix);
    RUN_TEST(test_parse_query_truncates);
    RUN_TEST(test_parse_u8);
    RUN_TEST(test_parse_u16);
    RUN_TEST(test_parse_bool);
    RUN_TEST(test_json_escape);
    RUN_TEST(test_json_escape_bounds);
    RUN_TEST(test_buf_appendf_normal);
    RUN_TEST(test_buf_appendf_overflow_is_safe);
    RUN_TEST(test_dns_build_response_valid);
    RUN_TEST(test_dns_build_response_malformed);
    RUN_TEST(test_dns_build_response_too_large);
    RUN_TEST(test_url_decode);
    RUN_TEST(test_sanitize_name);
    RUN_TEST(test_func_desired_headlight_directional);
    RUN_TEST(test_func_desired_headlight_keeps_other_aux);
    RUN_TEST(test_func_desired_plain_mask);
    RUN_TEST(test_func_desired_direction_gate);
    RUN_TEST(test_func_desired_speed_gate);
    RUN_TEST(test_voice_volume_default_layout);
    RUN_TEST(test_voice_volume_categories_override);
    RUN_TEST(test_ota_container_parse);
    RUN_TEST(test_ota_file_hdr_parse);
    RUN_TEST(test_guards_and_bad_args);
    RUN_TEST(test_fuzz_random_inputs);
    return UNITY_END();
}
