/* Control-source, CV/DCC, Wi-Fi, device, storage and factory-reset endpoints. */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "dcc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "motor.h"
#include "settings.h"
#include "sound.h"
#include "sound_graph_store.h"
#include "storage.h"
#include "web_html.h"

static const char *TAG_SETTINGS = "web";

esp_err_t control_source_get(httpd_req_t *req)
{
    settings_config_t cfg;
    if (!web_cfg_read(&cfg)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"source\":\"%s\"}",
             cfg.control_source == 1 ? "web" : "rails");
    return send_json(req, json);
}

esp_err_t control_source_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    if (!parse_query(query, "source", buf, sizeof(buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing source");
    }
    if (strcasecmp(buf, "web") != 0 && strcmp(buf, "1") != 0 &&
        strcasecmp(buf, "rails") != 0 && strcmp(buf, "0") != 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid source");
    if (!web_control_take(pdMS_TO_TICKS(100)))
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (web_maintenance_active()) {
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance\"}");
    }
    if (!web_cfg_lock(pdMS_TO_TICKS(100))) {
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    settings_config_t previous = *web_cfg();
    settings_config_t next = *web_cfg();
    next.control_source = (strcasecmp(buf, "web") == 0 || strcmp(buf, "1") == 0) ? 1 : 0;
    uint8_t cv29 = 0;
    if (next.control_source == 0) {
        esp_err_t err = settings_cv_read(29, &cv29);
        if (err != ESP_OK) {
            web_cfg_unlock();
            web_control_give();
            return send_json(req, "{\"ok\":false,\"error\":\"mode read failed\"}");
        }
    }
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    web_emergency_reset();
    /* Persist the control source first. The state transition is only accepted
     * after both stores succeed; otherwise the CV and config would disagree
     * until the next reboot. */
    if (settings_save(&next) != ESP_OK) {
        web_cfg_unlock();
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    *web_cfg() = next;
    if (next.control_source == 0) {
        esp_err_t err = settings_cv_write(29, cv29 & (uint8_t)~0x04U);
        if (err == ESP_OK) err = settings_cv_commit();
        if (err != ESP_OK) {
            /* Best effort: do not leave the RAM CV and the persisted config
             * disagreeing when the second store fails. */
            (void)settings_cv_write(29, cv29);
            (void)settings_cv_commit();
            (void)settings_save(&previous);
            *web_cfg() = previous;
            web_cfg_unlock();
            web_control_give();
            return send_json(req, "{\"ok\":false,\"error\":\"mode save failed\"}");
        }
    }
    web_control_bump_generation();
    web_log_event("Источник", "%s", next.control_source == 1 ? "Веб" : "Рельсы");
    web_cfg_unlock();
    web_control_give();
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"source\":\"%s\"}",
             next.control_source == 1 ? "web" : "rails");
    return send_json(req, json);
}

esp_err_t mode_get(httpd_req_t *req)
{
    uint8_t cv29 = 0;
    (void)settings_cv_read(29, &cv29);
    bool dc = (cv29 & 0x04U) != 0;
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", dc ? "dc" : "dcc");
    return send_json(req, json);
}

esp_err_t mode_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    if (!parse_query(query, "mode", buf, sizeof(buf))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mode");
    }
    bool dc = (strcasecmp(buf, "dc") == 0);
    if (!dc && strcasecmp(buf, "dcc") != 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode");
    if (!web_control_take(pdMS_TO_TICKS(100)))
        return send_json(req, "{\"ok\":false,\"error\":\"control busy\"}");
    if (web_maintenance_active()) {
        web_control_give();
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance\"}");
    }
    uint8_t cv29 = 0;
    (void)settings_cv_read(29, &cv29);
    if (dc) {
        cv29 |= 0x04U;
    } else {
        cv29 &= (uint8_t)~0x04U;
    }
    esp_err_t err = settings_cv_write(29, cv29);
    if (err == ESP_OK) err = settings_cv_commit();
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    web_emergency_reset();
    web_control_bump_generation();
    web_control_give();
    if (err != ESP_OK) return send_json(req, "{\"ok\":false,\"error\":\"mode save failed\"}");
    web_log_event("Режим", "%s", dc ? "DC (аналог)" : "DCC");
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", dc ? "dc" : "dcc");
    return send_json(req, json);
}

esp_err_t track_category_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint8_t slot = 0, cat = 0;
    if (!parse_u8(query, "slot", &slot) || slot < 1 || slot > SETTINGS_MAX_TRACKS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slot out of range");
    }
    if (!parse_u8(query, "cat", &cat) || cat > SETTINGS_TRACK_CAT_EFFECTS) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cat out of range");
    }
    web_func_track_cat_set(slot - 1U, cat);
    web_log_event("Звук", "слот %u: %s", (unsigned)slot,
                  cat == SETTINGS_TRACK_CAT_ENGINE ? "двигатель" : "эффект");

    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"slot\":%u,\"cat\":%u}",
             (unsigned)slot, (unsigned)cat);
    return send_json(req, json);
}

esp_err_t cv_read_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint16_t idx = 0;
    if (!parse_u16(query, "index", &idx) || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }
    uint8_t value = 0;
    (void)settings_cv_read(idx, &value);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"index\":%u,\"value\":%u}",
             (unsigned)idx, (unsigned)value);
    return send_json(req, json);
}

esp_err_t dcc_status_get(httpd_req_t *req)
{
    int64_t last = dcc_last_packet_us();
    int64_t age = -1;
    if (last > 0) {
        int64_t now = esp_timer_get_time();
        age = now > last ? (now - last) / 1000 : 0;
    }
    uint8_t dcc_speed = 0, dcc_step = 0, dcc_mode = 0;
    bool dcc_forward = true;
    dcc_get_speed_status(&dcc_speed, &dcc_step, &dcc_forward, &dcc_mode);
    uint8_t target = 0, applied = 0;
    bool target_forward = true, applied_forward = true;
    motor_get_status(&target, &target_forward);
    motor_get_applied_speed(&applied, &applied_forward);
    sound_graph_status_t snd = {0};
    sound_graph_status_get(&snd);

    char json[384];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"overruns\":%lu,\"lastPacketMs\":%lld,"
             "\"speed\":%u,\"step\":%u,\"mode\":%u,\"forward\":%s,"
             "\"targetSpeed\":%u,\"targetForward\":%s,"
             "\"appliedSpeed\":%u,\"appliedForward\":%s,"
             "\"soundSpeed\":%u,\"soundEngine\":%s}",
             (unsigned long)dcc_get_isr_overruns(), (long long)age,
             (unsigned)dcc_speed, (unsigned)dcc_step, (unsigned)dcc_mode,
             dcc_forward ? "true" : "false",
             (unsigned)target, target_forward ? "true" : "false",
             (unsigned)applied, applied_forward ? "true" : "false",
             (unsigned)snd.speed, snd.engine ? "true" : "false");
    return send_json(req, json);
}

esp_err_t cv_all_get(httpd_req_t *req)
{
    char json[3072];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"count\":%u,\"values\":[",
                (unsigned)SETTINGS_CV_COUNT);
    for (uint16_t i = 1; i <= SETTINGS_CV_COUNT; ++i) {
        uint8_t v = 0;
        (void)settings_cv_read(i, &v);
        buf_appendf(json, sizeof(json), &used, "%s%u", i == 1 ? "" : ",", (unsigned)v);
    }
    buf_appendf(json, sizeof(json), &used, "]}");
    return send_json(req, json);
}

esp_err_t cv_write_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    uint16_t idx = 0;
    uint8_t value = 0;
    if (!parse_u16(query, "index", &idx) || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad index");
    }
    if (!parse_u8(query, "value", &value)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad value");
    }
    esp_err_t werr = settings_cv_write(idx, value);
    if (werr != ESP_OK) {
        char json[64];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}",
                 esp_err_to_name(werr));
        return send_json(req, json);
    }
    if (parse_bool(query, "commit", false)) {
        esp_err_t cerr = settings_cv_commit();
        if (cerr != ESP_OK) {
            char json[96];
            snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}",
                     esp_err_to_name(cerr));
            return send_json(req, json);
        }
    }
    /* CV63 aliases the master volume: apply it live and keep the cached config
     * in sync so a later settings_save cannot revert it. */
    if (idx == 63U) {
        web_master_volume_changed(settings_volume_pct_from_cv63(value));
    }
    /* Address / speed-step / consist CVs must take effect immediately (the
     * runtime config is cached inside the DCC decoder, not read per packet).
     * CV8 = 8 is a factory reset of all CVs, so reload those as well. */
    if (idx == 1U || idx == 8U || idx == 17U || idx == 18U || idx == 19U ||
        idx == 21U || idx == 22U || idx == 29U) {
        dcc_reload_config();
    }
    web_log_event("CV", "CV %u = %u", (unsigned)idx, (unsigned)value);
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"index\":%u,\"value\":%u}",
             (unsigned)idx, (unsigned)value);
    return send_json(req, json);
}

esp_err_t wifi_get(httpd_req_t *req)
{
    settings_config_t cfg;
    if (!web_cfg_read(&cfg)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    char ap_ssid[SETTINGS_SSID_MAX * 2];
    json_escape(cfg.ap_ssid, ap_ssid, sizeof(ap_ssid));

    char json[384];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"mode\":%u,\"ap_ssid\":\"%s\",\"ap_password\":\"%s\","
             "\"auto_off_min\":%u,\"status\":\"%s\",\"ap_ip\":\"%s\"}",
             (unsigned)cfg.wifi_mode, ap_ssid,
             cfg.ap_password[0] ? "********" : "",
             (unsigned)cfg.auto_off_min,
             web_wifi_is_started() ? "ap_running" : "off", cfg.ap_ip);
    return send_json(req, json);
}

esp_err_t wifi_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query");
    if (!query_encoding_valid(query))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid query encoding");

    /* Parse into a local copy: on a validation error nothing may be applied,
     * otherwise a later deferred save would persist the partial change. */
    settings_config_t next;
    if (!web_cfg_read(&next)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    next.wifi_mode = 1; /* access point only */
    char buf[SETTINGS_PASS_MAX] = { 0 };
    if (query_has_key(query, "ap_ssid") && !parse_query(query, "ap_ssid", buf, sizeof(buf)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid SSID");
    if (parse_query(query, "ap_ssid", buf, sizeof(buf)) && buf[0]) {
        if (strlen(buf) > 32U)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID exceeds 32 bytes");
        strncpy(next.ap_ssid, buf, sizeof(next.ap_ssid) - 1);
        next.ap_ssid[sizeof(next.ap_ssid) - 1] = '\0';
    }
    memset(buf, 0, sizeof(buf));
    if (query_has_key(query, "ap_password") && !parse_query(query, "ap_password", buf, sizeof(buf)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid password");
    if (parse_query(query, "ap_password", buf, sizeof(buf)) && buf[0]) {
        if (strlen(buf) < 8U) {
            return send_json(req, "{\"ok\":false,\"error\":\"ap password must be >=8 chars\"}");
        }
        if (strlen(buf) == 64U) {
            for (size_t i = 0; i < 64U; ++i)
                if (!isxdigit((unsigned char)buf[i]))
                    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "64 byte PSK must be hex");
        }
        strncpy(next.ap_password, buf, sizeof(next.ap_password) - 1);
        next.ap_password[sizeof(next.ap_password) - 1] = '\0';
    }
    if (parse_bool(query, "ap_password_clear", false)) {
        next.ap_password[0] = '\0';
    }
    /* Configurable access point address (default 192.168.100.1). */
    memset(buf, 0, sizeof(buf));
    if (query_has_key(query, "ap_ip") && !parse_query(query, "ap_ip", buf, sizeof(buf)))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid IP");
    if (parse_query(query, "ap_ip", buf, sizeof(buf)) && buf[0]) {
        esp_ip4_addr_t parsed = { 0 };
        if (!parse_ip4(buf, &parsed)) {
            return send_json(req, "{\"ok\":false,\"error\":\"invalid ap ip\"}");
        }
        strncpy(next.ap_ip, buf, sizeof(next.ap_ip) - 1);
        next.ap_ip[sizeof(next.ap_ip) - 1] = '\0';
    }
    /* AP-only: the HTTP port is fixed at 80 and the captive portal is always on. */
    parse_u8(query, "auto_off_min", &next.auto_off_min);

    if (web_maintenance_begin(false) != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}");
    if (web_cfg_save(&next, false) != ESP_OK) {
        web_maintenance_end();
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    web_log_event("Wi-Fi", "настройки сохранены (IP %s), перезагрузка", next.ap_ip);
    send_json(req, "{\"ok\":true,\"reboot\":1}");
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

esp_err_t wifi_reset_post(httpd_req_t *req)
{
    settings_config_t next;
    if (!web_cfg_read(&next)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    if (web_maintenance_begin(false) != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}");
    /* Wi-Fi reset only touches Wi-Fi fields. Device name, volumes, control
     * source and track selection must survive it. */
    next.wifi_mode = 1;
    next.ap_password[0] = '\0';
    next.auto_off_min = 0;
    strncpy(next.ap_ip, AP_IP_DEFAULT, sizeof(next.ap_ip) - 1);
    next.ap_ip[sizeof(next.ap_ip) - 1] = '\0';
    strncpy(next.ap_ssid, "ADDITIPUS AURA-X", sizeof(next.ap_ssid) - 1);
    next.ap_ssid[sizeof(next.ap_ssid) - 1] = '\0';
    if (web_cfg_save(&next, false) != ESP_OK) {
        web_maintenance_end();
        return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
    }
    web_log_event("Wi-Fi", "сброс настроек, перезагрузка");
    send_json(req, "{\"ok\":true,\"reboot\":1}");
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* Full factory reset: wipe every stored setting (CVs, WiFi config, volumes,
 * name, control source, sound track slots, BEMF calibration) and reboot into
 * the factory state. Sound files on the storage are kept. */
esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (web_maintenance_begin(false) != ESP_OK)
        return send_json(req, "{\"ok\":false,\"error\":\"maintenance busy\"}");
    web_log_event("Сброс", "заводские настройки, перезагрузка");
    if (settings_factory_reset() != ESP_OK) {
        web_control_force_not_ready();
        web_maintenance_end();
        return graph_error(req, "500 Internal Server Error", "reset failed; actuation disabled; no reboot");
    }
    /* NVS and the external selector cannot form one atomic reset transaction. */
    if ((storage_is_mounted() || sound_graph_active()) && sg_store_select_legacy() != ESP_OK) {
        web_control_force_not_ready();
        web_maintenance_end();
        return graph_error(req, "500 Internal Server Error", "settings reset but graph selector reset failed; actuation disabled; no reboot");
    }
    sound_graph_deactivate();
    send_json(req, "{\"ok\":true,\"reboot\":1}");
    motor_emergency_stop();
    audio_stop_all();
    sound_stop_all();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

esp_err_t device_get(httpd_req_t *req)
{
    settings_config_t cfg;
    if (!web_cfg_read(&cfg)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    char esc[SETTINGS_NAME_MAX * 2];
    json_escape(cfg.device_name, esc, sizeof(esc));
    /* 32-bit seconds: fits for >100 years. Do NOT use %lld here — newlib nano
     * (CONFIG_NEWLIB_NANO_FORMAT) has no 64-bit printf conversion. */
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    char json[192];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"name\":\"%s\",\"version\":\"" WEB_FW_VERSION "\",\"uptime\":%lu}",
             esc, (unsigned long)uptime_s);
    return send_json(req, json);
}

esp_err_t device_post(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[SETTINGS_NAME_MAX] = { 0 };
    settings_config_t next;
    if (!web_cfg_read(&next)) {
        return send_json(req, "{\"ok\":false,\"error\":\"config busy\"}");
    }
    if (parse_query(query, "name", buf, sizeof(buf)) && buf[0]) {
        strncpy(next.device_name, buf, sizeof(next.device_name) - 1);
        next.device_name[sizeof(next.device_name) - 1] = '\0';
        if (web_cfg_save(&next, false) != ESP_OK) {
            return send_json(req, "{\"ok\":false,\"error\":\"save failed\"}");
        }
        web_log_event("Имя", "%s", next.device_name);
    }
    char esc[SETTINGS_NAME_MAX * 2];
    json_escape(next.device_name, esc, sizeof(esc));
    char json[128];
    snprintf(json, sizeof(json), "{\"ok\":true,\"name\":\"%s\"}", esc);
    return send_json(req, json);
}

esp_err_t storage_get(httpd_req_t *req)
{
    int64_t t0 = esp_timer_get_time();
    uint64_t free_bytes = 0;
    if (storage_get_free_bytes(&free_bytes) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage unavailable");
    }
    int64_t t1 = esp_timer_get_time();
    if (t1 - t0 > 10000) {
        ESP_LOGW(TAG_SETTINGS, "storage_get slow: took=%lu us", (unsigned long)(t1 - t0));
    }
    char json[96];
    uint32_t hi = (uint32_t)(free_bytes >> 32);
    uint32_t lo = (uint32_t)(free_bytes & 0xFFFFFFFFu);
    if (hi != 0) {
        snprintf(json, sizeof(json), "{\"ok\":true,\"free\":%lu%09lu}",
                 (unsigned long)hi, (unsigned long)lo);
    } else {
        snprintf(json, sizeof(json), "{\"ok\":true,\"free\":%lu}", (unsigned long)lo);
    }
    return send_json(req, json);
}

esp_err_t task_inputs_get(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"actions\":[1,2,3]}");
}
