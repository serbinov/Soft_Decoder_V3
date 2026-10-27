/* Function/AUX REST endpoints. State is owned by web_func.c. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "auxio.h"
#include "settings.h"
#include "sound.h"

#define WEB_FUNC_BIND_JSON_HDR 128U
#define WEB_FUNC_BIND_JSON_ENTRY 224U

esp_err_t functions_get(httpd_req_t *req)
{
    char json[128];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"states\":[");
    bool states[WEB_FN_COUNT];
    web_func_states(states);
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) {
        buf_appendf(json, sizeof(json), &used, "%s%d",
                    fn == 0 ? "" : ",", states[fn] ? 1 : 0);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

esp_err_t function_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t fn = 0;
    if (!parse_u8(query, "fn", &fn) || fn >= WEB_FN_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fn out of range");
    }
    bool state = parse_bool(query, "state", false);
    if (!query_bool_valid(query, "state"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid state");
    if (!web_control_take(pdMS_TO_TICKS(100)))
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!web_actuation_ready() || web_maintenance_active() || web_cfg()->control_source == 0) {
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"control unavailable\"}");
    }
    web_apply_function(fn, state);
    web_control_give();
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"fn\":%u,\"state\":%s}",
             (unsigned)fn, state ? "true" : "false");
    return send_json(req, json);
}

esp_err_t aux_effect_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t ch = 0, pwm_on = 255, pwm_off = 0, mode = 0;
    uint16_t period = 800;
    if (!parse_u8(query, "ch", &ch) || ch >= AUXIO_CH_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ch out of range");
    }
    bool enabled = parse_bool(query, "on", false);
    if (!query_bool_valid(query, "on"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid on");
    parse_u8(query, "pwm_on", &pwm_on);
    parse_u8(query, "pwm_off", &pwm_off);
    parse_u8(query, "mode", &mode);
    parse_u16(query, "period", &period);

    auxio_effect_t effect = ((uint32_t)mode < AUXIO_EFFECT_COUNT)
                                ? (auxio_effect_t)mode
                                : AUXIO_EFFECT_STEADY;
    if (!web_control_take(pdMS_TO_TICKS(100)))
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!web_actuation_ready() || web_maintenance_active() || web_cfg()->control_source == 0) {
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"control unavailable\"}");
    }
    esp_err_t err = auxio_set_effect(ch, enabled, pwm_on, pwm_off, effect, period);
    web_control_give();
    if (err != ESP_OK) return send_json(req, "{\"ok\":false,\"error\":\"AUX rejected\"}");
    web_log_event("AUX", "%s: %s, PWM %u/%u, режим %u",
                  web_func_output_name(ch), enabled ? "вкл" : "выкл",
                  (unsigned)pwm_on, (unsigned)pwm_off, (unsigned)mode);

    char json[96];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"ch\":%u,\"on\":%s,\"mode\":%u}",
             (unsigned)ch, enabled ? "true" : "false", (unsigned)mode);
    return send_json(req, json);
}

esp_err_t aux_state_get(httpd_req_t *req)
{
    char json[512];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"aux\":[");
    for (uint8_t i = 0; i < AUXIO_CH_COUNT; ++i) {
        bool on = false;
        esp_err_t err = auxio_get_enabled(i, &on);
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"ch\":%u,\"on\":%s,\"error\":%s}",
                    i == 0U ? "" : ",", (unsigned)i,
                    on ? "true" : "false", err == ESP_OK ? "false" : "true");
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

esp_err_t aux_cfg_get(httpd_req_t *req)
{
    settings_aux_cfg_t cfg[SETTINGS_AUX_COUNT];
    web_func_aux_snapshot(cfg);
    char json[512];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"aux\":[");
    for (uint8_t i = 0; i < SETTINGS_AUX_COUNT; ++i) {
        buf_appendf(json, sizeof(json), &used, "%s{\"level\":%u,\"effect\":%u}",
                    i == 0U ? "" : ",", (unsigned)cfg[i].level,
                    (unsigned)cfg[i].effect);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

esp_err_t aux_cfg_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t ch = 0, level = 0, fx = 0;
    if (!parse_u8(query, "ch", &ch) || ch >= SETTINGS_AUX_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ch");
    }
    if (!parse_u8(query, "level", &level) || level > 100U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad level");
    }
    if (!parse_u8(query, "effect", &fx) || (uint32_t)fx >= AUXIO_EFFECT_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad effect");
    }
    web_aux_update_t updated = web_func_aux_update(ch, level, fx);
    if (updated == WEB_AUX_UPDATE_SAVE_FAIL)
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    if (updated == WEB_AUX_UPDATE_APPLY_FAIL)
        return send_json(req, "{\"ok\":false,\"error\":\"apply failed\"}");
    web_log_event("AUX", "%s: уровень %u%%, эффект %u",
                  web_func_output_name(ch), (unsigned)level, (unsigned)fx);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"ch\":%u}", (unsigned)ch);
    return send_json(req, json);
}

/* GET /api/func-map?view=bind[|matrix] -> the canonical binding list. */
static esp_err_t func_bind_get(httpd_req_t *req)
{
    size_t count = web_func_bind_count_get();
    if (count > FUNC_BIND_MAX) count = FUNC_BIND_MAX;
    size_t cap = WEB_FUNC_BIND_JSON_HDR + count * WEB_FUNC_BIND_JSON_ENTRY;
    char *json = malloc(cap);
    if (json == NULL) {
        return send_json(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    size_t used = 0;
    buf_appendf(json, cap, &used, "{\"ok\":true,\"count\":%u,\"binds\":[",
                (unsigned)count);
    for (size_t i = 0; i < count; ++i) {
        func_binding_t binding;
        (void)web_func_bind_at(i, &binding);
        const func_binding_t *b = &binding;
        buf_appendf(json, cap, &used,
                    "%s{\"fn\":%u,\"type\":%u,\"id\":%u,\"dir\":%u,\"state\":%u,"
                    "\"mode\":%u,\"flags\":%u,\"short\":%u,\"short_ms\":%u,"
                    "\"min_ms\":%u,\"fade_ms\":%u}",
                    i == 0U ? "" : ",", (unsigned)b->fn, (unsigned)b->target_type,
                    (unsigned)b->target_id, (unsigned)b->dir, (unsigned)b->state,
                    (unsigned)b->mode, (unsigned)b->flags, (unsigned)b->short_table,
                    (unsigned)b->short_ms, (unsigned)b->min_ms, (unsigned)b->fade_ms);
    }
    if (used >= cap) {
        /* Should be unreachable with the per-entry budget, but never emit
         * truncated, unparsable JSON. */
        free(json);
        return send_json(req, "{\"ok\":false,\"error\":\"response too large\"}");
    }
    buf_appendf(json, cap, &used, "]}");
    if (used >= cap) {
        free(json);
        return send_json(req, "{\"ok\":false,\"error\":\"response too large\"}");
    }
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

/* POST /api/func-map?bind=1 -> add or (bind=1&remove=1&idx=N) remove a binding. */
static esp_err_t func_map_post_binding(httpd_req_t *req, const char *query)
{
    if (parse_bool(query, "remove", false)) {
        uint8_t idx = 0;
        if (!parse_u8(query, "idx", &idx) || (size_t)idx >= web_func_bind_count_get()) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad idx");
        }
        func_binding_t victim;
        (void)web_func_bind_at(idx, &victim);
        if (sound_graph_active() && (victim.target_type == FUNC_TARGET_SOUND || victim.target_type == FUNC_TARGET_SLOT))
            return graph_error(req, "409 Conflict", "graph mode: use /sound-editor/blocks.html");
        if (!web_func_bind_remove(idx)) {
            return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
        }
        return send_json(req, "{\"ok\":true}");
    }

    uint8_t fn = 0, type = 0, id = 0, dir = 0, state = 0, mode = 0, flags = 0, short_table = 0;
    uint16_t short_ms = 0, min_ms = 0, fade_ms = 0;
    if (!parse_u8(query, "fn", &fn) || fn >= WEB_FN_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fn");
    }
    if (!parse_u8(query, "type", &type) || type < FUNC_TARGET_OUTPUT || type > FUNC_TARGET_LOGIC) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad type");
    }
    if (type == FUNC_TARGET_SOUND || type == FUNC_TARGET_SLOT)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "sound targets are graph/legacy only");
    if (!parse_u8(query, "id", &id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
    }
    /* Reject a target id outside its type's domain up front (defence in depth:
     * func_eval already bounds OUTPUT, but a bad persisted record should never
     * be created in the first place). */
    if ((type == FUNC_TARGET_OUTPUT && id > 8U) ||
        (type == FUNC_TARGET_LOGIC && (id < 1U || id > 8U)) ||
        (type == FUNC_TARGET_SLOT && (id < 1U || id > SETTINGS_MAX_TRACKS)) ||
        (type == FUNC_TARGET_SOUND && id >= SG_MAX_EFFECTS)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
    }
    (void)parse_u8(query, "dir", &dir);
    (void)parse_u8(query, "state", &state);
    (void)parse_u8(query, "mode", &mode);
    (void)parse_u8(query, "flags", &flags);
    (void)parse_u8(query, "short", &short_table);
    (void)parse_u16(query, "short_ms", &short_ms);
    (void)parse_u16(query, "min_ms", &min_ms);
    (void)parse_u16(query, "fade_ms", &fade_ms);

    func_binding_t b;
    memset(&b, 0, sizeof(b));
    b.used = 1;
    b.fn = fn;
    b.target_type = type;
    b.target_id = id;
    b.dir = dir;
    b.state = state;
    b.mode = mode;
    b.flags = flags;
    b.short_table = short_table;
    b.short_ms = short_ms;
    b.min_ms = min_ms;
    b.fade_ms = fade_ms;
    if (!web_func_bind_add(&b)) {
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"count\":%u}", (unsigned)web_func_bind_count_get());
    return send_json(req, json);
}

esp_err_t func_map_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char view[16] = { 0 };
    if (parse_query(query, "view", view, sizeof(view)) &&
        (strcmp(view, "bind") == 0 || strcmp(view, "matrix") == 0)) {
        return func_bind_get(req);
    }

    char json[2048];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"map\":[");
    for (uint8_t f = 0; f < WEB_FN_COUNT; ++f) {
        uint8_t sa = 0, sb = 0, dir = 0, spd = 0;
        uint16_t aux = 0;
        (void)web_func_map_get(f, &sa, &sb, &aux, &dir, &spd);
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"a\":%u,\"b\":%u,\"aux\":%u,\"dir\":%u,\"speed\":%u}",
                    f == 0U ? "" : ",", (unsigned)sa, (unsigned)sb,
                    (unsigned)aux, (unsigned)dir, (unsigned)spd);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

esp_err_t func_map_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (parse_bool(query, "bind", false)) {
        return func_map_post_binding(req, query);
    }
    if (sound_graph_active()) return graph_error(req, "409 Conflict", "graph mode: use /sound-editor/blocks.html");
    uint8_t fn = 0, sa = 0, sb = 0, dir = 0, spd = 0;
    uint16_t aux = 0;
    if (!parse_u8(query, "fn", &fn) || fn >= WEB_FN_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad fn");
    }
    if (!parse_u8(query, "a", &sa) || sa > SETTINGS_MAX_TRACKS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad a");
    }
    if (!parse_u8(query, "b", &sb) || sb > SETTINGS_MAX_TRACKS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad b");
    }
    if (!parse_u16(query, "aux", &aux) || (aux & (uint16_t)~SETTINGS_FUNC_OUT_ALL) != 0U) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad aux");
    }
    if (!parse_u8(query, "dir", &dir) || dir > SETTINGS_FUNC_DIR_REV) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad dir");
    }
    if (!parse_u8(query, "speed", &spd) || spd > SETTINGS_FUNC_SPD_STOP) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad speed");
    }
    if (!web_func_map_set(fn, sa, sb, aux, dir, spd)) {
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"fn\":%u}", (unsigned)fn);
    return send_json(req, json);
}
