#include <unity.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define TEST_MKDIR(p) _mkdir(p)
#define TEST_RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define TEST_MKDIR(p) mkdir((p), 0755)
#define TEST_RMDIR(p) rmdir(p)
#endif

static int injected_rename(const char *from, const char *to);
static int injected_sync(int fd);
#define rename injected_rename
#ifdef _WIN32
#define _commit injected_sync
#else
#define fsync injected_sync
#endif
#include "../../components/sound/src/sound_graph_store.c"
#undef rename
#ifdef _WIN32
#undef _commit
#else
#undef fsync
#endif
#include "../../components/sound/src/sound_graph.c"

static char root[256];
static int leases, sync_calls, rename_calls, fail_sync, fail_rename;
static bool blocked, mounted, audio_failure, native_rename_mode;

bool storage_is_mounted(void) { return mounted; }
const char *storage_get_root(void) { return root; }
esp_err_t storage_access_begin(void)
{
    if (blocked) { return ESP_ERR_INVALID_STATE; }
    ++leases; return ESP_OK;
}
void storage_access_end(void) { --leases; }
esp_err_t audio_inspect_wav(const char *path)
{
    /* Store must hold admission even around this nested validator. */
    TEST_ASSERT_GREATER_THAN_INT(0, leases);
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) { return err; }
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); }
    storage_access_end();
    return audio_failure ? ESP_FAIL : (f ? ESP_OK : ESP_ERR_NOT_FOUND);
}
static int injected_sync(int fd)
{
    TEST_ASSERT_GREATER_THAN_INT(0, leases);
    if (++sync_calls == fail_sync) { errno = EIO; return -1; }
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}
static int injected_rename(const char *from, const char *to)
{
    TEST_ASSERT_GREATER_THAN_INT(0, leases);
    if (++rename_calls == fail_rename) { errno = EIO; return -1; }
#ifdef _WIN32
    if (native_rename_mode) { return rename(from, to); }
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { return 0; }
    errno = EIO; return -1;
#else
    return rename(from, to);
#endif
}

static void clean_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { continue; }
        char p[512]; snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        remove(p);
    }
    closedir(d); TEST_RMDIR(path);
}
void setUp(void)
{
    const char *temp = getenv("TEMP");
    snprintf(root, sizeof(root), "%s/sg_store_unit", temp ? temp : ".");
    char p[300]; snprintf(p, sizeof(p), "%s/graphs", root); clean_dir(p);
    snprintf(p, sizeof(p), "%s/audio", root); clean_dir(p);
    TEST_RMDIR(root); TEST_MKDIR(root); TEST_MKDIR(p);
    leases = sync_calls = rename_calls = fail_sync = fail_rename = 0;
    blocked = audio_failure = native_rename_mode = false; mounted = true;
    atomic_flag_clear(&s_store_busy);
}
void tearDown(void)
{
    TEST_ASSERT_EQUAL_INT(0, leases);
    char p[300]; snprintf(p, sizeof(p), "%s/graphs", root); clean_dir(p);
    snprintf(p, sizeof(p), "%s/audio", root); clean_dir(p); TEST_RMDIR(root);
}

static char *fixture(const char *id, const char *file, const sg_asset_t *asset)
{
    char *json = malloc(4096), manifest[512] = "";
    TEST_ASSERT_NOT_NULL(json);
    if (asset) {
        snprintf(manifest, sizeof(manifest), "{\"file\":\"%s\",\"size\":%lu,\"crc32\":\"%s\","
                 "\"sampleRate\":%lu,\"channels\":%lu,\"bits\":%lu,\"durationMs\":%lu}",
                 asset->file, (unsigned long)asset->size, asset->crc32,
                 (unsigned long)asset->sampleRate, (unsigned long)asset->channels,
                 (unsigned long)asset->bits, (unsigned long)asset->durationMs);
    }
    snprintf(json, 4096,
             " {\n\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"%s\",\"name\":\"Project %s\","
             "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":2,"
             "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000},"
             "{\"id\":\"run\",\"name\":\"Run\",\"file\":\"%s\",\"loop\":true,\"volume\":80,\"rate\":1000}],"
             "\"transitions\":[{\"id\":\"start\",\"source\":\"off\",\"target\":\"run\",\"priority\":1,"
             "\"timing\":\"immediate\",\"condition\":{\"type\":\"engine_on\"}}],\"effects\":[],"
             "\"assets\":[%s],\"editor\":{\"viewport\":{\"x\":1.234,\"y\":-23,\"zoom\":0.75},"
             "\"note\":\"opaque metadata\",\"nested\":[true,null,{\"color\":\"blue\"}]}} \n",
             id, id, file, manifest);
    return json;
}
static void write_bytes(const char *name, const void *data, size_t n)
{
    char path[512]; snprintf(path, sizeof(path), "%s/%s", root, name);
    FILE *f = fopen(path, "wb"); TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT(n, fwrite(data, 1, n, f)); TEST_ASSERT_EQUAL_INT(0, fclose(f));
}
static uint32_t save_json(const char *id, uint32_t expected, const char *json)
{
    uint32_t revision = 0; sg_diagnostic_t diag;
    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_OK, sg_store_save(id, expected, json, strlen(json), &revision, &diag), diag.message);
    return revision;
}
static void assert_read(const char *id, uint32_t revision, uint32_t expected, const char *json)
{
    char *out = NULL; size_t len = 0; uint32_t actual = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_read(id, revision, &out, &len, &actual));
    TEST_ASSERT_EQUAL_UINT32(expected, actual); TEST_ASSERT_EQUAL_UINT(strlen(json), len);
    TEST_ASSERT_EQUAL_MEMORY(json, out, len); TEST_ASSERT_EQUAL_INT(0, out[len]); free(out);
}

static void test_exact_roundtrip_and_historical_read(void)
{
    char *json = fixture("diesel", "", NULL);
    TEST_ASSERT_EQUAL_UINT32(1, save_json("diesel", 0, json));
    assert_read("diesel", 0, 1, json);
    char *changed = fixture("diesel", "", NULL); *strstr(changed, "opaque") = 'O';
    TEST_ASSERT_EQUAL_UINT32(2, save_json("diesel", 1, changed));
    assert_read("diesel", 1, 1, json); assert_read("diesel", 0, 2, changed);
    free(json); free(changed);
}
static void test_stale_revision_and_id_checks(void)
{
    char *json = fixture("a", "", NULL); save_json("a", 0, json);
    uint32_t revision = 99;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, sg_store_save("a", 0, json, strlen(json), &revision, NULL));
    TEST_ASSERT_EQUAL_UINT32(0, revision);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, sg_store_save("other", 0, json, strlen(json), &revision, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, sg_store_save("../a", 0, json, strlen(json), &revision, NULL));
    assert_read("a", 0, 1, json); free(json);
}
static void test_invalid_json_version_and_limits(void)
{
    char *json = fixture("a", "", NULL); uint32_t revision;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_save("a", 0, json, strlen(json) - 4, &revision, NULL));
    char *version = strstr(json, "schemaVersion\":1"); version[strlen("schemaVersion\":")] = '2';
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_save("a", 0, json, strlen(json), &revision, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, sg_store_save("a", 0, json, SG_MAX_JSON + 1, &revision, NULL));
    free(json);
}
static void test_selection_two_projects_and_legacy(void)
{
    char id[SG_ID_CAP]; uint32_t revision; bool selected;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_FALSE(selected);
    char *a = fixture("a", "", NULL), *b = fixture("b", "", NULL);
    save_json("a", 0, a); save_json("b", 0, b);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_select("a", 1));
    save_json("a", 1, a); /* Save must not alter selection. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected));
    TEST_ASSERT_TRUE(selected); TEST_ASSERT_EQUAL_STRING("a", id); TEST_ASSERT_EQUAL_UINT32(1, revision);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_select("b", 1));
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_EQUAL_STRING("b", id);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_select_legacy());
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_FALSE(selected);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_FALSE(selected);
    free(a); free(b);
}
static void test_corruption_is_safe_failure(void)
{
    char *json = fixture("a", "", NULL); save_json("a", 0, json); sg_store_select("a", 1);
    char id[SG_ID_CAP]; uint32_t revision; bool selected;
    *strstr(json, "opaque") = 'O'; write_bytes("graphs/a.1.json", json, strlen(json));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_TRUE(selected);
    char *out = (char *)1; size_t len; uint32_t actual;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_read("a", 0, &out, &len, &actual)); TEST_ASSERT_NULL(out); TEST_ASSERT_EQUAL_UINT(0, len);
    write_bytes("graphs/selection", "legacy\n", 7);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_TRUE(selected);
    free(json);
}
static void test_truncated_saved_json_and_pointer(void)
{
    char *json = fixture("a", "", NULL); save_json("a", 0, json);
    write_bytes("graphs/a.1.json", json, 20);
    char *out; size_t len; uint32_t actual;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_read("a", 1, &out, &len, &actual));
    write_bytes("graphs/a.latest", "SGP1 00", 7);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_read("a", 0, &out, &len, &actual)); free(json);
}
static void test_failures_preserve_latest_and_skip_orphans(void)
{
    char *json = fixture("a", "", NULL); save_json("a", 0, json);
    uint32_t rev; fail_rename = rename_calls + 3; /* latest replacement */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_save("a", 1, json, strlen(json), &rev, NULL));
    assert_read("a", 0, 1, json); fail_rename = 0;
    TEST_ASSERT_EQUAL_UINT32(3, save_json("a", 1, json));
    fail_sync = sync_calls + 2; /* commit marker flush failure leaves JSON orphan */
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_save("a", 3, json, strlen(json), &rev, NULL));
    assert_read("a", 0, 3, json); fail_sync = 0;
    TEST_ASSERT_EQUAL_UINT32(5, save_json("a", 3, json));
    bool referenced;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_file_referenced("other.wav", &referenced)); TEST_ASSERT_FALSE(referenced);
    free(json);
}
static void test_every_save_write_failure(void)
{
    char *json = fixture("a", "", NULL); uint32_t current = save_json("a", 0, json), rev;
    for (int stage = 1; stage <= 3; ++stage) {
        fail_sync = sync_calls + stage;
        TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_save("a", current, json, strlen(json), &rev, NULL));
        assert_read("a", 0, current, json); fail_sync = 0;
        current = save_json("a", current, json);
        fail_rename = rename_calls + stage;
        TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_save("a", current, json, strlen(json), &rev, NULL));
        assert_read("a", 0, current, json); fail_rename = 0;
        current = save_json("a", current, json);
    }
    free(json);
}
static void test_selection_failures_and_restoration(void)
{
    char *a = fixture("a", "", NULL), *b = fixture("b", "", NULL);
    save_json("a", 0, a); save_json("b", 0, b); sg_store_select("a", 1);
    char id[SG_ID_CAP]; uint32_t revision; bool selected;
    fail_sync = sync_calls + 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_select("b", 1)); fail_sync = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_EQUAL_STRING("a", id);
    fail_rename = rename_calls + 1;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_select_legacy()); fail_rename = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_EQUAL_STRING("a", id);
    sg_store_select("b", 1); sg_store_select("a", 1);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_EQUAL_STRING("a", id);
    free(a); free(b);
}
static void test_bounded_list_and_ignored_temp(void)
{
    char *a = fixture("a", "", NULL), *b = fixture("b", "", NULL); save_json("a", 0, a); save_json("b", 0, b);
    write_bytes("graphs/trash.json.tmp", "bad", 3); write_bytes("graphs/orphan.9.json", "bad", 3);
    sg_store_descriptor_t list[2]; size_t count;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_list(list, 2, &count)); TEST_ASSERT_EQUAL_UINT(2, count);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, sg_store_list(list, 1, &count));
    free(a); free(b);
}
static void test_storage_admission_and_serialization(void)
{
    char *json = fixture("a", "", NULL); uint32_t revision; char *out; size_t len;
    blocked = true;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, sg_store_save("a", 0, json, strlen(json), &revision, NULL));
    blocked = false; mounted = false;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, sg_store_read("a", 0, &out, &len, &revision));
    mounted = true; atomic_flag_test_and_set(&s_store_busy);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, sg_store_save("a", 0, json, strlen(json), &revision, NULL));
    atomic_flag_clear(&s_store_busy); save_json("a", 0, json); free(json);
}
static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void make_wav(void)
{
    unsigned char wav[44 + 2206] = {0};
    memcpy(wav, "RIFF", 4); put32(wav + 4, sizeof(wav) - 8); memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16); wav[20] = 1; wav[22] = 1; put32(wav + 24, 22050); put32(wav + 28, 44100);
    wav[32] = 2; wav[34] = 16; memcpy(wav + 36, "data", 4); put32(wav + 40, sizeof(wav) - 44);
    for (size_t i = 44; i < sizeof(wav); ++i) { wav[i] = (unsigned char)i; }
    write_bytes("audio/loop.wav", wav, sizeof(wav));
}
static void test_inspect_crc_and_manifest_validation(void)
{
    make_wav(); sg_asset_t asset; sg_diagnostic_t diag;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_asset_inspect("loop.wav", &asset, &diag));
    TEST_ASSERT_EQUAL_UINT32(2250, asset.size); TEST_ASSERT_EQUAL_UINT32(22050, asset.sampleRate);
    TEST_ASSERT_EQUAL_UINT32(1, asset.channels); TEST_ASSERT_EQUAL_UINT32(16, asset.bits); TEST_ASSERT_EQUAL_UINT32(50, asset.durationMs);
    TEST_ASSERT_EQUAL_STRING("loop.wav", asset.file); TEST_ASSERT_EQUAL_STRING("16fa94f8", asset.crc32);
    char *json = fixture("a", "loop.wav", &asset); sg_graph_t *g = malloc(sizeof(*g));
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_parse(json, strlen(json), g, &diag));
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_assets_validate(g, &diag));
    g->assets[0].crc32[0] = g->assets[0].crc32[0] == '0' ? '1' : '0';
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_assets_validate(g, &diag));
    for (unsigned field = 0; field < 5; ++field) {
        g->assets[0] = asset;
        uint32_t *dimensions[] = {&g->assets[0].size, &g->assets[0].sampleRate,
            &g->assets[0].channels, &g->assets[0].bits, &g->assets[0].durationMs};
        ++*dimensions[field];
        TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_assets_validate(g, &diag));
        TEST_ASSERT_EQUAL_STRING("run", diag.id); TEST_ASSERT_EQUAL_STRING("file", diag.field);
    }
    free(g); free(json);
}
static void test_missing_assets_and_missing_manifest_drafts(void)
{
    sg_asset_t asset = {.size=2250,.sampleRate=22050,.channels=1,.bits=16,.durationMs=50};
    strcpy(asset.file, "loop.wav"); strcpy(asset.crc32, "12345678");
    char *json = fixture("a", "loop.wav", &asset); save_json("a", 0, json);
    sg_graph_t *g = malloc(sizeof(*g)); TEST_ASSERT_EQUAL_INT(ESP_OK, sg_parse(json, strlen(json), g, NULL));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_assets_validate(g, NULL)); free(json);
    json = fixture("a", "loop.wav", NULL); save_json("a", 1, json);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_parse(json, strlen(json), g, NULL)); TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_assets_validate(g, NULL));
    free(g); free(json);
}
static void test_invalid_wav_and_paths(void)
{
    sg_asset_t asset;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, sg_asset_inspect("../loop.wav", &asset, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, sg_asset_inspect("/loop.wav", &asset, NULL));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_asset_inspect("loop.wav", &asset, NULL));
    write_bytes("audio/loop.wav", "RIFF", 4); TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_asset_inspect("loop.wav", &asset, NULL));
    make_wav(); audio_failure = true; TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_asset_inspect("loop.wav", &asset, NULL)); audio_failure = false;
    char path[512]; snprintf(path, sizeof(path), "%s/audio/loop.wav", root); FILE *f = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(f); fseek(f, 40, SEEK_SET); unsigned char value[4] = {0xff,0xff,0xff,0x7f}; fwrite(value, 1, 4, f); fclose(f);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_asset_inspect("loop.wav", &asset, NULL));
}
static void test_historical_references_and_fail_closed(void)
{
    make_wav(); sg_asset_t asset; sg_asset_inspect("loop.wav", &asset, NULL);
    char *json = fixture("a", "loop.wav", &asset); save_json("a", 0, json);
    char *silent = fixture("a", "", NULL); save_json("a", 1, silent);
    bool referenced = false;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_file_referenced("loop.wav", &referenced)); TEST_ASSERT_TRUE(referenced);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_file_referenced("other.wav", &referenced)); TEST_ASSERT_FALSE(referenced);
    write_bytes("graphs/a.1.json", "{}", 2);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_file_referenced("other.wav", &referenced)); TEST_ASSERT_TRUE(referenced);
    blocked = true; TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_file_referenced("other.wav", &referenced)); TEST_ASSERT_TRUE(referenced);
    free(json); free(silent);
}
static void test_missing_revision_fails_reference_scan(void)
{
    char *json = fixture("a", "", NULL); save_json("a", 0, json);
    char path[512]; snprintf(path, sizeof(path), "%s/graphs/a.1.json", root);
    TEST_ASSERT_EQUAL_INT(0, remove(path));
    bool referenced = false;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_file_referenced("other.wav", &referenced)); TEST_ASSERT_TRUE(referenced);
    free(json);
}
static void test_zero_duration_and_malformed_wav_chunks(void)
{
    const unsigned offsets[] = {4, 20, 22, 24, 28, 32, 34, 40};
    const uint32_t values[] = {8, 3, 3, 0, 1, 4, 8, 0};
    char path[512]; snprintf(path, sizeof(path), "%s/audio/loop.wav", root);
    sg_asset_t asset;
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        make_wav(); FILE *f = fopen(path, "r+b"); TEST_ASSERT_NOT_NULL(f);
        unsigned char value[4]; put32(value, values[i]);
        TEST_ASSERT_EQUAL_INT(0, fseek(f, offsets[i], SEEK_SET));
        TEST_ASSERT_EQUAL_UINT(4, fwrite(value, 1, 4, f)); fclose(f);
        TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_asset_inspect("loop.wav", &asset, NULL));
    }
    /* Structurally complete two-frame PCM sample rounds to zero milliseconds. */
    unsigned char short_wav[48] = {0};
    memcpy(short_wav, "RIFF", 4); put32(short_wav + 4, 40); memcpy(short_wav + 8, "WAVEfmt ", 8);
    put32(short_wav + 16, 16); short_wav[20] = short_wav[22] = 1;
    put32(short_wav + 24, 22050); put32(short_wav + 28, 44100); short_wav[32] = 2; short_wav[34] = 16;
    memcpy(short_wav + 36, "data", 4); put32(short_wav + 40, 4);
    write_bytes("audio/loop.wav", short_wav, sizeof(short_wav));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_asset_inspect("loop.wav", &asset, NULL));
}
static void test_reference_and_revision_scan_bounds(void)
{
    char *json = fixture("a", "", NULL); save_json("a", 0, json);
    for (unsigned i = 0; i < SG_STORE_MAX_ENTRIES; ++i) {
        char path[80]; snprintf(path, sizeof(path), "graphs/noise%u.tmp", i); write_bytes(path, "", 0);
    }
    bool referenced = false; uint32_t revision;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, sg_store_file_referenced("other.wav", &referenced)); TEST_ASSERT_TRUE(referenced);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, sg_store_save("a", 1, json, strlen(json), &revision, NULL));
    assert_read("a", 0, 1, json); free(json);
}
static void test_invalid_arguments_and_missing_selection_target(void)
{
    char *out; size_t len; uint32_t revision; bool selected;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, sg_store_read("../a", 0, &out, &len, &revision));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, sg_store_select("a", 0));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, sg_store_select("a", 1));
    char id[SG_ID_CAP]; TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_FALSE(selected);
    char *json = fixture("a", "", NULL); save_json("a", 0, json);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_select("a", 1));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, sg_store_selection(id, 1, &revision, &selected)); TEST_ASSERT_TRUE(selected);
    free(json);
}
static void test_native_atomic_pointer_replacement(void)
{
    char *json = fixture("a", "", NULL); save_json("a", 0, json);
    /* Windows CRT rename cannot replace; production must use MoveFileEx.
     * POSIX rename replaces without first unlinking the recovery pointer. */
    native_rename_mode = true;
    TEST_ASSERT_EQUAL_UINT32(2, save_json("a", 1, json)); assert_read("a", 0, 2, json);
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_select("a", 1));
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_select("a", 2));
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_select_legacy());
    char id[SG_ID_CAP]; uint32_t revision; bool selected;
    TEST_ASSERT_EQUAL_INT(ESP_OK, sg_store_selection(id, sizeof(id), &revision, &selected)); TEST_ASSERT_FALSE(selected);
    free(json);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exact_roundtrip_and_historical_read);
    RUN_TEST(test_stale_revision_and_id_checks);
    RUN_TEST(test_invalid_json_version_and_limits);
    RUN_TEST(test_selection_two_projects_and_legacy);
    RUN_TEST(test_corruption_is_safe_failure);
    RUN_TEST(test_truncated_saved_json_and_pointer);
    RUN_TEST(test_failures_preserve_latest_and_skip_orphans);
    RUN_TEST(test_every_save_write_failure);
    RUN_TEST(test_selection_failures_and_restoration);
    RUN_TEST(test_bounded_list_and_ignored_temp);
    RUN_TEST(test_storage_admission_and_serialization);
    RUN_TEST(test_inspect_crc_and_manifest_validation);
    RUN_TEST(test_missing_assets_and_missing_manifest_drafts);
    RUN_TEST(test_invalid_wav_and_paths);
    RUN_TEST(test_historical_references_and_fail_closed);
    RUN_TEST(test_missing_revision_fails_reference_scan);
    RUN_TEST(test_zero_duration_and_malformed_wav_chunks);
    RUN_TEST(test_reference_and_revision_scan_bounds);
    RUN_TEST(test_invalid_arguments_and_missing_selection_target);
    RUN_TEST(test_native_atomic_pointer_replacement);
    return UNITY_END();
}
