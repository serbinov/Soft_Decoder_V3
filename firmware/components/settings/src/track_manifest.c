/* Metadata manifest stored next to the sound files on the external storage.
 *
 * The track list (slot, file, label, category, enabled) and the function map
 * live in NVS. A full chip erase wipes NVS but leaves the sounds on the W25Q128,
 * so this manifest lets the firmware restore the names/categories/mapping after
 * such a reset.
 *
 * Format (text, one record per line, ';'-separated; file/label are last so a
 * label may contain spaces). Version 1 carries only tracks + the legacy
 * function map; version 2 adds canonical bindings; version 3 adds explicit
 * track/binding counts (0 = intentionally empty, -1 = missing). A v1 file is
 * read fine (unknown records are ignored) and an old parser would skip the
 * new 'B;' records, so v1/v2 are mutually backwards compatible:
 *   AURA-TRACKS 3
 *   C;<tracks_count>;<bindings_count>
 *   T;<slot>;<cat>;<enabled>;<file>;<label>
 *   F;<idx>;<slot_a>;<slot_b>;<aux_mask>;<dir>;<speed>
 *   B;<idx>;<fn>;<type>;<id>;<dir>;<state>;<mode>;<flags>;<short_table>;<short_ms>;<min_ms>;<fade_ms>
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#if defined(_WIN32) && !defined(ESP_PLATFORM)
#include <windows.h>
#endif

#include "esp_log.h"
#include "storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "manifest";

#ifndef MANIFEST_DIR
#define MANIFEST_DIR     "/userdata/audio"
#endif
#define MANIFEST_PATH    MANIFEST_DIR "/tracks.txt"
#define MANIFEST_MAGIC   "AURA-TRACKS"
#define MANIFEST_VERSION 3
#define MANIFEST_LINE_MAX 256

/* The manifest read/write workspaces are several kilobytes (tracks + cats +
 * function map + bindings). Keeping them on the stack overflows the 8 KB
 * provisioning-listener stack and the 16 KB app_main stack during NVS-loss
 * recovery, so they are heap-allocated per call (REV-S1). */
typedef struct {
    settings_track_t    tracks[SETTINGS_MAX_TRACKS];
    uint8_t             cats[SETTINGS_MAX_TRACKS];
    settings_func_map_t fmap[SETTINGS_FUNC_MAP_COUNT];
    func_binding_t      binds[FUNC_BIND_MAX];
} manifest_ws_t;

/* Set while restoring from the manifest so the NVS writes it performs do not
 * immediately rewrite the file being read. */
static bool s_manifest_loading;
static portMUX_TYPE s_manifest_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_manifest_owner;

bool settings_manifest_write_allowed(void)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_manifest_mux);
    bool allowed = !s_manifest_loading || s_manifest_owner == caller;
    portEXIT_CRITICAL(&s_manifest_mux);
    return allowed;
}

static esp_err_t manifest_begin(bool loading)
{
    TaskHandle_t caller = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_manifest_mux);
    if (s_manifest_owner != NULL) {
        bool restoring_here = s_manifest_loading && s_manifest_owner == caller && !loading;
        portEXIT_CRITICAL(&s_manifest_mux);
        return restoring_here ? ESP_ERR_NOT_FOUND : ESP_ERR_TIMEOUT;
    }
    s_manifest_owner = caller;
    s_manifest_loading = loading;
    portEXIT_CRITICAL(&s_manifest_mux);
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_manifest_mux);
        s_manifest_owner = NULL;
        s_manifest_loading = false;
        portEXIT_CRITICAL(&s_manifest_mux);
    }
    return err;
}

static void manifest_end(void)
{
    storage_access_end();
    portENTER_CRITICAL(&s_manifest_mux);
    s_manifest_owner = NULL;
    s_manifest_loading = false;
    portEXIT_CRITICAL(&s_manifest_mux);
}

static void sanitize_field(char *s)
{
    for (; *s != '\0'; ++s) {
        if (*s == ';' || *s == '\r' || *s == '\n') {
            *s = ' ';
        }
    }
}

/* Split line on ';' into at most max fields; returns the field count. */
static int split_semicolon(char *line, char *fields[], int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *sep = strchr(p, ';');
        if (sep == NULL) {
            break;
        }
        *sep = '\0';
        p = sep + 1;
    }
    return n;
}

static esp_err_t manifest_sync_owned(void)
{
    manifest_ws_t *ws = malloc(sizeof(*ws));
    if (ws == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t tcount = 0;
    esp_err_t err = settings_tracks_load(ws->tracks, &tcount);
    bool tracks_present = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) { free(ws); return err; }

    size_t ccount = 0;
    err = settings_track_cats_load(ws->cats, &ccount);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) { free(ws); return err; }

    size_t fcount = 0;
    err = settings_func_map_load(ws->fmap, &fcount);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) { free(ws); return err; }

    size_t bcount = 0;
    err = settings_func_bind_load(ws->binds, &bcount);
    bool bindings_present = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) { free(ws); return err; }

    /* Write to a temp file and rename it over the real manifest: fopen("w")
     * truncates the live file, so a reset or power loss mid-write would leave a
     * truncated manifest as the only recovery copy. */
    char tmp[sizeof(MANIFEST_PATH) + 8];
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", MANIFEST_PATH);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) {
        free(ws);
        return ESP_ERR_NOT_FOUND; /* storage not mounted: nothing to persist */
    }

    bool ok = fprintf(f, "%s %d\n", MANIFEST_MAGIC, MANIFEST_VERSION) >= 0;
    /* v3 declares emptiness explicitly; v1/v2 without T/B remain legacy-missing. */
    ok = ok && fprintf(f, "C;%d;%d\n", tracks_present ? (int)tcount : -1,
                        bindings_present ? (int)bcount : -1) >= 0;

    for (size_t i = 0; i < tcount && i < SETTINGS_MAX_TRACKS; ++i) {
        char file[SETTINGS_TRACK_FILE_MAX];
        char label[SETTINGS_TRACK_LABEL_MAX];
        (void)snprintf(file, sizeof(file), "%s", ws->tracks[i].file);
        (void)snprintf(label, sizeof(label), "%s", ws->tracks[i].label);
        sanitize_field(file);
        sanitize_field(label);
        uint8_t cat = (ws->tracks[i].slot >= 1U && ws->tracks[i].slot <= ccount)
                          ? ws->cats[ws->tracks[i].slot - 1U]
                          : (uint8_t)SETTINGS_TRACK_CAT_DEFAULT_SLOT(ws->tracks[i].slot);
        ok = ok && (fprintf(f, "T;%u;%u;%u;%s;%s\n", (unsigned)ws->tracks[i].slot,
                            (unsigned)cat, ws->tracks[i].enabled ? 1U : 0U, file, label) >= 0);
    }
    for (size_t i = 0; i < fcount && i < SETTINGS_FUNC_MAP_COUNT; ++i) {
        ok = ok && (fprintf(f, "F;%u;%u;%u;%u;%u;%u\n", (unsigned)i,
                            (unsigned)ws->fmap[i].slot_a, (unsigned)ws->fmap[i].slot_b,
                            (unsigned)ws->fmap[i].aux_mask, (unsigned)ws->fmap[i].dir,
                            (unsigned)ws->fmap[i].speed) >= 0);
    }
    for (size_t i = 0; i < bcount && i < FUNC_BIND_MAX; ++i) {
        const func_binding_t *b = &ws->binds[i];
        ok = ok && (fprintf(f, "B;%u;%u;%u;%u;%u;%u;%u;%u;%u;%u;%u;%u\n", (unsigned)i,
                            (unsigned)b->fn, (unsigned)b->target_type,
                            (unsigned)b->target_id, (unsigned)b->dir, (unsigned)b->state,
                            (unsigned)b->mode, (unsigned)b->flags, (unsigned)b->short_table,
                            (unsigned)b->short_ms, (unsigned)b->min_ms,
                            (unsigned)b->fade_ms) >= 0);
    }

    int rc_flush = fflush(f);
    int rc_sync = fsync(fileno(f));
    int rc_close = fclose(f);
    free(ws);
    if (!ok || rc_flush != 0 || rc_sync != 0 || rc_close != 0) { (void)remove(tmp); return ESP_FAIL; }
    if (rename(tmp, MANIFEST_PATH) != 0) {
        /* LittleFS rename replaces atomically. Never unlink the valid live
         * copy on any target failure. Windows uses its host-only replace API. */
#if defined(_WIN32) && !defined(ESP_PLATFORM)
        if (!MoveFileExA(tmp, MANIFEST_PATH, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return ESP_FAIL;
#else
        return ESP_FAIL;
#endif
    }
    return ESP_OK;
}

esp_err_t settings_manifest_sync(void)
{
    esp_err_t err = manifest_begin(false);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK; /* same-task restore setter */
    if (err != ESP_OK) { settings_manifest_result(err); return err; }
    bool recovering = false;
    err = settings_recovery_pending(&recovering);
    if (err == ESP_OK && recovering) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = settings_metadata_lock();
    if (err == ESP_OK) {
        err = manifest_sync_owned();
        settings_manifest_result(err);
        settings_metadata_unlock();
    } else settings_manifest_result(err);
    manifest_end();
    return err;
}

/* Remove the on-storage metadata backup so a factory reset is not undone by
 * boot recovery (which would otherwise restore names/categories/function map
 * from the file that survives the NVS erase). A missing file or unmounted
 * storage is a no-op success. */
esp_err_t settings_manifest_remove(void)
{
    esp_err_t err = manifest_begin(false);
    if (err == ESP_ERR_NOT_FOUND) return ESP_OK;      /* same-task restore setter */
    if (err == ESP_ERR_INVALID_STATE) return ESP_OK;  /* storage unavailable: nothing to clear */
    if (err != ESP_OK) return err;
    err = settings_metadata_lock();
    if (err == ESP_OK) {
        if (remove(MANIFEST_PATH) != 0 && errno != ENOENT) err = ESP_FAIL;
        (void)remove(MANIFEST_PATH ".tmp");
        settings_manifest_result(ESP_OK);
        settings_metadata_unlock();
    }
    manifest_end();
    return err;
}

static esp_err_t manifest_load_owned(void)
{
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Aggregates on the heap (see manifest_ws_t) to keep this frame small
     * (REV-S1). */
    manifest_ws_t *ws = malloc(sizeof(*ws));
    if (ws == NULL) {
        (void)fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t tcount = 0;
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        ws->cats[i] = (uint8_t)SETTINGS_TRACK_CAT_DEFAULT_SLOT((uint8_t)(i + 1U));
    }
    size_t fcount = 0;
    esp_err_t map_err = settings_func_map_load(ws->fmap, &fcount); /* defaults + overrides below */
    if (map_err != ESP_OK && map_err != ESP_ERR_NOT_FOUND) { free(ws); (void)fclose(f); return map_err; }
    size_t bcount = 0;
    bool have_fmap = false;
    bool have_bind = false;
    bool magic = false;
    bool declared = false;
    int declared_tracks = -1;
    int declared_bindings = -1;

    char line[MANIFEST_LINE_MAX];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        unsigned version = 0;
        if (sscanf(line, MANIFEST_MAGIC " %u", &version) == 1 && version >= 1U && version <= MANIFEST_VERSION) {
            magic = true;
            continue;
        }
        char *fields[16];
        int n = split_semicolon(line, fields, 16);
        if (n < 2) {
            continue;
        }
        if (strcmp(fields[0], "C") == 0 && n == 3) {
            declared_tracks = (int)strtol(fields[1], NULL, 10);
            declared_bindings = (int)strtol(fields[2], NULL, 10);
            declared = true;
        } else if (fields[0][0] == 'T' && fields[0][1] == '\0' && n >= 6) {
            unsigned slot = (unsigned)strtoul(fields[1], NULL, 10);
            if (slot < 1U || slot > SETTINGS_MAX_TRACKS || tcount >= SETTINGS_MAX_TRACKS) {
                continue;
            }
            settings_track_t *t = &ws->tracks[tcount];
            memset(t, 0, sizeof(*t));
            t->slot = (uint8_t)slot;
            t->enabled = strtoul(fields[3], NULL, 10) != 0U;
            (void)strncpy(t->file, fields[4], sizeof(t->file) - 1U);
            (void)strncpy(t->label, fields[5], sizeof(t->label) - 1U);
            ws->cats[slot - 1U] = (strtoul(fields[2], NULL, 10) != 0U) ? 1U : 0U;
            tcount++;
        } else if (fields[0][0] == 'F' && fields[0][1] == '\0' && n >= 7) {
            unsigned idx = (unsigned)strtoul(fields[1], NULL, 10);
            if (idx >= SETTINGS_FUNC_MAP_COUNT) {
                continue;
            }
            ws->fmap[idx].slot_a = (uint8_t)strtoul(fields[2], NULL, 10);
            ws->fmap[idx].slot_b = (uint8_t)strtoul(fields[3], NULL, 10);
            ws->fmap[idx].aux_mask = (uint16_t)strtoul(fields[4], NULL, 10);
            ws->fmap[idx].dir = (uint8_t)strtoul(fields[5], NULL, 10);
            ws->fmap[idx].speed = (uint8_t)strtoul(fields[6], NULL, 10);
            have_fmap = true;
        } else if (fields[0][0] == 'B' && fields[0][1] == '\0' && n >= 13 &&
                   bcount < FUNC_BIND_MAX) {
            func_binding_t *b = &ws->binds[bcount];
            memset(b, 0, sizeof(*b));
            /* idx (fields[1]) mirrors the array position; the record is
             * appended in order so the slot is implicit. */
            b->used = 1;
            b->fn = (uint8_t)strtoul(fields[2], NULL, 10);
            b->target_type = (uint8_t)strtoul(fields[3], NULL, 10);
            b->target_id = (uint8_t)strtoul(fields[4], NULL, 10);
            b->dir = (uint8_t)strtoul(fields[5], NULL, 10);
            b->state = (uint8_t)strtoul(fields[6], NULL, 10);
            b->mode = (uint8_t)strtoul(fields[7], NULL, 10);
            b->flags = (uint8_t)strtoul(fields[8], NULL, 10);
            b->short_table = (uint8_t)strtoul(fields[9], NULL, 10);
            b->short_ms = (uint16_t)strtoul(fields[10], NULL, 10);
            b->min_ms = (uint16_t)strtoul(fields[11], NULL, 10);
            b->fade_ms = (uint16_t)strtoul(fields[12], NULL, 10);
            bcount++;
            have_bind = true;
        }
    }
    bool read_error = ferror(f) != 0;
    int close_error = fclose(f);
    if (read_error || close_error != 0) { free(ws); return ESP_FAIL; }

    if (!magic) {
        free(ws);
        return ESP_ERR_NOT_FOUND;
    }

    if (declared && (declared_tracks < -1 || declared_tracks > SETTINGS_MAX_TRACKS ||
        (declared_tracks >= 0 && (size_t)declared_tracks != tcount) ||
        (declared_tracks == -1 && tcount != 0U) ||
        declared_bindings < -1 || declared_bindings > FUNC_BIND_MAX ||
        (declared_bindings >= 0 && (size_t)declared_bindings != bcount))) {
        free(ws);
        return ESP_ERR_INVALID_SIZE;
    }
    have_bind = have_bind || (declared && declared_bindings == 0);
    esp_err_t err = settings_recovery_set_pending(true);
    bool have_tracks = tcount > 0U || (declared && declared_tracks >= 0);
    if (err == ESP_OK && have_tracks) err = settings_tracks_save(ws->tracks, tcount);
    if (err == ESP_OK && have_tracks) err = settings_track_cats_save(ws->cats, SETTINGS_MAX_TRACKS);
    if (err == ESP_OK && have_fmap) err = settings_func_map_save(ws->fmap, SETTINGS_FUNC_MAP_COUNT);
    if (err == ESP_OK && have_bind) err = settings_func_bind_save(ws->binds, bcount);
    if (err == ESP_OK) err = settings_recovery_set_pending(false);
    if (err != ESP_OK) { settings_manifest_dirty(); free(ws); return err; }

    if (!have_tracks) {
        /* No track records, but the F;/B; metadata was still restored above.
         * Report NOT_FOUND so the caller rebuilds the track list from the audio
         * files without losing the function map/bindings (REV-ST1). */
        ESP_LOGW(TAG, "restored function metadata (no tracks) from %s", MANIFEST_PATH);
        free(ws);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "restored %u track(s)%s%s from %s", (unsigned)tcount,
             have_fmap ? " + function map" : "", have_bind ? " + bindings" : "",
             MANIFEST_PATH);
    free(ws);
    return ESP_OK;
}

esp_err_t settings_manifest_load(void)
{
    esp_err_t err = manifest_begin(true);
    if (err != ESP_OK) return err;
    /* Admission barrier: an already-running setter must finish before restore;
     * new competing setters see loading=true under their existing lock. */
    err = settings_metadata_lock();
    if (err == ESP_OK) {
        settings_metadata_unlock();
        err = manifest_load_owned();
    }
    manifest_end();
    return err;
}
