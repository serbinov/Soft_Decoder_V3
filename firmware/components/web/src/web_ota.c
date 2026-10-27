/* OTA update uploads, including the combined firmware + sounds container. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "settings.h"
#include "sound.h"
#include "storage.h"

static const char *TAG_OTA = "web";

/* Basename, keep [A-Za-z0-9._-] and UTF-8 bytes, force a .wav extension. */
static void ota_safe_name(const char *in, char *out, size_t out_len)
{
    if (out_len < 6U) {
        if (out_len != 0U) {
            out[0] = '\0';
        }
        return;
    }
    const char *base = in;
    for (const char *p = in; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t w = 0;
    for (const char *p = base; *p != '\0' && w + 5U < out_len; ++p) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out[w++] = c;
        } else if ((unsigned char)c >= 0x80U) {
            out[w++] = c; /* keep UTF-8 so Cyrillic names survive */
        } else {
            out[w++] = '_';
        }
    }
    /* Terminate before the extension check: the comparison must not read
     * whatever stale bytes follow the copied name. */
    out[w] = '\0';
    if (w < 4U || strcasecmp(out + (w - 4U), ".wav") != 0) {
        if (w + 4U < out_len) {
            out[w++] = '.';
            out[w++] = 'w';
            out[w++] = 'a';
            out[w++] = 'v';
        }
    } else {
        /* Normalize .WAV/.Wav to the canonical lowercase extension. */
        out[w - 4U] = '.';
        out[w - 3U] = 'w';
        out[w - 2U] = 'a';
        out[w - 1U] = 'v';
    }
    out[w] = '\0';
}

static int ota_slot_from_name(const char *name)
{
    if (strncmp(name, "slot", 4) != 0) {
        return 0;
    }
    char *end = NULL;
    long s = strtol(name + 4, &end, 10);
    if (end == name + 4 || *end != '.' || s < 1 || s > SETTINGS_MAX_TRACKS) {
        return 0;
    }
    return (int)s;
}

/* POST /api/ota/update
 * - plain ESP image          -> written to the next OTA partition;
 * - "AURAOTA2" container      -> firmware to the OTA partition and each sound
 *                               file to /userdata, then reboot. Lets one
 *                               upload carry both a firmware and its sounds. */
esp_err_t ota_update_post(httpd_req_t *req)
{
    web_transfer_reboot_set(false);
    if (req->content_len <= 0 || req->content_len > (8 * 1024 * 1024)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid size");
    }
    web_transfer_deadline_set(web_transfer_ota_deadline((int)req->content_len));

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (update == NULL) {
        ESP_LOGE(TAG_OTA, "OTA: no update partition");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
    }

    ota_stream_t *st = calloc(1, sizeof(ota_stream_t));
    uint8_t *buf = malloc(8192);
    if (st == NULL || buf == NULL) {
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    st->req = req;
    st->remaining = req->content_len;

    uint8_t hdr[OTA_CONTAINER_HDR_LEN];
    if (ota_stream_read(st, hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short header");
    }

    ota_container_hdr_t ch;
    bool combined = ota_container_parse(hdr, sizeof(hdr), &ch);
    if (!combined && memcmp(hdr, OTA_CONTAINER_MAGIC, OTA_CONTAINER_MAGIC_LEN) == 0) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid container header");
    }
    uint32_t fw_len;
    uint32_t n_files = 0;
    if (combined) {
        fw_len = ch.fw_len;
        n_files = ch.n_files;
        if (n_files > SETTINGS_MAX_TRACKS ||
            fw_len > (uint32_t)req->content_len - OTA_CONTAINER_HDR_LEN ||
            (n_files > 0U && !storage_is_mounted())) {
            free(st); free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid container size/storage");
        }
    } else {
        fw_len = (uint32_t)req->content_len; /* the 16 bytes are firmware start */
    }
    if (fw_len == 0U || fw_len > update->size) {
        ESP_LOGE(TAG_OTA, "OTA: bad firmware length %lu (partition %lu)",
                 (unsigned long)fw_len, (unsigned long)update->size);
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware too large");
    }
    if (!combined && hdr[0] != 0xE9U) {
        free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid firmware header");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_OTA, "OTA begin: %s", esp_err_to_name(err));
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }

    web_transfer_progress_begin(req->content_len, true, 0);
    ESP_LOGI(TAG_OTA, "OTA: begin %s size=%d combined=%d fw=%lu files=%lu",
             update->label, (int)req->content_len, combined ? 1 : 0,
             (unsigned long)fw_len, (unsigned long)n_files);

    uint32_t written = 0;
    bool fail = false;
    if (!combined) {
        err = esp_ota_write(handle, hdr, sizeof(hdr));
        if (err != ESP_OK) {
            fail = true;
        } else {
            written = (uint32_t)sizeof(hdr);
        }
    }
    while (!fail && written < fw_len) {
        size_t want = fw_len - written;
        if (want > 8192U) {
            want = 8192U;
        }
        int r = ota_stream_read(st, buf, want);
        if (r <= 0) {
            fail = true;
            break;
        }
        err = esp_ota_write(handle, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_OTA, "OTA write: %s", esp_err_to_name(err));
            fail = true;
            break;
        }
        written += (uint32_t)r;
        web_transfer_progress_bytes((int)written);
        web_transfer_wdt_feed((size_t)r);
    }
    if (fail || written != fw_len) {
        ESP_LOGE(TAG_OTA, "OTA: abort, fw written=%lu/%lu", (unsigned long)written,
                 (unsigned long)fw_len);
        web_transfer_progress_end();
        (void)esp_ota_abort(handle);
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_OTA, "OTA end: %s", esp_err_to_name(err));
        web_transfer_progress_end();
        free(st);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
    }
    uint32_t files_ok = 0;
    wav_tx_t *tx = n_files > 0U ? calloc(1, sizeof(*tx)) : NULL;
    if (n_files > 0U && tx == NULL) {
        web_transfer_progress_end(); free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no transaction memory; boot unchanged");
    }
    settings_track_t *tracks = tx != NULL ? tx->tracks : NULL;
    size_t tn = 0;
    bool metadata_attempted = false;
    uint32_t incoming_slots = 0U;
    if (n_files > 0U) {
        esp_err_t load_err = settings_tracks_load(tracks, &tn);
        if (load_err != ESP_OK && load_err != ESP_ERR_NOT_FOUND) {
            web_transfer_progress_end(); free(st); free(buf); free(tx);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "tracks load failed; boot unchanged");
        }
        if (load_err != ESP_OK) tn = 0;
        tx->file_count = n_files;
        memcpy(tx->previous, tracks, sizeof(tx->previous));
        tx->previous_count = tn;
        if (!web_cfg_read(&tx->previous_cfg)) {
            web_transfer_progress_end(); free(st); free(buf); free(tx);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config busy");
        }
    }
    if (combined && n_files > 0U && storage_is_mounted()) {
        for (uint32_t i = 0; i < n_files; ++i) {
            uint8_t fh[OTA_FILE_HDR_LEN];
            if (ota_stream_read(st, fh, sizeof(fh)) != (int)sizeof(fh)) {
                break;
            }
            ota_file_hdr_t f;
            if (!ota_file_hdr_parse(fh, sizeof(fh), &f)) {
                break;
            }
            char raw[OTA_FILE_NAME_MAX + 1] = { 0 };
            char label[OTA_FILE_LABEL_MAX + 1] = { 0 };
            if (ota_stream_read(st, (uint8_t *)raw, f.name_len) != (int)f.name_len) {
                break;
            }
            if (f.label_len > 0U &&
                ota_stream_read(st, (uint8_t *)label, f.label_len) != (int)f.label_len) {
                break;
            }
            if (memchr(raw, '\0', f.name_len) != NULL ||
                memchr(label, '\0', f.label_len) != NULL || f.data_len > UPLOAD_MAX ||
                f.data_len > (uint32_t)(st->remaining + (int)(st->len - st->pos))) break;

            char safe[80];
            ota_safe_name(raw, safe, sizeof(safe));
            snprintf(tx->files[i].canonical, sizeof(tx->files[i].canonical), WEB_AUDIO_DIR "/%s", safe);
            for (uint32_t j = 0; j < i; ++j) {
                if (strcmp(tx->files[j].canonical, tx->files[i].canonical) == 0) fail = true;
            }
            if (fail) break;
            uint64_t free_bytes = 0;
            if (storage_get_free_bytes(&free_bytes) != ESP_OK || free_bytes < (uint64_t)f.data_len + 65536U) {
                err = ESP_ERR_NO_MEM; fail = true; break;
            }
            char *path = tx->files[i].staged;
            char rel[SETTINGS_TRACK_FILE_MAX];
            char staging_rel[128];
            snprintf(rel, sizeof(rel), "audio/%s", safe);
            FILE *fp = create_staged_wav(path, sizeof(tx->files[i].staged), staging_rel, sizeof(staging_rel));
            if (fp != NULL) {
                (void)setvbuf(fp, NULL, _IOFBF, 4096);
            }

            uint32_t left = f.data_len;
            bool read_ok = true;
            bool write_ok = fp != NULL;
            while (left > 0U) {
                size_t want = left > 8192U ? 8192U : left;
                int r = ota_stream_read(st, buf, want);
                if (r <= 0) {
                    read_ok = false;
                    break;
                }
                if (fp != NULL && write_ok && WEB_FWRITE(buf, 1, (size_t)r, fp) != (size_t)r) {
                    write_ok = false;
                }
                left -= (uint32_t)r;
                web_transfer_progress_bytes(req->content_len - st->remaining - (int)(st->len - st->pos));
                if (write_ok) web_transfer_wdt_feed((size_t)r);
            }
            if (fp != NULL) {
                if (write_ok && WEB_FFLUSH(fp) != 0) {
                    write_ok = false;
                }
                if (fsync(fileno(fp)) != 0) write_ok = false;
                if (WEB_FCLOSE(fp) != 0) {
                    write_ok = false;
                }
            }
            if (!read_ok) {
                if (fp != NULL) {
                    (void)remove(path);
                }
                break; /* body desynced: stop */
            }
            if (!write_ok) {
                ESP_LOGW(TAG_OTA, "OTA: file '%s' write failed", safe);
                (void)remove(path);
                break;
            }
            if (audio_inspect_wav(path) != ESP_OK) {
                ESP_LOGW(TAG_OTA, "OTA: file '%s' invalid wav", safe);
                (void)remove(path);
                break;
            }
            files_ok++;

            {
                int slot = ota_slot_from_name(safe);
                if (slot == 0) {
                    slot = 1;
                    while (slot <= SETTINGS_MAX_TRACKS) {
                        bool used = (incoming_slots & (1UL << (slot - 1))) != 0U;
                        if (!used) {
                            break;
                        }
                        slot++;
                    }
                }
                if (slot >= 1 && slot <= SETTINGS_MAX_TRACKS) {
                    if ((incoming_slots & (1UL << (slot - 1))) != 0U) { fail = true; break; }
                    incoming_slots |= 1UL << (slot - 1);
                    size_t index = 0;
                    while (index < tn && tracks[index].slot != (uint8_t)slot) ++index;
                    if (index >= SETTINGS_MAX_TRACKS) { fail = true; break; }
                    if (index == tn) ++tn;
                    tracks[index].slot = (uint8_t)slot;
                    snprintf(tracks[index].file, sizeof(tracks[index].file), "%s", rel);
                    if (label[0] != '\0') {
                        snprintf(tracks[index].label, sizeof(tracks[index].label), "%.63s", label);
                    } else {
                        snprintf(tracks[index].label, sizeof(tracks[index].label), "%.63s", safe);
                        char *dot = strrchr(tracks[index].label, '.');
                        if (dot != NULL) {
                            *dot = '\0';
                        }
                    }
                    tracks[index].enabled = true;
                } else {
                    fail = true;
                    break;
                }
            }
        }
    }

    if (fail || web_transfer_expired() || files_ok != n_files || st->remaining != 0 || st->pos != st->len) {
        (void)wav_artifacts_clean(tx);
        free(tx);
        web_transfer_progress_end(); free(st); free(buf);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete/invalid container; boot unchanged");
    }
    if (n_files > 0U) {
        tx->count = tn;
        err = wav_files_prepare(tx);
        if (err != ESP_OK) {
            web_transfer_progress_end(); free(st); free(buf);
            return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project; boot unchanged" : "backup/space preflight failed; boot unchanged");
        }
        err = wav_files_publish(tx);
        if (err != ESP_OK) {
            web_transfer_progress_end(); free(st); free(buf);
            return wav_tx_abort(req, tx, false, false, tx->graph_asset_conflict ? "graph asset referenced by saved project; boot unchanged" : "canonical publication failed; boot unchanged");
        }
        metadata_attempted = true;
        err = settings_tracks_save(tracks, tn);
        if (err == ESP_OK) err = settings_manifest_sync();
        if (err != ESP_OK || web_transfer_expired()) {
            web_transfer_progress_end(); free(st); free(buf);
            return wav_tx_abort(req, tx, true, false, "metadata incomplete; boot unchanged");
        }
    }
    bool boot_attempted = !web_transfer_expired();
    err = boot_attempted ? esp_ota_set_boot_partition(update) : ESP_ERR_TIMEOUT;
    if (err != ESP_OK) {
        web_transfer_progress_end(); free(st); free(buf);
        return wav_tx_abort(req, tx, metadata_attempted, boot_attempted, "ota boot failed; restoration attempted");
    }
    bool cleanup_pending = !wav_artifacts_clean(tx);
    char recovery[160] = {0};
    if (tx != NULL) snprintf(recovery, sizeof(recovery), "%s", tx->journal);
    free(tx);

    web_transfer_progress_end();
    free(st);
    free(buf);

    ESP_LOGI(TAG_OTA, "OTA: success, rebooting...");
    if (combined) {
        web_log_event("OTA", "прошивка + %lu звук(ов), перезагрузка",
                      (unsigned long)files_ok);
    } else {
        web_log_event("OTA", "прошивка записана, перезагрузка");
    }
    char json[384];
    snprintf(json, sizeof(json), "{\"ok\":true,\"bytes\":%d,\"files\":%lu,\"recovery_cleanup_pending\":%s,\"recovery\":\"%s\"}",
             (int)req->content_len, (unsigned long)files_ok, cleanup_pending ? "true" : "false", recovery);
    send_json(req, json);
    web_transfer_reboot_set(true);
    return ESP_OK;
}
