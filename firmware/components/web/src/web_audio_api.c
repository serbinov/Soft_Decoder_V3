/* Sound-library preview, playback, status and browser-streaming endpoints.
 * Owns the single preview voice and its visible file name. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "ima_adpcm.h"
#include "settings.h"
#include "sound.h"
#include "sound_graph_store.h"
#include "storage.h"

static const char *TAG_AUDIO = "web";

typedef struct {
    char name[WEB_LIB_NAME_MAX];
    uint32_t size;
} web_lib_item_t;

static audio_voice_handle_t s_preview_voice;
static char s_preview_name[WEB_LIB_NAME_MAX];

/* A finished preview voice keeps its handle and name until the next play or an
 * explicit stop. The library/status pollers must see the preview as finished
 * even when another voice (engine sound, graph effect) is still playing. */
static void preview_sync(void)
{
    if (s_preview_name[0] == '\0') return;
    if (audio_voice_get_state(s_preview_voice) != AUDIO_VOICE_FINISHED) return;
    audio_voice_release_owned(s_preview_voice);
    memset(&s_preview_voice, 0, sizeof(s_preview_voice));
    s_preview_name[0] = '\0';
}

/* GET /api/sound-files — list of installed *.wav names for the block editor. */
esp_err_t sound_files_list_get(httpd_req_t *req)
{
    char *json = malloc(WEB_LIB_JSON_MAX);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    if (storage_access_begin() != ESP_OK) {
        free(json);
        return send_json(req, "{\"ok\":false,\"error\":\"storage busy\"}");
    }
    size_t used = 0;
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "{\"files\":[");
    bool first = true;
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!web_name_is_wav(ent->d_name)) continue;
            char en[WEB_LIB_NAME_MAX * 2];
            json_escape(ent->d_name, en, sizeof(en));
            buf_appendf(json, WEB_LIB_JSON_MAX, &used, "%s\"%s\"", first ? "" : ",", en);
            first = false;
        }
        closedir(d);
    }
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "]}");
    storage_access_end();
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

/* GET /sound-files/<name> — browser-playable PCM16 WAV. */
esp_err_t sound_file_get(httpd_req_t *req)
{
    const char *prefix = "/sound-files/";
    char dec[WEB_LIB_NAME_MAX * 2];
    url_decode(req->uri + strlen(prefix), dec, sizeof(dec));
    char base[WEB_LIB_NAME_MAX];
    if (!web_lib_base_name(dec, base, sizeof(base))) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    char path[192];
    if (storage_access_begin() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage busy");
    }
    if (!web_lib_resolve_path(base, path, sizeof(path))) {
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    (void)setvbuf(f, NULL, _IOFBF, 8192);

    uint16_t format, channels, block_align, bits, spb;
    uint32_t rate, data_off, data_len;
    if (!web_wav_probe(f, &format, &channels, &rate, &block_align, &bits, &spb,
                       &data_off, &data_len) ||
        !((format == 1U && bits == 16U) || format == 0x0011U)) {
        fclose(f);
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported audio");
    }
    (void)channels; (void)block_align; (void)bits; (void)spb;

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = ESP_OK;
    if (format == 0x0011U) {
        uint32_t blocks = data_len / IMA_ADPCM_BLOCK_ALIGN;
        uint32_t total = ima_adpcm_samples_for(data_len);
        uint8_t hdr[AUDIO_PACK_WAV_HDR_MAX];
        size_t hlen = audio_pack_wav_header(hdr, sizeof(hdr), total * 2U, rate, 1, 16);
        if (hlen == 0U || fseek(f, (long)data_off, SEEK_SET) != 0 ||
            httpd_resp_send_chunk(req, (const char *)hdr, (ssize_t)hlen) != ESP_OK) {
            err = ESP_FAIL;
        }
        uint8_t raw[IMA_ADPCM_BLOCK_ALIGN];
        int16_t pcm[IMA_ADPCM_SAMPLES_PER_BLOCK];
        for (uint32_t b = 0; err == ESP_OK && b < blocks; ++b) {
            if (fread(raw, 1, sizeof(raw), f) != sizeof(raw)) { err = ESP_FAIL; break; }
            int cnt = ima_adpcm_decode_block(raw, sizeof(raw), pcm, IMA_ADPCM_SAMPLES_PER_BLOCK);
            if (cnt <= 0) { err = ESP_FAIL; break; }
            if (httpd_resp_send_chunk(req, (const char *)pcm, (ssize_t)cnt * 2) != ESP_OK) {
                err = ESP_FAIL;
            }
        }
    } else {
        if (fseek(f, 0, SEEK_SET) != 0) {
            err = ESP_FAIL;
        }
        uint8_t buf[2048];
        size_t got;
        while (err == ESP_OK && (got = fread(buf, 1, sizeof(buf), f)) > 0U) {
            if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)got) != ESP_OK) {
                err = ESP_FAIL;
            }
        }
    }
    if (fclose(f) != 0 && err == ESP_OK) { err = ESP_FAIL; }
    storage_access_end();
    if (err != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t audio_status_get(httpd_req_t *req)
{
    preview_sync();
    settings_config_t cfg;
    if (!web_cfg_read(&cfg)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    char pe[WEB_LIB_NAME_MAX * 2];
    json_escape(s_preview_name, pe, sizeof(pe));
    char json[320];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"playing\":%s,\"active_slot\":%u,\"master_volume\":%u,"
             "\"engine_volume\":%u,\"effects_volume\":%u,\"preview\":\"%s\"}",
             audio_is_playing() ? "true" : "false",
             (unsigned)cfg.active_slot,
             (unsigned)cfg.master_volume,
             (unsigned)cfg.engine_volume,
             (unsigned)cfg.effects_volume, pe);
    return send_json(req, json);
}

esp_err_t audio_tracks_get(httpd_req_t *req)
{
    /* Heap, not stack: setting_track_t[20] + an 8 KB JSON frame would be ~12 KB
     * of the 16 KB httpd task stack. */
    settings_track_t *tracks = malloc(sizeof(settings_track_t) * SETTINGS_MAX_TRACKS);
    char *json = malloc(WEB_TRACKS_JSON_MAX);
    if (tracks == NULL || json == NULL) { free(tracks); free(json); return ESP_ERR_NO_MEM; }

    size_t count = 0;
    (void)settings_tracks_load(tracks, &count);
    ESP_LOGI(TAG_AUDIO, "GET /api/audio/tracks -> %u", (unsigned)count);

    size_t used = 0;
    buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "{\"ok\":true,\"cats\":[");
    for (size_t i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "%s%d",
                    i == 0 ? "" : ",", (int)web_func_track_cat(i));
    }
    buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "],\"tracks\":[");
    for (size_t i = 0; i < count; ++i) {
        char f[SETTINGS_TRACK_FILE_MAX * 2];
        char l[SETTINGS_TRACK_LABEL_MAX * 2];
        json_escape(tracks[i].file, f, sizeof(f));
        json_escape(tracks[i].label, l, sizeof(l));
        buf_appendf(json, WEB_TRACKS_JSON_MAX, &used,
                    "%s{\"slot\":%u,\"file\":\"%s\",\"label\":\"%s\",\"enabled\":%s}",
                    i == 0 ? "" : ",", (unsigned)tracks[i].slot, f, l,
                    tracks[i].enabled ? "true" : "false");
    }
    buf_appendf(json, WEB_TRACKS_JSON_MAX, &used, "]}");
    esp_err_t serr = send_json(req, json);
    ESP_LOGI(TAG_AUDIO, "tracks json len=%u send=%s: %.360s", (unsigned)used,
             esp_err_to_name(serr), json);
    free(tracks);
    free(json);
    return serr;
}

esp_err_t audio_play_post(httpd_req_t *req)
{
    if (!web_actuation_ready() || web_maintenance_active())
        return send_json(req, "{\"ok\":false,\"error\":\"actuation unavailable\"}");
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));

    /* Library preview: play one file by base name with its per-file volume. */
    char qname[WEB_LIB_NAME_MAX * 2];
    if (parse_query(query, "name", qname, sizeof(qname)) && qname[0] != '\0') {
        char base[WEB_LIB_NAME_MAX];
        if (!web_lib_base_name(qname, base, sizeof(base)))
            return send_json(req, "{\"ok\":false,\"error\":\"bad name\"}");
        char abs_path[192];
        if (!web_lib_resolve_path(base, abs_path, sizeof(abs_path))) {
            snprintf(abs_path, sizeof(abs_path), WEB_AUDIO_DIR "/%s.wav", base);
        }
        uint8_t vol = audio_library_volume_lookup(base);
        esp_err_t err = ESP_OK;
        if (audio_voice_get_state(s_preview_voice) == AUDIO_VOICE_FINISHED)
            err = audio_voice_alloc_owned(&s_preview_voice);
        if (err == ESP_OK)
            err = audio_voice_play_owned(&s_preview_voice, abs_path, false, vol);
        if (err != ESP_OK) {
            audio_voice_release_owned(s_preview_voice);
            memset(&s_preview_voice, 0, sizeof(s_preview_voice));
            s_preview_name[0] = '\0';
        } else {
            snprintf(s_preview_name, sizeof(s_preview_name), "%s", base);
            web_log_event("Звук", "плей: %s", base);
        }
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
        return send_json(req, json);
    }

    uint8_t slot = 0;
    parse_u8(query, "slot", &slot);

    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    size_t count = 0;
    (void)settings_tracks_load(tracks, &count);
    for (size_t i = 0; i < count; ++i) {
        if (tracks[i].slot == slot && tracks[i].file[0] != '\0') {
            if (web_cfg_lock(pdMS_TO_TICKS(100))) {
                web_cfg()->active_slot = slot;
                (void)settings_save_deferred(web_cfg());
                web_cfg_unlock();
            }
            char abs_path[160];
            snprintf(abs_path, sizeof(abs_path), WEB_USERDATA_DIR "/%s", tracks[i].file);
            esp_err_t err = ESP_OK;
            if (audio_voice_get_state(s_preview_voice) == AUDIO_VOICE_FINISHED)
                err = audio_voice_alloc_owned(&s_preview_voice);
            if (err == ESP_OK)
                err = audio_voice_play_owned(&s_preview_voice, abs_path, false, web_get_voice_volume(slot));
            if (err != ESP_OK) {
                audio_voice_release_owned(s_preview_voice);
                memset(&s_preview_voice, 0, sizeof(s_preview_voice));
            } else {
                s_preview_name[0] = '\0';
            }
            if (err == ESP_OK) {
                web_log_event("Звук", "слот %u", (unsigned)slot);
            }
            char json[64];
            snprintf(json, sizeof(json), "{\"ok\":%s}", err == ESP_OK ? "true" : "false");
            return send_json(req, json);
        }
    }
    return send_json(req, "{\"ok\":false,\"error\":\"no file\"}");
}

esp_err_t audio_stop_post(httpd_req_t *req)
{
    audio_voice_release_owned(s_preview_voice);
    memset(&s_preview_voice, 0, sizeof(s_preview_voice));
    s_preview_name[0] = '\0';
    web_log_event("Звук", "стоп предпрослушивания");
    return send_json(req, "{\"ok\":true}");
}

esp_err_t audio_volume_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    settings_config_t next;
    if (!web_cfg_read(&next)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    /* Only fields present in the request change: a partial slider update must
     * not reset the other two volumes to their defaults. */
    uint8_t old_m = next.master_volume;
    uint8_t old_e = next.engine_volume;
    uint8_t old_f = next.effects_volume;
    uint8_t v = 0;
    if (query_has_key(query, "master") && parse_u8(query, "master", &v)) {
        next.master_volume = v > 100U ? 100U : v;
    }
    if (query_has_key(query, "engine") && parse_u8(query, "engine", &v)) {
        next.engine_volume = v > 100U ? 100U : v;
    }
    if (query_has_key(query, "effects") && parse_u8(query, "effects", &v)) {
        next.effects_volume = v > 100U ? 100U : v;
    }
    bool vol_changed = (next.master_volume != old_m ||
                        next.engine_volume != old_e ||
                        next.effects_volume != old_f);
    uint8_t m = next.master_volume;
    uint8_t e = next.engine_volume;
    uint8_t f = next.effects_volume;
    /* Deferred: a slider drag must not trigger one flash write per move. */
    esp_err_t save_err = web_cfg_save(&next, true);
    if (save_err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    (void)audio_set_volume(m);
    sound_set_volumes(e, f);
    if (vol_changed) {
        web_log_event("Звук", "громкость: общ %u, двиг %u, эфф %u",
                      (unsigned)m, (unsigned)e, (unsigned)f);
    }
    return send_json(req, "{\"ok\":true}");
}

/* GET /api/audio/library — every installed WAV with its per-file volume. */
esp_err_t audio_library_get(httpd_req_t *req)
{
    preview_sync();
    esp_err_t err = storage_access_begin();
    if (err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"storage busy\"}");
    }
    web_lib_item_t *items = malloc(sizeof(*items) * AUDIO_PACK_MAX_FILES);
    web_vol_t *vols = malloc(sizeof(*vols) * AUDIO_PACK_MAX_FILES);
    char *json = malloc(WEB_LIB_JSON_MAX);
    if (items == NULL || vols == NULL || json == NULL) {
        free(items); free(vols); free(json);
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    size_t vn = web_vol_load(vols, AUDIO_PACK_MAX_FILES);
    size_t n = 0, total = 0;
    DIR *d = opendir(WEB_AUDIO_DIR);
    if (d != NULL) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!web_name_is_wav(ent->d_name)) continue;
            size_t l = strlen(ent->d_name);
            if (l - 4U >= WEB_LIB_NAME_MAX) continue;
            total++;
            if (n >= AUDIO_PACK_MAX_FILES) continue;
            memcpy(items[n].name, ent->d_name, l - 4U);
            items[n].name[l - 4U] = '\0';
            items[n].size = 0;
            n++;
        }
        closedir(d);
    }
    /* Read sizes with the directory stream closed (a plain fopen/ftell per
     * file, then sort). */
    for (size_t i = 0; i < n; ++i) {
        char path[320];
        snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s.wav", items[i].name);
        items[i].size = web_file_size(path);
    }
    storage_access_end();
    qsort(items, n, sizeof(*items), web_name_cmp);

    size_t used = 0;
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "{\"ok\":true,\"count\":%u,\"total\":%u,\"preview\":\"",
                (unsigned)n, (unsigned)total);
    char pe[WEB_LIB_NAME_MAX * 2];
    json_escape(s_preview_name, pe, sizeof(pe));
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "%s\",\"files\":[", pe);
    for (size_t i = 0; i < n; ++i) {
        uint8_t vol = 100;
        (void)web_vol_get(vols, vn, items[i].name, &vol);
        char en[WEB_LIB_NAME_MAX * 2];
        json_escape(items[i].name, en, sizeof(en));
        buf_appendf(json, WEB_LIB_JSON_MAX, &used,
                    "%s{\"name\":\"%s\",\"size\":%u,\"vol\":%u}",
                    i == 0U ? "" : ",", en, (unsigned)items[i].size, (unsigned)vol);
    }
    buf_appendf(json, WEB_LIB_JSON_MAX, &used, "]}");
    err = send_json(req, json);
    free(items);
    free(vols);
    free(json);
    return err;
}

/* POST /api/audio/library/volume?name=NAME&vol=0..100 */
esp_err_t audio_library_volume_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char raw[WEB_LIB_NAME_MAX * 2];
    uint8_t vol = 100;
    if (!parse_query(query, "name", raw, sizeof(raw)) || raw[0] == '\0' ||
        !parse_u8(query, "vol", &vol) || vol > 100U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid volume");
    }
    char base[WEB_LIB_NAME_MAX];
    if (!web_lib_base_name(raw, base, sizeof(base)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    esp_err_t err = audio_library_volume_set(base, vol);
    if (err != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    if (s_preview_name[0] != '\0' && strcmp(s_preview_name, base) == 0 &&
        audio_voice_get_state(s_preview_voice) != AUDIO_VOICE_FINISHED) {
        (void)audio_voice_set_volume_live(s_preview_voice.voice, vol);
    }
    return send_json(req, "{\"ok\":true}");
}

static bool track_file_is(const char *file, const char *wav_name)
{
    const char *base = strrchr(file, '/');
    base = base != NULL ? base + 1 : file;
    return strcasecmp(base, wav_name) == 0;
}

/* DELETE / POST /api/audio/library/delete?name=<name> — remove one library WAV
 * only when no saved graph, track slot or function binding uses it. Refusing
 * is deliberate: unlinking before unbinding metadata would leave a dangling
 * slot/scheme reference (and a later boot recovery could resurrect it). */
esp_err_t audio_library_delete_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char raw[WEB_LIB_NAME_MAX] = { 0 };
    char base[WEB_LIB_NAME_MAX] = { 0 };
    if (!parse_query(query, "name", raw, sizeof(raw)) ||
        !web_lib_base_name(raw, base, sizeof(base))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char wav_name[WEB_LIB_NAME_MAX + 8];
    snprintf(wav_name, sizeof(wav_name), "%s.wav", base);
    bool referenced = true;
    esp_err_t ref_err = sg_store_file_referenced(wav_name, &referenced);
    if (ref_err != ESP_OK) {
        return graph_error(req, "500 Internal Server Error", "graph reference check failed");
    }
    if (referenced) {
        return graph_error(req, "409 Conflict", "file referenced by saved sound scheme");
    }

    /* A library asset can also be bound as a legacy track slot, either by the
     * track list or by a canonical/legacy function binding. Check both before
     * unlinking so a delete cannot create a dangling sound reference. */
    settings_track_t *tracks = calloc(SETTINGS_MAX_TRACKS, sizeof(*tracks));
    if (tracks == NULL) {
        return graph_error(req, "500 Internal Server Error", "no memory");
    }
    size_t track_count = 0;
    esp_err_t tload = settings_tracks_load(tracks, &track_count);
    if (tload != ESP_OK && tload != ESP_ERR_NOT_FOUND) {
        free(tracks);
        return graph_error(req, "500 Internal Server Error", "tracks load failed");
    }
    bool slot_used[SETTINGS_MAX_TRACKS + 1] = { false };
    bool slot_ref = false;
    if (tload == ESP_OK) {
        if (track_count > SETTINGS_MAX_TRACKS) track_count = SETTINGS_MAX_TRACKS;
        for (size_t i = 0; i < track_count; ++i) {
            if (track_file_is(tracks[i].file, wav_name)) {
                slot_ref = true;
                if (tracks[i].slot >= 1U && tracks[i].slot <= SETTINGS_MAX_TRACKS) {
                    slot_used[tracks[i].slot] = true;
                }
            }
        }
    }
    free(tracks);
    if (slot_ref) {
        bool binding_ref = web_func_slots_referenced(slot_used);
        if (binding_ref) {
            return graph_error(req, "409 Conflict", "file bound to a function sound binding");
        }
        return graph_error(req, "409 Conflict", "file bound to a track slot");
    }
    if (strcmp(s_preview_name, base) == 0) {
        audio_voice_release_owned(s_preview_voice);
        memset(&s_preview_voice, 0, sizeof(s_preview_voice));
        s_preview_name[0] = '\0';
    }
    char path[192];
    if (!web_lib_resolve_path(base, path, sizeof(path))) {
        snprintf(path, sizeof(path), WEB_AUDIO_DIR "/%s.wav", base);
    }
    if (remove(path) != 0 && errno != ENOENT) {
        return send_json(req, "{\"ok\":false,\"error\":\"remove failed\"}");
    }
    web_log_event("Звук", "удалён %s.wav", base);
    char esc[WEB_LIB_NAME_MAX * 2];
    json_escape(base, esc, sizeof(esc));
    char json[160];
    snprintf(json, sizeof(json), "{\"ok\":true,\"name\":\"%s\"}", esc);
    return send_json(req, json);
}

/* Stream the raw stored WAV payload (without RIFF header) for /api/audio/raw/<name>. */
esp_err_t sound_file_raw_get(httpd_req_t *req)
{
    const char *prefix = "/api/audio/raw/";
    char dec[WEB_LIB_NAME_MAX * 2];
    url_decode(req->uri + strlen(prefix), dec, sizeof(dec));
    char base[WEB_LIB_NAME_MAX];
    if (!web_lib_base_name(dec, base, sizeof(base))) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    char path[192];
    if (storage_access_begin() != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage busy");
    }
    if (!web_lib_resolve_path(base, path, sizeof(path))) {
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    }
    uint16_t format, channels, block_align, bits, spb;
    uint32_t rate, data_off, data_len;
    if (!web_wav_probe(f, &format, &channels, &rate, &block_align, &bits, &spb,
                       &data_off, &data_len) ||
        !((format == 1U && bits == 16U) || format == 0x0011U)) {
        fclose(f);
        storage_access_end();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unsupported audio");
    }
    fclose(f);
    storage_access_end();
    char hdr_rate[16], hdr_ch[8], hdr_bits[8];
    snprintf(hdr_rate, sizeof(hdr_rate), "%lu", (unsigned long)rate);
    snprintf(hdr_ch, sizeof(hdr_ch), "%u", (unsigned)channels);
    snprintf(hdr_bits, sizeof(hdr_bits), "%u", (unsigned)(format == 0x0011U ? 4U : 16U));
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Audio-Rate", hdr_rate);
    httpd_resp_set_hdr(req, "X-Audio-Channels", hdr_ch);
    httpd_resp_set_hdr(req, "X-Audio-Bits", hdr_bits);
    /* Read a whole window under one lease, release it, then send. Reopening per
     * window (not per 2 KiB chunk) keeps the file handle valid across a possible
     * unmount while avoiding hundreds of LittleFS opens for a large preview. */
    uint8_t stack_buf[2048];
    uint8_t *window = malloc(16384);
    uint8_t *buf = window != NULL ? window : stack_buf;
    size_t window_size = window != NULL ? 16384U : sizeof(stack_buf);
    uint32_t offset = 0;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && offset < data_len) {
        if (storage_access_begin() != ESP_OK) { err = ESP_FAIL; break; }
        FILE *chunk = fopen(path, "rb");
        if (chunk == NULL) {
            storage_access_end();
            err = ESP_FAIL;
            break;
        }
        size_t want = (data_len - offset) > window_size ? window_size
                                                        : (size_t)(data_len - offset);
        size_t got = 0;
        if (fseek(chunk, (long)(data_off + offset), SEEK_SET) == 0) {
            got = fread(buf, 1, want, chunk);
        }
        fclose(chunk);
        storage_access_end();
        if (got == 0U) { err = ESP_FAIL; break; }
        offset += (uint32_t)got;
        err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)got);
    }
    free(window);
    if (err != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

void web_audio_preview_stop(void)
{
    audio_voice_release_owned(s_preview_voice);
    memset(&s_preview_voice, 0, sizeof(s_preview_voice));
    s_preview_name[0] = '\0';
}

void web_audio_preview_clear_if(const char *base)
{
    if (base != NULL && strcmp(s_preview_name, base) == 0) web_audio_preview_stop();
}
