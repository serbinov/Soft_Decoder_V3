/* Pack journal boot recovery and orphan-backup fallback. */

#include "web.h"
#include "web_internal.h"
#include "web_pack_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "audio_pack.h"
#include "esp_log.h"
#include "settings.h"
#include "sound_graph_store.h"
#include "storage.h"

static const char *TAG_PACK_REC = "web";

bool pack_journal_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;
    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* Remove a committed transaction's backup files. Returns true only when the
 * backup directory is gone (or never existed), so the journal can be deleted
 * without losing recovery evidence. */
bool pack_backup_cleanup(const char *backup_dir, char (*names)[WEB_LIB_NAME_MAX],
                                size_t count)
{
    if (backup_dir == NULL || backup_dir[0] == '\0') return true;
    bool clean = true;
    char path[224];
    for (size_t i = 0; i < count; ++i) {
        if (!pack_journal_name_ok(names[i])) continue;
        snprintf(path, sizeof(path), "%s/%s", backup_dir, names[i]);
        if (remove(path) != 0 && errno != ENOENT) clean = false;
    }
    if (WEB_RMDIR(backup_dir) != 0 && errno != ENOENT) clean = false;
    return clean;
}

/* Roll a PREPARED transaction back: remove published new files, restore every
 * old file from the backup directory, then reload the pre-pack metadata
 * manifest (it was only rewritten after a confirmed commit). */
static bool pack_name_in_names(const char *name,
                               char (*names)[WEB_LIB_NAME_MAX], size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

static bool pack_backup_restore(const char *backup_dir,
                                char (*old_names)[WEB_LIB_NAME_MAX], size_t old_count,
                                char (*new_names)[WEB_LIB_NAME_MAX], size_t new_count)
{
    if (backup_dir == NULL || backup_dir[0] == '\0') return true;
    char path[224], backup_path[224];
    bool restored = true;
    for (size_t i = 0; i < new_count; ++i) {
        if (!pack_journal_name_ok(new_names[i])) continue;
        snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s", new_names[i]);
        /* A name present in both lists may already have been restored by an
         * interrupted earlier attempt: its backup was consumed and the
         * canonical file is the old (good) copy. Never unlink that. */
        if (pack_name_in_names(new_names[i], old_names, old_count)) {
            snprintf(backup_path, sizeof(backup_path), "%s/%s", backup_dir, new_names[i]);
            struct stat bs, cs;
            if (stat(backup_path, &bs) != 0 && errno == ENOENT &&
                stat(path, &cs) == 0) {
                continue;
            }
        }
        if (remove(path) != 0 && errno != ENOENT) restored = false;
    }
    for (size_t i = 0; i < old_count; ++i) {
        if (!pack_journal_name_ok(old_names[i])) continue;
        snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s", old_names[i]);
        snprintf(backup_path, sizeof(backup_path), "%s/%s", backup_dir, old_names[i]);
        struct stat st;
        if (stat(backup_path, &st) != 0) {
            if (errno == ENOENT) {
                /* A retried rollback: this entry was already restored by the
                 * previous attempt, so leave the canonical file alone. */
                struct stat canonical;
                if (stat(path, &canonical) == 0) continue;
            }
            restored = false;
            continue;
        }
        if (remove(path) != 0 && errno != ENOENT) {
            restored = false;
            continue;
        }
        if (rename(backup_path, path) != 0) restored = false;
    }
    if (WEB_RMDIR(backup_dir) != 0 && errno != ENOENT) restored = false;
    if (restored) {
        /* The manifest still describes the pre-pack library. */
        (void)settings_manifest_load();
    }
    return restored;
}

/* Boot scan for backup directories left behind by a crash. This is a fallback
 * for a corrupt/missing journal; a valid journal is handled by
 * web_recover_pack_journal(). */
static void pack_recover_orphan_backups(void)
{
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d == NULL) return;
    bool recovered = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, ".pb", 3) != 0) continue;
        char dir[WEB_PATH_MAX];
        snprintf(dir, sizeof(dir), WEB_AUDIO_DIR "/%s", ent->d_name);
        DIR *bd = opendir(dir);
        if (bd == NULL) {
            (void)WEB_RMDIR(dir);
            continue;
        }
        struct dirent *be;
        while ((be = readdir(bd)) != NULL) {
            if (!pack_journal_name_ok(be->d_name)) continue;
            char canonical[WEB_PATH_MAX], backup[WEB_PATH_MAX * 2];
            snprintf(canonical, sizeof(canonical), WEB_AUDIO_DIR "/%s", be->d_name);
            snprintf(backup, sizeof(backup), "%s/%s", dir, be->d_name);
            (void)remove(canonical);
            if (rename(backup, canonical) != 0) {
                ESP_LOGE(TAG_PACK_REC, "PACK: orphan recovery cannot restore %s", be->d_name);
            } else {
                recovered = true;
            }
        }
        closedir(bd);
        (void)WEB_RMDIR(dir);
        ESP_LOGW(TAG_PACK_REC, "PACK: restored orphan backup %s", ent->d_name);
    }
    closedir(d);
    /* Only reload the (old) manifest when we actually rolled something back;
     * a normal boot must never let the sidecar override newer NVS metadata. */
    if (recovered) (void)settings_manifest_load();
}

/* Boot recovery entry, called once from app_main() before the settings caches
 * are loaded and before the sound graph is restored. Safe to call when storage
 * is absent; never blocks boot on a damaged journal. */
static esp_err_t web_recover_pack_journal(void)
{
    if (!storage_is_mounted()) return ESP_ERR_INVALID_STATE;
    FILE *f = fopen(PACK_JOURNAL_PATH, "rb");
    if (f == NULL) {
        if (errno == ENOENT) {
            pack_recover_orphan_backups();
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    pack_journal_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    const size_t prefix = offsetof(pack_journal_hdr_t, old_selection);
    bool ok = fread(&hdr, 1, prefix, f) == prefix;
    bool selection_known = ok && memcmp(hdr.magic, PACK_JOURNAL_MAGIC, 8) == 0;
    bool legacy = ok && memcmp(hdr.magic, PACK_JOURNAL_MAGIC_OLD, 8) == 0;
    if (selection_known) {
        ok = fread(&hdr.old_selection, 1, sizeof(hdr) - prefix, f) == sizeof(hdr) - prefix;
    } else if (legacy) {
        memset(&hdr.old_selection, 0, sizeof(hdr) - prefix);
    } else {
        ok = false;
    }
    ok = ok && (selection_known || legacy) &&
         (hdr.phase == PACK_JOURNAL_PREPARED || hdr.phase == PACK_JOURNAL_COMMITTED) &&
         hdr.old_count <= PACK_JOURNAL_MAX_NAMES &&
         hdr.new_count <= PACK_JOURNAL_MAX_NAMES;
    char (*old_names)[WEB_LIB_NAME_MAX] = NULL;
    char (*new_names)[WEB_LIB_NAME_MAX] = NULL;
    if (ok && hdr.old_count != 0U) {
        old_names = malloc((size_t)hdr.old_count * sizeof(*old_names));
        ok = old_names != NULL &&
             fread(old_names, sizeof(*old_names), hdr.old_count, f) == hdr.old_count;
    }
    if (ok && hdr.new_count != 0U) {
        new_names = malloc((size_t)hdr.new_count * sizeof(*new_names));
        ok = new_names != NULL &&
             fread(new_names, sizeof(*new_names), hdr.new_count, f) == hdr.new_count;
    }
    fclose(f);
    if (!ok) {
        free(old_names);
        free(new_names);
        /* Corrupt journal: drop it and fall back to the orphan scan. */
        (void)remove(PACK_JOURNAL_PATH);
        pack_recover_orphan_backups();
        return ESP_ERR_INVALID_STATE;
    }

    bool cleaned = true;
    if (hdr.phase == PACK_JOURNAL_COMMITTED) {
        cleaned = pack_backup_cleanup(hdr.backup_dir, old_names, hdr.old_count);
    } else {
        /* A PREPARED journal without a backup directory means the transaction
         * never moved any old file (only staged files existed); there is
         * nothing to roll back. */
        DIR *bd = opendir(hdr.backup_dir);
        if (bd != NULL) {
            closedir(bd);
            cleaned = pack_backup_restore(hdr.backup_dir, old_names, hdr.old_count,
                                          new_names, hdr.new_count);
        }
        /* Roll the graph selection back only if it still points at the scheme
         * this failed transaction installed. A factory reset or a newer user
         * selection must not be overwritten by an old journal. J1 journals
         * carry no selection information at all. */
        if (cleaned && selection_known && hdr.new_selection_valid != 0U &&
            sg_id_valid(hdr.new_selection)) {
            char current[SG_ID_CAP] = {0};
            uint32_t current_rev = 0;
            bool selected = false;
            if (sg_store_selection(current, sizeof(current), &current_rev, &selected) != ESP_OK) {
                cleaned = false;
            } else if (selected && strcmp(current, hdr.new_selection) == 0) {
                if (hdr.old_selection_valid != 0U && sg_id_valid(hdr.old_selection) &&
                    hdr.old_selection_rev != 0U) {
                    cleaned = sg_store_select(hdr.old_selection, hdr.old_selection_rev) == ESP_OK;
                } else {
                    cleaned = sg_store_select_legacy() == ESP_OK;
                }
            }
        }
    }
    free(old_names);
    free(new_names);
    if (cleaned) {
        (void)remove(PACK_JOURNAL_PATH);
        return ESP_OK;
    }
    ESP_LOGW(TAG_PACK_REC, "PACK: recovery incomplete, journal retained: %s", PACK_JOURNAL_PATH);
    return ESP_FAIL;
}

/* Public boot entry: recovery must run before the sound graph is restored from
 * the selection file, so app_main calls it right after storage/settings init.
 * It must not be repeated later: a partially completed rollback can only be
 * retried safely while the graph is still inactive. */
esp_err_t web_storage_recovery(void)
{
    return web_recover_pack_journal();
}
