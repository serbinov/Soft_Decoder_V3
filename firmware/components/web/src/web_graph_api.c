/* Sound-graph REST endpoints and their shared error/diagnostic helpers. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"
#include "sound.h"
#include "sound_graph_store.h"
#include "storage.h"

static const char *TAG_GRAPH = "web";

esp_err_t graph_error(httpd_req_t *req, const char *status, const char *message)
{
    char escaped[512], json[576];
    json_escape(message, escaped, sizeof(escaped));
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", escaped);
    httpd_resp_set_status(req, status);
    if (req->content_len != 0U) httpd_resp_set_hdr(req, "Connection", "close");
    return send_json(req, json);
}

static esp_err_t graph_diagnostic_response(httpd_req_t *req, const sg_diagnostic_t *diag, bool validation)
{
    char message[768], code[192], id[SG_ID_CAP * 6], field[192], result[1600];
    json_escape(diag->message, message, sizeof(message));
    json_escape(diag->code[0] ? diag->code : "validation", code, sizeof(code));
    json_escape(diag->id, id, sizeof(id));
    json_escape(diag->field, field, sizeof(field));
    snprintf(result, sizeof(result), "{\"ok\":%s,%s\"diagnostics\":[{\"code\":\"%s\",\"id\":\"%s\",\"field\":\"%s\",\"message\":\"%s\",\"offset\":%lu}]}",
             validation ? "true" : "false", validation ? "\"valid\":false," : "\"error\":\"project validation failed\",", code, id, field, message, (unsigned long)diag->offset);
    if (!validation) httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, result);
}

static esp_err_t graph_result_error(httpd_req_t *req, esp_err_t err)
{
    const char *status = err == ESP_ERR_INVALID_ARG ? "400 Bad Request" :
        err == ESP_ERR_NOT_FOUND ? "404 Not Found" :
        err == ESP_ERR_INVALID_STATE ? "409 Conflict" :
        (err == ESP_ERR_NO_MEM || err == ESP_ERR_TIMEOUT) ? "503 Service Unavailable" : "500 Internal Server Error";
    return graph_error(req, status, esp_err_to_name(err));
}

static bool graph_query(httpd_req_t *req, char *query, size_t cap)
{
    esp_err_t err = httpd_req_get_url_query_str(req, query, cap);
    return (err == ESP_OK || err == ESP_ERR_NOT_FOUND) && query_encoding_valid(query);
}

static bool graph_param(const char *query, const char *key, char *out, size_t capacity)
{
    char decoded[WEB_QUERY_MAX];
    size_t matches = 0, key_length = strlen(key);
    for (const char *p = query; p != NULL && *p != '\0'; p = strchr(p, '&')) {
        if (*p == '&') ++p;
        if (strncmp(p, key, key_length) == 0 && p[key_length] == '=') ++matches;
    }
    if (matches != 1U || !parse_query(query, key, decoded, sizeof(decoded)) ||
        strlen(decoded) >= capacity) return false;
    memcpy(out, decoded, strlen(decoded) + 1U);
    return true;
}

static bool graph_revision(const char *query, const char *key, uint32_t *revision)
{
    char value[16];
    if (!graph_param(query, key, value, sizeof(value)) || value[0] == '\0') return false;
    uint32_t n = 0;
    for (const char *p = value; *p; ++p) {
        if (*p < '0' || *p > '9' || n > (UINT32_MAX - (uint32_t)(*p - '0')) / 10U) return false;
        n = n * 10U + (uint32_t)(*p - '0');
    }
    *revision = n;
    return true;
}

esp_err_t graph_capabilities_get(httpd_req_t *req)
{
    char json[1024];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"format\":\"sound-graph\",\"schemaVersion\":1,"
             "\"limits\":{\"states\":%d,\"soundStates\":%d,\"transitions\":%d,"
             "\"effects\":%d,\"assets\":%d,\"variables\":%d,\"jsonBytes\":%u},"
             "\"conditions\":[\"fn_press\",\"fn_release\",\"fn_on\",\"fn_off\","
             "\"engine_on\",\"engine_off\",\"speed\",\"accel\",\"decel\","
             "\"sample_done\",\"dir_fwd\",\"dir_rev\",\"random\",\"timeout\","
             "\"var_cmp\",\"state_enter\"],"
             "\"variableKinds\":[\"speed\",\"forward\",\"moving\",\"engine\"],"
             "\"compareOps\":[\"eq\",\"ne\",\"lt\",\"le\",\"gt\",\"ge\"]}",
             SG_MAX_STATES, SG_MAX_SOUND_STATES, SG_MAX_TRANSITIONS,
             SG_MAX_EFFECTS, SG_MAX_ASSETS, SG_MAX_VARIABLES,
             (unsigned)SG_MAX_JSON);
    return send_json(req, json);
}

esp_err_t graph_projects_get(httpd_req_t *req)
{
    sg_store_descriptor_t *items = calloc(SG_STORE_MAX_PROJECTS, sizeof(*items));
    char *json = malloc(32768U);
    if (items == NULL || json == NULL) { free(items); free(json); return graph_result_error(req, ESP_ERR_NO_MEM); }
    size_t count = 0, used = 0;
    esp_err_t err = sg_store_list(items, SG_STORE_MAX_PROJECTS, &count);
    if (err == ESP_OK) {
        buf_appendf(json, 32768U, &used, "{\"ok\":true,\"projects\":[");
        for (size_t i = 0; i < count; ++i) {
            char name[SG_NAME_CAP * 6], id[SG_ID_CAP * 6];
            json_escape(items[i].name, name, sizeof(name));
            json_escape(items[i].id, id, sizeof(id));
            buf_appendf(json, 32768U, &used, "%s{\"id\":\"%s\",\"name\":\"%s\",\"revision\":%lu}", i ? "," : "", id, name, (unsigned long)items[i].revision);
        }
        buf_appendf(json, 32768U, &used, "]}");
        err = send_json(req, json);
    } else err = graph_result_error(req, err);
    free(items); free(json);
    return err;
}

esp_err_t graph_project_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, id[SG_ID_CAP];
    uint32_t revision;
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "id", id, sizeof(id)) || !sg_id_valid(id) ||
        !graph_revision(query, "revision", &revision)) return graph_result_error(req, ESP_ERR_INVALID_ARG);
    char *json = NULL; size_t len = 0; uint32_t actual = 0;
    esp_err_t err = sg_store_read(id, revision, &json, &len, &actual);
    if (err != ESP_OK) return graph_result_error(req, err);
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "{\"ok\":true,\"revision\":%lu,\"project\":", (unsigned long)actual);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send_chunk(req, prefix, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, json, len);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "}", 1);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    free(json);
    return err;
}

esp_err_t graph_state_get(httpd_req_t *req)
{
    sound_graph_status_t st = {0};
    sound_graph_status_get(&st);
    uint16_t outputs = 0, logic = 0;
    uint32_t functions = 0;
    web_func_io_state(&outputs, &logic, &functions);
    char json[1024], id[SG_ID_CAP * 6]; size_t used = 0;
    json_escape(st.id, id, sizeof(id));
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"active\":%s,\"id\":\"%s\",\"revision\":%lu,\"engine\":%s,\"armed\":%s,\"failedChannels\":%lu,\"retryChannels\":%lu,\"states\":[", st.active ? "true" : "false", id, (unsigned long)st.revision, st.engine ? "true" : "false", st.armed ? "true" : "false", (unsigned long)st.failed_channels, (unsigned long)st.retry_channels);
    for (size_t i = 0; i < SG_MAX_EFFECTS + 1; ++i) buf_appendf(json, sizeof(json), &used, "%s%u", i ? "," : "", st.states[i]);
    buf_appendf(json, sizeof(json), &used, "],\"speed\":%u,\"fault\":%s,\"outputs\":%u,\"logic\":%u,\"functions\":%lu}", (unsigned)st.speed, st.fault ? "true" : "false", (unsigned)outputs, (unsigned)logic, (unsigned long)functions);
    return send_json(req, json);
}

esp_err_t graph_asset_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, file[SG_FILE_CAP];
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "file", file, sizeof(file)) || !sg_filename_valid(file)) return graph_result_error(req, ESP_ERR_INVALID_ARG);
    sg_asset_t asset; sg_diagnostic_t diag = {0};
    esp_err_t err = sg_asset_inspect(file, &asset, &diag);
    if (err != ESP_OK) return graph_result_error(req, err);
    char json[512];
    snprintf(json, sizeof(json), "{\"ok\":true,\"asset\":{\"file\":\"%s\",\"size\":%lu,\"crc32\":\"%s\",\"sampleRate\":%lu,\"channels\":%lu,\"bits\":%lu,\"durationMs\":%lu}}", asset.file, (unsigned long)asset.size, asset.crc32, (unsigned long)asset.sampleRate, (unsigned long)asset.channels, (unsigned long)asset.bits, (unsigned long)asset.durationMs);
    return send_json(req, json);
}

static esp_err_t graph_receive(httpd_req_t *req, char **out)
{
    *out = NULL;
    if (req->content_len == 0U || req->content_len > SG_MAX_JSON) {
        return close_rejected_body(req, graph_error(req, req->content_len > SG_MAX_JSON ? "413 Payload Too Large" : "400 Bad Request", "invalid JSON body size"));
    }
    char mime[96];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", mime, sizeof(mime)) != ESP_OK ||
        strncasecmp(mime, "application/json", 16) != 0 || (mime[16] != '\0' && mime[16] != ';' && !isspace((unsigned char)mime[16]))) {
        return close_rejected_body(req, graph_error(req, "415 Unsupported Media Type", "application/json required"));
    }
    const char *parameters = mime + 16;
    while (isspace((unsigned char)*parameters)) ++parameters;
    if (*parameters != '\0' && *parameters != ';')
        return close_rejected_body(req, graph_error(req, "415 Unsupported Media Type", "invalid JSON media type"));
    char *json = malloc(req->content_len + 1U);
    if (json == NULL) return close_rejected_body(req, graph_result_error(req, ESP_ERR_NO_MEM));
    size_t got = 0; unsigned retries = 0;
    int64_t deadline = esp_timer_get_time() + 15000000;
    while (got < req->content_len && esp_timer_get_time() < deadline) {
        size_t wanted = req->content_len - got;
        if (wanted > 4096U) wanted = 4096U;
        int n = httpd_req_recv(req, json + got, wanted);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++retries <= 3U) continue;
        if (n <= 0 || (size_t)n > wanted) break;
        got += (size_t)n;
    }
    if (got != req->content_len || esp_timer_get_time() >= deadline || memchr(json, '\0', got) != NULL) {
        free(json);
        return close_rejected_body(req, graph_error(req, "400 Bad Request", "incomplete or NUL JSON body"));
    }
    json[got] = '\0'; *out = json;
    return ESP_OK;
}

esp_err_t graph_validate_post(httpd_req_t *req)
{
    char *json = NULL;
    esp_err_t response = graph_receive(req, &json);
    if (json == NULL) return response;
    sg_graph_t *graph = malloc(sizeof(*graph)); sg_diagnostic_t diag = {0};
    if (graph == NULL) { free(json); return graph_result_error(req, ESP_ERR_NO_MEM); }
    esp_err_t err = sg_parse(json, req->content_len, graph, &diag);
    free(json); free(graph);
    if (err == ESP_ERR_NO_MEM) return graph_result_error(req, err);
    if (err == ESP_OK) return send_json(req, "{\"ok\":true,\"valid\":true,\"diagnostics\":[]}");
    return graph_diagnostic_response(req, &diag, true);
}

esp_err_t graph_save_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, id[SG_ID_CAP]; uint32_t expected;
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "id", id, sizeof(id)) || !sg_id_valid(id) || !graph_revision(query, "expectedRevision", &expected)) return close_rejected_body(req, graph_result_error(req, ESP_ERR_INVALID_ARG));
    char draft_value[8] = {0};
    bool draft_set = graph_param(query, "draft", draft_value, sizeof(draft_value));
    bool draft = draft_set && (!strcmp(draft_value, "1") || !strcmp(draft_value, "true"));
    if (draft_set && !draft) return close_rejected_body(req, graph_result_error(req, ESP_ERR_INVALID_ARG));
    char *json = NULL;
    esp_err_t response = graph_receive(req, &json);
    if (json == NULL) return response;
    uint32_t revision = 0; sg_diagnostic_t diag = {0};
    esp_err_t err = draft ? sg_store_save_draft(id, expected, json, req->content_len, &revision, &diag)
                          : sg_store_save(id, expected, json, req->content_len, &revision, &diag);
    free(json);
    if (err == ESP_ERR_INVALID_ARG && diag.message[0]) return graph_diagnostic_response(req, &diag, false);
    if (err == ESP_ERR_INVALID_STATE && diag.message[0]) return graph_error(req, "409 Conflict", diag.message);
    if (err != ESP_OK) return graph_result_error(req, err);
    char result[64];
    snprintf(result, sizeof(result), "{\"ok\":true,\"revision\":%lu}", (unsigned long)revision);
    return send_json(req, result);
}

esp_err_t graph_apply_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, id[SG_ID_CAP]; uint32_t revision;
    if (req->content_len != 0U) return close_rejected_body(req, graph_result_error(req, ESP_ERR_INVALID_ARG));
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "id", id, sizeof(id)) || !sg_id_valid(id) || !graph_revision(query, "revision", &revision) || revision == 0U) return graph_result_error(req, ESP_ERR_INVALID_ARG);
    if (!web_control_motor_stopped()) return graph_error(req, "409 Conflict", "motor must be stopped");
    char *json = NULL; size_t len = 0; uint32_t actual = 0;
    sg_graph_t *graph = malloc(sizeof(*graph)); sg_diagnostic_t diag = {0};
    if (graph == NULL) return graph_result_error(req, ESP_ERR_NO_MEM);
    sound_graph_prepared_t *prepared = NULL;
    esp_err_t err = sg_store_read(id, revision, &json, &len, &actual);
    if (err == ESP_OK) err = sg_parse(json, len, graph, &diag);
    free(json);
    /* Reserve after preparation so validation/OOM never disturbs live audio. */
    if (err == ESP_OK) err = sound_graph_prepare(graph, id, actual, &prepared);
    bool maintenance = false, lease = false;
    const char *conflict = NULL;
    if (err == ESP_OK) {
        err = web_control_maintenance_reserve(true);
        maintenance = err == ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        /* Motor speed can change between the early check and the reserve (DCC
         * traffic is asynchronous); report the same conflict, not a raw code. */
        conflict = !web_control_motor_stopped() ? "motor must be stopped" :
            (web_maintenance_active() ? "another operation is in progress" : "device is not ready");
    }
    if (err == ESP_OK && !web_control_motor_stopped()) {
        err = ESP_ERR_INVALID_STATE;
        conflict = "motor must be stopped";
    }
    if (err == ESP_OK) err = web_control_maintenance_quiesce(false);
    if (err == ESP_OK) { err = storage_access_begin(); lease = err == ESP_OK; }
    if (err == ESP_OK) err = sg_assets_validate(graph, &diag);
    if (err == ESP_OK) err = sg_store_select(id, actual);
    if (err == ESP_OK) {
        sound_graph_commit(prepared); prepared = NULL;
        sound_graph_prime_functions(web_func_levels());
    }
    sound_graph_prepared_free(prepared); free(graph);
    if (lease) storage_access_end();
    if (maintenance) web_maintenance_end();
    if (conflict != NULL) return graph_error(req, "409 Conflict", conflict);
    if (err != ESP_OK && diag.message[0] && err != ESP_ERR_NO_MEM && err != ESP_ERR_TIMEOUT) return graph_diagnostic_response(req, &diag, false);
    if (err != ESP_OK) return graph_result_error(req, err);
    return send_json(req, "{\"ok\":true}");
}

esp_err_t graph_reference_post(httpd_req_t *req)
{
    if (req->content_len != 0U) return close_rejected_body(req, graph_result_error(req, ESP_ERR_INVALID_ARG));
    if (!web_control_motor_stopped()) return graph_error(req, "409 Conflict", "motor must be stopped");
    bool maintenance = false, lease = false;
    esp_err_t err = web_control_maintenance_reserve(true);
    maintenance = err == ESP_OK;
    if (err == ESP_OK) err = web_control_maintenance_quiesce(false);
    if (err == ESP_OK) { err = storage_access_begin(); lease = err == ESP_OK; }

    sg_store_descriptor_t *projects = NULL;
    sg_graph_t *graph = NULL;
    sound_graph_prepared_t *prepared = NULL;
    sg_diagnostic_t diag = {0};
    char *json = NULL;
    size_t json_len = 0;
    uint32_t revision = 0, actual = 0;
    bool found = false;

    if (err == ESP_OK) {
        projects = calloc(SG_STORE_MAX_PROJECTS, sizeof(*projects));
        if (projects == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        size_t count = 0;
        err = sg_store_list(projects, SG_STORE_MAX_PROJECTS, &count);
        for (size_t i = 0; err == ESP_OK && i < count; ++i) {
            if (strcmp(projects[i].id, SG_DEFAULT_PROJECT_ID) == 0 &&
                (!found || projects[i].revision > revision)) {
                revision = projects[i].revision;
                found = true;
            }
        }
        if (err == ESP_OK && !found) err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        graph = malloc(sizeof(*graph));
        if (graph == NULL) err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) err = sg_store_read(SG_DEFAULT_PROJECT_ID, revision, &json, &json_len, &actual);
    if (err == ESP_OK) err = sg_parse(json, json_len, graph, &diag);
    free(json);
    if (err == ESP_OK) err = sg_assets_validate(graph, &diag);
    if (err == ESP_OK) err = sound_graph_prepare(graph, SG_DEFAULT_PROJECT_ID, actual, &prepared);
    if (err == ESP_OK) err = sg_store_select(SG_DEFAULT_PROJECT_ID, actual);
    if (err == ESP_OK) {
        sound_graph_commit(prepared); prepared = NULL;
        sound_graph_prime_functions(web_func_levels());
    }
    sound_graph_prepared_free(prepared); free(graph); free(projects);
    if (lease) storage_access_end();
    if (maintenance) web_maintenance_end();
    if (err == ESP_ERR_NOT_FOUND) return graph_error(req, "404 Not Found", "default project not installed");
    if (err != ESP_OK) {
        ESP_LOGW(TAG_GRAPH, "reference install failed: %s", esp_err_to_name(err));
        if (diag.message[0] && err != ESP_ERR_NO_MEM && err != ESP_ERR_TIMEOUT)
            return graph_diagnostic_response(req, &diag, false);
        return graph_result_error(req, err);
    }
    return send_json(req, "{\"ok\":true}");
}

esp_err_t graph_project_delete_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = {0}, id[SG_ID_CAP];
    if (req->content_len != 0U) return close_rejected_body(req, graph_result_error(req, ESP_ERR_INVALID_ARG));
    if (!graph_query(req, query, sizeof(query)) || !graph_param(query, "id", id, sizeof(id)) ||
        !sg_id_valid(id)) {
        return graph_result_error(req, ESP_ERR_INVALID_ARG);
    }
    bool was_selected = false;
    esp_err_t err = sg_store_delete(id, &was_selected);
    if (err != ESP_OK) return graph_result_error(req, err);
    if (was_selected) sound_graph_deactivate();
    return send_json(req, "{\"ok\":true}");
}
