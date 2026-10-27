/* HTTP server, route table, admission dispatcher and component init. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"
#include "sound_editor_assets.h"

#include <stddef.h>
#include <string.h>

#include "audio.h"
#include "esp_log.h"
#include "settings.h"
#include "storage.h"

static const char *TAG = "web";

static httpd_handle_t s_server;
static size_t s_route_count;

static esp_err_t root_handler(httpd_req_t *req)
{
    return send_root_html(req);
}

static esp_err_t sound_editor_get(httpd_req_t *req)
{
    size_t len = strcspn(req->uri, "?");
    const char *path = req->uri;
    if ((len == 13U && strncmp(path, "/sound-editor", len) == 0) ||
        (len == 14U && strncmp(path, "/sound-editor/", len) == 0)) {
        path = "/sound-editor/blocks.html";
        len = strlen(path);
    }
    for (size_t i = 0; i < sound_editor_asset_count; ++i) {
        const sound_editor_asset_t *a = &sound_editor_assets[i];
        if (strlen(a->path) != len || strncmp(path, a->path, len) != 0) continue;
        httpd_resp_set_type(req, a->mime);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
        httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
        httpd_resp_set_hdr(req, "Cache-Control", a->immutable ? "public, max-age=31536000, immutable" : "no-cache");
        return httpd_resp_send(req, (const char *)a->data, a->length);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "asset not found");
}












































/* ---------- sound library (unpacked WAV files under the audio dir) ---------
 * The "Sound upload" page lists every WAV on the storage with a play button and
 * a per-file volume. A pack upload replaces the whole library. Volumes persist
 * in a small sidecar text file so they survive a reboot and match by file name
 * (the pack index seeds them on unpack). */














/* ---------- browser playback over HTTP (play a sound on the PC) -----------
 * The block editor and the sound page can play a file in the browser of the PC
 * that has the UI open. PCM16 files are served as-is; IMA ADPCM files are
 * decoded on the fly into a PCM16 WAV, since browsers cannot play ADPCM. */











/* The asynchronous transfer task owns its FILE and buffer until close. No
 * second writer can outlive an aborted request or its stack context. */


















































/* ---------- server ---------- */











/* Browser CSRF guard: a mutating request with an Origin/Referer must name the
 * same authority as Host. Requests without either header (curl, device-to-
 * device clients) remain accepted. */
static bool request_same_origin(httpd_req_t *req)
{
    char origin[256] = { 0 };
    char referer[512] = { 0 };
    char host[256] = { 0 };
    esp_err_t origin_err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    esp_err_t referer_err = httpd_req_get_hdr_value_str(req, "Referer", referer, sizeof(referer));
    esp_err_t host_err = httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    if ((origin_err != ESP_OK && origin_err != ESP_ERR_NOT_FOUND) ||
        (referer_err != ESP_OK && referer_err != ESP_ERR_NOT_FOUND) ||
        (host_err != ESP_OK && host_err != ESP_ERR_NOT_FOUND)) {
        return false;
    }
    return web_util_same_origin(origin_err == ESP_OK ? origin : NULL,
                                referer_err == ESP_OK ? referer : NULL,
                                host_err == ESP_OK ? host : NULL);
}

static esp_err_t mutation_dispatch(httpd_req_t *req)
{
    if (!request_same_origin(req)) {
        return close_rejected_body(req,
            httpd_resp_send_err(req, (httpd_err_code_t)403, "cross-origin request"));
    }
    char query[WEB_QUERY_MAX] = { 0 };
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_req_get_url_query_len(req) >= sizeof(query) ||
        (query_err != ESP_OK && query_err != ESP_ERR_NOT_FOUND) || !query_encoding_valid(query))
        return close_rejected_body(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query"));
    const web_route_t *route = (const web_route_t *)req->user_ctx;
    if (route == NULL || route->handler == NULL)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "missing route");
    esp_err_t (*handler)(httpd_req_t *) = route->handler;
    if (route->admit == WEB_ADMIT_TRANSFER)
        return web_transfer_start(req, handler, handler == ota_update_post);
    if (req->content_len != 0U && route->admit != WEB_ADMIT_JSON)
        return close_rejected_body(req, httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unexpected body"));
    /* Cancellation only closes its own calibration, even before actuation admission. */
    if (route->admit == WEB_ADMIT_BEMF_CAL) {
        int action = bemf_cal_action(query);
        if (action == 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid calibration action");
        if (action == 3) return handler(req);
    }
    if (route->admit == WEB_ADMIT_BEMF_USE) {
        bool enabled;
        if (!bemf_bool_query(query, "enabled", &enabled))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid enabled");
    }
    bool ready = false;
    bool admitted = web_control_admit_mutation(&ready);
    if (!admitted) return close_rejected_body(req, graph_error(req, "503 Service Unavailable", "maintenance"));
    bool actuation = route->admit == WEB_ADMIT_ACTUATION ||
                     route->admit == WEB_ADMIT_BEMF_CAL ||
                     route->admit == WEB_ADMIT_BEMF_USE;
    esp_err_t err;
    if (actuation && !ready) err = send_json(req, "{\"ok\":false,\"error\":\"actuation not ready\"}");
    else if (route->admit == WEB_ADMIT_STORAGE) {
        /* Deleting an unreferenced library asset must not inhibit the motor or
         * the sound engine: a storage lease is enough to keep the filesystem
         * mounted while the directory entry is removed. */
        err = storage_access_begin();
        if (err == ESP_OK) {
            err = handler(req);
            storage_access_end();
        } else err = send_json(req, "{\"ok\":false,\"error\":\"storage unavailable or maintenance\"}");
    } else if (route->admit == WEB_ADMIT_MAINTENANCE) {
        err = web_maintenance_begin(false);
        if (err == ESP_OK) {
            err = storage_access_begin();
            if (err == ESP_OK) { err = handler(req); storage_access_end(); }
            web_maintenance_end();
        }
        if (err != ESP_OK) err = send_json(req, "{\"ok\":false,\"error\":\"delete admission failed\"}");
    } else if (route->admit == WEB_ADMIT_JSON) {
        err = storage_access_begin();
        if (err == ESP_OK) {
            err = handler(req);
            storage_access_end();
        } else err = close_rejected_body(req, graph_error(req, "503 Service Unavailable", "storage unavailable or maintenance"));
    } else err = handler(req);
    web_control_finish_mutation();
    return err;
}

void web_server_register_routes(const web_route_t *routes, size_t count)
{
    if (routes == NULL) return;
    for (size_t i = 0; i < count; ++i) {
        httpd_uri_t u = { .uri = routes[i].uri, .method = routes[i].method,
            .handler = routes[i].method == HTTP_POST ? mutation_dispatch : routes[i].handler,
            .user_ctx = (void *)&routes[i] };
        esp_err_t err = httpd_register_uri_handler(s_server, &u);
        if (err == ESP_OK) {
            s_route_count++;
        } else {
            ESP_LOGE(TAG, "route %s registration failed: %s", routes[i].uri, esp_err_to_name(err));
        }
    }
}

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Fixed at 80 (see wifi_post): the captive redirect has no port. */
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.stack_size = 16384;
    cfg.max_uri_handlers = 64;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    /* More sockets so the 3 s status poll + a transfer + the port-81 progress
     * poll never exhaust the pool; short timeouts so one stalled client cannot
     * hold a socket for a whole minute. */
    cfg.max_open_sockets = 8;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }
    if (web_transfer_start_supervisor() != ESP_OK) {
        ESP_LOGW(TAG, "transfer supervisor create failed");
    }

    static const web_route_t routes[] = {
        { "/", HTTP_GET, root_handler, WEB_ADMIT_PLAIN },
        { "/sound-editor", HTTP_GET, sound_editor_get, WEB_ADMIT_PLAIN },
        { "/sound-editor/*", HTTP_GET, sound_editor_get, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/capabilities", HTTP_GET, graph_capabilities_get, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/projects", HTTP_GET, graph_projects_get, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/project", HTTP_GET, graph_project_get, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/state", HTTP_GET, graph_state_get, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/asset", HTTP_GET, graph_asset_get, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/validate", HTTP_POST, graph_validate_post, WEB_ADMIT_JSON },
        { "/api/sound/graph/save", HTTP_POST, graph_save_post, WEB_ADMIT_JSON },
        { "/api/sound/graph/apply", HTTP_POST, graph_apply_post, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/delete", HTTP_POST, graph_project_delete_post, WEB_ADMIT_PLAIN },
        { "/api/sound/graph/reference", HTTP_POST, graph_reference_post, WEB_ADMIT_PLAIN },
        { "/api/control/source", HTTP_GET, control_source_get, WEB_ADMIT_PLAIN },
        { "/api/control/source", HTTP_POST, control_source_post, WEB_ADMIT_PLAIN },
        { "/api/mode", HTTP_GET, mode_get, WEB_ADMIT_PLAIN },
        { "/api/mode", HTTP_POST, mode_post, WEB_ADMIT_PLAIN },
        { "/api/bemf/cal", HTTP_GET, bemf_cal_get, WEB_ADMIT_PLAIN },
        { "/api/bemf/base", HTTP_GET, bemf_base_get, WEB_ADMIT_PLAIN },
        { "/api/bemf/calibrate", HTTP_POST, bemf_cal_post, WEB_ADMIT_BEMF_CAL },
        { "/api/bemf/use", HTTP_GET, bemf_use_get, WEB_ADMIT_PLAIN },
        { "/api/bemf/use", HTTP_POST, bemf_use_post, WEB_ADMIT_BEMF_USE },
        { "/api/motor", HTTP_GET, motor_get, WEB_ADMIT_PLAIN },
        { "/api/motor", HTTP_POST, motor_post, WEB_ADMIT_ACTUATION },
        { "/api/emergency", HTTP_POST, emergency_post, WEB_ADMIT_PLAIN },
        { "/api/functions", HTTP_GET, functions_get, WEB_ADMIT_PLAIN },
        { "/api/function", HTTP_POST, function_post, WEB_ADMIT_ACTUATION },
        { "/api/aux/effect", HTTP_POST, aux_effect_post, WEB_ADMIT_ACTUATION },
        { "/api/audio/status", HTTP_GET, audio_status_get, WEB_ADMIT_PLAIN },
        { "/api/audio/tracks", HTTP_GET, audio_tracks_get, WEB_ADMIT_PLAIN },
        { "/api/audio/play", HTTP_POST, audio_play_post, WEB_ADMIT_ACTUATION },
        { "/api/audio/stop", HTTP_POST, audio_stop_post, WEB_ADMIT_PLAIN },
        { "/api/audio/volume", HTTP_POST, audio_volume_post, WEB_ADMIT_PLAIN },
        { "/api/audio/upload", HTTP_POST, audio_upload_post, WEB_ADMIT_TRANSFER },
        { "/api/audio/delete", HTTP_POST, audio_track_delete_post, WEB_ADMIT_MAINTENANCE },
        { "/api/audio/library", HTTP_GET, audio_library_get, WEB_ADMIT_PLAIN },
        { "/api/audio/library/volume", HTTP_POST, audio_library_volume_post, WEB_ADMIT_PLAIN },
        { "/api/audio/library/delete", HTTP_POST, audio_library_delete_post, WEB_ADMIT_STORAGE },
        { "/api/audio/pack", HTTP_POST, audio_pack_upload_post, WEB_ADMIT_TRANSFER },
        { "/api/audio/raw/*", HTTP_GET, sound_file_raw_get, WEB_ADMIT_PLAIN },
        { "/api/sound-files", HTTP_GET, sound_files_list_get, WEB_ADMIT_PLAIN },
        { "/sound-files/*", HTTP_GET, sound_file_get, WEB_ADMIT_PLAIN },
        { "/api/track/category", HTTP_POST, track_category_post, WEB_ADMIT_PLAIN },
        { "/api/func-map", HTTP_GET, func_map_get, WEB_ADMIT_PLAIN },
        { "/api/func-map", HTTP_POST, func_map_post, WEB_ADMIT_PLAIN },
        { "/api/aux/cfg", HTTP_GET, aux_cfg_get, WEB_ADMIT_PLAIN },
        { "/api/aux/state", HTTP_GET, aux_state_get, WEB_ADMIT_PLAIN },
        { "/api/dcc/status", HTTP_GET, dcc_status_get, WEB_ADMIT_PLAIN },
        { "/api/aux/cfg", HTTP_POST, aux_cfg_post, WEB_ADMIT_PLAIN },
        { "/api/cv/read", HTTP_GET, cv_read_get, WEB_ADMIT_PLAIN },
        { "/api/cv/all", HTTP_GET, cv_all_get, WEB_ADMIT_PLAIN },
        { "/api/cv/write", HTTP_POST, cv_write_post, WEB_ADMIT_PLAIN },
        { "/api/wifi", HTTP_GET, wifi_get, WEB_ADMIT_PLAIN },
        { "/api/wifi", HTTP_POST, wifi_post, WEB_ADMIT_PLAIN },
        { "/api/wifi/reset", HTTP_POST, wifi_reset_post, WEB_ADMIT_PLAIN },
        { "/api/reset", HTTP_POST, factory_reset_post, WEB_ADMIT_PLAIN },
        { "/api/device", HTTP_GET, device_get, WEB_ADMIT_PLAIN },
        { "/api/device", HTTP_POST, device_post, WEB_ADMIT_PLAIN },
        { "/api/storage", HTTP_GET, storage_get, WEB_ADMIT_PLAIN },
        { "/api/ota/update", HTTP_POST, ota_update_post, WEB_ADMIT_TRANSFER },
        { "/api/task-inputs", HTTP_GET, task_inputs_get, WEB_ADMIT_PLAIN },
        { "/api/log", HTTP_GET, log_get, WEB_ADMIT_PLAIN },
        { "/api/clientlog", HTTP_GET, clientlog_get, WEB_ADMIT_PLAIN },
    };
    web_server_register_routes(routes, WEB_ARRAY_SIZE(routes));

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_handler);

    ESP_LOGI(TAG, "Web UI started on port %u", (unsigned)cfg.server_port);
    return ESP_OK;
}

esp_err_t web_init(void)
{
    esp_err_t cfg_err = web_cfg_init();
    esp_err_t control_err = web_control_init();
    esp_err_t log_err = web_log_init();
    if (cfg_err != ESP_OK || control_err != ESP_OK || log_err != ESP_OK)
        return ESP_ERR_NO_MEM;
    /* Storage recovery is done once from app_main(), before the sound graph is
     * restored. Repeating it here could retry a partially restored rollback
     * after the graph is already live. */
    esp_err_t load_err = settings_load(web_cfg());
    if (load_err != ESP_OK) return load_err;
    (void)audio_set_volume(web_cfg()->master_volume);
    esp_err_t func_err = web_func_init();
    if (func_err != ESP_OK) return func_err;

    esp_err_t err = wifi_start(web_cfg());
    if (err != ESP_OK && web_cfg()->wifi_mode != 0) {
        ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    }

    if (web_cfg()->wifi_mode != 0) {
        start_dns_hijack();
        esp_err_t err = start_http_server();
        if (err == ESP_OK) {
            (void)web_transfer_start_progress_server();
        }
        if (web_cfg()->auto_off_min != 0U) {
            wifi_start_auto_off();
        }
        return err;
    }
    return ESP_OK;
}