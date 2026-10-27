/* WAV upload transaction and track-slot deletion. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "settings.h"
#include "sound.h"
#include "sound_graph_store.h"
#include "storage.h"

static const char *TAG_UPLOAD = "web";

/* Private files are never WAV orphan-recovery candidates. */
FILE *create_private_file(char *path, size_t path_len, char *rel, size_t rel_len,
                                 const char *extension)
{
    for (unsigned attempt = 0; attempt < 64U; ++attempt) {
        snprintf(rel, rel_len, "audio/u%08lx_%08lx.%s",
                 (unsigned long)(uint32_t)esp_timer_get_time(), (unsigned long)web_transfer_next_sequence(),
                 extension);
        snprintf(path, path_len, WEB_USERDATA_DIR "/%s", rel);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            FILE *f = fdopen(fd, "wb");
            if (f == NULL) { close(fd); (void)remove(path); path[0] = rel[0] = '\0'; }
            return f;
        }
        if (errno != EEXIST) break;
    }
    path[0] = rel[0] = '\0';
    return NULL;
}

FILE *create_staged_wav(char *path, size_t path_len, char *rel, size_t rel_len)
{
    return create_private_file(path, path_len, rel, rel_len, "tmp");
}

bool wav_artifacts_clean(wav_tx_t *tx)
{
    if (tx == NULL) return true;
    bool clean = true;
    for (size_t i = 0; i < tx->file_count; ++i) {
        char *paths[] = {tx->files[i].staged, tx->files[i].backup};
        for (size_t j = 0; j < 2U; ++j) {
            if (paths[j][0]) {
                if (remove(paths[j]) != 0 && errno != ENOENT) clean = false;
                else paths[j][0] = '\0';
            }
        }
    }
    if (clean && tx->journal[0]) {
        if (remove(tx->journal) != 0 && errno != ENOENT) clean = false;
        else tx->journal[0] = '\0';
    }
    return clean;
}

static bool wav_files_restore(wav_tx_t *tx, bool metadata_uncertain)
{
    if (tx == NULL) return true;
    bool restored = true;
    for (size_t n = tx->file_count; n > 0U; --n) {
        wav_file_tx_t *file = &tx->files[n - 1U];
        if (!file->published) continue;
        if (file->original) {
            if (rename(file->backup, file->canonical) == 0) {
                file->backup[0] = '\0';
                file->published = false;
            } else restored = false;
        } else if (!metadata_uncertain) {
            if (remove(file->canonical) == 0 || errno == ENOENT) file->published = false;
            else restored = false;
        } else {
            /* An uncertain NVS commit may reference this newly created path. */
            restored = false;
        }
    }
    return restored;
}

esp_err_t wav_files_prepare(wav_tx_t *tx)
{
    uint64_t required = 65536U; /* bounded reserve for manifest + recovery record */
    for (size_t i = 0; i < tx->file_count; ++i) {
        const char *name = strrchr(tx->files[i].canonical, '/');
        bool referenced = true;
        esp_err_t ref_err = sg_store_file_referenced(name ? name + 1 : tx->files[i].canonical, &referenced);
        if (ref_err != ESP_OK) return ref_err;
        if (referenced) { tx->graph_asset_conflict = true; return ESP_ERR_INVALID_STATE; }
        struct stat st;
        if (stat(tx->files[i].canonical, &st) == 0) {
            if (!S_ISREG(st.st_mode) || st.st_size < 0) return ESP_ERR_INVALID_STATE;
            tx->files[i].original = true;
            if ((uint64_t)st.st_size > UINT64_MAX - required) return ESP_ERR_NO_MEM;
            required += (uint64_t)st.st_size;
        } else if (errno != ENOENT) return ESP_FAIL;
    }
    uint64_t free_bytes = 0;
    if (storage_get_free_bytes(&free_bytes) != ESP_OK || free_bytes < required) return ESP_ERR_NO_MEM;
    uint8_t *buffer = malloc(8192);
    if (buffer == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < tx->file_count && err == ESP_OK; ++i) {
        wav_file_tx_t *file = &tx->files[i];
        if (!file->original) continue;
        char rel[128];
        FILE *src = fopen(file->canonical, "rb");
        FILE *dst = create_private_file(file->backup, sizeof(file->backup), rel, sizeof(rel), "bak");
        if (src == NULL || dst == NULL) err = ESP_FAIL;
        if (src != NULL && dst != NULL) {
            size_t n;
            while ((n = fread(buffer, 1, 8192, src)) > 0U) {
                if (web_transfer_aborted() || web_transfer_expired() ||
                    WEB_FWRITE(buffer, 1, n, dst) != n) { err = ESP_FAIL; break; }
                web_transfer_wdt_feed(n);
            }
            if (ferror(src)) err = ESP_FAIL;
        }
        if (src != NULL && fclose(src) != 0) err = ESP_FAIL;
        if (dst != NULL) {
            if (WEB_FFLUSH(dst) != 0 || fsync(fileno(dst)) != 0) err = ESP_FAIL;
            if (WEB_FCLOSE(dst) != 0) err = ESP_FAIL;
        }
    }
    free(buffer);
    if (err != ESP_OK) return err;
    char rel[128];
    FILE *journal = create_private_file(tx->journal, sizeof(tx->journal), rel, sizeof(rel), "bak");
    if (journal == NULL) return ESP_FAIL;
    /* Versioned recovery evidence, not an automatically replayed commit:
     * magic, counts, old config/tracks, then canonical-to-backup path records. */
    const char magic[8] = "WAVTXN1";
    uint32_t counts[2] = {(uint32_t)tx->previous_count, (uint32_t)tx->file_count};
    bool ok = WEB_FWRITE(magic, 1, sizeof(magic), journal) == sizeof(magic) &&
        WEB_FWRITE(counts, 1, sizeof(counts), journal) == sizeof(counts) &&
        WEB_FWRITE(&tx->previous_cfg, 1, sizeof(tx->previous_cfg), journal) == sizeof(tx->previous_cfg) &&
        WEB_FWRITE(tx->previous, sizeof(tx->previous[0]), tx->previous_count, journal) == tx->previous_count &&
        WEB_FWRITE(tx->files, sizeof(tx->files[0]), tx->file_count, journal) == tx->file_count;
    if (WEB_FFLUSH(journal) != 0 || fsync(fileno(journal)) != 0) ok = false;
    if (WEB_FCLOSE(journal) != 0) ok = false;
    return ok && !web_transfer_expired() ? ESP_OK : ESP_FAIL;
}

esp_err_t wav_files_publish(wav_tx_t *tx)
{
    /* Preflight every destination before publishing even the first file. */
    for (size_t i = 0; i < tx->file_count; ++i) {
        const char *name = strrchr(tx->files[i].canonical, '/');
        bool referenced = true;
        esp_err_t err = sg_store_file_referenced(name ? name + 1 : tx->files[i].canonical, &referenced);
        if (err != ESP_OK) return err;
        if (referenced) { tx->graph_asset_conflict = true; return ESP_ERR_INVALID_STATE; }
    }
    for (size_t i = 0; i < tx->file_count; ++i) {
        if (web_transfer_expired() || rename(tx->files[i].staged, tx->files[i].canonical) != 0) return ESP_FAIL;
        tx->files[i].staged[0] = '\0';
        tx->files[i].published = true;
    }
    return ESP_OK;
}

esp_err_t wav_tx_abort(httpd_req_t *req, wav_tx_t *tx, bool metadata_attempted,
                              bool boot_uncertain, const char *reason)
{
    bool metadata_restored = true;
    if (tx != NULL && metadata_attempted) {
        metadata_restored = settings_tracks_save(tx->previous, tx->previous_count) == ESP_OK;
        if (metadata_restored && settings_manifest_sync() != ESP_OK) metadata_restored = false;
    }
    bool restored = wav_files_restore(tx, !metadata_restored);
    bool clean = restored && metadata_restored && !boot_uncertain && wav_artifacts_clean(tx);
    char json[512];
    snprintf(json, sizeof(json),
             "{\"ok\":false,\"error\":\"%s\",\"partial\":%s,\"metadata_uncertain\":%s,\"boot_selection_uncertain\":%s,\"recovery\":\"%s\"}",
             reason, clean ? "false" : "true", metadata_restored ? "false" : "true",
             boot_uncertain ? "true" : "false",
             tx != NULL ? tx->journal : "");
    if (!clean && tx != NULL) ESP_LOGE(TAG_UPLOAD, "WAV recovery record retained: %s", tx->journal);
    free(tx);
    httpd_resp_set_status(req, strncmp(reason, "graph asset", 11) == 0 ? "409 Conflict" : "500 Internal Server Error");
    return send_json(req, json);
}

esp_err_t audio_upload_post(httpd_req_t *req)
{
    if (!storage_is_mounted()) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    }
    if (req->content_len <= 0 || req->content_len > UPLOAD_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }

    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0;
    bool asset_only = parse_bool(query, "asset", false);
    if ((strstr(query, "slot=") != NULL && !parse_u8(query, "slot", &slot)) ||
        slot > SETTINGS_MAX_TRACKS)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
    if (asset_only && slot != 0U)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "asset upload cannot bind a slot");

    char hdr[160] = { 0 };
    esp_err_t header_err = httpd_req_get_hdr_value_str(req, "X-File-Name", hdr, sizeof(hdr));
    if (header_err != ESP_OK && header_err != ESP_ERR_NOT_FOUND)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename header");

    char decoded[128] = { 0 };
    char encoded_name[sizeof(hdr) + 5];
    snprintf(encoded_name, sizeof(encoded_name), "n=%s", hdr);
    if (!parse_query(encoded_name, "n", decoded, sizeof(decoded)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename encoding");

    char name[64] = { 0 };
    if (strlen(decoded) >= sizeof(name))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename too long");
    if (decoded[0] != '\0') {
        sanitize_name(decoded, name, sizeof(name));
    }

    /* Reject storage-owned sidecars/backup names before they can be opened,
     * and require the .wav suffix for both bound and asset uploads. The
     * extension is normalized to lowercase so every later lookup (library,
     * play, delete, graph preflight) uses the same canonical name. */
    if (name[0] != '\0') {
        if (web_name_reserved(name)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "reserved filename");
        }
        if (!web_name_is_wav(name)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       asset_only ? "wav filename required for asset upload"
                                                  : "wav filename required");
        }
        web_name_lower_wav(name, sizeof(name));
    }

    if (asset_only && name[0] == '\0')
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wav filename required for asset upload");

    /* Load the track list first: it is also used to auto-assign a slot when the
     * request did not carry a valid one (slot 0 is not displayable/deletable).
     * Asset-only uploads bypass the 20 legacy slots entirely. */
    wav_tx_t *tx = calloc(1, sizeof(*tx));
    if (tx == NULL) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no transaction memory");
    settings_track_t *tracks = tx->tracks;
    size_t count = 0;
    bool have_tracks = false;
    if (!asset_only) {
        esp_err_t load_err = settings_tracks_load(tracks, &count);
        if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) {
            free(tx);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "tracks load failed");
        }
        have_tracks = load_err == ESP_OK;
        if (!have_tracks) count = 0;
    }
    if (!asset_only && slot == 0U) {
        for (uint16_t cand = 1U; cand <= SETTINGS_MAX_TRACKS && slot == 0U; ++cand) {
            bool used = false;
            for (size_t i = 0; have_tracks && i < count; ++i) {
                if (tracks[i].slot == cand) { used = true; break; }
            }
            if (!used) { slot = cand; }
        }
        if (slot == 0U) {
            free(tx);
            return send_json(req, "{\"ok\":false,\"error\":\"track slots full\"}");
        }
    }
    if (!asset_only && name[0] == '\0') {
        snprintf(name, sizeof(name), "slot%u.wav", (unsigned)slot);
    }

    tx->file_count = 1;
    tx->previous_count = asset_only ? 0U : count;
    memcpy(tx->previous, tracks, sizeof(tx->previous));
    if (!web_cfg_read(&tx->previous_cfg)) {
        free(tx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config busy");
    }
    char *path = tx->files[0].staged;
    /* Relative path (no /userdata prefix): matches the V0 convention and the
     * web UI which strips the "audio/" prefix for display. */
    char rel_file[128];
    char staging_rel[128];
    snprintf(rel_file, sizeof(rel_file), "audio/%s", name);
    snprintf(tx->files[0].canonical, sizeof(tx->files[0].canonical), WEB_USERDATA_DIR "/%s", rel_file);
    FILE *f = create_staged_wav(path, sizeof(tx->files[0].staged), staging_rel, sizeof(staging_rel));
    if (f == NULL) { free(tx); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); }

    ESP_LOGI(TAG_UPLOAD, "UP: enter slot=%u name=%s len=%d", (unsigned)slot, name, (int)req->content_len);

    ESP_LOGI(TAG_UPLOAD, "UP: opened %s", path);
    /* 16 KB stdio buffer: batches the SPIFFS flush so page-program time is
     * amortized (without it every 512 B write caps the upload at ~80 KB/s). */
    (void)setvbuf(f, NULL, _IOFBF, 16384);

    int total = 0;
    web_transfer_progress_begin(req->content_len, false, slot);
    esp_err_t perr = web_transfer_pipe_upload(req, f, &total);

    /* Always close the stream before removing the file: a short-circuit
     * would leave the handle open and make remove() fail on the host and on
     * LittleFS. */
    int close_rc = fflush(f);
    close_rc |= fsync(fileno(f));
    close_rc |= fclose(f);
    if (close_rc != 0) {
        ESP_LOGE(TAG_UPLOAD, "UP: flush/close failed total=%d/%d", total, (int)req->content_len);
        (void)remove(path);
        free(tx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }

    if (perr != ESP_OK || web_transfer_expired()) {
        ESP_LOGE(TAG_UPLOAD, "UP: fail total=%d/%d err=%s", total, (int)req->content_len, esp_err_to_name(perr));
        (void)remove(path);
        free(tx);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed");
    }
    ESP_LOGI(TAG_UPLOAD, "UP: done total=%d", total);

    /* Uploads run under maintenance, which inhibits playback. Inspection is
     * read-only and explicitly allowed while inhibited; audio_validate_wav()
     * is for callers that must not touch the filesystem during maintenance. */
    if (audio_inspect_wav(path) != ESP_OK || web_transfer_expired()) {
        (void)remove(path);
        free(tx);
        return send_json(req, "{\"ok\":false,\"error\":\"invalid wav\"}");
    }

    /* Bind the track to the slot, replacing any previous entry for it (also
     * compacts duplicate slot entries left over from earlier uploads so a
     * stale copy can never win the display / playback lookup). */
    size_t w = count;
    bool bind_track = !asset_only;
    if (bind_track) {
        bool replaced = false;
        w = 0;
        for (size_t i = 0; i < count; ++i) {
            if (tracks[i].slot == slot) {
                if (replaced) {
                    continue; /* drop duplicate entry for this slot */
                }
                strncpy(tracks[i].file, rel_file, sizeof(tracks[i].file) - 1);
                tracks[i].file[sizeof(tracks[i].file) - 1] = '\0';
                strncpy(tracks[i].label, name, sizeof(tracks[i].label) - 1);
                tracks[i].label[sizeof(tracks[i].label) - 1] = '\0';
                tracks[i].enabled = true;
                replaced = true;
            }
            if (w != i) {
                tracks[w] = tracks[i];
            }
            w++;
        }
        if (!replaced && w < SETTINGS_MAX_TRACKS) {
            tracks[w].slot = slot;
            strncpy(tracks[w].file, rel_file, sizeof(tracks[w].file) - 1);
            tracks[w].file[sizeof(tracks[w].file) - 1] = '\0';
            strncpy(tracks[w].label, name, sizeof(tracks[w].label) - 1);
            tracks[w].label[sizeof(tracks[w].label) - 1] = '\0';
            tracks[w].enabled = true;
            w++;
            replaced = true;
        }
        if (!replaced) {
            ESP_LOGE(TAG_UPLOAD, "UP: no free track slot for slot=%u", (unsigned)slot);
            (void)remove(path);
            free(tx);
            return send_json(req, "{\"ok\":false,\"error\":\"track slots full\"}");
        }
        count = w;
    }
    tx->count = bind_track ? count : 0U;

    esp_err_t file_err = wav_files_prepare(tx);
    if (file_err != ESP_OK)
        return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project" : "backup/space preflight failed");
    file_err = wav_files_publish(tx);
    if (file_err != ESP_OK)
        return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project" : "canonical publication failed");

    if (bind_track) {
        esp_err_t save_err = settings_tracks_save(tracks, count);
        ESP_LOGI(TAG_UPLOAD, "UP: track bound slot=%u count=%u save=%s",
                 (unsigned)slot, (unsigned)count, esp_err_to_name(save_err));
        if (save_err != ESP_OK || web_transfer_expired()) {
            ESP_LOGE(TAG_UPLOAD, "UP: tracks save failed, keeping old track");
            return wav_tx_abort(req, tx, true, false, "tracks save failed");
        }
        if (settings_manifest_sync() != ESP_OK || web_transfer_expired())
            return wav_tx_abort(req, tx, true, false, "manifest save failed");
        web_func_refresh_tracks();
    }
    bool cleanup_pending = !wav_artifacts_clean(tx);
    char recovery[160];
    snprintf(recovery, sizeof(recovery), "%s", tx->journal);
    free(tx);

    /* Only this canonical asset was explicitly replaced. Never unlink an old
     * different slot path: saved projects and other slots may still use it. */

    char esc_file[320];
    json_escape(rel_file, esc_file, sizeof(esc_file));
    char esc_label[128];
    json_escape(name, esc_label, sizeof(esc_label));
    char json[768];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"asset\":%s,\"slot\":%u,\"file\":\"%s\",\"label\":\"%s\",\"enabled\":true,\"bytes\":%d,\"recovery_cleanup_pending\":%s,\"recovery\":\"%s\"}",
             asset_only ? "true" : "false", (unsigned)(asset_only ? 0U : slot), esc_file,
             esc_label, (int)req->content_len, cleanup_pending ? "true" : "false", recovery);
    return send_json(req, json);
}

esp_err_t audio_track_delete_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0;
    parse_u8(query, "slot", &slot);

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    if (!parse_u8(query, "slot", &slot) || slot < 1U || slot > SETTINGS_MAX_TRACKS)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
    if (settings_tracks_load(tracks, &count) == ESP_OK) {
        size_t w = 0;
        for (size_t i = 0; i < count; ++i) {
            if (tracks[i].slot == slot) {
                continue;
            }
            if (w != i) {
                tracks[w] = tracks[i];
            }
            w++;
        }
        count = w;
    } else {
        return send_json(req, "{\"ok\":false,\"error\":\"tracks load failed\"}");
    }

    esp_err_t save_err = settings_tracks_save(tracks, count);
    if (save_err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"tracks save failed\"}");
    }
    web_func_refresh_tracks();

    /* Unbind, do not unlink a path potentially owned by a sound project. */

    bool was_active = false;
    if (web_cfg_lock(pdMS_TO_TICKS(100))) {
        was_active = web_cfg()->active_slot == slot;
        if (was_active) {
            web_cfg()->active_slot = 0;
            (void)settings_save(web_cfg());
        }
        web_cfg_unlock();
    }
    if (was_active) web_audio_preview_stop();

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"slot\":%u}", (unsigned)slot);
    return send_json(req, json);
}
