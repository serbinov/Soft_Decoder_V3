/* Sound-pack import transaction, journal and boot recovery. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"
#include "web_pack_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio.h"
#include "audio_pack.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "settings.h"
#include "sound.h"
#include "sound_graph_store.h"
#include "storage.h"

static const char *TAG_PACK = "web";


static bool pack_name_in_index(const audio_pack_entry_t *idx, size_t count,
                               const char *name)
{
    char base[WEB_LIB_NAME_MAX];
    if (!web_lib_base_name(name, base, sizeof(base))) return false;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(idx[i].name, base) == 0) return true;
    }
    return false;
}

/* Remember the selected graph before a bundled scheme replaces it. Returns an
 * error if the selection cannot be read: a rollback must not guess and replace
 * a valid user selection with the legacy fallback. */
static esp_err_t pack_capture_selection(pack_tx_t *tx)
{
    tx->old_selection[0] = '\0';
    tx->old_selection_rev = 0;
    tx->old_selection_valid = false;
    bool selected = false;
    esp_err_t err = ESP_FAIL;
    for (unsigned attempt = 0; attempt < 3U; ++attempt) {
        err = sg_store_selection(tx->old_selection, sizeof(tx->old_selection),
                                 &tx->old_selection_rev, &selected);
        if (err == ESP_OK) break;
        if (attempt + 1U < 3U) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK) {
        tx->old_selection[0] = '\0';
        tx->old_selection_rev = 0;
        selected = false;
        return err;
    }
    if (selected && sg_id_valid(tx->old_selection) && tx->old_selection_rev != 0U) {
        tx->old_selection_valid = true;
    }
    return ESP_OK;
}

static esp_err_t pack_collect_old(pack_tx_t *tx)
{
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d == NULL) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    size_t cap = 0;
    while (true) {
        struct dirent *ent = readdir(d);
        if (ent == NULL) break;
        size_t name_len = strlen(ent->d_name);
        if (!web_name_is_wav(ent->d_name) || name_len >= WEB_LIB_NAME_MAX) continue;
        if (tx->old_count >= 512U) {
            closedir(d);
            return ESP_ERR_INVALID_SIZE;
        }
        if (tx->old_count == cap) {
            size_t next = cap ? cap * 2U : 32U;
            char (*grown)[WEB_LIB_NAME_MAX] =
                realloc(tx->old_names, next * sizeof(*grown));
            if (grown == NULL) {
                closedir(d);
                return ESP_ERR_NO_MEM;
            }
            tx->old_names = grown;
            cap = next;
        }
        memcpy(tx->old_names[tx->old_count], ent->d_name, name_len + 1U);
        ++tx->old_count;
    }
    closedir(d);
    return ESP_OK;
}

/* Undo a partially applied pack: remove new canonical files, remove staged
 * files, and move every original file back from the backup directory. */
static void pack_tx_rollback(pack_tx_t *tx)
{
    if (tx == NULL) return;
    if (tx->files != NULL) {
        for (size_t i = 0; i < tx->file_count; ++i) {
            if (tx->files[i].published) {
                (void)remove(tx->files[i].canonical);
                tx->files[i].published = false;
            }
            if (tx->files[i].staged[0]) {
                (void)remove(tx->files[i].staged);
                tx->files[i].staged[0] = '\0';
            }
        }
    }
    if (tx->old_names != NULL && tx->old_moved != NULL && tx->backup_dir[0]) {
        char old_path[192], backup_path[224];
        for (size_t i = 0; i < tx->old_count; ++i) {
            if (!tx->old_moved[i]) continue;
            snprintf(old_path, sizeof(old_path), WEB_AUDIO_DIR "/%s", tx->old_names[i]);
            snprintf(backup_path, sizeof(backup_path), "%s/%s",
                     tx->backup_dir, tx->old_names[i]);
            if (rename(backup_path, old_path) != 0) {
                ESP_LOGE(TAG_PACK, "PACK: cannot restore %s", tx->old_names[i]);
            } else {
                tx->old_moved[i] = false;
            }
        }
    }
    bool clean = true;
    if (tx->backup_dir[0] && WEB_RMDIR(tx->backup_dir) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG_PACK, "PACK: backup dir retained: %s", tx->backup_dir);
        clean = false;
    }
    if (clean && tx->journal_written && tx->journal[0] != '\0') {
        if (remove(tx->journal) != 0 && errno != ENOENT) clean = false;
    }
    if (!clean) {
        ESP_LOGW(TAG_PACK, "PACK: recovery journal retained: %s", tx->journal);
    }
}

static void pack_tx_free(pack_tx_t *tx)
{
    if (tx == NULL) return;
    free(tx->old_names);
    free(tx->old_moved);
    free(tx->new_names);
    free(tx->files);
    free(tx);
}


static esp_err_t pack_journal_write(pack_tx_t *tx, uint32_t phase)
{
    if (tx == NULL || tx->journal[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (tx->old_count > PACK_JOURNAL_MAX_NAMES ||
        tx->new_count > PACK_JOURNAL_MAX_NAMES) {
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *f = fopen(PACK_JOURNAL_TMP, "wb");
    if (f == NULL) return ESP_FAIL;
    pack_journal_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, PACK_JOURNAL_MAGIC, 8);
    hdr.phase = phase;
    hdr.old_count = (uint32_t)tx->old_count;
    hdr.new_count = (uint32_t)tx->new_count;
    snprintf(hdr.backup_dir, sizeof(hdr.backup_dir), "%s", tx->backup_dir);
    snprintf(hdr.old_selection, sizeof(hdr.old_selection), "%s",
             tx->old_selection_valid ? tx->old_selection : "");
    hdr.old_selection_rev = tx->old_selection_valid ? tx->old_selection_rev : 0U;
    hdr.old_selection_valid = tx->old_selection_valid ? 1U : 0U;
    snprintf(hdr.new_selection, sizeof(hdr.new_selection), "%s",
             tx->new_selection_valid ? tx->new_selection : "");
    hdr.new_selection_valid = tx->new_selection_valid ? 1U : 0U;
    bool ok = fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
    if (ok && tx->old_count != 0U) {
        ok = fwrite(tx->old_names, sizeof(tx->old_names[0]), tx->old_count, f) == tx->old_count;
    }
    if (ok && tx->new_count != 0U) {
        ok = fwrite(tx->new_names, sizeof(tx->new_names[0]), tx->new_count, f) == tx->new_count;
    }
    if (fflush(f) != 0) ok = false;
    if (fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        (void)remove(PACK_JOURNAL_TMP);
        return ESP_FAIL;
    }
    if (rename(PACK_JOURNAL_TMP, tx->journal) != 0) {
        (void)remove(PACK_JOURNAL_TMP);
        return ESP_FAIL;
    }
    return ESP_OK;
}







static bool pack_tx_commit_cleanup(pack_tx_t *tx)
{
    if (tx == NULL) return true;
    bool clean = pack_backup_cleanup(tx->backup_dir, tx->old_names, tx->old_count);
    if (clean && tx->journal[0] != '\0') {
        if (remove(tx->journal) != 0 && errno != ENOENT) clean = false;
    }
    if (!clean && tx->backup_dir[0]) {
        ESP_LOGW(TAG_PACK, "PACK: backup cleanup pending: %s", tx->backup_dir);
    }
    return clean;
}

/* A pack replaces every file. Drop track/binding references to sounds that no
 * longer exist instead of leaving slots pointing at deleted paths. Returns an
 * error when NVS tracks, the function map/bindings or the on-storage manifest
 * could not be confirmed; callers must then keep the backup/journal. */
static esp_err_t pack_reconcile_metadata(void)
{
    esp_err_t result = ESP_OK;
    settings_track_t *tracks = calloc(SETTINGS_MAX_TRACKS, sizeof(*tracks));
    if (tracks == NULL) return ESP_ERR_NO_MEM;
    size_t count = 0;
    esp_err_t load_err = settings_tracks_load(tracks, &count);
    if (load_err == ESP_ERR_NOT_FOUND) {
        count = 0;
    } else if (load_err != ESP_OK) {
        free(tracks);
        return load_err;
    }
    if (count > SETTINGS_MAX_TRACKS) count = SETTINGS_MAX_TRACKS;
    bool slots[SETTINGS_MAX_TRACKS + 1] = { false };
    size_t kept = 0;
    bool tracks_changed = false;
    for (size_t i = 0; i < count; ++i) {
        const char *file = tracks[i].file;
        if (file[0] == '\0' || file[0] == '/' || strstr(file, "..") != NULL) {
            tracks_changed = true;
            continue;
        }
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", storage_get_root(), file);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (kept != i) tracks[kept] = tracks[i];
            if (tracks[kept].slot <= SETTINGS_MAX_TRACKS) {
                slots[tracks[kept].slot] = true;
            }
            ++kept;
        } else {
            tracks_changed = true;
        }
    }
    if (tracks_changed) {
        result = settings_tracks_save(tracks, kept);
    }

    if (web_cfg_lock(pdMS_TO_TICKS(100))) {
        if (web_cfg()->active_slot > SETTINGS_MAX_TRACKS ||
            (web_cfg()->active_slot != 0U && !slots[web_cfg()->active_slot])) {
            web_cfg()->active_slot = 0;
            esp_err_t save = settings_save_deferred(web_cfg());
            if (result == ESP_OK) result = save;
        }
        web_cfg_unlock();
    } else if (result == ESP_OK) {
        result = ESP_ERR_TIMEOUT;
    }

    esp_err_t func_result = web_func_reconcile_slots(slots);
    if (result == ESP_OK) result = func_result;

    /* Manifest last: it is the sidecar copy used by boot recovery. */
    {
        esp_err_t manifest = settings_manifest_sync();
        if (result == ESP_OK) result = manifest;
    }
    free(tracks);
    web_func_refresh_tracks();
    return result;
}

/* POST /api/audio/pack — unpack an AURA Sound Pack straight from the request
 * body. New files are staged first, old files are moved to a private backup
 * directory, and only then are the new files published. Any error restores the
 * old library; a failed upload must never leave the decoder soundless. */
esp_err_t audio_pack_upload_post(httpd_req_t *req)
{
    const char *errmsg = "invalid sound pack";
    int errstatus = 500;
    if (!storage_is_mounted())
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    if (req->content_len < AUDIO_PACK_HDR_LEN || req->content_len > AUDIO_PACK_MAX_BYTES)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    /* A pack can be far larger than a single WAV, so give the transfer more
     * than the generic 120 s window (the UI XHR timeout matches). */
    web_transfer_deadline_set(esp_timer_get_time() + 600000000);

    ota_stream_t *st = calloc(1, sizeof(*st));
    uint8_t *buf = malloc(8192);
    audio_pack_entry_t *idx = NULL;
    pack_tx_t *ptx = NULL;
    char *scheme_json = NULL;
    size_t scheme_len = 0;
    sg_graph_t *scheme_graph = NULL;
    bool keep_recovery = false;
    if (st == NULL || buf == NULL) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    st->req = req;
    st->remaining = (int)req->content_len;

    uint8_t hdr[AUDIO_PACK_HDR_LEN];
    audio_pack_header_t ph;
    if (ota_stream_read(st, hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        !audio_pack_header_parse(hdr, sizeof(hdr), &ph)) {
        errmsg = "invalid pack header";
        goto fail;
    }
    idx = calloc(ph.count, sizeof(*idx));
    if (idx == NULL) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    uint32_t index_end = AUDIO_PACK_HDR_LEN;
    uint32_t payload_off = ph.data_off;
    uint64_t total_data = 0;
    uint32_t wav_count = 0;
    for (uint32_t i = 0; i < ph.count; ++i) {
        uint8_t eh[AUDIO_PACK_ENTRY_HDR_LEN];
        if (ota_stream_read(st, eh, sizeof(eh)) != (int)sizeof(eh)) { errmsg = "truncated index"; goto fail; }
        index_end += AUDIO_PACK_ENTRY_HDR_LEN;
        uint8_t nlen = 0;
        if (!audio_pack_entry_head(eh, req->content_len, &idx[i], &nlen)) { errmsg = "bad entry"; goto fail; }
        uint8_t nm[AUDIO_PACK_NAME_MAX + 1];
        if (ota_stream_read(st, nm, nlen) != (int)nlen) { errmsg = "truncated name"; goto fail; }
        index_end += nlen;
        if (!audio_pack_entry_name(&idx[i], nm, nlen)) { errmsg = "bad name"; goto fail; }
        if (idx[i].bits == AUDIO_PACK_BITS_SCHEME) {
            if (strcmp(idx[i].name, AUDIO_PACK_SCHEME_NAME) != 0) { errmsg = "bad scheme entry"; goto fail; }
        } else {
            wav_count++;
        }
        char base[WEB_LIB_NAME_MAX];
        if (!web_lib_base_name(idx[i].name, base, sizeof(base))) { errmsg = "bad name"; goto fail; }
        for (uint32_t j = 0; j < i; ++j) {
            if (strcasecmp(idx[j].name, base) == 0) { errmsg = "duplicate sound name"; goto fail; }
        }
        snprintf(idx[i].name, sizeof(idx[i].name), "%s", base);
        if (idx[i].data_off != payload_off) { errmsg = "non-contiguous payload"; goto fail; }
        payload_off += idx[i].data_len;
        if ((uint64_t)payload_off > (uint64_t)req->content_len) { errmsg = "payload past end"; goto fail; }
        total_data += idx[i].data_len;
    }
    if (wav_count == 0U) { errmsg = "no audio entries"; goto fail; }
    if (index_end != ph.data_off) { errmsg = "index length mismatch"; goto fail; }
    if ((uint64_t)payload_off != (uint64_t)req->content_len) { errmsg = "size mismatch"; goto fail; }

    uint64_t freeb = 0;
    (void)storage_get_free_bytes(&freeb);
    uint64_t need = total_data + (uint64_t)ph.count * 4096ULL + 262144ULL;
    /* Old files are kept intact while the new library is staged, so the peak
     * requirement is new data + overhead, not the post-delete free space. */
    if (freeb < need) {
        free(idx);
        free(st); free(buf);
        return send_json(req, "{\"ok\":false,\"error\":\"not enough space\"}");
    }

    ptx = calloc(1, sizeof(*ptx));
    if (ptx == NULL) {
        errmsg = "no transaction memory";
        goto fail_tx;
    }
    ptx->file_count = ph.count;
    ptx->files = calloc(ph.count, sizeof(*ptx->files));
    ptx->new_names = calloc(ph.count, sizeof(*ptx->new_names));
    if (ptx->files == NULL || ptx->new_names == NULL) {
        errmsg = "no transaction memory";
        goto fail_tx;
    }
    snprintf(ptx->journal, sizeof(ptx->journal), "%s", PACK_JOURNAL_PATH);
    snprintf(ptx->backup_dir, sizeof(ptx->backup_dir), WEB_AUDIO_DIR "/.pb%08lx_%08lx",
             (unsigned long)(uint32_t)esp_timer_get_time(),
             (unsigned long)web_transfer_next_sequence());
    ptx->old_count = 0;
    if (pack_capture_selection(ptx) != ESP_OK) {
        errmsg = "cannot read graph selection";
        goto fail_tx;
    }
    if (pack_collect_old(ptx) != ESP_OK) {
        errmsg = "cannot enumerate library";
        goto fail_tx;
    }
    if (ptx->old_count != 0U) {
        ptx->old_moved = calloc(ptx->old_count, sizeof(*ptx->old_moved));
        if (ptx->old_moved == NULL) {
            errmsg = "no transaction memory";
            goto fail_tx;
        }
    }

    web_audio_preview_stop();
    web_transfer_progress_slot(0);

    /* Stage all new files while the old library is still intact. */
    for (uint32_t i = 0; i < ph.count; ++i) {
        if (idx[i].bits == AUDIO_PACK_BITS_SCHEME) {
            scheme_len = idx[i].data_len;
            scheme_json = malloc(scheme_len + 1U);
            if (scheme_json == NULL) { errmsg = "no scheme memory"; goto fail_tx; }
            size_t got = 0;
            while (got < scheme_len) {
                size_t want = (scheme_len - got) > 8192U ? 8192U : (scheme_len - got);
                int r = ota_stream_read(st, (uint8_t *)scheme_json + got, want);
                if (r <= 0) break;
                got += (size_t)r;
                web_transfer_progress_bytes((int)req->content_len - st->remaining - (int)(st->len - st->pos));
            }
            if (got != scheme_len || memchr(scheme_json, '\0', scheme_len) != NULL) {
                errmsg = "invalid embedded scheme";
                goto fail_tx;
            }
            scheme_json[scheme_len] = '\0';
            scheme_graph = malloc(sizeof(*scheme_graph));
            if (scheme_graph == NULL) { errmsg = "no scheme memory"; goto fail_tx; }
            sg_diagnostic_t sdiag = {0};
            if (sg_parse(scheme_json, scheme_len, scheme_graph, &sdiag) != ESP_OK ||
                sg_validate(scheme_graph, &sdiag) != ESP_OK) {
                errmsg = sdiag.message[0] ? "embedded scheme validation failed" : "invalid embedded scheme";
                goto fail_tx;
            }
            continue;
        }
        char rel[128];
        FILE *f = create_private_file(ptx->files[i].staged,
                                      sizeof(ptx->files[i].staged),
                                      rel, sizeof(rel), "tmp");
        if (f == NULL) { errmsg = "staging failed"; goto fail_tx; }
        snprintf(ptx->files[i].canonical, sizeof(ptx->files[i].canonical),
                 WEB_AUDIO_DIR "/%s.wav", idx[i].name);
        if (ptx->new_count < ph.count) {
            snprintf(ptx->new_names[ptx->new_count], sizeof(ptx->new_names[0]),
                     "%s.wav", idx[i].name);
            ++ptx->new_count;
        }
        (void)setvbuf(f, NULL, _IOFBF, 8192);
        uint8_t wh[AUDIO_PACK_WAV_HDR_MAX];
        size_t whl = audio_pack_wav_header(wh, sizeof(wh), idx[i].data_len,
                                           idx[i].sample_rate, idx[i].channels, idx[i].bits);
        bool ok = whl > 0U && fwrite(wh, 1, whl, f) == whl;
        uint32_t left = idx[i].data_len;
        while (ok && left > 0U) {
            size_t want = left > 8192U ? 8192U : (size_t)left;
            int r = ota_stream_read(st, buf, want);
            if (r <= 0) { ok = false; break; }
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) { ok = false; break; }
            left -= (uint32_t)r;
            web_transfer_progress_bytes((int)req->content_len - st->remaining - (int)(st->len - st->pos));
        }
        if (ok && fflush(f) != 0) ok = false;
        if (fsync(fileno(f)) != 0) ok = false;
        if (fclose(f) != 0) ok = false;
        if (!ok || web_transfer_expired() || audio_inspect_wav(ptx->files[i].staged) != ESP_OK) {
            errmsg = "write failed";
            goto fail_tx;
        }
    }
    if (st->remaining != 0 || st->pos != st->len) {
        errmsg = "truncated pack payload";
        goto fail_tx;
    }

    /* The bundled project is replaced as a whole, so its assets may disappear.
     * Files used by any other saved project would be left dangling: reject
     * those, even though the pack carries a scheme. */
    {
        const char *except_id = scheme_graph != NULL ? scheme_graph->id : NULL;
        for (size_t i = 0; i < ptx->old_count; ++i) {
            if (pack_name_in_index(idx, ph.count, ptx->old_names[i])) continue;
            bool referenced = false;
            if (sg_store_file_referenced_except(except_id, ptx->old_names[i], &referenced) != ESP_OK) {
                errmsg = "graph reference check failed";
                goto fail_tx;
            }
            if (referenced) {
                errmsg = "graph asset referenced by another saved project";
                errstatus = 409;
                goto fail_tx;
            }
        }
    }

    /* Durable intent record before touching the old library. It records both
     * the backup directory and the names that will be published, so boot can
     * roll the transaction back after a power loss. */
    /* Record which scheme this transaction installs. Boot rollback restores the
     * old selection only while it still points at this scheme, so a factory
     * reset or a newer user choice is never overwritten. */
    ptx->new_selection[0] = '\0';
    ptx->new_selection_valid = scheme_graph != NULL;
    if (ptx->new_selection_valid) {
        snprintf(ptx->new_selection, sizeof(ptx->new_selection), "%s", scheme_graph->id);
    }
    if (pack_journal_write(ptx, PACK_JOURNAL_PREPARED) != ESP_OK) {
        errmsg = "recovery journal failed";
        goto fail_tx;
    }
    ptx->journal_written = true;
    /* Always create the backup directory, even for an empty old library: its
     * presence tells boot recovery that publication may have started. */
    if (WEB_MKDIR(ptx->backup_dir) != 0) {
        errmsg = "backup directory failed";
        goto fail_tx;
    }
    /* Move the old library aside. Nothing is deleted yet. */
    if (ptx->old_count != 0U) {
        char old_path[192], backup_path[224];
        for (size_t i = 0; i < ptx->old_count; ++i) {
            snprintf(old_path, sizeof(old_path), WEB_AUDIO_DIR "/%s", ptx->old_names[i]);
            snprintf(backup_path, sizeof(backup_path), "%s/%s",
                     ptx->backup_dir, ptx->old_names[i]);
            if (rename(old_path, backup_path) != 0) {
                errmsg = "backup move failed";
                goto fail_tx;
            }
            ptx->old_moved[i] = true;
        }
    }

    /* Publish staged files. A failure here rolls every old file back. */
    for (uint32_t i = 0; i < ph.count; ++i) {
        if (idx[i].bits == AUDIO_PACK_BITS_SCHEME) continue;
        if (rename(ptx->files[i].staged, ptx->files[i].canonical) != 0) {
            errmsg = "canonical publication failed";
            goto fail_tx;
        }
        ptx->files[i].staged[0] = '\0';
        ptx->files[i].published = true;
    }

    if (web_transfer_expired()) {
        errmsg = "transfer timeout";
        goto fail_tx;
    }

    /* The bundled scheme is part of the import transaction: validate and apply
     * it before the volume sidecar is replaced. The old library backup is still
     * present, so any failure here can roll the sounds back. */
    bool scheme_applied = false;
    uint32_t scheme_rev = 0;
    if (scheme_json != NULL && scheme_graph != NULL) {
        sg_diagnostic_t sdiag = {0};
        if (sg_assets_validate(scheme_graph, &sdiag) != ESP_OK) {
            errmsg = "embedded scheme does not match the pack";
            goto fail_tx;
        }
        if (sg_store_save_force(scheme_graph->id, scheme_json, scheme_len, &scheme_rev, &sdiag) != ESP_OK) {
            errmsg = "embedded scheme save failed";
            goto fail_tx;
        }
        sound_graph_prepared_t *prepared = NULL;
        if (sound_graph_prepare(scheme_graph, scheme_graph->id, scheme_rev, &prepared) != ESP_OK) {
            sound_graph_prepared_free(prepared);
            errmsg = "embedded scheme prepare failed";
            goto fail_tx;
        }
        if (sg_store_select(scheme_graph->id, scheme_rev) != ESP_OK) {
            sound_graph_prepared_free(prepared);
            errmsg = "embedded scheme select failed";
            goto fail_tx;
        }
        sound_graph_commit(prepared);
        sound_graph_prime_functions(web_func_levels());
        scheme_applied = true;
    }

    /* Per-file volumes are best-effort: a failure must not roll back an
     * already-applied scheme, it only leaves the default volume of 100. */
    {
        web_vol_t *v = malloc(sizeof(*v) * wav_count);
        bool ok = v != NULL;
        if (ok) {
            uint32_t vi = 0;
            for (uint32_t i = 0; i < ph.count; ++i) {
                if (idx[i].bits == AUDIO_PACK_BITS_SCHEME) continue;
                snprintf(v[vi].name, sizeof(v[vi].name), "%s", idx[i].name);
                v[vi].vol = idx[i].volume;
                vi++;
            }
            ok = web_vol_save(v, wav_count) == ESP_OK;
            free(v);
        }
        if (!ok) {
            ESP_LOGW(TAG_PACK, "PACK: volume sidecar not saved");
        }
    }

    esp_err_t meta_err = pack_reconcile_metadata();
    bool recovery_pending = false;
    if (meta_err != ESP_OK) {
        /* NVS tracks/manifest are not confirmed. Keep the new files, backup
         * directory and PREPARED journal so boot can roll back consistently;
         * never delete the only copy of the old library here. */
        ESP_LOGE(TAG_PACK, "PACK: metadata not confirmed (%s); backup/journal retained",
                 esp_err_to_name(meta_err));
        keep_recovery = true;
        errmsg = "metadata save failed; previous library will be restored on reboot";
        errstatus = 500;
        goto fail_tx;
    }
    if (pack_journal_write(ptx, PACK_JOURNAL_COMMITTED) != ESP_OK) {
        /* The PREPARED journal would make the next boot roll the library and
         * its graph selection back. Do not report success for a state that is
         * about to be undone. Keep all artifacts so the rollback is complete. */
        ESP_LOGE(TAG_PACK, "PACK: commit journal write failed; boot will roll back");
        keep_recovery = true;
        errmsg = "commit journal write failed; library will be rolled back on reboot";
        errstatus = 500;
        goto fail_tx;
    } else if (!pack_tx_commit_cleanup(ptx)) {
        recovery_pending = true;
    }
    pack_tx_free(ptx);
    ptx = NULL;

    ESP_LOGI(TAG_PACK, "PACK: %lu file(s), %lu bytes, scheme=%d",
             (unsigned long)wav_count, (unsigned long)total_data, (int)scheme_applied);
    web_log_event("Звук", "пакет: %lu звук(ов)%s", (unsigned long)wav_count,
                  scheme_applied ? " + звуковая схема" : "");
    char json[256];
    if (scheme_applied && scheme_graph != NULL) {
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"files\":%lu,\"bytes\":%lu,\"scheme\":\"%s\",\"revision\":%lu,"
                 "\"recovery_cleanup_pending\":%s}",
                 (unsigned long)wav_count, (unsigned long)total_data,
                 scheme_graph->id, (unsigned long)scheme_rev,
                 recovery_pending ? "true" : "false");
    } else {
        snprintf(json, sizeof(json),
                 "{\"ok\":true,\"files\":%lu,\"bytes\":%lu,\"recovery_cleanup_pending\":%s}",
                 (unsigned long)wav_count, (unsigned long)total_data,
                 recovery_pending ? "true" : "false");
    }
    free(scheme_json);
    free(scheme_graph);
    free(idx);
    free(st);
    free(buf);
    return send_json(req, json);

fail_tx:
    free(scheme_json);
    free(scheme_graph);
    if (!keep_recovery) pack_tx_rollback(ptx);
    pack_tx_free(ptx);
    free(idx);
    free(st);
    free(buf);
    if (errstatus == 409) {
        char json[128];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", errmsg);
        httpd_resp_set_status(req, "409 Conflict");
        return send_json(req, json);
    }
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, errmsg);

fail:
    free(scheme_json);
    free(scheme_graph);
    free(idx);
    free(st);
    free(buf);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errmsg);
}