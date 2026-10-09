#include "sound_graph_store.h"
#include "audio.h"
#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define SG_MKDIR(p) _mkdir(p)
#define SG_FILENO(f) _fileno(f)
#define SG_FSYNC(fd) _commit(fd)
#else
#define SG_MKDIR(p) mkdir((p), 0755)
#define SG_FILENO(f) fileno(f)
#define SG_FSYNC(fd) fsync(fd)
#endif

#define SG_PATH_CAP 256U
#define SG_SCAN_BYTES (32U * 1024U * 1024U)
static atomic_flag s_store_busy = ATOMIC_FLAG_INIT;

static esp_err_t diagnostic(sg_diagnostic_t *d, esp_err_t err, const char *message)
{
    if (d) {
        memset(d, 0, sizeof(*d));
        snprintf(d->code, sizeof(d->code), "validation");
        snprintf(d->message, sizeof(d->message), "%s", message);
    }
    return err;
}

static esp_err_t enter(void)
{
    if (atomic_flag_test_and_set(&s_store_busy)) { return ESP_ERR_TIMEOUT; }
    esp_err_t err = storage_access_begin();
    if (err == ESP_OK && !storage_is_mounted()) {
        storage_access_end(); err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) { atomic_flag_clear(&s_store_busy); }
    return err;
}

static void leave(void)
{
    storage_access_end();
    atomic_flag_clear(&s_store_busy);
}

static uint32_t crc_update(uint32_t crc, const unsigned char *p, size_t n)
{
    while (n--) {
        crc ^= *p++;
        for (unsigned b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t crc_bytes(const void *p, size_t n)
{
    return ~crc_update(UINT32_MAX, p, n);
}

static esp_err_t path_make(char *out, const char *id, uint32_t rev, const char *ext)
{
    int n = rev ? snprintf(out, SG_PATH_CAP, "%s/graphs/%s.%lu.%s",
                           storage_get_root(), id, (unsigned long)rev, ext)
                : snprintf(out, SG_PATH_CAP, "%s/graphs/%s", storage_get_root(), id);
    return n < 0 || (size_t)n >= SG_PATH_CAP ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static esp_err_t ensure_dir(void)
{
    char path[SG_PATH_CAP];
    esp_err_t err = path_make(path, "", 0, NULL);
    if (err != ESP_OK) { return err; }
    if (SG_MKDIR(path) != 0 && errno != EEXIST) { return ESP_FAIL; }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
}

static esp_err_t atomic_write(const char *path, const void *data, size_t len)
{
    char temp[SG_PATH_CAP + 8];
    int n = snprintf(temp, sizeof(temp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(temp)) { return ESP_ERR_INVALID_SIZE; }
    FILE *f = fopen(temp, "wb");
    if (!f) { return ESP_FAIL; }
    bool ok = fwrite(data, 1, len, f) == len;
    int flush = fflush(f);
    int sync = SG_FSYNC(SG_FILENO(f));
    int close = fclose(f);
    if (!ok || flush || sync || close) { remove(temp); return ESP_FAIL; }
    if (rename(temp, path) != 0) {
#if defined(_WIN32) && !defined(ESP_PLATFORM)
        if ((errno != EEXIST && errno != EACCES) ||
            !MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            remove(temp); return ESP_FAIL;
        }
#else
        remove(temp); return ESP_FAIL;
#endif
    }
    return ESP_OK;
}

/* Small records carry their own checksum; JSON itself remains unmodified. */
static esp_err_t record_write(const char *path, const char *payload)
{
    char record[192];
    int n = snprintf(record, sizeof(record), "SGP1 %08lx\n%s",
                     (unsigned long)crc_bytes(payload, strlen(payload)), payload);
    if (n < 0 || (size_t)n >= sizeof(record)) { return ESP_ERR_INVALID_SIZE; }
    return atomic_write(path, record, (size_t)n);
}

static esp_err_t record_read(const char *path, char *payload, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL; }
    char record[192];
    size_t n = fread(record, 1, sizeof(record), f);
    bool ok = !ferror(f) && n < sizeof(record);
    int close = fclose(f);
    if (!ok || close || n < 14 || memcmp(record, "SGP1 ", 5) || record[13] != '\n' ||
        memchr(record, 0, n) || n - 14 >= cap) { return ESP_FAIL; }
    record[n] = 0;
    unsigned long expected; int used = 0;
    if (sscanf(record, "SGP1 %8lx%n", &expected, &used) != 1 || used != 13 ||
        expected != crc_bytes(record + 14, n - 14)) { return ESP_FAIL; }
    memcpy(payload, record + 14, n - 14 + 1);
    return ESP_OK;
}

static esp_err_t meta_read(const char *path, uint32_t *rev, uint32_t *len, uint32_t *crc)
{
    char record[96];
    esp_err_t err = record_read(path, record, sizeof(record));
    if (err != ESP_OK) { return err; }
    unsigned long r, l, c; int used = 0;
    if (sscanf(record, "%lu %lu %lx\n%n", &r, &l, &c, &used) != 3 ||
        record[used] || !r || r > UINT32_MAX || !l || l > SG_MAX_JSON || c > UINT32_MAX) {
        return ESP_FAIL;
    }
    *rev = (uint32_t)r; *len = (uint32_t)l; *crc = (uint32_t)c;
    return ESP_OK;
}

static esp_err_t latest_read(const char *id, uint32_t *rev, uint32_t *len, uint32_t *crc)
{
    char name[SG_ID_CAP + 8], path[SG_PATH_CAP];
    snprintf(name, sizeof(name), "%s.latest", id);
    esp_err_t err = path_make(path, name, 0, NULL);
    return err == ESP_OK ? meta_read(path, rev, len, crc) : err;
}

static esp_err_t json_read(const char *id, uint32_t rev, char **out, size_t *len)
{
    char path[SG_PATH_CAP]; uint32_t actual, size, crc;
    esp_err_t err = path_make(path, id, rev, "commit");
    if (err == ESP_OK) { err = meta_read(path, &actual, &size, &crc); }
    if (err != ESP_OK) { return err; }
    if (actual != rev) { return ESP_FAIL; }
    err = path_make(path, id, rev, "json");
    if (err != ESP_OK) { return err; }
    FILE *f = fopen(path, "rb");
    if (!f) { return ESP_FAIL; }
    char *json = malloc((size_t)size + 1);
    if (!json) { fclose(f); return ESP_ERR_NO_MEM; }
    bool ok = fread(json, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    int close = fclose(f);
    if (!ok || close || crc_bytes(json, size) != crc) { free(json); return ESP_FAIL; }
    json[size] = 0;
    sg_graph_t *graph = malloc(sizeof(*graph));
    if (!graph) { free(json); return ESP_ERR_NO_MEM; }
    err = sg_parse(json, size, graph, NULL);
    if (err == ESP_OK) { err = sg_validate(graph, NULL); }
    if (err == ESP_OK && strcmp(graph->id, id)) { err = ESP_FAIL; }
    free(graph);
    if (err != ESP_OK) { free(json); return err; }
    *out = json; *len = size;
    return ESP_OK;
}

static esp_err_t read_locked(const char *id, uint32_t rev, char **out, size_t *len,
                             uint32_t *actual)
{
    uint32_t size = 0, crc = 0;
    esp_err_t err = ESP_OK;
    bool latest = rev == 0;
    if (latest) { err = latest_read(id, &rev, &size, &crc); }
    if (err == ESP_OK) { err = json_read(id, rev, out, len); }
    if (err == ESP_OK && latest && (*len != size || crc_bytes(*out, *len) != crc)) {
        free(*out); *out = NULL; *len = 0; err = ESP_FAIL;
    }
    if (err == ESP_OK) { *actual = rev; }
    return err;
}

esp_err_t sg_store_read(const char *id, uint32_t rev, char **out, size_t *len, uint32_t *actual)
{
    if (!out || !len || !actual) { return ESP_ERR_INVALID_ARG; }
    *out = NULL; *len = 0; *actual = 0;
    if (!sg_id_valid(id)) { return ESP_ERR_INVALID_ARG; }
    esp_err_t err = enter();
    if (err != ESP_OK) { return err; }
    err = read_locked(id, rev, out, len, actual);
    leave(); return err;
}

/* Scan even uncommitted JSON so an interrupted save never reuses its filename. */
static esp_err_t next_revision(const char *id, uint32_t *next)
{
    char path[SG_PATH_CAP];
    esp_err_t err = path_make(path, "", 0, NULL);
    if (err != ESP_OK) { return err; }
    DIR *dir = opendir(path);
    if (!dir) { return ESP_FAIL; }
    uint32_t max = 0; size_t entries = 0;
    struct dirent *entry;
    while (err == ESP_OK) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) { err = ESP_FAIL; } break; }
        if (++entries > SG_STORE_MAX_ENTRIES) { err = ESP_ERR_INVALID_SIZE; break; }
        size_t n = strlen(id);
        if (strncmp(entry->d_name, id, n) || entry->d_name[n] != '.') { continue; }
        const char *p = entry->d_name + n + 1;
        if (*p < '1' || *p > '9') { continue; }
        char *end; errno = 0;
        unsigned long value = strtoul(p, &end, 10);
        if (errno || value > UINT32_MAX || (*end != '.') ||
            (strcmp(end, ".json") && strcmp(end, ".commit") && strcmp(end, ".json.tmp") &&
             strcmp(end, ".commit.tmp"))) { continue; }
        if (value > max) { max = (uint32_t)value; }
        errno = 0;
    }
    if (closedir(dir) != 0) { err = ESP_FAIL; }
    if (err != ESP_OK) { return err; }
    if (max == UINT32_MAX) { return ESP_ERR_INVALID_SIZE; }
    *next = max + 1; return ESP_OK;
}

esp_err_t sg_store_save(const char *id, uint32_t expected, const char *json, size_t len,
                        uint32_t *newrev, sg_diagnostic_t *diag)
{
    if (diag) { memset(diag, 0, sizeof(*diag)); }
    if (newrev) { *newrev = 0; }
    if (!newrev || !json || !sg_id_valid(id)) { return ESP_ERR_INVALID_ARG; }
    if (!len || len > SG_MAX_JSON) { return ESP_ERR_INVALID_SIZE; }
    sg_graph_t *graph = malloc(sizeof(*graph));
    if (!graph) { return ESP_ERR_NO_MEM; }
    esp_err_t err = sg_parse(json, len, graph, diag);
    if (err == ESP_OK) { err = sg_validate(graph, diag); }
    if (err == ESP_OK && strcmp(graph->id, id)) {
        err = diagnostic(diag, ESP_ERR_INVALID_ARG, "Project ID does not match JSON");
    }
    free(graph);
    if (err != ESP_OK) { return err; }
    err = enter();
    if (err != ESP_OK) { return err; }
    uint32_t current = 0, size = 0, crc = 0, revision = 0;
    err = latest_read(id, &current, &size, &crc);
    if (err == ESP_ERR_NOT_FOUND) { err = ESP_OK; }
    if (err == ESP_OK && current != expected) {
        err = diagnostic(diag, ESP_ERR_INVALID_STATE, "Stale expectedRevision");
    }
    if (err == ESP_OK) { err = ensure_dir(); }
    if (err == ESP_OK) { err = next_revision(id, &revision); }
    char path[SG_PATH_CAP], record[96], name[SG_ID_CAP + 8];
    if (err == ESP_OK) { err = path_make(path, id, revision, "json"); }
    if (err == ESP_OK) { err = atomic_write(path, json, len); }
    snprintf(record, sizeof(record), "%lu %lu %08lx\n", (unsigned long)revision,
             (unsigned long)len, (unsigned long)crc_bytes(json, len));
    if (err == ESP_OK) { err = path_make(path, id, revision, "commit"); }
    if (err == ESP_OK) { err = record_write(path, record); }
    snprintf(name, sizeof(name), "%s.latest", id);
    if (err == ESP_OK) { err = path_make(path, name, 0, NULL); }
    if (err == ESP_OK) { err = record_write(path, record); }
    if (err == ESP_OK) { *newrev = revision; }
    else if (diag && !diag->message[0]) {
        diagnostic(diag, err, "Graph storage write failed; previous latest preserved");
    }
    leave(); return err;
}

static esp_err_t selection_write(const char *payload)
{
    char path[SG_PATH_CAP];
    esp_err_t err = ensure_dir();
    if (err == ESP_OK) { err = path_make(path, "selection", 0, NULL); }
    return err == ESP_OK ? record_write(path, payload) : err;
}

esp_err_t sg_store_select(const char *id, uint32_t revision)
{
    if (!sg_id_valid(id) || !revision) { return ESP_ERR_INVALID_ARG; }
    esp_err_t err = enter();
    if (err != ESP_OK) { return err; }
    char *json = NULL; size_t len; uint32_t actual;
    err = read_locked(id, revision, &json, &len, &actual);
    free(json);
    if (err == ESP_OK) {
        char record[96];
        snprintf(record, sizeof(record), "graph %s %lu\n", id, (unsigned long)revision);
        err = selection_write(record);
    }
    leave(); return err;
}

esp_err_t sg_store_select_legacy(void)
{
    esp_err_t err = enter();
    if (err != ESP_OK) { return err; }
    err = selection_write("legacy\n");
    leave(); return err;
}

esp_err_t sg_store_selection(char *id, size_t cap, uint32_t *rev, bool *selected)
{
    if (!id || !cap || !rev || !selected) { return ESP_ERR_INVALID_ARG; }
    id[0] = 0; *rev = 0; *selected = false;
    esp_err_t err = enter();
    if (err != ESP_OK) { return err; }
    char path[SG_PATH_CAP], record[96];
    err = path_make(path, "selection", 0, NULL);
    if (err == ESP_OK) { err = record_read(path, record, sizeof(record)); }
    if (err == ESP_ERR_NOT_FOUND) { leave(); return ESP_OK; }
    if (err == ESP_OK && !strcmp(record, "legacy\n")) { leave(); return ESP_OK; }
    /* Any existing invalid selection is a graph-mode failure, not legacy. */
    *selected = true;
    if (err == ESP_OK) {
        char project[SG_ID_CAP]; unsigned long revision; int used = 0;
        if (sscanf(record, "graph %31s %lu\n%n", project, &revision, &used) != 2 ||
            record[used] || !sg_id_valid(project) || !revision || revision > UINT32_MAX) {
            err = ESP_FAIL;
        } else if (strlen(project) >= cap) { err = ESP_ERR_INVALID_SIZE; }
        else {
            strcpy(id, project); *rev = (uint32_t)revision;
            char *json = NULL; size_t len; uint32_t actual;
            err = read_locked(project, *rev, &json, &len, &actual);
            free(json);
        }
    }
    leave(); return err;
}

esp_err_t sg_store_list(sg_store_descriptor_t *out, size_t cap, size_t *count)
{
    if (!count || (!out && cap)) { return ESP_ERR_INVALID_ARG; }
    *count = 0;
    esp_err_t err = enter();
    if (err != ESP_OK) { return err; }
    char path[SG_PATH_CAP];
    err = path_make(path, "", 0, NULL);
    DIR *dir = err == ESP_OK ? opendir(path) : NULL;
    if (!dir) { if (err == ESP_OK) { err = errno == ENOENT ? ESP_OK : ESP_FAIL; } leave(); return err; }
    size_t entries = 0, bytes = 0; struct dirent *entry;
    while (err == ESP_OK) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) { err = ESP_FAIL; } break; }
        if (++entries > SG_STORE_MAX_ENTRIES) { err = ESP_ERR_INVALID_SIZE; break; }
        size_t n = strlen(entry->d_name);
        if (n <= 7 || strcmp(entry->d_name + n - 7, ".latest")) { continue; }
        if (n - 7 >= SG_ID_CAP) { err = ESP_FAIL; break; }
        char id[SG_ID_CAP]; memcpy(id, entry->d_name, n - 7); id[n - 7] = 0;
        if (!sg_id_valid(id)) { err = ESP_FAIL; break; }
        if (*count >= cap || *count >= SG_STORE_MAX_PROJECTS) { err = ESP_ERR_INVALID_SIZE; break; }
        char *json = NULL; size_t len = 0; uint32_t revision;
        err = read_locked(id, 0, &json, &len, &revision);
        if (err == ESP_OK && (bytes += len) > SG_SCAN_BYTES) { err = ESP_ERR_INVALID_SIZE; }
        sg_graph_t *graph = err == ESP_OK ? malloc(sizeof(*graph)) : NULL;
        if (err == ESP_OK && !graph) { err = ESP_ERR_NO_MEM; }
        if (err == ESP_OK) { err = sg_parse(json, len, graph, NULL); }
        if (err == ESP_OK) {
            strcpy(out[*count].id, id); strcpy(out[*count].name, graph->name);
            out[*count].revision = revision; ++*count;
        }
        free(graph); free(json);
    }
    if (closedir(dir)) { err = ESP_FAIL; }
    leave(); return err;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }

static esp_err_t asset_inspect_locked(const char *file, sg_asset_t *out, sg_diagnostic_t *diag)
{
    char path[SG_PATH_CAP];
    int n = snprintf(path, sizeof(path), "%s/audio/%s", storage_get_root(), file);
    if (n < 0 || (size_t)n >= sizeof(path)) { return ESP_ERR_INVALID_SIZE; }
    esp_err_t err = audio_inspect_wav(path);
    if (err != ESP_OK) { return diagnostic(diag, err, "Missing or invalid WAV asset"); }
    FILE *f = fopen(path, "rb");
    if (!f) { return diagnostic(diag, ESP_ERR_NOT_FOUND, "Missing WAV asset"); }
    unsigned char header[16];
    bool ok = fseek(f, 0, SEEK_END) == 0;
    long end = ok ? ftell(f) : -1;
    ok = ok && end >= 12 && (uint64_t)end <= UINT32_MAX && fseek(f, 0, SEEK_SET) == 0 &&
         fread(header, 1, 12, f) == 12 && !memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVE", 4);
    uint32_t riff_end = 0, rate = 0, data = 0, channels = 0, bits = 0, align = 0;
    uint32_t spb = 0; /* IMA ADPCM samples per block; 0 for PCM16 */
    bool fmt = false, have_data = false;
    if (ok) {
        uint64_t boundary = (uint64_t)le32(header + 4) + 8;
        ok = boundary == (uint64_t)end && boundary <= LONG_MAX;
        riff_end = (uint32_t)boundary;
    }
    uint32_t pos = 12;
    while (ok && pos < riff_end) {
        if (riff_end - pos < 8 || fseek(f, (long)pos, SEEK_SET) || fread(header, 1, 8, f) != 8) { ok = false; break; }
        uint32_t chunk = le32(header + 4);
        uint64_t next = (uint64_t)pos + 8 + chunk + (chunk & 1U);
        if (next > riff_end) { ok = false; break; }
        if (!memcmp(header, "fmt ", 4)) {
            if (fmt || chunk < 16 || fread(header, 1, 16, f) != 16) { ok = false; break; }
            uint16_t format = le16(header);
            fmt = true; channels = le16(header + 2); rate = le32(header + 4);
            align = le16(header + 12); bits = le16(header + 14);
            if (format == 1) {
                ok = (channels == 1 || channels == 2) && bits == 16 &&
                     rate > 0 && rate <= 192000 && align == channels * 2 &&
                     le32(header + 8) == rate * align;
            } else if (format == 0x0011) { /* WAVE_FORMAT_IMA_ADPCM, mono */
                unsigned char ext[4];
                ok = chunk >= 20 && channels == 1 && bits == 4 && align == 256 &&
                     rate > 0 && rate <= 192000 && fread(ext, 1, sizeof(ext), f) == sizeof(ext);
                if (ok) { spb = le16(ext + 2); ok = spb > 0; }
            } else {
                ok = false;
            }
        } else if (!memcmp(header, "data", 4)) {
            if (have_data) { ok = false; break; }
            have_data = true; data = chunk;
        }
        pos = (uint32_t)next;
    }
    ok = ok && fmt && have_data && data >= align && data % align == 0;
    uint32_t crc = UINT32_MAX;
    unsigned char buffer[512];
    if (ok) { ok = fseek(f, 0, SEEK_SET) == 0; }
    uint32_t total = 0;
    while (ok) {
        size_t got = fread(buffer, 1, sizeof(buffer), f);
        crc = crc_update(crc, buffer, got); total += (uint32_t)got;
        if (got < sizeof(buffer)) { ok = !ferror(f) && total == (uint32_t)end; break; }
    }
    if (fclose(f)) { ok = false; }
    if (!ok) { return diagnostic(diag, ESP_FAIL, "Truncated or unsupported WAV"); }
    memset(out, 0, sizeof(*out)); strcpy(out->file, file);
    out->size = total; out->sampleRate = rate; out->channels = channels; out->bits = bits;
    uint64_t duration;
    if (spb != 0U) {
        duration = (uint64_t)(data / align) * (uint64_t)spb * 1000 / (uint64_t)rate;
    } else {
        duration = (uint64_t)data * 1000 / ((uint64_t)rate * align);
    }
    if (!duration || duration > UINT32_MAX) {
        memset(out, 0, sizeof(*out)); return diagnostic(diag, ESP_FAIL, "Invalid WAV duration");
    }
    out->durationMs = (uint32_t)duration;
    snprintf(out->crc32, sizeof(out->crc32), "%08lx", (unsigned long)~crc);
    return ESP_OK;
}

esp_err_t sg_asset_inspect(const char *file, sg_asset_t *out, sg_diagnostic_t *diag)
{
    if (diag) { memset(diag, 0, sizeof(*diag)); }
    if (!out) { return ESP_ERR_INVALID_ARG; }
    memset(out, 0, sizeof(*out));
    if (!sg_filename_valid(file)) { return ESP_ERR_INVALID_ARG; }
    esp_err_t err = enter();
    if (err != ESP_OK) { return err; }
    err = asset_inspect_locked(file, out, diag);
    leave(); return err;
}

esp_err_t sg_assets_validate(const sg_graph_t *graph, sg_diagnostic_t *diag)
{
    if (!graph) { return ESP_ERR_INVALID_ARG; }
    sg_graph_t *copy = malloc(sizeof(*copy));
    if (!copy) { return ESP_ERR_NO_MEM; }
    memcpy(copy, graph, sizeof(*copy));
    esp_err_t err = sg_validate(copy, diag); free(copy);
    if (err != ESP_OK) { return err; }
    err = enter();
    if (err != ESP_OK) { return err; }
    for (unsigned s = 0; s < graph->state_count && err == ESP_OK; ++s) {
        const char *file = graph->states[s].file;
        if (!file[0]) { continue; }
        unsigned a;
        for (a = 0; a < graph->asset_count; ++a) { if (!strcmp(file, graph->assets[a].file)) { break; } }
        if (a == graph->asset_count) {
            err = diagnostic(diag, ESP_FAIL, "Referenced WAV requires an asset manifest");
            if (diag) { strcpy(diag->id, graph->states[s].id); strcpy(diag->field, "file"); }
            break;
        }
        /* Repeated references only inspect the same file once. */
        bool earlier = false;
        for (unsigned i = 0; i < s; ++i) { if (!strcmp(file, graph->states[i].file)) { earlier = true; break; } }
        if (earlier) { continue; }
        sg_asset_t actual;
        err = asset_inspect_locked(file, &actual, diag);
        const sg_asset_t *expected = &graph->assets[a];
        char *end; unsigned long checksum = strtoul(expected->crc32, &end, 16);
        if (err == ESP_OK && (actual.size != expected->size || actual.sampleRate != expected->sampleRate ||
            actual.channels != expected->channels || actual.bits != expected->bits ||
            actual.durationMs != expected->durationMs || *end ||
            checksum != strtoul(actual.crc32, NULL, 16))) {
            err = diagnostic(diag, ESP_FAIL, "WAV does not match asset manifest");
        }
        if (err != ESP_OK && diag) { strcpy(diag->id, graph->states[s].id); strcpy(diag->field, "file"); }
    }
    leave(); return err;
}

esp_err_t sg_store_file_referenced(const char *file, bool *referenced)
{
    if (!referenced) { return ESP_ERR_INVALID_ARG; }
    *referenced = true;
    if (!sg_filename_valid(file)) { return ESP_ERR_INVALID_ARG; }
    esp_err_t err = enter();
    if (err != ESP_OK) { return err; }
    char path[SG_PATH_CAP];
    err = path_make(path, "", 0, NULL);
    DIR *dir = err == ESP_OK ? opendir(path) : NULL;
    if (!dir) {
        if (err == ESP_OK && errno == ENOENT) { *referenced = false; err = ESP_OK; }
        else if (err == ESP_OK) { err = ESP_FAIL; }
        leave(); return err;
    }
    size_t entries = 0, bytes = 0; bool found = false; struct dirent *entry;
    while (err == ESP_OK) {
        errno = 0; entry = readdir(dir);
        if (!entry) { if (errno) { err = ESP_FAIL; } break; }
        if (++entries > SG_STORE_MAX_ENTRIES) { err = ESP_ERR_INVALID_SIZE; break; }
        size_t n = strlen(entry->d_name);
        bool is_json = n > 5 && !strcmp(entry->d_name + n - 5, ".json");
        bool is_commit = n > 7 && !strcmp(entry->d_name + n - 7, ".commit");
        bool is_latest = n > 7 && !strcmp(entry->d_name + n - 7, ".latest");
        if (!is_json && !is_commit && !is_latest) { continue; }
        const char *dot = strchr(entry->d_name, '.');
        if (!dot || (size_t)(dot - entry->d_name) >= SG_ID_CAP) { err = ESP_FAIL; break; }
        char id[SG_ID_CAP]; size_t ilen = (size_t)(dot - entry->d_name);
        memcpy(id, entry->d_name, ilen); id[ilen] = 0;
        if (!sg_id_valid(id)) { err = ESP_FAIL; break; }
        char *end = NULL; unsigned long rev = 0;
        if (!is_latest) {
            errno = 0; rev = strtoul(dot + 1, &end, 10);
            if (errno || !rev || rev > UINT32_MAX || strcmp(end, is_json ? ".json" : ".commit")) {
                err = ESP_FAIL; break;
            }
        }
        bool orphan = false;
        if (is_json) {
            /* The commit walk checks committed JSON once. Orphan JSON is not
             * selectable, but conservatively protect its asset references. */
            char commit[SG_PATH_CAP]; struct stat st;
            err = path_make(commit, id, (uint32_t)rev, "commit");
            if (err != ESP_OK) { break; }
            if (stat(commit, &st) == 0) { continue; }
            if (errno != ENOENT) { err = ESP_FAIL; break; }
            orphan = true;
        }
        char *json = NULL; size_t len = 0;
        if (is_latest) {
            uint32_t actual;
            err = read_locked(id, 0, &json, &len, &actual);
        } else if (orphan) {
            err = path_make(path, id, (uint32_t)rev, "json");
            struct stat st;
            if (err == ESP_OK && (stat(path, &st) || st.st_size <= 0 ||
                (uint64_t)st.st_size > SG_MAX_JSON)) { err = ESP_FAIL; }
            FILE *f = err == ESP_OK ? fopen(path, "rb") : NULL;
            if (err == ESP_OK && !f) { err = ESP_FAIL; }
            if (err == ESP_OK) {
                len = (size_t)st.st_size; json = malloc(len + 1);
                if (!json) { err = ESP_ERR_NO_MEM; }
                else if (fread(json, 1, len, f) != len || fgetc(f) != EOF || ferror(f)) { err = ESP_FAIL; }
                else { json[len] = 0; }
            }
            if (f && fclose(f)) { err = ESP_FAIL; }
        } else { err = json_read(id, (uint32_t)rev, &json, &len); }
        if (err == ESP_OK && (bytes += len) > SG_SCAN_BYTES) { err = ESP_ERR_INVALID_SIZE; }
        sg_graph_t *graph = err == ESP_OK ? malloc(sizeof(*graph)) : NULL;
        if (err == ESP_OK && !graph) { err = ESP_ERR_NO_MEM; }
        if (err == ESP_OK) { err = sg_parse(json, len, graph, NULL); }
        if (err == ESP_OK && strcmp(graph->id, id)) { err = ESP_FAIL; }
        if (err == ESP_OK) {
            found = found || sg_file_used(graph, file);
            for (unsigned a = 0; a < graph->asset_count; ++a) { found = found || !strcmp(graph->assets[a].file, file); }
        }
        free(graph); free(json);
    }
    if (closedir(dir)) { err = ESP_FAIL; }
    if (err == ESP_OK) { *referenced = found; }
    leave(); return err;
}
