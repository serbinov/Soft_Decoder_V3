#include <unity.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

/* White-box include of the binary scheme store (CRC + header validation) and
 * the sound engine core, with the host ESP-IDF stubs. */
static int sound_test_rename(const char *from, const char *to);
#define rename sound_test_rename
#define static
#include "../../components/sound/src/sound_store.c"
#undef rename
#include "../../components/sound/src/sound.c"
#undef static

#include "../../test_libs/teststubs/stubs.c"

static bool s_rename_fail;
static int sound_test_rename(const char *from, const char *to)
{
    if (s_rename_fail) { errno = EIO; return -1; }
#if defined(_WIN32) && defined(ESP_PLATFORM)
    /* Exercise the target error branch with target-like atomic replacement,
     * rather than the Windows CRT's refusal to replace an existing file. */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

/* Minimal storage stubs: the sound component only needs the mount flag/root. */
static bool s_mounted = false;
static char s_root[512];

bool storage_is_mounted(void) { return s_mounted; }
const char *storage_get_root(void) { return s_root; }
static int s_storage_leases;
static bool s_storage_blocked;
esp_err_t storage_access_begin(void)
{
    if (s_storage_blocked) { return ESP_ERR_INVALID_STATE; }
    ++s_storage_leases;
    return ESP_OK;
}
void storage_access_end(void) { --s_storage_leases; }

/* ---- audio stubs (engine voice 18 + effect voices) ---- */
static bool s_av_active[AUDIO_MAX_VOICES];
static bool s_av_busy[AUDIO_MAX_VOICES];
static uint32_t s_av_generation[AUDIO_MAX_VOICES];
static bool s_av_pending[AUDIO_MAX_VOICES];
static uint16_t s_av_rate[AUDIO_MAX_VOICES];
static char s_av_path[AUDIO_MAX_VOICES][192];
static int s_av_play_calls;
static int s_av_stop_calls;

esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume)
{
    (void)loop;
    (void)volume;
    if (voice >= AUDIO_MAX_VOICES || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_av_active[voice] = false;
    s_av_pending[voice] = true;
    ++s_av_generation[voice];
    s_av_play_calls++;
    snprintf(s_av_path[voice], sizeof(s_av_path[voice]), "%s", path);
    return ESP_OK;
}

esp_err_t audio_voice_stop(uint8_t voice)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return ESP_ERR_INVALID_ARG;
    }
    s_av_active[voice] = false;
    s_av_pending[voice] = false;
    s_av_stop_calls++;
    return ESP_OK;
}

bool audio_voice_is_active(uint8_t voice)
{
    /* Legacy assertions test request existence, not the sequencer contract. */
    return voice < AUDIO_MAX_VOICES && (s_av_active[voice] || s_av_pending[voice]);
}

esp_err_t audio_voice_play_generation(uint8_t voice, const char *path, bool loop,
                                      uint8_t volume, audio_voice_handle_t *out)
{
    esp_err_t err = audio_voice_play(voice, path, loop, volume);
    if (err == ESP_OK && out != NULL) {
        *out = (audio_voice_handle_t){voice, s_av_generation[voice]};
    }
    return err;
}

audio_voice_state_t audio_voice_get_state(audio_voice_handle_t h)
{
    if (h.voice >= AUDIO_MAX_VOICES || h.generation != s_av_generation[h.voice]) {
        return AUDIO_VOICE_FINISHED;
    }
    if (s_av_pending[h.voice]) { return AUDIO_VOICE_PENDING; }
    return s_av_active[h.voice] ? AUDIO_VOICE_PLAYING : AUDIO_VOICE_FINISHED;
}

static void mock_audio_complete(uint8_t voice)
{
    s_av_pending[voice] = false;
    s_av_active[voice] = false;
    s_av_busy[voice] = false;
}

uint32_t audio_voice_position(uint8_t voice)
{
    (void)voice;
    return 0U;
}

esp_err_t audio_voice_set_rate(uint8_t voice, uint16_t permille)
{
    if (voice >= AUDIO_MAX_VOICES) {
        return ESP_ERR_INVALID_ARG;
    }
    s_av_rate[voice] = permille;
    return ESP_OK;
}

uint8_t audio_voice_alloc(void)
{
    for (int i = 0; i < AUDIO_DYNAMIC_VOICES; ++i) {
        if (!s_av_busy[i]) {
            s_av_busy[i] = true;
            ++s_av_generation[i];
            return (uint8_t)i;
        }
    }
    return AUDIO_VOICE_NONE;
}

esp_err_t audio_voice_alloc_owned(audio_voice_handle_t *out)
{
    uint8_t v = audio_voice_alloc();
    if (v == AUDIO_VOICE_NONE) { return ESP_ERR_NO_MEM; }
    *out = (audio_voice_handle_t){v, s_av_generation[v]};
    return ESP_OK;
}

esp_err_t audio_voice_play_owned(audio_voice_handle_t *h, const char *path,
                                bool loop, uint8_t volume)
{
    if (h->voice >= AUDIO_DYNAMIC_VOICES || !s_av_busy[h->voice] ||
        h->generation != s_av_generation[h->voice]) { return ESP_ERR_INVALID_STATE; }
    return audio_voice_play_generation(h->voice, path, loop, volume, h);
}

void audio_voice_release_owned(audio_voice_handle_t h)
{
    if (h.voice < AUDIO_DYNAMIC_VOICES && h.generation == s_av_generation[h.voice]) {
        (void)audio_voice_stop(h.voice);
        s_av_busy[h.voice] = false;
        ++s_av_generation[h.voice];
    }
}

void audio_voice_release(uint8_t voice)
{
    if (voice < AUDIO_MAX_VOICES) {
        s_av_busy[voice] = false;
    }
}

/* ---- motor stub (applied speed polled by the task) ---- */
static uint8_t s_mock_applied_speed;
static bool s_mock_applied_fwd = true;

void motor_get_applied_speed(uint8_t *out_speed128, bool *out_forward)
{
    if (out_speed128 != NULL) {
        *out_speed128 = s_mock_applied_speed;
    }
    if (out_forward != NULL) {
        *out_forward = s_mock_applied_fwd;
    }
}

/* ---- settings stubs ---- */
static func_binding_t s_test_binds[FUNC_BIND_MAX];
static size_t s_test_bind_count;
static int s_test_bind_ret = ESP_ERR_NOT_FOUND;
static char s_test_active[64];

esp_err_t settings_func_bind_load(func_binding_t *b, size_t *c)
{
    if (b != NULL && c != NULL) {
        memcpy(b, s_test_binds, sizeof(s_test_binds));
        *c = s_test_bind_count;
    }
    return (esp_err_t)s_test_bind_ret;
}

esp_err_t settings_active_scheme_get(char *out, size_t cap)
{
    if (out == NULL || cap == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(out, cap, "%s", s_test_active);
    return s_test_active[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t settings_active_scheme_set(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_test_active, sizeof(s_test_active), "%s", name);
    return ESP_OK;
}

static uint8_t s_test_cv[SETTINGS_CV_COUNT + 1];
static uint16_t s_test_cv_fail_idx;

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (out == NULL || idx < 1U || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (idx == s_test_cv_fail_idx) {
        return ESP_FAIL;
    }
    *out = s_test_cv[idx];
    return ESP_OK;
}

static char s_dir[512];
static char s_file[512];

static void build_paths(void)
{
    const char *t = getenv("TEMP");
    if (t == NULL) {
        t = ".";
    }
    /* Separate host/ESP-branch runs (and other host runners) must not share files. */
#ifdef _WIN32
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    snprintf(s_dir, sizeof(s_dir), "%s/dcc_test_sound_%lu", t, pid);
    snprintf(s_file, sizeof(s_file), "%s/scheme.mds", s_dir);
}

void setUp(void)
{
    build_paths();
    (void)MKDIR(s_dir);
    (void)remove(s_file);
    s_mounted = false;
    s_storage_leases = 0;
    s_storage_blocked = false;
    s_rename_fail = false;
    snprintf(s_root, sizeof(s_root), "/userdata");

    memset(s_av_active, 0, sizeof(s_av_active));
    memset(s_av_pending, 0, sizeof(s_av_pending));
    memset(s_av_generation, 0, sizeof(s_av_generation));
    memset(s_av_busy, 0, sizeof(s_av_busy));
    memset(s_av_rate, 0, sizeof(s_av_rate));
    s_av_play_calls = 0;
    s_av_stop_calls = 0;

    memset(s_test_binds, 0, sizeof(s_test_binds));
    s_test_bind_count = 0;
    s_test_bind_ret = ESP_ERR_NOT_FOUND;
    s_test_active[0] = '\0';

    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_NONE;
    s_engine_key = false;
    s_speed = 0;
    s_forward = true;
    s_prev_speed = 0;
    s_accel = 0;
    s_accel_q = 0;
    s_inhibited = false;
    s_control_armed = true;
    s_table = SOUND_TABLE_NONE;
    s_phase = TB_STOP;
    s_group = 0;
    s_plays = 0;
    s_bind_count = 0;
    memset(s_fn_voice, 0xFF, sizeof(s_fn_voice));
    memset(s_fn_state, 0, sizeof(s_fn_state));
    memset(s_fn_time, 0, sizeof(s_fn_time));
    s_lock = NULL;
    s_active[0] = '\0';
    s_task_iter_cap = 0;
    s_mock_applied_speed = 0;
    s_mock_applied_fwd = true;

    memset(s_test_cv, 0, sizeof(s_test_cv));
    s_test_cv_fail_idx = 0;
    memset(s_extra_voice, 0xFF, sizeof(s_extra_voice));
    memset(s_extra_on, 0, sizeof(s_extra_on));
    memset(s_extra_next, 0, sizeof(s_extra_next));
    s_brake_done = false;
    s_mute_stop = false;
    s_mute_move = false;
    s_mute_light = false;
    s_rng = 0x12345678u;

    mock_task_create_ok = 1;
    mock_mutex_create_fail = 0;
    mock_timer_now_us = 0;
}

void tearDown(void)
{
    (void)remove(s_file);
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_file);
    (void)remove(tmp);
}

/* ---- defaults ---- */

static void test_default(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_default(NULL));

    sound_scheme_t s;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_default(&s));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_NONE, s.type);
    TEST_ASSERT_EQUAL_UINT8(1, s.engine.engine_start_fn);
    TEST_ASSERT_EQUAL_UINT8(0, s.table_count);
}

/* ---- round-trip ---- */

static void test_roundtrip(void)
{
    sound_scheme_t s;
    (void)sound_store_default(&s);
    s.type = SOUND_SCHEME_DIESEL;
    s.table_count = 2;
    s.tables[1].used = true;
    snprintf(s.tables[1].name, sizeof(s.tables[1].name), "D1");
    snprintf(s.tables[1].loop[0].file, sizeof(s.tables[1].loop[0].file), "audio/slot4.wav");
    s.engine.engine_start_fn = 15;
    s.engine.drive[0] = 2;
    s.brake.min_brake_speed = 10;

    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(s_file, &s));

    sound_scheme_t back;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_load(s_file, &back));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_DIESEL, back.type);
    TEST_ASSERT_EQUAL_UINT8(2, back.table_count);
    TEST_ASSERT_EQUAL_STRING("D1", back.tables[1].name);
    TEST_ASSERT_EQUAL_STRING("audio/slot4.wav", back.tables[1].loop[0].file);
    TEST_ASSERT_EQUAL_UINT8(15, back.engine.engine_start_fn);
    TEST_ASSERT_EQUAL_UINT8(2, back.engine.drive[0]);
    TEST_ASSERT_EQUAL_UINT8(10, back.brake.min_brake_speed);
}

/* ---- load error paths ---- */

static void test_load_missing_returns_default(void)
{
    sound_scheme_t s;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sound_store_load(s_file, &s));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_NONE, s.type);
}

static void test_load_bad_magic(void)
{
    FILE *f = fopen(s_file, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("XXXX\x01\x00\x00\x00\x00\x00\x00\x00", 1, 12, f);
    fclose(f);

    sound_scheme_t s;
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_load(s_file, &s));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_NONE, s.type);
}

static void test_load_truncated(void)
{
    FILE *f = fopen(s_file, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("MDS1", 1, 4, f); /* header incomplete */
    fclose(f);

    sound_scheme_t s;
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_load(s_file, &s));
}

static void test_load_bad_crc(void)
{
    sound_scheme_t s;
    (void)sound_store_default(&s);
    s.type = SOUND_SCHEME_STEAM;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(s_file, &s));

    /* Corrupt one byte of the payload, leaving the header CRC stale. */
    FILE *f = fopen(s_file, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 12 + 16, SEEK_SET);
    fputc(0x55, f);
    fclose(f);

    sound_scheme_t back;
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_load(s_file, &back));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_NONE, back.type);
}

static void test_null_args(void)
{
    sound_scheme_t s;
    (void)sound_store_default(&s);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_save(NULL, &s));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_save(s_file, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_load(NULL, &s));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_load(s_file, NULL));
}

static void test_save_open_failure(void)
{
    sound_scheme_t s;
    (void)sound_store_default(&s);
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_save("no_such_dir_xyz/scheme.mds", &s));
}

static void test_save_path_too_long_and_overwrite(void)
{
    sound_scheme_t s;
    (void)sound_store_default(&s);

    /* Path longer than the temp buffer is rejected before touching the flash. */
    char longpath[300];
    memset(longpath, 'a', sizeof(longpath) - 1);
    longpath[sizeof(longpath) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_store_save(longpath, &s));

    /* Saving twice over an existing file exercises the rename() fallback. */
    s.type = SOUND_SCHEME_DIESEL;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(s_file, &s));
    s.type = SOUND_SCHEME_STEAM;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(s_file, &s));

    sound_scheme_t back;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_load(s_file, &back));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_STEAM, back.type);
}

/* ---- path / directory ---- */

static void test_store_path(void)
{
    char out[256];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_path(NULL, sizeof(out), "x"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_path(out, sizeof(out), NULL));

    s_mounted = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_store_path(out, sizeof(out), "x"));

    s_mounted = true;
    snprintf(s_root, sizeof(s_root), "%s", s_dir);
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_path(out, sizeof(out), "myproj"));
    TEST_ASSERT_NOT_NULL(strstr(out, "projects"));
    TEST_ASSERT_NOT_NULL(strstr(out, "myproj.mds"));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_store_path(out, 5, "myproj"));
}

static void test_ensure_dir(void)
{
    s_mounted = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_store_ensure_dir());

    s_mounted = true;
    snprintf(s_root, sizeof(s_root), "%s", s_dir);
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_ensure_dir()); /* creates projects/ */
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_ensure_dir()); /* EEXIST is fine */

    snprintf(s_root, sizeof(s_root), "no_such_parent_xyz");
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_ensure_dir());

    memset(s_root, 'a', sizeof(s_root) - 1);
    s_root[sizeof(s_root) - 1] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_store_ensure_dir());
}

/* ---------- engine core (R3) ---------- */

static void set_table_files(uint8_t idx, const char *init0, const char *loop0, const char *end0)
{
    s_scheme.tables[idx].used = true;
    if (init0 != NULL) { snprintf(s_scheme.tables[idx].init[0].file, SOUND_FILE_MAX, "%s", init0); }
    if (loop0 != NULL) { snprintf(s_scheme.tables[idx].loop[0].file, SOUND_FILE_MAX, "%s", loop0); }
    if (end0 != NULL) { snprintf(s_scheme.tables[idx].end[0].file, SOUND_FILE_MAX, "%s", end0); }
}

static void test_sound_pure_helpers(void)
{
    TEST_ASSERT_EQUAL_UINT8(0, sound_step_for_speed(0));
    TEST_ASSERT_EQUAL_UINT8(0, sound_step_for_speed(1));
    TEST_ASSERT_EQUAL_UINT8(2, sound_step_for_speed(128));
    TEST_ASSERT_EQUAL_UINT8(4, sound_step_for_speed(255));

    TEST_ASSERT_EQUAL_UINT16(AUDIO_RATE_MIN, sound_rate_for(0, 255, 100));
    TEST_ASSERT_EQUAL_UINT16(1000, sound_rate_for(0, 255, 1000));
    TEST_ASSERT_EQUAL_UINT16(AUDIO_RATE_MAX, sound_rate_for(127, 255, 3000));

}

static void test_sound_accessors(void)
{
    sound_scheme_t sc;
    sound_table_t t;
    sound_extra_t e;
    char nm[64];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_get(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_set(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_table_get(SOUND_MAX_TABLES, &t));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_table_get(0, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_table_set(0, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_table_set(SOUND_MAX_TABLES, &t));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_extra_get(SOUND_MAX_EXTRAS, &e));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_extra_get(0, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_extra_set(0, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_extra_set(SOUND_MAX_EXTRAS, &e));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_active_name_get(NULL, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_active_name_get(nm, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_load_scheme(NULL));

    memset(&t, 0, sizeof(t));
    t.used = true;
    t.name[0] = 'D';
    TEST_ASSERT_EQUAL(ESP_OK, sound_table_set(3, &t));
    TEST_ASSERT_EQUAL_UINT8(4, s_scheme.table_count);
    TEST_ASSERT_EQUAL(ESP_OK, sound_table_get(3, &t));
    TEST_ASSERT_EQUAL_UINT8('D', t.name[0]);

    memset(&e, 0, sizeof(e));
    e.table = 3;
    TEST_ASSERT_EQUAL(ESP_OK, sound_extra_set(2, &e));
    TEST_ASSERT_EQUAL_UINT8(3, s_scheme.extra_count);
    TEST_ASSERT_EQUAL(ESP_OK, sound_extra_get(2, &e));
    TEST_ASSERT_EQUAL_UINT8(3, e.table);

    memset(&sc, 0, sizeof(sc));
    sc.type = SOUND_SCHEME_DIESEL;
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_set(&sc));
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_get(&sc));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_DIESEL, sc.type);

    TEST_ASSERT_EQUAL(ESP_OK, sound_active_name_get(nm, sizeof(nm)));
    TEST_ASSERT_EQUAL_STRING("", nm);

    /* Fine-grained engine/type accessors used by the REST layer. */
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_DIESEL, sound_type_get());
    TEST_ASSERT_EQUAL(ESP_OK, sound_type_set(SOUND_SCHEME_STEAM));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_STEAM, sound_type_get());
    sound_engine_t eng;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_engine_get(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, sound_engine_get(&eng));
    eng.engine_start_fn = 7;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_engine_set(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, sound_engine_set(&eng));
    TEST_ASSERT_EQUAL_UINT8(7, s_scheme.engine.engine_start_fn);

    /* Brake accessors (used by the REST scheme view). */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_brake_get(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_brake_set(NULL));
    sound_brake_t br;
    memset(&br, 0, sizeof(br));
    br.max_on_speed = 5;
    br.min_brake_speed = 9;
    TEST_ASSERT_EQUAL(ESP_OK, sound_brake_set(&br));
    sound_brake_t br2;
    TEST_ASSERT_EQUAL(ESP_OK, sound_brake_get(&br2));
    TEST_ASSERT_EQUAL_UINT8(5, br2.max_on_speed);
    TEST_ASSERT_EQUAL_UINT8(9, br2.min_brake_speed);
}

static void test_sound_load_scheme(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_load_scheme("bad name"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_load_scheme("../escape"));

    TEST_ASSERT_EQUAL(ESP_OK, sound_load_scheme(""));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_NONE, s_scheme.type);
    TEST_ASSERT_EQUAL_STRING("", s_active);

    s_mounted = true;
    snprintf(s_root, sizeof(s_root), "%s", s_dir);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sound_load_scheme("nope"));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_NONE, s_scheme.type);
    /* A missing file must not leave a dangling active pointer (REV-S8). */
    TEST_ASSERT_EQUAL_STRING("", s_active);
    TEST_ASSERT_EQUAL_STRING("", s_test_active);

    char proj[600];
    snprintf(proj, sizeof(proj), "%s/projects", s_dir);
    (void)MKDIR(proj);
    char path[700];
    snprintf(path, sizeof(path), "%s/projects/mysc.mds", s_dir);
    sound_scheme_t sc;
    (void)sound_store_default(&sc);
    sc.type = SOUND_SCHEME_STEAM;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(path, &sc));
    TEST_ASSERT_EQUAL(ESP_OK, sound_load_scheme("mysc"));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_STEAM, s_scheme.type);
    TEST_ASSERT_EQUAL_STRING("mysc", s_active);
    (void)remove(path);
}

static void clear_projects(const char *proj)
{
    DIR *d = opendir(proj);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char p[700];
        snprintf(p, sizeof(p), "%s/%s", proj, e->d_name);
        (void)remove(p);
    }
    closedir(d);
}

static void test_sound_store_verify_read_name(void)
{
    TEST_ASSERT_FALSE(sound_store_name_ok(NULL));
    TEST_ASSERT_FALSE(sound_store_name_ok(""));
    TEST_ASSERT_TRUE(sound_store_name_ok("diesel_1-x"));
    TEST_ASSERT_FALSE(sound_store_name_ok("bad name"));
    TEST_ASSERT_FALSE(sound_store_name_ok("a/b"));
    TEST_ASSERT_FALSE(sound_store_name_ok("a.b"));
    TEST_ASSERT_FALSE(sound_store_name_ok("a\\b"));
    char longname[SOUND_FILE_MAX + 4];
    memset(longname, 'a', sizeof(longname));
    longname[sizeof(longname) - 1] = '\0';
    TEST_ASSERT_FALSE(sound_store_name_ok(longname));

    s_mounted = true;
    snprintf(s_root, sizeof(s_root), "%s", s_dir);
    char proj[600];
    snprintf(proj, sizeof(proj), "%s/projects", s_dir);
    (void)MKDIR(proj);
    clear_projects(proj);

    sound_scheme_t sc;
    (void)sound_store_default(&sc);
    sc.type = SOUND_SCHEME_STEAM;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(s_file, &sc));

    uint8_t buf[SOUND_STORE_MAX_BYTES];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_read(NULL, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_read(s_file, NULL, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_read(s_file, buf, sizeof(buf), NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_store_read(s_file, buf, 4, &len));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sound_store_read("no_such_scheme.mds", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_read(s_file, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_UINT32(SOUND_STORE_MAX_BYTES, len);

    sound_scheme_t out;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_verify(NULL, len, &out));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_store_verify(buf, len, NULL));
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_verify(buf, len - 1U, &out));
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_verify(buf, len, &out));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_STEAM, out.type);

    uint8_t bad[SOUND_STORE_MAX_BYTES];
    memcpy(bad, buf, sizeof(bad));
    bad[0] = 'X';
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_verify(bad, len, &out));
    memcpy(bad, buf, sizeof(bad));
    bad[4] ^= 0xFFU; /* version */
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_verify(bad, len, &out));
    memcpy(bad, buf, sizeof(bad));
    bad[6] ^= 0xFFU; /* size */
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_verify(bad, len, &out));
    memcpy(bad, buf, sizeof(bad));
    bad[SOUND_STORE_HDR_LEN] ^= 0xFFU; /* payload -> CRC mismatch */
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_verify(bad, len, &out));

    /* A truncated file (short) and one with a trailing byte (long) are both
     * rejected by the exact-size reader. */
    FILE *f = fopen(s_file, "wb");
    TEST_ASSERT_NOT_NULL(f);
    (void)fwrite(buf, 1, 4, f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_read(s_file, buf, sizeof(buf), &len));
    f = fopen(s_file, "ab");
    TEST_ASSERT_NOT_NULL(f);
    (void)fputc(0, f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_read(s_file, buf, sizeof(buf), &len));
}

static void test_sound_scheme_projects(void)
{
    char names[8][SOUND_FILE_MAX];
    size_t count = 0;

    /* Everything but the list guards requires the FS; all refuse cleanly. */
    s_mounted = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_scheme_create("a", SOUND_SCHEME_DIESEL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_scheme_delete("a"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_scheme_import("a", (const uint8_t *)names, 1, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_scheme_list(names, 8, &count));

    s_mounted = true;
    snprintf(s_root, sizeof(s_root), "%s", s_dir);
    char proj[600];
    snprintf(proj, sizeof(proj), "%s/projects", s_dir);
    (void)MKDIR(proj);
    clear_projects(proj);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_create(NULL, SOUND_SCHEME_DIESEL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_create("bad name", SOUND_SCHEME_DIESEL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_create("a", 99));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_delete(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      sound_scheme_export("a/b", (uint8_t *)names, sizeof(names), &count));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_import("a", NULL, 1, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_list(NULL, 8, &count));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_list(names, 0, &count));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, sound_scheme_list(names, 8, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_create("diesel", SOUND_SCHEME_DIESEL));
    TEST_ASSERT_EQUAL_STRING("diesel", s_active);
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_DIESEL, s_scheme.type);
    TEST_ASSERT_EQUAL_STRING("diesel", s_test_active);

    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_list(names, 8, &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_STRING("diesel", names[0]);

    uint8_t buf[SOUND_STORE_MAX_BYTES];
    size_t len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_export("diesel", buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_UINT32(SOUND_STORE_MAX_BYTES, len);
    sound_scheme_t chk;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_verify(buf, len, &chk));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_DIESEL, chk.type);
    size_t nlen = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sound_scheme_export("nope", buf, sizeof(buf), &nlen));

    /* Import without activation must not touch the active scheme. */
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_import("copy", buf, len, false));
    TEST_ASSERT_EQUAL_STRING("diesel", s_active);
    char path[700];
    snprintf(path, sizeof(path), "%s/projects/copy.mds", s_dir);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);
    /* Import with activation installs the scheme and the NVS pointer. */
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_import("copy", buf, len, true));
    TEST_ASSERT_EQUAL_STRING("copy", s_active);
    TEST_ASSERT_EQUAL_STRING("copy", s_test_active);
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_scheme_import("bad", buf, len - 1U, true));

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sound_scheme_delete("missing"));
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_delete("copy"));
    TEST_ASSERT_EQUAL_STRING("", s_active);
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_NONE, s_scheme.type);
    TEST_ASSERT_EQUAL_STRING("", s_test_active);
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_delete("diesel"));

    /* list filters non-.mds, bare ".mds" and over-long names, and respects max. */
    snprintf(path, sizeof(path), "%s/projects/readme.txt", s_dir);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/projects/.mds", s_dir);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);
    char toolong[SOUND_FILE_MAX + 8];
    memset(toolong, 'a', SOUND_FILE_MAX);
    toolong[SOUND_FILE_MAX] = '\0';
    snprintf(path, sizeof(path), "%s/projects/%s.mds", s_dir, toolong);
    f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_create("one", SOUND_SCHEME_DIESEL));
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_create("two", SOUND_SCHEME_STEAM));
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_list(names, 8, &count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_list(names, 1, &count));
    TEST_ASSERT_EQUAL_UINT32(1, count);

    /* A root long enough to overflow the path buffer is rejected. */
    char big[400];
    memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';
    snprintf(s_root, sizeof(s_root), "%s", big);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_scheme_list(names, 8, &count));

    /* A missing projects directory surfaces as NOT_FOUND. */
    snprintf(s_root, sizeof(s_root), "%s/nothere", s_dir);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, sound_scheme_list(names, 8, &count));

    /* Path errors: a root long enough to overflow the .mds path buffer. */
    char longroot[220];
    memset(longroot, 'r', sizeof(longroot) - 1U);
    longroot[sizeof(longroot) - 1U] = '\0';
    snprintf(s_root, sizeof(s_root), "%s", longroot);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_scheme_create("x", SOUND_SCHEME_DIESEL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_scheme_delete("x"));
    size_t elen = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, sound_scheme_export("x", buf, sizeof(buf), &elen));

    /* Save failure: a mounted root that has no projects/ directory. */
    char noproj[700];
    snprintf(noproj, sizeof(noproj), "%s/noproj", s_dir);
    (void)MKDIR(noproj);
    snprintf(s_root, sizeof(s_root), "%s", noproj);
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_scheme_create("x", SOUND_SCHEME_DIESEL));
    snprintf(s_root, sizeof(s_root), "%s", s_dir);

    (void)remove(path);
    snprintf(path, sizeof(path), "%s/projects/one.mds", s_dir);
    (void)remove(path);
    snprintf(path, sizeof(path), "%s/projects/two.mds", s_dir);
    (void)remove(path);
    snprintf(path, sizeof(path), "%s/projects/readme.txt", s_dir);
    (void)remove(path);
    snprintf(path, sizeof(path), "%s/projects/.mds", s_dir);
    (void)remove(path);
}

static void test_sound_engine_pick(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.stop_table = 2;
    s_scheme.engine.drive[3] = 5;
    s_scheme.engine.accel[3] = 6;
    s_scheme.engine.coast[3] = 7;

    s_engine_key = false;
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, sound_engine_pick(200, 0, 0));
    s_engine_key = true;
    TEST_ASSERT_EQUAL_UINT8(2, sound_engine_pick(0, 0, 0));
    TEST_ASSERT_EQUAL_UINT8(6, sound_engine_pick(200, 5, 0));
    TEST_ASSERT_EQUAL_UINT8(7, sound_engine_pick(200, -5, 0));
    TEST_ASSERT_EQUAL_UINT8(5, sound_engine_pick(200, 0, 0));
    /* CV30 skip flags. */
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, sound_engine_pick(0, 0, SOUND_ENG_SKIP_D1TOS));
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, sound_engine_pick(10, 0, SOUND_ENG_SKIP_STOD1));
    s_scheme.engine.drive[3] = SOUND_TABLE_NONE;
    TEST_ASSERT_EQUAL_UINT8(2, sound_engine_pick(200, 0, 0));
}

static void test_sound_cv_rate(void)
{
    /* CV -> rate permille: 0 -> 0.5x, 51 -> 1.0x, 255 -> 3.0x. */
    s_test_cv[114] = 0;
    TEST_ASSERT_EQUAL_UINT16(500, cv_rate_permille(114, 0));
    s_test_cv[114] = 51;
    TEST_ASSERT_EQUAL_UINT16(1000, cv_rate_permille(114, 0));
    s_test_cv[114] = 255;
    TEST_ASSERT_EQUAL_UINT16(3000, cv_rate_permille(114, 0));

    /* The engine rate follows CV114 (base), scaled by the table rate_scale. */
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.engine_start_fn = 1;
    s_scheme.engine.drive[3] = 5;
    set_table_files(5, NULL, "drive.wav", NULL);
    s_test_cv[114] = 120; /* ~1.68x base */
    sound_engine_power(true);
    sound_set_speed(200, true);
    sound_tick();
    TEST_ASSERT_TRUE(s_av_rate[18] > 1000);

    /* A failing CV read falls back to the supplied default. */
    s_test_cv_fail_idx = 30;
    TEST_ASSERT_EQUAL_UINT8(7, cv_u8(30, 7));
    s_test_cv_fail_idx = 0;
}

static void test_sound_extras(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    set_table_files(5, NULL, "spare.wav", NULL);
    s_scheme.tables[5].max_plays = 1; /* random extras require a finite table */

    /* A key-driven extra (fn set) is ignored by the tick. */
    s_scheme.extra_count = 3;
    s_scheme.extras[2].fn = 3;
    s_scheme.extras[2].table = 5;
    s_scheme.extras[2].mode = SOUND_MODE_STATE;
    extras_tick();
    TEST_ASSERT_EQUAL_UINT8(0, s_extra_on[2]);

    /* RANDOM: no period configured -> skipped; NULL table -> no crash. */
    s_scheme.extras[0].fn = SOUND_FN_NONE;
    s_scheme.extras[0].mode = SOUND_MODE_RANDOM;
    s_scheme.extras[0].table = 5;
    s_scheme.extras[0].random_max_ms = 0;
    s_scheme.extras[1].fn = SOUND_FN_NONE;
    s_scheme.extras[1].mode = SOUND_MODE_RANDOM;
    s_scheme.extras[1].table = 9; /* unused table */
    s_scheme.extras[1].random_min_ms = 100;
    s_scheme.extras[1].random_max_ms = 100;
    s_av_play_calls = 0;
    mock_timer_now_us = 0;
    extras_tick(); /* schedules extras[1] and hits the max==0 skip */
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);
    mock_timer_now_us = 300000; /* past the 100 ms delay */
    extras_tick(); /* fires, but table 9 has no file */
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);

    /* RANDOM with a real table plays on the secondary voice. */
    s_scheme.extras[1].table = 5;
    s_extra_next[1] = 0;
    mock_timer_now_us = 0;
    extras_tick();
    mock_timer_now_us = 300000;
    extras_tick();
    TEST_ASSERT_EQUAL_INT(1, s_av_play_calls);
    TEST_ASSERT_EQUAL_STRING("/userdata/spare.wav", s_av_path[19]);

    s_scheme.extras[1].random_min_ms = 100;
    s_scheme.extras[1].random_max_ms = 200;
    s_extra_next[1] = 0;
    mock_timer_now_us = 0;
    extras_tick(); /* span > 0 exercises the RNG schedule */

    /* STATE: matches only when the motion state/dir matches. */
    memset(&s_scheme.extras[0], 0, sizeof(s_scheme.extras[0]));
    s_scheme.extras[0].fn = SOUND_FN_NONE;
    s_scheme.extras[0].mode = SOUND_MODE_STATE;
    s_scheme.extras[0].table = 5;
    s_scheme.extras[0].state = FUNC_STATE_MOVING;
    s_scheme.extras[0].dir = FUNC_DIR_ANY;
    s_scheme.extra_count = 1;
    s_speed = 0;
    extras_tick();
    TEST_ASSERT_EQUAL_UINT8(0, s_extra_on[0]);
    s_speed = 100;
    extras_tick();
    TEST_ASSERT_EQUAL_UINT8(1, s_extra_on[0]);
    TEST_ASSERT_TRUE(audio_voice_is_active(s_extra_voice[0]));
    s_speed = 0;
    extras_tick();
    TEST_ASSERT_EQUAL_UINT8(0, s_extra_on[0]);
}

static void test_sound_key_extras(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    set_table_files(5, NULL, "ex.wav", NULL);
    s_scheme.extra_count = 4;

    s_scheme.extras[0].fn = 4;
    s_scheme.extras[0].table = 5;
    s_scheme.extras[0].mode = SOUND_MODE_ONE_SHOT;
    s_scheme.extras[1].fn = 4;
    s_scheme.extras[1].table = 5;
    s_scheme.extras[1].mode = SOUND_MODE_LOOP_HELD;
    s_scheme.extras[2].fn = 5;
    s_scheme.extras[2].table = 5;
    s_scheme.extras[2].mode = SOUND_MODE_LATCHED;
    s_scheme.extras[3].fn = 6;
    s_scheme.extras[3].table = 5;
    s_scheme.extras[3].mode = SOUND_MODE_TRIGGER;

    memset(s_fn_state, 0, sizeof(s_fn_state));
    s_mute_light = false;
    s_av_play_calls = 0;

    /* F4 press: one-shot + loop-held both start; release stops the loop. */
    sound_function(4, true);
    TEST_ASSERT_EQUAL_INT(2, s_av_play_calls);
    TEST_ASSERT_EQUAL_UINT8(1, s_extra_on[1]);
    sound_function(4, false);
    TEST_ASSERT_EQUAL_UINT8(0, s_extra_on[1]);

    /* LATCHED toggles on each press edge. */
    sound_function(5, true);
    TEST_ASSERT_EQUAL_UINT8(1, s_extra_on[2]);
    sound_function(5, false);
    sound_function(5, true);
    TEST_ASSERT_EQUAL_UINT8(0, s_extra_on[2]);
    sound_function(5, false);

    /* TRIGGER ignores a press while its voice is still sounding. */
    sound_function(6, true);
    TEST_ASSERT_TRUE(audio_voice_is_active(s_extra_voice[3]));
    int calls = s_av_play_calls;
    sound_function(6, false);
    sound_function(6, true);
    TEST_ASSERT_EQUAL_INT(calls, s_av_play_calls);

    /* LIGHT mute suppresses effect extras. */
    s_mute_light = true;
    s_av_play_calls = 0;
    sound_function(4, false);
    sound_function(4, true);
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);
    s_mute_light = false;

    /* A key with no matching extra, and an unset table, do nothing. */
    s_scheme.extras[0].table = SOUND_TABLE_NONE;
    s_scheme.extras[1].table = SOUND_TABLE_NONE;
    s_av_play_calls = 0;
    sound_function(4, false);
    sound_function(4, true);
    sound_function(7, true);
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);

    /* An extra whose table has no file is a no-op, not a crash. */
    s_scheme.extras[0].table = 5;
    s_scheme.extras[1].table = 5;
    s_scheme.tables[5].loop[0].file[0] = '\0';
    s_av_play_calls = 0;
    sound_function(4, false);
    sound_function(4, true);
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);
}

static void test_sound_brake(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.brake.min_brake_speed = 100;
    set_table_files(2, NULL, "stop.wav", NULL);
    s_scheme.engine.stop_table = 2;

    /* Engine off -> nothing. */
    s_engine_key = false;
    s_speed = 50;
    s_accel = -10;
    brake_tick();
    TEST_ASSERT_FALSE(s_brake_done);

    /* min_brake_speed == 0 -> disabled. */
    s_engine_key = true;
    s_scheme.brake.min_brake_speed = 0;
    brake_tick();
    TEST_ASSERT_FALSE(s_brake_done);

    /* Too fast / not decelerating -> nothing. */
    s_scheme.brake.min_brake_speed = 100;
    s_speed = 150;
    brake_tick();
    TEST_ASSERT_FALSE(s_brake_done);
    s_speed = 50;
    s_accel = 0;
    brake_tick();
    TEST_ASSERT_FALSE(s_brake_done);

    /* Hard deceleration below the threshold fires once. */
    s_accel = -10;
    s_av_play_calls = 0;
    brake_tick();
    TEST_ASSERT_TRUE(s_brake_done);
    TEST_ASSERT_EQUAL_INT(1, s_av_play_calls);
    TEST_ASSERT_EQUAL_STRING("/userdata/stop.wav", s_av_path[19]);
    brake_tick(); /* already fired */
    TEST_ASSERT_EQUAL_INT(1, s_av_play_calls);

    /* Suppressed above max_on_speed: below it the latch resets. */
    s_scheme.brake.max_on_speed = 60;
    s_speed = 50;
    s_accel = -10;
    s_brake_done = true;
    brake_tick();
    TEST_ASSERT_FALSE(s_brake_done);
    s_scheme.brake.max_on_speed = 0;

    /* Rising speed resets the latch; no stop table -> no fire. */
    s_speed = 200;
    brake_tick();
    TEST_ASSERT_FALSE(s_brake_done);
    s_scheme.engine.stop_table = SOUND_TABLE_NONE;
    s_speed = 50;
    s_accel = -10;
    s_av_play_calls = 0;
    brake_tick();
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);
}

static void test_sound_sequencer(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    set_table_files(1, "i.wav", "l.wav", "e.wav");
    s_scheme.tables[1].max_plays = 2;

    tb_start(1);
    TEST_ASSERT_EQUAL_UINT8(TB_INIT, s_phase);
    TEST_ASSERT_EQUAL_STRING("/userdata/i.wav", s_av_path[18]);

    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_UINT8(TB_LOOP, s_phase);
    TEST_ASSERT_EQUAL_STRING("/userdata/l.wav", s_av_path[18]);

    mock_audio_complete(18);
    tb_advance(); /* play 1 < max_plays */
    TEST_ASSERT_EQUAL_UINT8(TB_LOOP, s_phase);

    mock_audio_complete(18);
    tb_advance(); /* reach max_plays -> End */
    TEST_ASSERT_EQUAL_UINT8(TB_END, s_phase);
    TEST_ASSERT_EQUAL_STRING("/userdata/e.wav", s_av_path[18]);

    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_UINT8(TB_STOP, s_phase);

    /* No Init/Loop: goes straight to End, then stops. */
    memset(&s_scheme.tables[2], 0, sizeof(s_scheme.tables[2]));
    set_table_files(2, NULL, NULL, "onlyend.wav");
    tb_start(2);
    TEST_ASSERT_EQUAL_UINT8(TB_END, s_phase);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_UINT8(TB_STOP, s_phase);

    /* Multi-group (steam) Init sequence. */
    memset(&s_scheme.tables[3], 0, sizeof(s_scheme.tables[3]));
    s_scheme.tables[3].used = true;
    snprintf(s_scheme.tables[3].init[0].file, SOUND_FILE_MAX, "c0.wav");
    snprintf(s_scheme.tables[3].init[1].file, SOUND_FILE_MAX, "c1.wav");
    tb_start(3);
    TEST_ASSERT_EQUAL_STRING("/userdata/c0.wav", s_av_path[18]);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_STRING("/userdata/c1.wav", s_av_path[18]);
    mock_audio_complete(18);
    tb_advance(); /* third group empty, no loop -> End */
    TEST_ASSERT_EQUAL_UINT8(TB_END, s_phase);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_UINT8(TB_STOP, s_phase);

    /* A gap in the cylinder groups is skipped (init[1] empty, init[2] set). */
    memset(&s_scheme.tables[4], 0, sizeof(s_scheme.tables[4]));
    set_table_files(4, "g0.wav", NULL, NULL);
    snprintf(s_scheme.tables[4].init[2].file, SOUND_FILE_MAX, "g2.wav");
    tb_start(4);
    TEST_ASSERT_EQUAL_STRING("/userdata/g0.wav", s_av_path[18]);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_STRING("/userdata/g2.wav", s_av_path[18]);

    /* min_plays is a floor even when max_plays is smaller (REV-S6). */
    memset(&s_scheme.tables[6], 0, sizeof(s_scheme.tables[6]));
    set_table_files(6, NULL, "ml.wav", NULL);
    s_scheme.tables[6].max_plays = 1;
    s_scheme.tables[6].min_plays = 2;
    tb_start(6);
    TEST_ASSERT_EQUAL_UINT8(TB_LOOP, s_phase);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_UINT8(TB_LOOP, s_phase);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_UINT8(TB_LOOP, s_phase);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL_UINT8(TB_END, s_phase);

    /* Unknown table -> stop. */
    tb_start(SOUND_TABLE_NONE);
    TEST_ASSERT_EQUAL_UINT8(TB_STOP, s_phase);
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, s_table);
    tb_start(SOUND_MAX_TABLES + 5U); /* out of range: table_at() guards */

    /* Direct call with no current table. */
    s_table = SOUND_TABLE_NONE;
    TEST_ASSERT_FALSE(tb_play_init());
}

static void test_sound_table_first_file_paths(void)
{
    char buf[192];
    sound_table_t t;

    memset(&t, 0, sizeof(t));
    snprintf(t.init[0].file, SOUND_FILE_MAX, "init.wav");
    TEST_ASSERT_EQUAL_STRING("/userdata/init.wav", table_first_file(&t, buf, sizeof(buf)));

    memset(&t, 0, sizeof(t));
    snprintf(t.loop[0].file, SOUND_FILE_MAX, "/abs/loop.wav");
    TEST_ASSERT_EQUAL_STRING("/abs/loop.wav", table_first_file(&t, buf, sizeof(buf)));

    memset(&t, 0, sizeof(t));
    snprintf(t.end[0].file, SOUND_FILE_MAX, "end.wav");
    TEST_ASSERT_EQUAL_STRING("/userdata/end.wav", table_first_file(&t, buf, sizeof(buf)));

    memset(&t, 0, sizeof(t));
    TEST_ASSERT_NULL(table_first_file(&t, buf, sizeof(buf)));
}

static void test_sound_fx_guards(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_bind_count = 2;
    memset(s_binds, 0, sizeof(s_binds));

    /* Target table does not exist -> t == NULL. */
    s_binds[0].used = 1; s_binds[0].fn = 6; s_binds[0].target_type = FUNC_TARGET_SOUND;
    s_binds[0].target_id = SOUND_TABLE_NONE; s_binds[0].mode = SOUND_MODE_ONE_SHOT;
    sound_function(6, true);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fn_voice[6]);
    sound_function(6, true); /* unchanged -> early return */

    /* Target table exists but has no files. */
    s_scheme.tables[8].used = true;
    s_binds[1].used = 1; s_binds[1].fn = 7; s_binds[1].target_type = FUNC_TARGET_SOUND;
    s_binds[1].target_id = 8; s_binds[1].mode = SOUND_MODE_ONE_SHOT;
    sound_function(7, true);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fn_voice[7]);

    /* LATCHED toggles on each press. */
    set_table_files(9, NULL, "latch.wav", NULL);
    s_binds[0].fn = 8; s_binds[0].target_id = 9; s_binds[0].mode = SOUND_MODE_LATCHED;
    sound_function(8, true);
    TEST_ASSERT_TRUE(s_fn_voice[8] != AUDIO_VOICE_NONE);
    sound_function(8, false); /* release ignored */
    TEST_ASSERT_TRUE(s_fn_voice[8] != AUDIO_VOICE_NONE);
    sound_function(8, true);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fn_voice[8]);

    /* Unknown mode: default branch plays nothing. */
    set_table_files(10, NULL, "x.wav", NULL);
    s_binds[1].fn = 9; s_binds[1].target_id = 10; s_binds[1].mode = 99;
    sound_function(9, true);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fn_voice[9]);

    /* No free effect voice: voices 0..17 busy -> alloc yields 18 -> rejected. */
    for (int i = 0; i < SOUND_VOICE_ENGINE; ++i) {
        s_av_busy[i] = true;
    }
    s_binds[1].fn = 10; s_binds[1].target_id = 9; s_binds[1].mode = SOUND_MODE_ONE_SHOT;
    sound_function(10, true);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fn_voice[10]);
    for (int i = 0; i < SOUND_VOICE_ENGINE; ++i) {
        s_av_busy[i] = false;
    }

    /* dir gate: a FWD-only binding does not fire in reverse. */
    set_table_files(12, NULL, "fwd.wav", NULL);
    memset(&s_binds[0], 0, sizeof(s_binds[0]));
    s_binds[0].used = 1; s_binds[0].fn = 12; s_binds[0].target_type = FUNC_TARGET_SOUND;
    s_binds[0].target_id = 12; s_binds[0].mode = SOUND_MODE_ONE_SHOT;
    s_binds[0].dir = FUNC_DIR_FWD;
    s_forward = false;
    s_speed = 100;
    s_av_play_calls = 0;
    sound_function(12, true);
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);
    s_forward = true;
    sound_function(12, false);
    sound_function(12, true);
    TEST_ASSERT_EQUAL_INT(1, s_av_play_calls);

    /* state gate: a STOPPED-only binding does not fire while moving. */
    set_table_files(14, NULL, "stopgate.wav", NULL);
    memset(&s_binds[0], 0, sizeof(s_binds[0]));
    s_binds[0].used = 1; s_binds[0].fn = 14; s_binds[0].target_type = FUNC_TARGET_SOUND;
    s_binds[0].target_id = 14; s_binds[0].mode = SOUND_MODE_ONE_SHOT;
    s_binds[0].state = FUNC_STATE_STOPPED;
    s_speed = 100;
    s_forward = true;
    s_av_play_calls = 0;
    sound_function(14, true);
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);

    /* TRIGGER: one run to completion, ignored while still sounding. */
    set_table_files(13, NULL, "trig.wav", NULL);
    memset(&s_binds[1], 0, sizeof(s_binds[1]));
    s_binds[1].used = 1; s_binds[1].fn = 13; s_binds[1].target_type = FUNC_TARGET_SOUND;
    s_binds[1].target_id = 13; s_binds[1].mode = SOUND_MODE_TRIGGER;
    s_av_play_calls = 0;
    sound_function(13, true);
    TEST_ASSERT_EQUAL_INT(1, s_av_play_calls);
    uint8_t vtrig = s_fn_voice[13];
    sound_function(13, false);
    sound_function(13, true); /* still active -> ignored */
    TEST_ASSERT_EQUAL_INT(1, s_av_play_calls);
    mock_audio_complete(vtrig);
    sound_function(13, false);
    sound_function(13, true);
    TEST_ASSERT_EQUAL_INT(2, s_av_play_calls);
}

static void test_sound_status_and_save(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_engine_key = true;
    s_table = 5;
    s_phase = TB_LOOP;
    s_speed = 100;
    s_forward = false;
    snprintf(s_active, sizeof(s_active), "mysc");

    sound_status_t st;
    sound_status_get(NULL); /* no crash */
    sound_status_get(&st);
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_DIESEL, st.type);
    TEST_ASSERT_TRUE(st.enabled);
    TEST_ASSERT_TRUE(st.engine);
    TEST_ASSERT_EQUAL_UINT8(5, st.table);
    TEST_ASSERT_EQUAL_UINT8(TB_LOOP, st.phase);
    TEST_ASSERT_EQUAL_UINT8(100, st.speed);
    TEST_ASSERT_FALSE(st.forward);
    TEST_ASSERT_EQUAL_STRING("mysc", st.name);

    s_active[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_scheme_save());

    s_mounted = true;
    snprintf(s_root, sizeof(s_root), "%s", s_dir);
    char proj[600];
    snprintf(proj, sizeof(proj), "%s/projects", s_dir);
    (void)MKDIR(proj);
    snprintf(s_active, sizeof(s_active), "mysc");
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_save());

    s_mounted = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_scheme_save());
}

static void test_sound_scheme_enabled_and_set(void)
{
    s_scheme.type = SOUND_SCHEME_NONE;
    TEST_ASSERT_FALSE(sound_scheme_enabled());
    s_scheme.type = SOUND_SCHEME_LEGACY;
    TEST_ASSERT_FALSE(sound_scheme_enabled());
    s_scheme.type = SOUND_SCHEME_DIESEL;
    TEST_ASSERT_TRUE(sound_scheme_enabled());
    sound_engine_power(true);
    TEST_ASSERT_TRUE(sound_engine_is_on());
    sound_engine_power(false);
    TEST_ASSERT_FALSE(sound_engine_is_on());
}

static void test_sound_task_body(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.engine_start_fn = 1;
    s_scheme.engine.stop_table = 2;
    set_table_files(2, NULL, "stop.wav", NULL);
    sound_function(1, true);

    s_task_iter_cap = 2; /* run the body twice, then exit */
    sound_task(NULL);
    TEST_ASSERT_EQUAL_UINT8(2, s_table);

    /* Applied speed 126 (motor max) normalizes to 255 -> drive step 4. */
    s_scheme.engine.drive[4] = 7;
    set_table_files(7, NULL, "d5.wav", NULL);
    s_mock_applied_speed = 126;
    s_task_iter_cap = 1;
    sound_task(NULL);
    TEST_ASSERT_EQUAL_UINT8(7, s_table);

    /* Out-of-range applied speed is clamped, not overflowed. */
    s_mock_applied_speed = 200;
    s_task_iter_cap = 1;
    sound_task(NULL);
    TEST_ASSERT_EQUAL_UINT8(7, s_table);
}

static void test_sound_logic_and_sync(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.engine_start_fn = 1;
    s_scheme.engine.stop_table = 2;
    set_table_files(2, NULL, "stop.wav", NULL);

    s_bind_count = 1;
    memset(s_binds, 0, sizeof(s_binds));
    s_binds[0].used = 1;
    s_binds[0].fn = 8;
    s_binds[0].target_type = FUNC_TARGET_LOGIC;
    s_binds[0].target_id = FUNC_LOGIC_MUTE_STOP;

    /* MUTE_STOP: no engine sound while stopped. */
    sound_function(8, true);
    TEST_ASSERT_TRUE(s_mute_stop);
    s_engine_key = true;
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, sound_engine_pick(0, 0, 0));
    sound_function(8, false);
    TEST_ASSERT_FALSE(s_mute_stop);
    TEST_ASSERT_EQUAL_UINT8(2, sound_engine_pick(0, 0, 0));

    /* MUTE_MOVE, and an unimplemented logic is a safe no-op. */
    s_binds[0].target_id = FUNC_LOGIC_MUTE_MOVE;
    sound_function(8, true);
    TEST_ASSERT_TRUE(s_mute_move);
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, sound_engine_pick(100, 0, 0));
    sound_function(8, false);
    s_binds[0].target_id = FUNC_LOGIC_DRIVE_HOLD;
    sound_function(8, true);
    sound_function(8, false);

    /* MUTE_LIGHT silences effect bindings. */
    set_table_files(9, NULL, "fx.wav", NULL);
    s_bind_count = 2;
    s_binds[1].used = 1;
    s_binds[1].fn = 5;
    s_binds[1].target_type = FUNC_TARGET_SOUND;
    s_binds[1].target_id = 9;
    s_binds[1].mode = SOUND_MODE_ONE_SHOT;
    s_binds[0].target_id = FUNC_LOGIC_MUTE_LIGHT;
    s_av_play_calls = 0;
    sound_function(8, true); /* mute light on */
    sound_function(5, true);
    TEST_ASSERT_EQUAL_INT(0, s_av_play_calls);
    sound_function(8, false); /* mute light off */
    sound_function(5, false);
    sound_function(5, true);
    TEST_ASSERT_EQUAL_INT(1, s_av_play_calls);

    /* Defensive guard: an out-of-range fn in a stored record is ignored. */
    func_binding_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.used = 1;
    bad.fn = SOUND_FN_COUNT;
    bad.target_type = FUNC_TARGET_SOUND;
    binding_apply(&bad, true);

    /* LOGIC gate: a FWD-only mute does not engage in reverse. */
    s_binds[0].fn = 8;
    s_binds[0].target_type = FUNC_TARGET_LOGIC;
    s_binds[0].target_id = FUNC_LOGIC_MUTE_STOP;
    s_binds[0].dir = FUNC_DIR_FWD;
    s_forward = false;
    s_speed = 100;
    s_mute_stop = false;
    sound_function(8, true);
    TEST_ASSERT_FALSE(s_mute_stop);

    /* sync_motion: the engine key follows the wheels. */
    s_scheme.engine.sync_motion = true;
    s_binds[0].fn = 8;
    s_binds[0].target_id = 99; /* unknown logic: no-op */
    s_engine_key = false;
    sound_set_speed(0, true);
    sound_tick();
    TEST_ASSERT_FALSE(s_engine_key);
    sound_set_speed(100, true);
    sound_tick();
    TEST_ASSERT_TRUE(s_engine_key);
}

static void test_sound_lint(void)
{
    char rep[64];
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_NONE;
    TEST_ASSERT_EQUAL_INT(0, sound_lint(NULL, 0));

    s_scheme.type = SOUND_SCHEME_DIESEL;
    TEST_ASSERT_EQUAL_INT(1, sound_lint(rep, sizeof(rep)));
    TEST_ASSERT_NOT_NULL(strstr(rep, "start"));

    s_scheme.engine.start_table = 1;
    set_table_files(1, NULL, "l.wav", NULL);
    s_scheme.tables[1].min_speed = 10;
    s_scheme.tables[1].max_speed = 5;
    TEST_ASSERT_EQUAL_INT(1, sound_lint(rep, sizeof(rep)));
    s_scheme.tables[1].min_speed = 5;
    s_scheme.tables[1].max_speed = 10;
    TEST_ASSERT_EQUAL_INT(0, sound_lint(NULL, 0));

    /* A random extra must target a finite table (max_plays != 0). */
    s_scheme.extra_count = 1;
    s_scheme.extras[0].mode = SOUND_MODE_RANDOM;
    s_scheme.extras[0].table = 1;
    s_scheme.tables[1].max_plays = 0;
    TEST_ASSERT_EQUAL_INT(1, sound_lint(NULL, 0));
    s_scheme.tables[1].max_plays = 1;
    TEST_ASSERT_EQUAL_INT(0, sound_lint(NULL, 0));
}

static void test_sound_function_engine_toggle(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.engine_start_fn = 1;

    sound_function(SOUND_FN_COUNT, true); /* out of range */
    sound_function(1, true);
    TEST_ASSERT_TRUE(s_engine_key);
    TEST_ASSERT_TRUE(sound_engine_is_on());
    sound_function(1, false); /* release does not toggle */
    TEST_ASSERT_TRUE(s_engine_key);
    sound_function(1, true); /* press toggles off */
    TEST_ASSERT_FALSE(s_engine_key);
    TEST_ASSERT_FALSE(sound_engine_is_on());
}

static void test_sound_binding_modes(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.engine_start_fn = 1;
    set_table_files(5, NULL, "horn.wav", NULL);
    set_table_files(6, NULL, "short.wav", NULL);
    set_table_files(7, NULL, "oneshot.wav", NULL);

    s_bind_count = 3;
    memset(s_binds, 0, sizeof(s_binds));
    s_binds[0].used = 1; s_binds[0].fn = 2; s_binds[0].target_type = FUNC_TARGET_SOUND;
    s_binds[0].target_id = 5; s_binds[0].mode = SOUND_MODE_SHORT_LONG;
    s_binds[0].short_table = 6; s_binds[0].short_ms = 400;
    s_binds[1].used = 1; s_binds[1].fn = 3; s_binds[1].target_type = FUNC_TARGET_SOUND;
    s_binds[1].target_id = 5; s_binds[1].mode = SOUND_MODE_LOOP_HELD;
    s_binds[2].used = 1; s_binds[2].fn = 4; s_binds[2].target_type = FUNC_TARGET_SOUND;
    s_binds[2].target_id = 7; s_binds[2].mode = SOUND_MODE_ONE_SHOT;

    /* SHORT_LONG short tap (< short_ms): short_table */
    s_av_play_calls = 0;
    mock_timer_now_us = 0;
    sound_function(2, true);
    TEST_ASSERT_TRUE(s_fn_voice[2] != AUDIO_VOICE_NONE);
    uint8_t v = s_fn_voice[2];
    mock_timer_now_us = 100000; /* 100 ms */
    sound_function(2, false);
    TEST_ASSERT_EQUAL_STRING("/userdata/short.wav", s_av_path[v]);

    /* SHORT_LONG long hold: loop stops, no short */
    mock_timer_now_us = 0;
    sound_function(2, true);
    v = s_fn_voice[2];
    mock_timer_now_us = 900000; /* 900 ms */
    sound_function(2, false);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fn_voice[2]);

    /* LOOP_HELD: release stops */
    sound_function(3, true);
    TEST_ASSERT_TRUE(s_fn_voice[3] != AUDIO_VOICE_NONE);
    sound_function(3, false);
    TEST_ASSERT_EQUAL_UINT8(0xFF, s_fn_voice[3]);

    /* ONE_SHOT ignores release */
    sound_function(4, true);
    uint8_t v4 = s_fn_voice[4];
    TEST_ASSERT_TRUE(v4 != AUDIO_VOICE_NONE);
    sound_function(4, false);
    TEST_ASSERT_EQUAL_UINT8(v4, s_fn_voice[4]);
    sound_function(4, true); /* unchanged -> no-op */

    /* A binding that is not a SOUND target is ignored here. */
    s_binds[0].target_type = FUNC_TARGET_SLOT;
    s_binds[0].fn = 5;
    sound_function(5, true);
    /* state still updated but no crash */
    TEST_ASSERT_TRUE(s_fn_state[5]);
}

static void test_sound_tick_and_stop(void)
{
    memset(&s_scheme, 0, sizeof(s_scheme));
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.engine_start_fn = 1;
    s_scheme.engine.stop_table = 2;
    s_scheme.engine.drive[3] = 5;
    set_table_files(2, NULL, "stop.wav", NULL);
    set_table_files(5, NULL, "drive.wav", NULL);

    sound_tick(); /* scheme loaded but engine off: nothing */
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, s_table);

    sound_function(1, true); /* engine on */
    sound_set_speed(200, true);
    sound_tick();
    TEST_ASSERT_EQUAL_UINT8(5, s_table);
    TEST_ASSERT_EQUAL_STRING("/userdata/drive.wav", s_av_path[18]);
    TEST_ASSERT_TRUE(s_av_rate[18] >= AUDIO_RATE_MIN);

    sound_set_speed(0, true);
    sound_tick();
    TEST_ASSERT_EQUAL_UINT8(2, s_table);
    TEST_ASSERT_EQUAL_STRING("/userdata/stop.wav", s_av_path[18]);

    sound_stop_all();
    TEST_ASSERT_FALSE(s_engine_key);
    TEST_ASSERT_EQUAL_UINT8(TB_STOP, s_phase);
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, s_table);
    TEST_ASSERT_FALSE(audio_voice_is_active(18));

    /* A NONE/legacy scheme short-circuits the tick. */
    s_scheme.type = SOUND_SCHEME_NONE;
    sound_tick();
    TEST_ASSERT_EQUAL_UINT8(SOUND_TABLE_NONE, s_table);
}

static void test_sound_init_paths(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_set_inhibited(true));
    TEST_ASSERT_EQUAL(ESP_OK, sound_init());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_init());
    vSemaphoreDelete(s_lock);
    s_lock = NULL; /* Simulate a fresh boot for each independent init case. */

    s_test_bind_count = 2;
    s_test_binds[0].used = 1;
    s_test_bind_ret = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, sound_init());
    TEST_ASSERT_EQUAL_UINT32(2, s_bind_count);
    s_test_bind_ret = ESP_ERR_NOT_FOUND;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;

    /* Non-empty active name with storage unmounted -> loader falls back. */
    snprintf(s_test_active, sizeof(s_test_active), "x");
    TEST_ASSERT_EQUAL(ESP_OK, sound_init());
    s_test_active[0] = '\0';
    vSemaphoreDelete(s_lock);
    s_lock = NULL;

    mock_mutex_create_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, sound_init());
    TEST_ASSERT_NULL(s_lock);
    mock_mutex_create_fail = 0;

    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, sound_init());
    TEST_ASSERT_NULL(s_lock);
    mock_task_create_ok = 1;
    TEST_ASSERT_EQUAL(ESP_OK, sound_init());
}

static void test_mds_semantic_corpus_crc_trusted(void)
{
    const struct { size_t offset; uint8_t value; } bad[] = {
        {offsetof(sound_scheme_t, type), 255},
        {offsetof(sound_scheme_t, table_count), 33},
        {offsetof(sound_scheme_t, extra_count), 25},
        {offsetof(sound_scheme_t, tables) + offsetof(sound_table_t, used), 2},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, sync_motion), 255},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, start_table), 32},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, stop_table), 32},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, shutdown_table), 32},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, drive), 32},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, accel), 32},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, coast), 32},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, engine_start_fn), 29},
        {offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, flags), 128},
        {offsetof(sound_scheme_t, tables) + offsetof(sound_table_t, end_table), 32},
        {offsetof(sound_scheme_t, tables) + offsetof(sound_table_t, next_accel), 32},
        {offsetof(sound_scheme_t, tables) + offsetof(sound_table_t, next_decel), 32},
        {offsetof(sound_scheme_t, tables) + offsetof(sound_table_t, rate_scale), 128},
        {offsetof(sound_scheme_t, extras) + offsetof(sound_extra_t, table), 32},
        {offsetof(sound_scheme_t, extras) + offsetof(sound_extra_t, fn), 29},
        {offsetof(sound_scheme_t, extras) + offsetof(sound_extra_t, dir), 3},
        {offsetof(sound_scheme_t, extras) + offsetof(sound_extra_t, state), 3},
        {offsetof(sound_scheme_t, extras) + offsetof(sound_extra_t, mode), 7},
        {offsetof(sound_scheme_t, extras) + offsetof(sound_extra_t, volume), 101}
    };
    sound_scheme_t scheme, out;
    uint8_t raw[SOUND_STORE_MAX_BYTES];
    sound_store_hdr_t hdr = {{'M','D','S','1'}, 1, sizeof(scheme), 0};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]) + 5; ++i) {
        sound_store_default(&scheme);
        if (i < sizeof(bad) / sizeof(bad[0])) {
            ((uint8_t *)&scheme)[bad[i].offset] = bad[i].value;
        } else {
            size_t string = i - sizeof(bad) / sizeof(bad[0]);
            sound_table_t *last = &scheme.tables[SOUND_MAX_TABLES - 1];
            if (string == 0) { memset(last->name, 'x', sizeof(last->name)); }
            if (string == 1) { memset(last->init[3].file, 'x', SOUND_FILE_MAX); }
            if (string == 2) { memset(last->loop[3].file, 'x', SOUND_FILE_MAX); }
            if (string == 3) { memset(last->end[3].file, 'x', SOUND_FILE_MAX); }
            if (string == 4) {
                scheme.extras[SOUND_MAX_EXTRAS - 1].random_min_ms = 2;
                scheme.extras[SOUND_MAX_EXTRAS - 1].random_max_ms = 1;
            }
        }
        hdr.crc = crc32_bytes((const uint8_t *)&scheme, sizeof(scheme));
        memcpy(raw, &hdr, sizeof(hdr));
        memcpy(raw + sizeof(hdr), &scheme, sizeof(scheme));
        TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_verify(raw, sizeof(raw), &out));
        TEST_ASSERT_EQUAL(SOUND_SCHEME_NONE, out.type);
        FILE *f = fopen(s_file, "wb");
        TEST_ASSERT_NOT_NULL(f);
        fwrite(raw, 1, sizeof(raw), f);
        fclose(f);
        TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_load(s_file, &out));
        TEST_ASSERT_EQUAL(SOUND_SCHEME_NONE, out.type);
        TEST_ASSERT_EQUAL(0, s_storage_leases);
    }
}

static void test_store_rename_failure_preserves_both_copies(void)
{
    sound_scheme_t old, replacement, readback;
    sound_store_default(&old);
    old.type = SOUND_SCHEME_DIESEL;
    replacement = old;
    replacement.type = SOUND_SCHEME_STEAM;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(s_file, &old));
    s_rename_fail = true;
    TEST_ASSERT_EQUAL(ESP_FAIL, sound_store_save(s_file, &replacement));
    s_rename_fail = false;
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_load(s_file, &readback));
    TEST_ASSERT_EQUAL(SOUND_SCHEME_DIESEL, readback.type);
    char tmp[520];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_file);
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_load(tmp, &readback));
    TEST_ASSERT_EQUAL(SOUND_SCHEME_STEAM, readback.type);
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

static void test_pending_generation_sequencer_and_owner_reuse(void)
{
    s_scheme.type = SOUND_SCHEME_DIESEL;
    set_table_files(1, "init.wav", "loop.wav", "end.wav");
    tb_start(1);
    audio_voice_handle_t init = s_engine_handle;
    for (int i = 0; i < 10; ++i) { tb_advance(); }
    TEST_ASSERT_EQUAL(TB_INIT, s_phase);
    TEST_ASSERT_EQUAL(init.generation, s_engine_handle.generation);
    s_av_pending[18] = false;
    s_av_active[18] = true;
    tb_advance();
    TEST_ASSERT_EQUAL(TB_INIT, s_phase);
    mock_audio_complete(18);
    tb_advance();
    TEST_ASSERT_EQUAL(TB_LOOP, s_phase);
    TEST_ASSERT_EQUAL(AUDIO_VOICE_PENDING, audio_voice_get_state(s_engine_handle));

    set_table_files(2, NULL, "fx.wav", NULL);
    fx_play_table(2, false, &s_fn_voice[2]);
    uint8_t reused = s_fn_voice[2];
    mock_audio_complete(reused);
    fx_play_table(2, true, &s_fn_voice[3]);
    TEST_ASSERT_EQUAL(reused, s_fn_voice[3]);
    int calls = s_av_stop_calls;
    fx_voice_stop(&s_fn_voice[2]);
    TEST_ASSERT_EQUAL(calls, s_av_stop_calls);
    TEST_ASSERT_TRUE(fx_running(&s_fn_voice[3]));
    fx_play_table(2, false, &s_fn_voice[2]);
    TEST_ASSERT_NOT_EQUAL(reused, s_fn_voice[2]);
}

static void test_lifecycle_mute_disable_inhibit(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_scheme.type = SOUND_SCHEME_DIESEL;
    s_scheme.engine.sync_motion = true;
    s_scheme.engine.drive[1] = 1;
    set_table_files(1, NULL, "fx.wav", NULL);
    s_scheme.extra_count = 1;
    s_scheme.extras[0] = (sound_extra_t){.table=1, .fn=SOUND_FN_NONE,
                                      .mode=SOUND_MODE_STATE};
    func_binding_t held = {.fn=2, .target_type=FUNC_TARGET_SOUND, .target_id=1,
                           .mode=SOUND_MODE_LOOP_HELD};
    binding_apply(&held, true);
    s_mute_light = true;
    binding_apply(&held, false); /* muted release still cleans up */
    TEST_ASSERT_EQUAL(AUDIO_VOICE_NONE, s_fn_voice[2]);
    int plays = s_av_play_calls;
    extras_tick();
    TEST_ASSERT_EQUAL(plays, s_av_play_calls);
    s_mute_light = false;
    extras_tick();
    TEST_ASSERT_TRUE(fx_running(&s_extra_voice[0]));
    sound_set_speed(60, true);
    sound_engine_power(true);
    sound_tick();
    TEST_ASSERT_EQUAL(ESP_OK, sound_type_set(SOUND_SCHEME_NONE));
    TEST_ASSERT_FALSE(sound_engine_is_on());
    TEST_ASSERT_EQUAL(AUDIO_VOICE_NONE, s_extra_voice[0]);
    TEST_ASSERT_EQUAL(TB_STOP, s_phase);
    TEST_ASSERT_EQUAL(ESP_OK, sound_type_set(SOUND_SCHEME_DIESEL));
    plays = s_av_play_calls;
    sound_tick();
    TEST_ASSERT_EQUAL(plays, s_av_play_calls); /* no old sync-motion resurrection */
    sound_set_speed(61, true);
    sound_tick();
    TEST_ASSERT_TRUE(s_av_play_calls > plays);
    TEST_ASSERT_EQUAL(ESP_OK, sound_set_inhibited(true));
    sound_engine_power(true);
    sound_function(1, true);
    sound_set_speed(62, true);
    plays = s_av_play_calls;
    sound_tick();
    TEST_ASSERT_EQUAL(plays, s_av_play_calls);
    TEST_ASSERT_EQUAL(ESP_OK, sound_set_inhibited(false));
    sound_tick();
    TEST_ASSERT_EQUAL(plays, s_av_play_calls);
    TEST_ASSERT_FALSE(sound_engine_is_on());
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, sound_set_inhibited(true));
    TEST_ASSERT_FALSE(s_inhibited);
    mock_sem_take_fail = 0;
    func_binding_t mute = {.fn=3, .target_type=FUNC_TARGET_LOGIC,
                          .target_id=FUNC_LOGIC_MUTE_LIGHT, .dir=FUNC_DIR_FWD,
                          .state=FUNC_STATE_MOVING};
    s_forward = true;
    s_speed = 60;
    binding_apply(&mute, true);
    TEST_ASSERT_TRUE(s_mute_light);
    s_forward = false;
    s_speed = 0;
    binding_apply(&mute, false); /* Gate changes must not latch a mute forever. */
    TEST_ASSERT_FALSE(s_mute_light);
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
}

static void test_scheme_switch_clear_delete_release_only_owned_voices(void)
{
    s_mounted = true;
    snprintf(s_root, sizeof(s_root), "%s", s_dir);
    char proj[600];
    snprintf(proj, sizeof(proj), "%s/projects", s_dir);
    (void)MKDIR(proj);
    for (int action = 0; action < 3; ++action) {
        TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_create("lifecycle", SOUND_SCHEME_DIESEL));
        set_table_files(1, NULL, "loop.wav", NULL);
        s_scheme.engine.sync_motion = true;
        s_scheme.extra_count = 1;
        s_scheme.extras[0] = (sound_extra_t){.table=1, .fn=SOUND_FN_NONE,
                                          .mode=SOUND_MODE_STATE};
        TEST_ASSERT_NOT_EQUAL(AUDIO_VOICE_NONE, fx_play_table(1, true, &s_fn_voice[2]));
        TEST_ASSERT_NOT_EQUAL(AUDIO_VOICE_NONE, fx_play_table(1, true, &s_extra_voice[0]));
        s_extra_on[0] = 1;
        uint8_t function = s_fn_voice[2], extra = s_extra_voice[0];
        tb_start(1);
        audio_voice_play(19, "secondary.wav", true, 100);
        audio_voice_handle_t unrelated;
        TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&unrelated));
        TEST_ASSERT_EQUAL(ESP_OK,
                          audio_voice_play_owned(&unrelated, "other.wav", true, 100));
        if (action == 0) {
            sound_scheme_t replacement;
            sound_store_default(&replacement);
            replacement.type = SOUND_SCHEME_STEAM;
            TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_set(&replacement));
        } else if (action == 1) {
            TEST_ASSERT_EQUAL(ESP_OK, sound_load_scheme(""));
        } else {
            TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_delete("lifecycle"));
            TEST_ASSERT_EQUAL_STRING("", s_test_active);
        }
        TEST_ASSERT_FALSE(audio_voice_is_active(function));
        TEST_ASSERT_FALSE(audio_voice_is_active(extra));
        TEST_ASSERT_FALSE(audio_voice_is_active(18));
        TEST_ASSERT_FALSE(audio_voice_is_active(19));
        TEST_ASSERT_TRUE(audio_voice_is_active(unrelated.voice));
        TEST_ASSERT_EQUAL(TB_STOP, s_phase);
        TEST_ASSERT_EQUAL(SOUND_TABLE_NONE, s_table);
        TEST_ASSERT_FALSE(s_control_armed);
        TEST_ASSERT_EQUAL(AUDIO_VOICE_NONE, s_fn_voice[2]);
        TEST_ASSERT_EQUAL(AUDIO_VOICE_NONE, s_extra_voice[0]);
        int plays = s_av_play_calls;
        sound_tick();
        TEST_ASSERT_EQUAL(plays, s_av_play_calls);
        audio_voice_release_owned(unrelated);
    }
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

static void test_storage_admission_rejects_store_operations(void)
{
    sound_scheme_t scheme, out;
    sound_store_default(&scheme);
    TEST_ASSERT_EQUAL(ESP_OK, sound_store_save(s_file, &scheme));
    s_storage_blocked = true;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_store_save(s_file, &scheme));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_store_load(s_file, &out));
    TEST_ASSERT_EQUAL(SOUND_SCHEME_NONE, out.type);
    uint8_t raw[SOUND_STORE_MAX_BYTES];
    size_t len = 123;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_store_read(s_file, raw, sizeof(raw), &len));
    TEST_ASSERT_EQUAL(0, len);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sound_store_ensure_dir());
    TEST_ASSERT_EQUAL(0, s_storage_leases);
}

static void test_fixed_point_accel_decay(void)
{
    s_scheme.type = SOUND_SCHEME_DIESEL;
    sound_set_speed(10, true);
    sound_tick();
    TEST_ASSERT_EQUAL(3, s_accel);
    for (int i = 0; i < 40; ++i) { sound_tick(); }
    TEST_ASSERT_EQUAL(0, s_accel);
    TEST_ASSERT_EQUAL(0, s_accel_q);
    sound_set_speed(0, true);
    sound_tick();
    TEST_ASSERT_EQUAL(-3, s_accel);
    for (int i = 0; i < 40; ++i) { sound_tick(); }
    TEST_ASSERT_EQUAL(0, s_accel);
    TEST_ASSERT_EQUAL(0, s_accel_q);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_load_missing_returns_default);
    RUN_TEST(test_load_bad_magic);
    RUN_TEST(test_load_truncated);
    RUN_TEST(test_load_bad_crc);
    RUN_TEST(test_null_args);
    RUN_TEST(test_save_open_failure);
    RUN_TEST(test_save_path_too_long_and_overwrite);
    RUN_TEST(test_store_path);
    RUN_TEST(test_ensure_dir);
    RUN_TEST(test_sound_pure_helpers);
    RUN_TEST(test_sound_accessors);
    RUN_TEST(test_sound_load_scheme);
    RUN_TEST(test_sound_store_verify_read_name);
    RUN_TEST(test_sound_scheme_projects);
    RUN_TEST(test_sound_engine_pick);
    RUN_TEST(test_sound_cv_rate);
    RUN_TEST(test_sound_extras);
    RUN_TEST(test_sound_key_extras);
    RUN_TEST(test_sound_brake);
    RUN_TEST(test_sound_sequencer);
    RUN_TEST(test_sound_table_first_file_paths);
    RUN_TEST(test_sound_fx_guards);
    RUN_TEST(test_sound_status_and_save);
    RUN_TEST(test_sound_scheme_enabled_and_set);
    RUN_TEST(test_sound_task_body);
    RUN_TEST(test_sound_logic_and_sync);
    RUN_TEST(test_sound_lint);
    RUN_TEST(test_sound_function_engine_toggle);
    RUN_TEST(test_sound_binding_modes);
    RUN_TEST(test_sound_tick_and_stop);
    RUN_TEST(test_sound_init_paths);
    RUN_TEST(test_mds_semantic_corpus_crc_trusted);
    RUN_TEST(test_store_rename_failure_preserves_both_copies);
    RUN_TEST(test_pending_generation_sequencer_and_owner_reuse);
    RUN_TEST(test_lifecycle_mute_disable_inhibit);
    RUN_TEST(test_scheme_switch_clear_delete_release_only_owned_voices);
    RUN_TEST(test_storage_admission_rejects_store_operations);
    RUN_TEST(test_fixed_point_accel_decay);
    return UNITY_END();
}
