/* BEMF calibration endpoints and query parsing. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "motor.h"
#include "settings.h"


static const char *bemf_result_name(motor_bemf_cal_state_t result)
{
    switch (result) {
    case MOTOR_BEMF_CAL_IDLE: return "idle";
    case MOTOR_BEMF_CAL_RUNNING: return "running";
    case MOTOR_BEMF_CAL_SAVING: return "saving";
    case MOTOR_BEMF_CAL_SUCCEEDED: return "succeeded";
    case MOTOR_BEMF_CAL_CANCELLED: return "cancelled";
    case MOTOR_BEMF_CAL_FAILED: return "failed";
    default: return "failed";
    }
}

static const char *bemf_phase_name(uint8_t phase)
{
    switch (phase) {
    case MOTOR_BEMF_CAL_PHASE_IDLE: return "idle";
    case MOTOR_BEMF_CAL_PHASE_PREFLIGHT: return "preflight";
    case MOTOR_BEMF_CAL_PHASE_WINDOW: return "window";
    case MOTOR_BEMF_CAL_PHASE_CURVE: return "curve";
    case MOTOR_BEMF_CAL_PHASE_SAVING: return "saving";
    default: return "idle";
    }
}

static const char *bemf_error_name(motor_bemf_cal_error_t reason)
{
    switch (reason) {
    case MOTOR_BEMF_CAL_ERR_NONE: return "none";
    case MOTOR_BEMF_CAL_ERR_ADC: return "adc";
    case MOTOR_BEMF_CAL_ERR_RAIL: return "rail";
    case MOTOR_BEMF_CAL_ERR_TIMEOUT: return "timeout";
    case MOTOR_BEMF_CAL_ERR_CONTROL: return "control";
    case MOTOR_BEMF_CAL_ERR_CURVE: return "curve";
    case MOTOR_BEMF_CAL_ERR_STORAGE: return "storage";
    case MOTOR_BEMF_CAL_ERR_START: return "start";
    case MOTOR_BEMF_CAL_ERR_SENSOR: return "sensor";
    case MOTOR_BEMF_CAL_ERR_NO_RISE: return "no-rise";
    default: return "start";
    }
}

/* These actions accept one unambiguous boolean, never duplicate/extra keys. */
bool bemf_bool_query(const char *query, const char *key, bool *value)
{
    size_t n = strlen(key);
    if (strncmp(query, key, n) != 0 || query[n] != '=' || strchr(query, '&') != NULL ||
        !query_encoding_valid(query) || !query_bool_valid(query, key)) return false;
    *value = parse_bool(query, key, false);
    return true;
}

static int bemf_cal_key_index(const char *key, size_t len)
{
    static const char *const keys[] = { "start", "apply_pid" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (strlen(keys[i]) == len && strncmp(key, keys[i], len) == 0) return (int)i;
    }
    return -1;
}

static bool bemf_cal_query_keys_ok(const char *query)
{
    const char *p = query;
    uint32_t seen = 0;
    while (p != NULL && *p != '\0') {
        const char *amp = strchr(p, '&');
        size_t len = amp != NULL ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq == NULL || eq == p) return false;
        int idx = bemf_cal_key_index(p, (size_t)(eq - p));
        if (idx < 0 || (seen & (1UL << idx)) != 0) return false;
        seen |= 1UL << idx;
        p = amp != NULL ? amp + 1 : NULL;
    }
    return true;
}

static bool bemf_cal_bool_value(const char *query, const char *key, bool *out)
{
    if (!query_has_key(query, key)) return true;
    char value[16];
    if (!parse_query(query, key, value, sizeof(value))) return false;
    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "yes") == 0) {
        *out = true;
    } else if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
               strcasecmp(value, "no") == 0) {
        *out = false;
    } else {
        return false;
    }
    return true;
}

static bool bemf_cal_parse_config(const char *query, motor_bemf_cal_config_t *cfg)
{
    if (!bemf_cal_query_keys_ok(query)) return false;
    memset(cfg, 0, sizeof(*cfg));
    bool bv = false;
    if (!bemf_cal_bool_value(query, "apply_pid", &bv)) return false;
    if (query_has_key(query, "apply_pid")) cfg->apply_pid = bv;
    return true;
}

int bemf_cal_action(const char *query)
{
    bool value = false;
    if (query[0] == '\0') return 1; /* Existing bodyless callers start. */
    if (query_has_key(query, "start") && parse_bool(query, "start", false)) {
        motor_bemf_cal_config_t cfg;
        return bemf_cal_parse_config(query, &cfg) ? 1 : 0;
    }
    if (bemf_bool_query(query, "reset", &value) && value) return 2;
    if (bemf_bool_query(query, "cancel", &value) && value) return 3;
    return 0;
}

esp_err_t bemf_cal_get(httpd_req_t *req)
{
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    char json[1792];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used,
                "{\"ok\":true,\"active\":%s,\"phase\":\"%s\",\"progress\":%u,\"total\":%u,"
                "\"valid\":%s,\"stored\":%s,\"use\":%s,"
                "\"runId\":%lu,\"result\":\"%s\",\"error\":\"%s\",\"errorCode\":%d,\"points\":[",
                info.active ? "true" : "false",
                bemf_phase_name(info.phase),
                (unsigned)info.step, (unsigned)info.total,
                info.valid ? "true" : "false",
                info.stored ? "true" : "false",
                motor_get_bemf_enabled() ? "true" : "false",
                (unsigned long)info.run_id, bemf_result_name(info.result),
                bemf_error_name(info.reason), (int)info.error_code);
    for (uint8_t i = 0; i < info.count; ++i) {
        uint16_t pct = (uint16_t)((info.frac[i] * 100U) / 1024U);
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"speed\":%u,\"frac\":%u}",
                    i == 0 ? "" : ",", (unsigned)info.speed[i], (unsigned)pct);
    }
    buf_appendf(json, sizeof(json), &used,
                "],\"profile\":{\"mode\":%u,\"phase\":\"%s\",\"sensorOk\":%s,"
                "\"optimalUs\":%u,\"dwellMs\":%u,\"planned\":%u,\"measured\":%u,"
                "\"stalled\":%u,\"ceiling\":%u,\"first\":%u,\"firstFrac\":%u,\"settle\":%u,"
                "\"gain\":%u,\"kp\":%u,\"ki\":%u,\"kd\":%u,\"quality\":%u,"
                "\"pidApplied\":%s,\"pwmMin\":%u,\"pwmStart\":%u,\"pwmMax\":%u}}",
                (unsigned)info.profile.mode,
                bemf_phase_name(info.profile.phase),
                info.profile.sensor_ok ? "true" : "false",
                (unsigned)info.profile.optimal_us,
                (unsigned)info.profile.dwell_ms,
                (unsigned)info.profile.planned_points,
                (unsigned)info.profile.measured_points,
                (unsigned)info.profile.stalled_points,
                (unsigned)info.profile.ceiling_speed,
                (unsigned)info.profile.first_move_speed,
                (unsigned)info.profile.first_frac,
                (unsigned)info.profile.settle_ms_avg,
                (unsigned)info.profile.plant_gain_x1000,
                (unsigned)info.profile.kp_x100, (unsigned)info.profile.ki_x1000,
                (unsigned)info.profile.kd_x100, (unsigned)info.profile.quality,
                info.profile.pid_applied ? "true" : "false",
                (unsigned)info.profile.pwm_min_pct,
                (unsigned)info.profile.pwm_start_pct,
                (unsigned)info.profile.pwm_max_pct);
    return send_json(req, json);
}

esp_err_t bemf_base_get(httpd_req_t *req)
{
    motor_bemf_base_info_t info;
    motor_bemf_base_info(&info);
    char json[1024];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used,
                "{\"ok\":true,\"start\":%u,\"full\":%u,\"points\":[",
                (unsigned)((info.start_frac * 100U) / 1024U),
                (unsigned)((info.full_frac * 100U) / 1024U));
    for (uint8_t i = 0; i < info.count; ++i) {
        uint16_t pct = (uint16_t)((info.frac[i] * 100U) / 1024U);
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"speed\":%u,\"frac\":%u}",
                    i == 0 ? "" : ",", (unsigned)info.speed[i], (unsigned)pct);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

esp_err_t bemf_cal_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    int action = bemf_cal_action(query);
    if (httpd_req_get_url_query_len(req) >= sizeof(query) ||
        (query_err != ESP_OK && query_err != ESP_ERR_NOT_FOUND) ||
        !query_encoding_valid(query) || action == 0 || req->content_len != 0U)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid calibration action");
    motor_bemf_cal_config_t cfg;
    if (action == 1 && !bemf_cal_parse_config(query, &cfg)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid calibration profile");
    }
    if (action == 3) {
        esp_err_t err = motor_bemf_cal_cancel();
        return send_json(req, err == ESP_OK ? "{\"ok\":true}" :
                         "{\"ok\":false,\"error\":\"cancel failed\"}");
    }
    if (!web_control_take(pdMS_TO_TICKS(100)))
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!web_actuation_ready() || web_maintenance_active()) {
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"actuation unavailable\"}");
    }
    if (action == 2) {
        esp_err_t err = motor_bemf_cal_clear();
        web_control_give();
        return send_json(req, err == ESP_OK
                                  ? "{\"ok\":true}"
                                  : "{\"ok\":false,\"error\":\"calibration busy\"}");
    }
    esp_err_t err = motor_bemf_cal_start_ex(&cfg);
    web_control_give();
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, "{\"ok\":false,\"error\":\"calibration running\"}");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json(req, "{\"ok\":false,\"error\":\"invalid profile\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, "{\"ok\":false,\"error\":\"start failed\"}");
    }
    return send_json(req, "{\"ok\":true}");
}

esp_err_t bemf_use_get(httpd_req_t *req)
{
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"enabled\":%s}",
             motor_get_bemf_enabled() ? "true" : "false");
    return send_json(req, json);
}

esp_err_t bemf_use_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    esp_err_t query_err = httpd_req_get_url_query_str(req, query, sizeof(query));
    bool enabled;
    if (httpd_req_get_url_query_len(req) >= sizeof(query) || query_err != ESP_OK ||
        !bemf_bool_query(query, "enabled", &enabled))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid enabled");
    const char *error = NULL;
    if (!web_control_take(pdMS_TO_TICKS(100))) {
        error = "control busy";
    } else {
        if (!web_actuation_ready() || web_maintenance_active()) error = "actuation unavailable";
        else if (settings_bemf_use_save(enabled) != ESP_OK) error = "storage failed";
        else motor_set_bemf_enabled(enabled);
        enabled = motor_get_bemf_enabled();
        web_control_give();
    }
    if (error != NULL) enabled = motor_get_bemf_enabled();
    else web_log_event("BEMF", "регулятор %s", enabled ? "включён" : "выключен, только ШИМ");
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"enabled\":%s,\"error\":\"%s\"}",
             error == NULL ? "true" : "false", enabled ? "true" : "false", error == NULL ? "" : error);
    return send_json(req, json);
}
