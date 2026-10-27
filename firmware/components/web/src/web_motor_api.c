/* Motor and emergency-stop endpoints. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "motor.h"
#include "sound.h"

static const char *TAG_MOTOR = "web";

typedef enum {
    MOTOR_UI_UNKNOWN = 0,
    MOTOR_UI_IDLE,
    MOTOR_UI_START,
    MOTOR_UI_ACCEL,
    MOTOR_UI_DECEL,
    MOTOR_UI_CRUISE,
    MOTOR_UI_REVERSE,
    MOTOR_UI_INHIBITED,
    MOTOR_UI_NO_TICK,
} motor_ui_mode_t;

static const char *motor_ui_mode_name(motor_ui_mode_t mode)
{
    switch (mode) {
    case MOTOR_UI_IDLE: return "idle";
    case MOTOR_UI_START: return "start";
    case MOTOR_UI_ACCEL: return "accel";
    case MOTOR_UI_DECEL: return "decel";
    case MOTOR_UI_CRUISE: return "cruise";
    case MOTOR_UI_REVERSE: return "reverse";
    case MOTOR_UI_INHIBITED: return "inhibited";
    case MOTOR_UI_NO_TICK: return "no_tick";
    default: return "unknown";
    }
}

static motor_ui_mode_t motor_ui_mode(uint8_t target, bool target_forward,
                                     uint8_t applied, bool applied_forward,
                                     bool inhibited, int64_t tick_age_ms)
{
    if (inhibited) return MOTOR_UI_INHIBITED;
    if (tick_age_ms < 0 || tick_age_ms > 500) return MOTOR_UI_NO_TICK;
    if (target == 0U && applied == 0U) return MOTOR_UI_IDLE;
    if (applied > 0U && target_forward != applied_forward) return MOTOR_UI_REVERSE;
    if (target == 0U) return MOTOR_UI_DECEL;
    if (applied == 0U) return MOTOR_UI_START;
    if (target > applied) return MOTOR_UI_ACCEL;
    if (target < applied) return MOTOR_UI_DECEL;
    return MOTOR_UI_CRUISE;
}

esp_err_t motor_get(httpd_req_t *req)
{
    uint8_t target = 0, applied = 0;
    bool target_forward = true, applied_forward = true;
    motor_get_status(&target, &target_forward);
    motor_get_applied_speed(&applied, &applied_forward);

    motor_bemf_diag_t diag;
    motor_bemf_diag(&diag);
    bool inhibited = motor_is_inhibited();
    int64_t last_tick = motor_last_tick_us();
    int64_t tick_age_ms = -1;
    if (last_tick > 0) {
        int64_t now = esp_timer_get_time();
        tick_age_ms = now > last_tick ? (now - last_tick) / 1000 : 0;
    }
    motor_ui_mode_t mode = motor_ui_mode(target, target_forward, applied,
                                         applied_forward, inhibited, tick_age_ms);
    bool closed_loop = motor_get_bemf_enabled() && diag.cal_valid &&
                       diag.bemf_acquired && diag.bemf_valid &&
                       !diag.open_loop_fallback;

    char json[768];
    int n = snprintf(json, sizeof(json),
        "{\"ok\":true,\"speed\":%u,\"forward\":%s,"
        "\"targetSpeed\":%u,\"targetForward\":%s,"
        "\"appliedSpeed\":%u,\"appliedForward\":%s,"
        "\"duty\":%lu,\"dutyMax\":1023,"
        "\"railMv\":%lu,\"railMotorMv\":%lu,"
        "\"bemfMv\":%lu,\"bemfMotorMv\":%lu,\"bemfFilteredMv\":%lu,"
        "\"bemfEnabled\":%s,\"bemfValid\":%s,\"bemfAcquired\":%s,"
        "\"bemfClosedLoop\":%s,\"openLoop\":%s,\"pidOk\":%s,"
        "\"corr\":%ld,\"err\":%ld,\"windowUs\":%u,"
        "\"inhibited\":%s,\"inhibit\":%lu,\"tickAgeMs\":%lld,"
        "\"mode\":%u,\"modeName\":\"%s\"}",
        (unsigned)target, target_forward ? "true" : "false",
        (unsigned)target, target_forward ? "true" : "false",
        (unsigned)applied, applied_forward ? "true" : "false",
        (unsigned long)diag.duty,
        (unsigned long)diag.rail_mv, (unsigned long)diag.rail_motor_mv,
        (unsigned long)diag.bemf1_mv, (unsigned long)diag.bemf1_motor_mv,
        (unsigned long)diag.bemf_filtered_mv,
        motor_get_bemf_enabled() ? "true" : "false",
        diag.bemf_valid ? "true" : "false",
        diag.bemf_acquired ? "true" : "false",
        closed_loop ? "true" : "false",
        diag.open_loop_fallback ? "true" : "false",
        diag.pid_ok ? "true" : "false",
        (long)diag.corr, (long)diag.error, (unsigned)diag.window_us,
        inhibited ? "true" : "false", (unsigned long)0,
        (long long)tick_age_ms, (unsigned)mode, motor_ui_mode_name(mode));
    if (n < 0 || (size_t)n >= sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "motor diag too large");
    }
    return send_json(req, json);
}

esp_err_t motor_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));

    uint8_t speed = 0;
    bool has_speed = parse_u8(query, "speed", &speed);
    if ((query_has_key(query, "speed") && !has_speed) || !query_bool_valid(query, "forward"))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid motor parameters");
    bool forward = parse_bool(query, "forward", false);
    bool has_forward = parse_query(query, "forward", (char[16]){0}, 16);

    if (!web_control_take(pdMS_TO_TICKS(100)))
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (!web_actuation_ready() || web_maintenance_active() || web_cfg()->control_source == 0) {
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"control unavailable\"}");
    }
    uint8_t cur = 0;
    bool cur_fwd = true;
    motor_get_status(&cur, &cur_fwd);
    if (!has_speed) {
        speed = cur;
    }
    if (!has_forward) {
        forward = cur_fwd;
    }
    if (speed > 126) {
        web_control_give();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "speed out of range");
    }
    if (speed != cur || forward != cur_fwd) {
        web_log_event("Мотор", "скор %u, напр %s", (unsigned)speed,
                      forward ? "вперёд" : "назад");
    }
    esp_err_t err = motor_set_speed(speed, forward);
    if (err == ESP_OK) web_motion_changed(speed, forward);
    web_control_give();
    if (err != ESP_OK) return send_json(req, "{\"ok\":false,\"error\":\"motor rejected\"}");

    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":true,\"speed\":%u,\"forward\":%s}",
             (unsigned)speed, forward ? "true" : "false");
    return send_json(req, json);
}

/* Emergency stop: cut the motor and all sound immediately. Deliberately not
 * gated on the control source or actuation readiness so the safety action is
 * always available while the board is responsive. */
esp_err_t emergency_post(httpd_req_t *req)
{
    /* Cut motion and sound first. The control mutex is only for invalidating a
     * concurrent source lease; an emergency action must never be refused just
     * because a REST handler is doing NVS work under that mutex. */
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    web_emergency_reset();
    web_control_bump_generation();
    web_log_event("Стоп", "ЭКСТРЕННЫЙ СТОП");
    return send_json(req, "{\"ok\":true}");
}
