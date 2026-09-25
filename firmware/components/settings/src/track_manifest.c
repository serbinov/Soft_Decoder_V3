/* Metadata manifest stored next to the sound files on the external storage.
 *
 * The track list (slot, file, label, category, enabled) and the function map
 * live in NVS. A full chip erase wipes NVS but leaves the sounds on the W25Q128,
 * so this manifest lets the firmware restore the names/categories/mapping after
 * such a reset.
 *
 * Format (text, one record per line, ';'-separated; file/label are last so a
 * label may contain spaces):
 *   AURA-TRACKS 1
 *   T;<slot>;<cat>;<enabled>;<file>;<label>
 *   F;<idx>;<slot_a>;<slot_b>;<aux_mask>;<dir>;<speed>
 */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"

static const char *TAG = "manifest";

#define MANIFEST_PATH    "/userdata/audio/tracks.txt"
#define MANIFEST_MAGIC   "AURA-TRACKS"
#define MANIFEST_VERSION 1
#define MANIFEST_LINE_MAX 256

/* Set while restoring from the manifest so the NVS writes it performs do not
 * immediately rewrite the file being read. */
static bool s_manifest_loading;

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

void settings_manifest_sync(void)
{
    if (s_manifest_loading) {
        return;
    }

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t tcount = 0;
    (void)settings_tracks_load(tracks, &tcount);

    uint8_t cats[SETTINGS_MAX_TRACKS];
    size_t ccount = 0;
    (void)settings_track_cats_load(cats, &ccount);

    settings_func_map_t fmap[SETTINGS_FUNC_MAP_COUNT];
    size_t fcount = 0;
    (void)settings_func_map_load(fmap, &fcount);

    FILE *f = fopen(MANIFEST_PATH, "w");
    if (f == NULL) {
        return; /* storage not mounted: nothing to persist */
    }

    (void)fprintf(f, "%s %d\n", MANIFEST_MAGIC, MANIFEST_VERSION);

    for (size_t i = 0; i < tcount && i < SETTINGS_MAX_TRACKS; ++i) {
        char file[SETTINGS_TRACK_FILE_MAX];
        char label[SETTINGS_TRACK_LABEL_MAX];
        (void)snprintf(file, sizeof(file), "%s", tracks[i].file);
        (void)snprintf(label, sizeof(label), "%s", tracks[i].label);
        sanitize_field(file);
        sanitize_field(label);
        uint8_t cat = (tracks[i].slot >= 1U && tracks[i].slot <= ccount)
                          ? cats[tracks[i].slot - 1U]
                          : (uint8_t)SETTINGS_TRACK_CAT_DEFAULT_SLOT(tracks[i].slot);
        (void)fprintf(f, "T;%u;%u;%u;%s;%s\n", (unsigned)tracks[i].slot,
                      (unsigned)cat, tracks[i].enabled ? 1U : 0U, file, label);
    }
    for (size_t i = 0; i < fcount && i < SETTINGS_FUNC_MAP_COUNT; ++i) {
        (void)fprintf(f, "F;%u;%u;%u;%u;%u;%u\n", (unsigned)i,
                      (unsigned)fmap[i].slot_a, (unsigned)fmap[i].slot_b,
                      (unsigned)fmap[i].aux_mask, (unsigned)fmap[i].dir,
                      (unsigned)fmap[i].speed);
    }

    (void)fflush(f);
    (void)fsync(fileno(f));
    (void)fclose(f);
}

esp_err_t settings_manifest_load(void)
{
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t tcount = 0;
    uint8_t cats[SETTINGS_MAX_TRACKS];
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        cats[i] = (uint8_t)SETTINGS_TRACK_CAT_DEFAULT_SLOT((uint8_t)(i + 1U));
    }
    settings_func_map_t fmap[SETTINGS_FUNC_MAP_COUNT];
    size_t fcount = 0;
    (void)settings_func_map_load(fmap, &fcount); /* defaults + overrides below */
    bool have_fmap = false;
    bool magic = false;

    char line[MANIFEST_LINE_MAX];
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }
        if (strncmp(line, MANIFEST_MAGIC, strlen(MANIFEST_MAGIC)) == 0) {
            magic = true;
            continue;
        }
        char *fields[8];
        int n = split_semicolon(line, fields, 8);
        if (n < 2) {
            continue;
        }
        if (fields[0][0] == 'T' && fields[0][1] == '\0' && n >= 6) {
            unsigned slot = (unsigned)strtoul(fields[1], NULL, 10);
            if (slot < 1U || slot > SETTINGS_MAX_TRACKS || tcount >= SETTINGS_MAX_TRACKS) {
                continue;
            }
            settings_track_t *t = &tracks[tcount];
            memset(t, 0, sizeof(*t));
            t->slot = (uint8_t)slot;
            t->enabled = strtoul(fields[3], NULL, 10) != 0U;
            (void)strncpy(t->file, fields[4], sizeof(t->file) - 1U);
            (void)strncpy(t->label, fields[5], sizeof(t->label) - 1U);
            cats[slot - 1U] = (strtoul(fields[2], NULL, 10) != 0U) ? 1U : 0U;
            tcount++;
        } else if (fields[0][0] == 'F' && fields[0][1] == '\0' && n >= 7) {
            unsigned idx = (unsigned)strtoul(fields[1], NULL, 10);
            if (idx >= SETTINGS_FUNC_MAP_COUNT) {
                continue;
            }
            fmap[idx].slot_a = (uint8_t)strtoul(fields[2], NULL, 10);
            fmap[idx].slot_b = (uint8_t)strtoul(fields[3], NULL, 10);
            fmap[idx].aux_mask = (uint16_t)strtoul(fields[4], NULL, 10);
            fmap[idx].dir = (uint8_t)strtoul(fields[5], NULL, 10);
            fmap[idx].speed = (uint8_t)strtoul(fields[6], NULL, 10);
            have_fmap = true;
        }
    }
    (void)fclose(f);

    if (!magic || tcount == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    s_manifest_loading = true;
    (void)settings_tracks_save(tracks, tcount);
    (void)settings_track_cats_save(cats, SETTINGS_MAX_TRACKS);
    if (have_fmap) {
        (void)settings_func_map_save(fmap, SETTINGS_FUNC_MAP_COUNT);
    }
    s_manifest_loading = false;

    ESP_LOGW(TAG, "restored %u track(s)%s from %s", (unsigned)tcount,
             have_fmap ? " + function map" : "", MANIFEST_PATH);
    return ESP_OK;
}
