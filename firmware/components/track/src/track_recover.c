/* Track-list recovery from storage.
 *
 * If the stored track list is gone (e.g. NVS was erased after a firmware
 * update) but the WAV files are still on the external storage, rebuild the
 * list so the sounds are usable again. Order:
 *   1. NVS already has a list -> keep it.
 *   2. otherwise restore the metadata manifest (names/categories/func map).
 *   3. otherwise rebuild from the .wav file names.
 * Only runs when there is no metadata at all, so an intentionally emptied list
 * stays empty. Moved out of app_main so it can be unit-tested on the host.
 */
#include "track.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "settings.h"

static const char *TAG = "recover";

static bool name_is_wav(const char *n)
{
    size_t l = strlen(n);
    if (l <= 4U) {
        return false;
    }
    const char *e = n + l - 4U;
    return e[0] == '.' &&
           (e[1] == 'w' || e[1] == 'W') &&
           (e[2] == 'a' || e[2] == 'A') &&
           (e[3] == 'v' || e[3] == 'V');
}

/* Slot number encoded in "slotN.wav", else 0. */
static int slot_from_name(const char *n)
{
    if (strncmp(n, "slot", 4) != 0) {
        return 0;
    }
    char *end = NULL;
    long s = strtol(n + 4, &end, 10);
    if (end == n + 4 || *end != '.' || s < 1 || s > SETTINGS_MAX_TRACKS) {
        return 0;
    }
    return (int)s;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Collect .wav file paths from one directory; `prefix` is prepended to build
 * the storage-relative path (e.g. "audio/"). */
static size_t scan_wavs(const char *dirpath, const char *prefix,
                        char rel[][SETTINGS_TRACK_FILE_MAX], size_t cap)
{
    DIR *dir = opendir(dirpath);
    if (dir == NULL) {
        return 0;
    }
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < cap) {
        if (!name_is_wav(ent->d_name)) {
            continue;
        }
        snprintf(rel[n], SETTINGS_TRACK_FILE_MAX, "%s%.120s", prefix, ent->d_name);
        n++;
    }
    closedir(dir);
    return n;
}

void track_recover_from_storage(const char *audio_dir, const char *root_dir,
                                track_recover_result_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));

    /* Multi-kilobyte aggregates live on the heap: as stack locals they exceed
     * the 16 KB app_main stack on the recovery path (REV-S1). */
    settings_track_t *existing = malloc((size_t)SETTINGS_MAX_TRACKS * sizeof(*existing));
    char (*rel)[SETTINGS_TRACK_FILE_MAX] =
        malloc((size_t)SETTINGS_MAX_TRACKS * SETTINGS_TRACK_FILE_MAX);
    settings_track_t *tracks = malloc((size_t)SETTINGS_MAX_TRACKS * sizeof(*tracks));
    if (existing == NULL || rel == NULL || tracks == NULL) {
        ESP_LOGE(TAG, "recovery: out of memory");
        free(existing);
        free(rel);
        free(tracks);
        return;
    }

    size_t count = 0;
    esp_err_t lerr = settings_tracks_load(existing, &count);

    /* Always list what is actually on the storage, for diagnosis. */
    size_t na = (audio_dir != NULL) ? scan_wavs(audio_dir, "audio/", rel, SETTINGS_MAX_TRACKS) : 0;
    size_t nr = 0;
    if (na == 0 && root_dir != NULL) {
        nr = scan_wavs(root_dir, "", rel, SETTINGS_MAX_TRACKS);
    }
    size_t nf = na > 0 ? na : nr;
    ESP_LOGI(TAG, "nvs=%s count=%u | files: audio_dir=%u root_dir=%u",
             esp_err_to_name(lerr), (unsigned)count, (unsigned)na, (unsigned)nr);
    for (size_t i = 0; i < nf; ++i) {
        ESP_LOGI(TAG, "file %u/%u %s", (unsigned)(i + 1), (unsigned)nf, rel[i]);
    }

    if (lerr == ESP_OK && count > 0) {
        out->had_nvs = true;
        out->count = count;
        goto done;
    }

    /* NVS list is gone: restore it from the metadata manifest if present. */
    if (settings_manifest_load() == ESP_OK) {
        size_t rc = 0;
        (void)settings_tracks_load(existing, &rc);
        out->from_manifest = true;
        out->count = rc;
        goto done;
    }

    out->files_found = nf;
    if (nf == 0) {
        goto done;
    }

    qsort(rel, nf, sizeof(rel[0]), cmp_names); /* deterministic slots */

    bool used[SETTINGS_MAX_TRACKS + 1];
    memset(used, 0, sizeof(used));
    size_t n = 0;

    /* Pass 1: honour a slot number encoded in the file name (provisioning). */
    for (size_t i = 0; i < nf && n < SETTINGS_MAX_TRACKS; ++i) {
        const char *base = strrchr(rel[i], '/');
        base = base ? base + 1 : rel[i];
        int s = slot_from_name(base);
        if (s == 0 || used[s]) {
            continue;
        }
        used[s] = true;
        tracks[n].slot = (uint8_t)s;
        snprintf(tracks[n].file, sizeof(tracks[n].file), "%.120s", rel[i]);
        snprintf(tracks[n].label, sizeof(tracks[n].label), "Слот %d", s);
        tracks[n].enabled = true;
        n++;
    }
    /* Pass 2: files without a slot prefix get the lowest free slots. */
    for (size_t i = 0; i < nf && n < SETTINGS_MAX_TRACKS; ++i) {
        const char *base = strrchr(rel[i], '/');
        base = base ? base + 1 : rel[i];
        if (slot_from_name(base) != 0) {
            continue;
        }
        int s = 1;
        while (s <= SETTINGS_MAX_TRACKS && used[s]) {
            s++;
        }
        /* n < SETTINGS_MAX_TRACKS here, so a free slot always exists. */
        used[s] = true;
        tracks[n].slot = (uint8_t)s;
        snprintf(tracks[n].file, sizeof(tracks[n].file), "%.120s", rel[i]);
        snprintf(tracks[n].label, sizeof(tracks[n].label), "%.63s", base);
        char *dot = strrchr(tracks[n].label, '.');
        if (dot != NULL) {
            *dot = '\0';
        }
        tracks[n].enabled = true;
        n++;
    }

    if (n > 0 && settings_tracks_save(tracks, n) == ESP_OK) {
        out->rebuilt = true;
        out->count = n;
    }

done:
    free(existing);
    free(rel);
    free(tracks);
}
