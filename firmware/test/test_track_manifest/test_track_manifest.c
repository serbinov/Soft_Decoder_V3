#include <unity.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
int fsync(int fd); /* MinGW has no fsync(); provided at the bottom */
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif

/* Host directory for the manifest (must be defined before the white-box include
 * of track_manifest.c so MANIFEST_DIR/MANIFEST_PATH point at a writable path). */
#define MANIFEST_DIR "tm_tmp"

/* settings.c and track_manifest.c each declare a static TAG; here `static` is
 * stripped for the white-box build, so give them distinct names. */
#define static
#define TAG settings_TAG
#include "../../components/settings/src/settings.c"
#undef TAG
#undef static

#define static
#define TAG manifest_TAG
#include "../../components/settings/src/track_manifest.c"
#undef TAG
#undef static

#include "../../test_libs/teststubs/stubs.c"

#ifdef _WIN32
int fsync(int fd) { (void)fd; return 0; }
#endif

static void wipe_manifest(void)
{
    (void)remove(MANIFEST_PATH);
}

static void seed_track(settings_track_t *t, uint8_t slot, const char *file,
                       const char *label, bool enabled)
{
    memset(t, 0, sizeof(*t));
    t->slot = slot;
    snprintf(t->file, sizeof(t->file), "%s", file);
    snprintf(t->label, sizeof(t->label), "%s", label);
    t->enabled = enabled;
}

void setUp(void)
{
    mock_nvs_reset();
    s_lock = (SemaphoreHandle_t)1;
    (void)MKDIR(MANIFEST_DIR);
    wipe_manifest();
}

void tearDown(void)
{
    wipe_manifest();
}

/* ---- roundtrip: tracks (slot/file/label/enabled) ---- */

static void test_manifest_roundtrip_tracks(void)
{
    settings_track_t t[2];
    seed_track(&t[0], 1, "audio/slot1.wav", "Гудок", true);
    seed_track(&t[1], 3, "audio/slot3.wav", "Bell", false);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(t, 2));

    FILE *f = fopen(MANIFEST_PATH, "r");
    TEST_ASSERT_NOT_NULL(f);
    char buf[512] = { 0 };
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    TEST_ASSERT_TRUE(rd > 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "AURA-TRACKS"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "Гудок"));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, back[0].slot);
    TEST_ASSERT_EQUAL_STRING("audio/slot1.wav", back[0].file);
    TEST_ASSERT_EQUAL_STRING("Гудок", back[0].label);
    TEST_ASSERT_TRUE(back[0].enabled);
    TEST_ASSERT_EQUAL_UINT8(3, back[1].slot);
    TEST_ASSERT_FALSE(back[1].enabled);
}

/* ---- roundtrip: per-slot categories ---- */

static void test_manifest_roundtrip_categories(void)
{
    /* The category is stored as part of each track record, so exercise two
     * occupied slots (1 and 3). */
    settings_track_t t[2];
    seed_track(&t[0], 1, "audio/slot1.wav", "One", true);
    seed_track(&t[1], 3, "audio/slot3.wav", "Three", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(t, 2));

    uint8_t cats[SETTINGS_MAX_TRACKS];
    memset(cats, 0, sizeof(cats));
    cats[0] = 1; /* slot 1 -> effects */
    cats[2] = 0; /* slot 3 -> engine */
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_save(cats, SETTINGS_MAX_TRACKS));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    uint8_t back[SETTINGS_MAX_TRACKS];
    size_t cn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_load(back, &cn));
    TEST_ASSERT_EQUAL_UINT8(1, back[0]);
    TEST_ASSERT_EQUAL_UINT8(0, back[2]);
}

/* ---- roundtrip: function map ---- */

static void test_manifest_roundtrip_func_map(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "One", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    settings_func_map_t m[SETTINGS_FUNC_MAP_COUNT];
    for (size_t i = 0; i < SETTINGS_FUNC_MAP_COUNT; ++i) {
        m[i].slot_a = (uint8_t)(i + 1U);
        m[i].slot_b = (uint8_t)(i & 0x03U);
        m[i].aux_mask = (uint16_t)(i * 3U);
        m[i].dir = (uint8_t)(i % 3U);
        m[i].speed = (uint8_t)(i % 4U);
    }
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_map_save(m, SETTINGS_FUNC_MAP_COUNT));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_func_map_t back[SETTINGS_FUNC_MAP_COUNT];
    size_t fn = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_func_map_load(back, &fn));
    TEST_ASSERT_EQUAL_UINT32(SETTINGS_FUNC_MAP_COUNT, fn);
    for (size_t i = 0; i < SETTINGS_FUNC_MAP_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT8(m[i].slot_a, back[i].slot_a);
        TEST_ASSERT_EQUAL_UINT8(m[i].slot_b, back[i].slot_b);
        TEST_ASSERT_EQUAL_UINT16(m[i].aux_mask, back[i].aux_mask);
        TEST_ASSERT_EQUAL_UINT8(m[i].dir, back[i].dir);
        TEST_ASSERT_EQUAL_UINT8(m[i].speed, back[i].speed);
    }
}

/* ---- labels with a semicolon are sanitised (no field break) ---- */

static void test_manifest_sanitizes_semicolon_in_label(void)
{
    settings_track_t t;
    seed_track(&t, 2, "audio/slot2.wav", "a;b;c", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_STRING("a b c", back[0].label);
}

/* ---- UTF-8 label survives byte-for-byte ---- */

static void test_manifest_utf8_label_roundtrip(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "Перед-Задний", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_STRING("Перед-Задний", back[0].label);
}

/* ---- long label is truncated safely, no crash ---- */

static void test_manifest_long_label_truncated(void)
{
    settings_track_t t;
    char big[200];
    memset(big, 'X', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    seed_track(&t, 1, "audio/slot1.wav", big, true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT(strlen(t.label), strlen(back[0].label));
}

/* ---- error paths ---- */

static void test_manifest_absent_returns_not_found(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

static void test_manifest_header_only_returns_not_found(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("AURA-TRACKS 1\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

static void test_manifest_without_magic_returns_not_found(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("T;1;0;1;audio/slot1.wav;One\n", f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

static void test_manifest_bad_track_lines_are_skipped(void)
{
    FILE *f = fopen(MANIFEST_PATH, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs("AURA-TRACKS 1\n", f);
    fputs("garbage line\n", f);
    fputs("T;zzz;0;1;audio/x.wav;Bad\n", f); /* invalid slot -> skipped */
    fputs("T;0;0;1;audio/y.wav;Zero\n", f);  /* slot 0 -> skipped */
    fputs("F;999;1;2;3;4;5\n", f);           /* idx out of range -> skipped */
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, settings_manifest_load());
}

/* ---- sync is a no-op (no crash) when storage is not mounted ---- */

static void test_manifest_sync_without_dir_no_crash(void)
{
    settings_track_t t;
    seed_track(&t, 1, "audio/slot1.wav", "One", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    wipe_manifest();
    (void)RMDIR(MANIFEST_DIR); /* manifest dir gone: fopen must simply fail */

    settings_manifest_sync(); /* must not crash */

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
}

/* A track whose slot is beyond the stored categories must fall back to the
 * default category (no out-of-range read). */
static void test_manifest_track_slot_without_category(void)
{
    uint8_t cats[5] = { 1, 0, 1, 0, 1 };
    TEST_ASSERT_EQUAL(ESP_OK, settings_track_cats_save(cats, 5));

    settings_track_t t;
    seed_track(&t, 10, "audio/slot10.wav", "Ten", true);
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_save(&t, 1));

    mock_nvs_reset();
    TEST_ASSERT_EQUAL(ESP_OK, settings_manifest_load());

    settings_track_t back[SETTINGS_MAX_TRACKS];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, settings_tracks_load(back, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT8(10, back[0].slot);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_manifest_roundtrip_tracks);
    RUN_TEST(test_manifest_roundtrip_categories);
    RUN_TEST(test_manifest_roundtrip_func_map);
    RUN_TEST(test_manifest_sanitizes_semicolon_in_label);
    RUN_TEST(test_manifest_utf8_label_roundtrip);
    RUN_TEST(test_manifest_long_label_truncated);
    RUN_TEST(test_manifest_absent_returns_not_found);
    RUN_TEST(test_manifest_header_only_returns_not_found);
    RUN_TEST(test_manifest_without_magic_returns_not_found);
    RUN_TEST(test_manifest_bad_track_lines_are_skipped);
    RUN_TEST(test_manifest_sync_without_dir_no_crash);
    RUN_TEST(test_manifest_track_slot_without_category);
    return UNITY_END();
}
