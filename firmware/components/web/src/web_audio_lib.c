/* Sound-library naming, path, volume-sidecar and WAV probing helpers. */

#include "web_internal.h"
#include "web_util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "storage.h"

bool web_name_is_wav(const char *n)
{
    size_t l = strlen(n);
    if (l <= 4U) return false;
    const char *e = n + l - 4U;
    return e[0] == '.' && (e[1] == 'w' || e[1] == 'W') &&
           (e[2] == 'a' || e[2] == 'A') && (e[3] == 'v' || e[3] == 'V');
}

/* Names the library/storage code owns privately. A user upload must never be
 * able to collide with the metadata sidecars, a pack backup directory or an
 * in-progress staging file. */
bool web_name_reserved(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (name[0] == '.' && name[1] == 'p' && name[2] == 'b') return true;
    size_t l = strlen(name);
    if (l >= 4U && strcasecmp(name + l - 4U, ".tmp") == 0) return true;
    return strcasecmp(name, "tracks.txt") == 0 ||
           strcasecmp(name, "volumes.txt") == 0;
}

/* Force the stored/displayed extension to lowercase so upload, OTA, library,
 * play and delete all agree on the canonical file name. */
void web_name_lower_wav(char *name, size_t cap)
{
    size_t l = strlen(name);
    if (l < 4U || l + 1U > cap) return;
    name[l - 4U] = '.';
    name[l - 3U] = 'w';
    name[l - 2U] = 'a';
    name[l - 1U] = 'v';
}

int web_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* File size via stdio. Some littlefs VFS ports do not fill st_size/st_mode, so
 * stat() is unreliable here; fopen+fseek/ftell works on every mount. */
uint32_t web_file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    uint32_t size = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long end = ftell(f);
        if (end > 0) size = (uint32_t)end;
    }
    fclose(f);
    return size;
}

/* Strip directory and a trailing .wav, then sanitize exactly like the packer
 * and the previous upload path. Empty on failure. */
bool web_lib_base_name(const char *in, char *out, size_t out_len)
{
    char safe[WEB_LIB_NAME_MAX];
    sanitize_name(in, safe, sizeof(safe));
    size_t sl = strlen(safe);
    if (sl > 4U && strcasecmp(safe + sl - 4U, ".wav") == 0) {
        safe[sl - 4U] = '\0';
        sl -= 4U;
    }
    if (sl == 0U || strcmp(safe, ".") == 0 || strcmp(safe, "..") == 0) {
        return false;
    }
    snprintf(out, out_len, "%s", safe);
    return true;
}

/* Resolve a library base name to the canonical on-disk path. New uploads are
 * stored as `<base>.wav`, but libraries written by older firmware may still
 * contain `<base>.WAV`; accept those case-insensitively so play/delete/raw
 * references agree with the ingest normalization. */
bool web_lib_resolve_path(const char *base, char *path, size_t cap)
{
    char safe_base[WEB_LIB_NAME_MAX];
    size_t base_len = strlen(base);
    if (base_len >= sizeof(safe_base)) return false;
    memcpy(safe_base, base, base_len + 1U);

    snprintf(path, cap, WEB_AUDIO_DIR "/%s.wav", safe_base);
    FILE *f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return true;
    }
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d == NULL) return false;
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!web_name_is_wav(ent->d_name)) continue;
        char cand[WEB_LIB_NAME_MAX];
        if (!web_lib_base_name(ent->d_name, cand, sizeof(cand))) continue;
        if (strcasecmp(cand, safe_base) == 0) {
            size_t name_len = strlen(ent->d_name);
            if (name_len >= sizeof(cand)) continue;
            memcpy(cand, ent->d_name, name_len + 1U);
            snprintf(path, cap, WEB_AUDIO_DIR "/%s", cand);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/* Read volumes.txt into `out` (cap entries). Returns the entry count (0 if the
 * file is missing or malformed). Caller must hold a storage lease. */
size_t web_vol_load(web_vol_t *out, size_t cap)
{
    FILE *f = fopen(AUDIO_VOL_FILE, "r");
    if (f == NULL) return 0;
    char line[160];
    size_t n = 0;
    if (fgets(line, sizeof(line), f) == NULL || strncmp(line, "AURAVOL1", 8) != 0) {
        fclose(f);
        return 0;
    }
    while (n < cap && fgets(line, sizeof(line), f) != NULL) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) continue;
        *tab = '\0';
        long vol = strtol(line, NULL, 10);
        char *name = tab + 1;
        char *nl = strchr(name, '\n');
        if (nl != NULL) *nl = '\0';
        if (name[0] == '\0') continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", name);
        out[n].vol = (uint8_t)(vol < 0 ? 0 : (vol > 100 ? 100 : vol));
        n++;
    }
    fclose(f);
    return n;
}

bool web_vol_get(const web_vol_t *v, size_t n, const char *name, uint8_t *out)
{
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(v[i].name, name) == 0) {
            *out = v[i].vol;
            return true;
        }
    }
    return false;
}

/* Atomic write of the whole sidecar. Caller must hold a storage lease. */
esp_err_t web_vol_save(const web_vol_t *v, size_t n)
{
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s.tmp", AUDIO_VOL_FILE);
    FILE *f = fopen(tmp, "w");
    if (f == NULL) return ESP_FAIL;
    bool ok = fputs("AURAVOL1\n", f) >= 0;
    for (size_t i = 0; i < n && ok; ++i) {
        ok = fprintf(f, "%u\t%s\n", (unsigned)v[i].vol, v[i].name) > 0;
    }
    if (fflush(f) != 0) ok = false;
    if (fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) { (void)remove(tmp); return ESP_FAIL; }
    if (rename(tmp, AUDIO_VOL_FILE) != 0) { (void)remove(tmp); return ESP_FAIL; }
    return ESP_OK;
}

/* Per-file volume (default 100), safe to call from any task. */
uint8_t audio_library_volume_lookup(const char *name)
{
    uint8_t vol = 100;
    web_vol_t *v = malloc(sizeof(*v) * AUDIO_PACK_MAX_FILES);
    if (v == NULL) return vol;
    if (storage_access_begin() != ESP_OK) { free(v); return vol; }
    size_t n = web_vol_load(v, AUDIO_PACK_MAX_FILES);
    (void)web_vol_get(v, n, name, &vol);
    storage_access_end();
    free(v);
    return vol;
}

esp_err_t audio_library_volume_set(const char *name, uint8_t vol)
{
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) return err;
    web_vol_t *v = malloc(sizeof(*v) * AUDIO_PACK_MAX_FILES);
    if (v == NULL) { storage_access_end(); return ESP_ERR_NO_MEM; }
    size_t n = web_vol_load(v, AUDIO_PACK_MAX_FILES);
    size_t i = 0;
    while (i < n && strcmp(v[i].name, name) != 0) ++i;
    if (i < n) {
        v[i].vol = vol;
    } else if (n < AUDIO_PACK_MAX_FILES) {
        snprintf(v[n].name, sizeof(v[n].name), "%s", name);
        v[n].vol = vol;
        n++;
    } else {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = web_vol_save(v, n);
    storage_access_end();
    free(v);
    return err;
}

/* Minimal RIFF probe for the two encodings we store. */
bool web_wav_probe(FILE *f, uint16_t *format, uint16_t *channels,
                          uint32_t *sample_rate, uint16_t *block_align,
                          uint16_t *bits, uint16_t *spb,
                          uint32_t *data_off, uint32_t *data_len)
{
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long flen = ftell(f);
    if (flen < 12 || fseek(f, 0, SEEK_SET) != 0) return false;
    char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        return false;
    }
    *format = 0; *channels = 0; *sample_rate = 0; *block_align = 0; *bits = 0; *spb = 0;
    long pos = 12;
    bool has_fmt = false, has_data = false;
    while (pos + 8 <= flen) {
        char cid[4];
        uint32_t csize;
        if (fseek(f, pos, SEEK_SET) != 0) return false;
        if (fread(cid, 1, 4, f) != 4 || fread(&csize, 1, 4, f) != 4) return false;
        long body = pos + 8;
        if ((long)((uint32_t)body + csize) > flen) return false;
        if (!memcmp(cid, "fmt ", 4) && csize >= 16U) {
            uint8_t fm[20];
            size_t want = csize < sizeof(fm) ? csize : sizeof(fm);
            if (fseek(f, body, SEEK_SET) != 0 || fread(fm, 1, want, f) != want) return false;
            *format = (uint16_t)(fm[0] | (fm[1] << 8));
            *channels = (uint16_t)(fm[2] | (fm[3] << 8));
            *sample_rate = (uint32_t)fm[4] | ((uint32_t)fm[5] << 8) |
                           ((uint32_t)fm[6] << 16) | ((uint32_t)fm[7] << 24);
            *block_align = (uint16_t)(fm[12] | (fm[13] << 8));
            *bits = (uint16_t)(fm[14] | (fm[15] << 8));
            if (*format == 0x0011U && want >= 20U) {
                *spb = (uint16_t)(fm[18] | (fm[19] << 8));
            }
            has_fmt = true;
        } else if (!memcmp(cid, "data", 4)) {
            *data_off = (uint32_t)body;
            *data_len = csize;
            has_data = true;
        }
        pos = body + (long)csize + (long)(csize & 1U);
    }
    return has_fmt && has_data;
}
