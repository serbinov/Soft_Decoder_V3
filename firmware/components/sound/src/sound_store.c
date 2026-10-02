#include "sound_store.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "storage.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define SOUND_MKDIR(p)   _mkdir(p)
#define SOUND_FILENO(f)  _fileno(f)
#define SOUND_FSYNC(fd)  _commit(fd)
#else
#include <sys/stat.h>
#define SOUND_MKDIR(p)   mkdir((p), 0755)
#define SOUND_FILENO(f)  fileno(f)
#define SOUND_FSYNC(fd)  fsync(fd)
#endif

#define SOUND_STORE_MAGIC   "MDS1"
#define SOUND_STORE_VER     1u
#define SOUND_STORE_PATH_MAX 160

_Static_assert(sizeof(sound_scheme_t) <= 0xFFFFu,
               "sound_scheme_t does not fit the .mds u16 size field");

typedef struct {
    char     magic[4];
    uint16_t ver;
    uint16_t size;
    uint32_t crc;
} sound_store_hdr_t;

_Static_assert(sizeof(sound_store_hdr_t) == SOUND_STORE_HDR_LEN,
               "unexpected .mds header padding");

static uint32_t crc32_bytes(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

esp_err_t sound_store_validate(const sound_scheme_t *s)
{
    if (s == NULL) { return ESP_ERR_INVALID_ARG; }
    const uint8_t *bytes = (const uint8_t *)s;
    if (s->type > SOUND_SCHEME_ELECTRIC || s->table_count > SOUND_MAX_TABLES ||
        s->extra_count > SOUND_MAX_EXTRAS ||
        bytes[offsetof(sound_scheme_t, engine) + offsetof(sound_engine_t, sync_motion)] > 1 ||
        s->engine.engine_start_fn > 28 ||
        (s->engine.flags & ~(SOUND_ENG_SKIP_STOD1 | SOUND_ENG_SKIP_D1TOS)) != 0 ||
        s->engine.start_table >= SOUND_MAX_TABLES ||
        s->engine.stop_table >= SOUND_MAX_TABLES ||
        s->engine.shutdown_table >= SOUND_MAX_TABLES) { return ESP_FAIL; }
    for (size_t i = 0; i < SOUND_ENGINE_STEPS; ++i) {
        if (s->engine.drive[i] >= SOUND_MAX_TABLES ||
            s->engine.accel[i] >= SOUND_MAX_TABLES ||
            s->engine.coast[i] >= SOUND_MAX_TABLES) { return ESP_FAIL; }
    }
    for (size_t i = 0; i < SOUND_MAX_TABLES; ++i) {
        const sound_table_t *t = &s->tables[i];
        /* Do not evaluate a potentially noncanonical _Bool from an MDS file. */
        if (bytes[offsetof(sound_scheme_t, tables) + i * sizeof(*t) +
                  offsetof(sound_table_t, used)] > 1 ||
            memchr(t->name, 0, sizeof(t->name)) == NULL || t->rate_scale > 127 ||
            t->end_table >= SOUND_MAX_TABLES || t->next_accel >= SOUND_MAX_TABLES ||
            t->next_decel >= SOUND_MAX_TABLES) { return ESP_FAIL; }
        for (size_t g = 0; g < SOUND_MAX_GROUPS; ++g) {
            if (memchr(t->init[g].file, 0, SOUND_FILE_MAX) == NULL ||
                memchr(t->loop[g].file, 0, SOUND_FILE_MAX) == NULL ||
                memchr(t->end[g].file, 0, SOUND_FILE_MAX) == NULL) { return ESP_FAIL; }
        }
    }
    for (size_t i = 0; i < SOUND_MAX_EXTRAS; ++i) {
        const sound_extra_t *e = &s->extras[i];
        if (e->table >= SOUND_MAX_TABLES || (e->fn > 28 && e->fn != SOUND_FN_NONE) ||
            e->dir > FUNC_DIR_REV || e->state > FUNC_STATE_STOPPED ||
            e->mode > SOUND_MODE_STATE || e->volume > 100 ||
            e->random_min_ms > e->random_max_ms) { return ESP_FAIL; }
    }
    return ESP_OK;
}

esp_err_t sound_store_default(sound_scheme_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->type = SOUND_SCHEME_NONE;
    out->engine.engine_start_fn = 1;
    return ESP_OK;
}

static esp_err_t store_load(const char *path, sound_scheme_t *out)
{
    if (path == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)sound_store_default(out);

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    sound_store_hdr_t hdr;
    /* Read straight into the caller's buffer: a ~26 KB stack staging copy would
     * overflow the app_main task stack (16 KB). On any failure the (possibly
     * partially written) scheme is reset to the safe default. */
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr.magic, SOUND_STORE_MAGIC, 4) != 0 ||
        hdr.ver != SOUND_STORE_VER ||
        hdr.size != (uint16_t)sizeof(sound_scheme_t) ||
        fread(out, 1, sizeof(*out), f) != sizeof(*out) ||
        hdr.crc != crc32_bytes((const uint8_t *)out, sizeof(*out)) ||
        sound_store_validate(out) != ESP_OK || fgetc(f) != EOF || ferror(f)) {
        fclose(f);
        (void)sound_store_default(out);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}

static esp_err_t store_save(const char *path, const sound_scheme_t *in)
{
    if (path == NULL || in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sound_store_validate(in) != ESP_OK) { return ESP_ERR_INVALID_ARG; }
    /* Write a sibling temp file and rename it over the live scheme: fopen("wb")
     * truncates in place, so a reset mid-write would leave a corrupt .mds while
     * NVS active_scheme still points at it (the loader would then silently fall
     * back to SOUND_SCHEME_NONE). Mirrors settings_manifest_sync(). */
    char tmp[SOUND_STORE_PATH_MAX + 8];
    int pn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (pn < 0 || (size_t)pn >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    sound_store_hdr_t hdr;
    memcpy(hdr.magic, SOUND_STORE_MAGIC, 4);
    hdr.ver = SOUND_STORE_VER;
    hdr.size = (uint16_t)sizeof(sound_scheme_t);
    hdr.crc = crc32_bytes((const uint8_t *)in, sizeof(*in));

    bool ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr)) &&
              (fwrite(in, 1, sizeof(*in), f) == sizeof(*in));
    int rc_flush = fflush(f);
    int rc_sync = SOUND_FSYNC(SOUND_FILENO(f));
    int rc_close = fclose(f);
    if (!ok || rc_flush != 0 || rc_sync != 0 || rc_close != 0) { (void)remove(tmp); return ESP_FAIL; }
    if (rename(tmp, path) != 0) {
        /* Never unlink the live recovery copy after an ESP/LittleFS failure. */
#if defined(_WIN32) && !defined(ESP_PLATFORM)
        /* Host CRT cannot overwrite: use the OS atomic replace operation. */
        if (errno != EEXIST && errno != EACCES) { return ESP_FAIL; }
        if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return ESP_FAIL;
        }
#else
        return ESP_FAIL;
#endif
    }
    return ESP_OK;
}

esp_err_t sound_store_path(char *out, size_t cap, const char *name)
{
    if (out == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    int n = snprintf(out, cap, "%s/%s/%s%s", storage_get_root(), SOUND_PROJECTS_DIR,
                     name, SOUND_STORE_EXT);
    if (n < 0 || (size_t)n >= cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t store_ensure_dir(void)
{
    if (!storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    char dir[SOUND_STORE_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/%s", storage_get_root(), SOUND_PROJECTS_DIR);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (SOUND_MKDIR(dir) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sound_store_verify(const uint8_t *buf, size_t len, sound_scheme_t *out)
{
    if (buf == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len != SOUND_STORE_MAX_BYTES) {
        (void)sound_store_default(out);
        return ESP_FAIL;
    }
    sound_store_hdr_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (memcmp(hdr.magic, SOUND_STORE_MAGIC, 4) != 0 ||
        hdr.ver != SOUND_STORE_VER ||
        hdr.size != (uint16_t)sizeof(sound_scheme_t)) {
        (void)sound_store_default(out);
        return ESP_FAIL;
    }
    /* The payload may be unaligned in an HTTP body: copy straight into the
     * caller's buffer. A ~26 KB stack staging copy would overflow the 16 KB
     * httpd stack that serves POST /api/sound/upload (REV-A2). On a CRC
     * mismatch reset the buffer to the safe default so the caller never keeps
     * unverified data. */
    memcpy(out, buf + sizeof(hdr), sizeof(*out));
    if (hdr.crc != crc32_bytes((const uint8_t *)out, sizeof(*out)) ||
        sound_store_validate(out) != ESP_OK) {
        (void)sound_store_default(out);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t store_read(const char *path, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (path == NULL || buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_len = 0U;
    if (cap < SOUND_STORE_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = fread(buf, 1, SOUND_STORE_MAX_BYTES, f);
    int extra = fgetc(f);
    bool io_err = (ferror(f) != 0);
    fclose(f);
    if (io_err || n != SOUND_STORE_MAX_BYTES || extra != EOF) {
        return ESP_FAIL;
    }
    *out_len = n;
    return ESP_OK;
}

bool sound_store_name_ok(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t n = strlen(name);
    if (n == 0U || n >= SOUND_FILE_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t sound_store_load(const char *path, sound_scheme_t *out)
{
    if (path == NULL || out == NULL) { return ESP_ERR_INVALID_ARG; }
    (void)sound_store_default(out);
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) { return err; }
    err = store_load(path, out);
    storage_access_end();
    return err;
}

esp_err_t sound_store_save(const char *path, const sound_scheme_t *in)
{
    if (path == NULL || in == NULL) { return ESP_ERR_INVALID_ARG; }
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) { return err; }
    err = store_save(path, in);
    storage_access_end();
    return err;
}

esp_err_t sound_store_read(const char *path, uint8_t *buf, size_t cap, size_t *out_len)
{
    if (path == NULL || buf == NULL || out_len == NULL) { return ESP_ERR_INVALID_ARG; }
    *out_len = 0;
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) { return err; }
    err = store_read(path, buf, cap, out_len);
    storage_access_end();
    return err;
}

esp_err_t sound_store_ensure_dir(void)
{
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) { return err; }
    err = store_ensure_dir();
    storage_access_end();
    return err;
}
