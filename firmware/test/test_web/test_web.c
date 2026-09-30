/* White-box host tests for components/web/src/web.c.
 *
 * web.c is compiled into this translation unit with `static` stripped (so the
 * internal handlers/helpers are callable) together with the pure web_util.c
 * helpers, the shared ESP-IDF stubs and the web-specific mock server /
 * dependency stubs in test_libs/teststubs/web_stubs.c.
 *
 * Storage paths are redirected to a relative web_tmp/ tree; malloc/calloc and
 * fsync are renamed so fault injection (OOM, fsync failure) can reach the
 * defensive branches. */

#include <unity.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <sys/stat.h>
#include <unistd.h>

#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)

/* Keep the filesystem writes inside the repository working tree. */
#define WEB_USERDATA_DIR "web_tmp"

void *mock_web_malloc(size_t n);
void *mock_web_calloc(size_t n, size_t s);
int web_mock_fsync(int fd);

#define malloc(n) mock_web_malloc(n)
#define calloc(n, s) mock_web_calloc(n, s)
#define fsync web_mock_fsync

size_t web_mock_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int web_mock_fflush(FILE *f);
int web_mock_fclose(FILE *f);
#define WEB_FWRITE(p, sz, n, f) web_mock_fwrite((p), (sz), (n), (f))
#define WEB_FFLUSH(f) web_mock_fflush(f)
#define WEB_FCLOSE(f) web_mock_fclose(f)

/* Log an upload-progress line on every 4 KB so the branch is cheap to reach. */
#define WEB_UP_PROGRESS_STEP 4096

#define static
#include "../../components/web/src/web_util.c"
#include "../../components/web/src/web.c"
#include "../../test_libs/teststubs/stubs.c"
#include "../../test_libs/teststubs/web_stubs.c"

/* ---------- helpers ---------- */

static httpd_req_t make_req(size_t content_len)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.uri = "/test";
    req.content_len = content_len;
    return req;
}

static void set_query(const char *q)
{
    snprintf(mock_query, sizeof(mock_query), "%s", q != NULL ? q : "");
}

static void set_body(const void *data, size_t len)
{
    mock_httpd_set_body(data, len);
}

static void reset_resp(void)
{
    mock_resp_body_len = 0;
    mock_resp_body[0] = '\0';
    mock_resp_type[0] = '\0';
    mock_resp_status[0] = '\0';
    mock_resp_hdr[0] = '\0';
    mock_resp_send_err_code = 0;
    mock_resp_send_err_msg[0] = '\0';
    mock_resp_send_calls = 0;
}

/* Remove every entry in web_tmp/audio (files and probe directories) so no
 * state leaks from one test into the next. */
static void clear_files(void)
{
    DIR *d = opendir("web_tmp/audio");
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char p[320];
        snprintf(p, sizeof(p), "web_tmp/audio/%s", e->d_name);
        remove(p);
        RMDIR(p);
    }
    closedir(d);
}

void setUp(void)
{
    mock_web_reset();
    mock_nvs_reset();
    clear_files();
    MKDIR("web_tmp");
    MKDIR("web_tmp/audio");
    /* Bindings are per-suite state in web.c; keep the legacy fallback path
     * active unless a test populates them explicitly. */
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind_count = 0;
}

void tearDown(void)
{
    clear_files();
}

/* ---------- event log ---------- */

static void test_web_log_event_null_mutex(void)
{
    s_evlog_mutex = NULL;
    web_log_event("T", "ignored %d", 1);
    TEST_ASSERT_EQUAL_UINT32(0, s_evlog_seq);
}

static void test_web_log_event_stores(void)
{
    web_log_event("Tag", "value %u", 7U);
    TEST_ASSERT_EQUAL_UINT32(1, s_evlog_seq);
    web_log_event(NULL, "no tag");
    TEST_ASSERT_EQUAL_UINT32(2, s_evlog_seq);
    TEST_ASSERT_EQUAL_STRING("no tag", s_evlog[1].text);
}

static void test_web_log_event_utf8_and_truncation(void)
{
    char big[300];
    memset(big, 'A', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    web_log_event(big, "%s", big);
    TEST_ASSERT_EQUAL_UINT32(1, s_evlog_seq);
    TEST_ASSERT_TRUE(strlen(s_evlog[0].tag) <= WEB_EVLOG_TAG_MAX - 1U);
    TEST_ASSERT_TRUE(strlen(s_evlog[0].text) <= WEB_EVLOG_TEXT_MAX - 1U);
    /* Multi-byte text that would be cut in the middle. */
    web_log_event("u", "\xd0\xb0\xd0\xb1\xd0\xb2");
    TEST_ASSERT_EQUAL_STRING("\xd0\xb0\xd0\xb1\xd0\xb2", s_evlog[1].text);
}

/* ---------- function state ---------- */

static void test_get_function_state_bounds(void)
{
    s_fn[3] = true;
    TEST_ASSERT_TRUE(web_get_function_state(3));
    TEST_ASSERT_FALSE(web_get_function_state(WEB_FN_COUNT));
}

static void test_control_is_rails(void)
{
    s_cfg.control_source = 0;
    TEST_ASSERT_TRUE(web_control_is_rails());
    s_cfg.control_source = 1;
    TEST_ASSERT_FALSE(web_control_is_rails());
}

/* ---------- wifi event handler ---------- */

static void test_wifi_event_handler(void)
{
    wifi_event_ap_staconnected_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.aid = 1;
    wifi_event_ap_stadisconnected_t disc;
    memset(&disc, 0, sizeof(disc));
    disc.aid = 2;
    disc.reason = 3;

    s_ap_sta_count = 0;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_START, NULL);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STOP, NULL);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &conn);
    TEST_ASSERT_EQUAL_UINT8(1, s_ap_sta_count);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &disc);
    TEST_ASSERT_EQUAL_UINT8(0, s_ap_sta_count);
    /* Saturating / floor behaviour. */
    s_ap_sta_count = 255;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, &conn);
    TEST_ASSERT_EQUAL_UINT8(255, s_ap_sta_count);
    s_ap_sta_count = 0;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, &disc);
    TEST_ASSERT_EQUAL_UINT8(0, s_ap_sta_count);
    /* Unknown event id and a different base. */
    wifi_event_handler(NULL, WIFI_EVENT, 9999, NULL);
    ip_event_assigned_ip_to_client_t assigned;
    memset(&assigned, 0, sizeof(assigned));
    wifi_event_handler(NULL, IP_EVENT, IP_EVENT_ASSIGNED_IP_TO_CLIENT, &assigned);
    wifi_event_handler(NULL, IP_EVENT, 42, &assigned);
    TEST_PASS();
}

/* ---------- channel pick ---------- */

static void test_wifi_pick_channel_scan_fail(void)
{
    mock_scan_start_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL_UINT8(1, wifi_pick_channel());
}

static void test_wifi_pick_channel_no_aps(void)
{
    mock_scan_start_ret = ESP_OK;
    mock_scan_num_ret = ESP_OK;
    mock_scan_num = 0;
    TEST_ASSERT_EQUAL_UINT8(1, wifi_pick_channel());
    TEST_ASSERT_EQUAL_INT(1, mock_clear_ap_list_calls);
}

static void test_wifi_pick_channel_num_fail(void)
{
    mock_scan_start_ret = ESP_OK;
    mock_scan_num_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL_UINT8(1, wifi_pick_channel());
    TEST_ASSERT_EQUAL_INT(1, mock_clear_ap_list_calls);
}

static void test_wifi_pick_channel_records_fail(void)
{
    mock_scan_start_ret = ESP_OK;
    mock_scan_num_ret = ESP_OK;
    mock_scan_num = 2;
    mock_scan_records_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL_UINT8(1, wifi_pick_channel());
    TEST_ASSERT_EQUAL_INT(1, mock_clear_ap_list_calls);
}

static void test_wifi_pick_channel_scores(void)
{
    mock_scan_start_ret = ESP_OK;
    mock_scan_num_ret = ESP_OK;
    mock_scan_num = 20; /* exercises the >16 clamp */
    mock_scan_records_n = 3;
    mock_scan_records[0].rssi = -50;
    mock_scan_records[0].primary = 1;
    mock_scan_records[1].rssi = -90; /* too far: ignored */
    mock_scan_records[1].primary = 6;
    mock_scan_records[2].rssi = -60;
    mock_scan_records[2].primary = 11;
    uint8_t ch = wifi_pick_channel();
    TEST_ASSERT_TRUE(ch == 1 || ch == 6 || ch == 11);
}

/* ---------- function outputs / mapping ---------- */

static void test_web_apply_function_bounds(void)
{
    web_apply_function(SETTINGS_FUNC_MAP_COUNT, true);
    TEST_ASSERT_FALSE(web_get_function_state(SETTINGS_FUNC_MAP_COUNT));
}

static void test_web_apply_function_mutex_null(void)
{
    s_func_mutex = NULL;
    s_func_map[1].aux_mask = 0;
    web_apply_function(1, true);
    TEST_ASSERT_TRUE(s_fn[1]);
}

static void test_web_apply_function_output_delta(void)
{
    s_func_map[1].aux_mask = SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_AUX1;
    s_func_map[1].dir = SETTINGS_FUNC_DIR_NONE;
    s_func_map[1].speed = SETTINGS_FUNC_SPD_NONE;
    s_motion_initialized = true;
    s_motion_forward = true;
    s_motion_speed = SETTINGS_FUNC_SPD_MOVING;
    web_apply_function(1, true);
    int on = mock_auxio_set_enabled_calls;
    TEST_ASSERT_TRUE(on > 0);
    web_apply_function(1, false);
    TEST_ASSERT_TRUE(mock_auxio_set_enabled_calls > on);
}

static void test_web_apply_function_unchanged(void)
{
    s_func_map[2].aux_mask = 0;
    s_motion_initialized = true;
    web_apply_function(2, true);
    int log_seq = (int)s_evlog_seq;
    web_apply_function(2, true);
    TEST_ASSERT_EQUAL_INT(log_seq, (int)s_evlog_seq);
}

static void test_web_apply_function_starts_sound(void)
{
    s_func_map[1].slot_a = 3;
    s_func_map[1].slot_b = 0;
    mock_tracks_count = 1;
    mock_tracks[0].slot = 3;
    mock_tracks[0].enabled = true;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/slot3.wav");
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_INT(1, mock_audio_voice_play_calls);
    TEST_ASSERT_EQUAL_STRING("web_tmp/audio/slot3.wav", mock_audio_last_path);
}

static void test_web_apply_function_sound_missing_stops(void)
{
    s_func_map[1].slot_a = 9;
    s_func_map[1].slot_b = 0;
    mock_tracks_count = 0;
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_INT(2, mock_audio_voice_stop_calls);
}

static void test_web_apply_function_tracks_load_fail(void)
{
    s_func_map[1].slot_a = 4;
    mock_tracks_load_ret = ESP_FAIL;
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_INT(0, mock_audio_voice_play_calls);
}

static void test_web_apply_function_double_voice(void)
{
    s_func_map[2].slot_a = 1;
    s_func_map[2].slot_b = 2;
    mock_tracks_count = 2;
    mock_tracks[0].slot = 1;
    mock_tracks[0].enabled = true;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/a.wav");
    mock_tracks[1].slot = 2;
    mock_tracks[1].enabled = true;
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/b.wav");
    web_apply_function(2, true);
    TEST_ASSERT_EQUAL_INT(2, mock_audio_voice_play_calls);
}

static void test_web_apply_function_b_voice_ignored_above_10(void)
{
    s_func_map[11].slot_a = 1;
    s_func_map[11].slot_b = 2;
    mock_tracks_count = 1;
    mock_tracks[0].slot = 1;
    mock_tracks[0].enabled = true;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/a.wav");
    web_apply_function(11, true);
    TEST_ASSERT_EQUAL_INT(1, mock_audio_voice_play_calls);
}

static void test_web_apply_function_out_of_audio_range(void)
{
    s_func_map[25].aux_mask = SETTINGS_FUNC_OUT_F0F;
    web_apply_function(25, true);
    TEST_ASSERT_EQUAL_INT(0, mock_audio_voice_play_calls);
}

static void test_web_apply_function_scheme_routes_to_sound(void)
{
    /* With a scheme active the engine owns F routing; the legacy slot mapping
     * must not start a voice (engine voices 18/19 are reserved). */
    mock_sound_scheme_enabled = 1;
    s_func_map[2].slot_a = 3;
    mock_tracks_count = 1;
    mock_tracks[0].slot = 3;
    mock_tracks[0].enabled = true;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/slot3.wav");

    web_apply_function(2, true);
    TEST_ASSERT_EQUAL_INT(1, mock_sound_function_calls);
    TEST_ASSERT_EQUAL_UINT8(2, mock_sound_last_fn);
    TEST_ASSERT_TRUE(mock_sound_last_state);
    TEST_ASSERT_EQUAL_INT(0, mock_audio_voice_play_calls);

    web_apply_function(2, false);
    TEST_ASSERT_EQUAL_INT(2, mock_sound_function_calls);
    TEST_ASSERT_FALSE(mock_sound_last_state);
}

static void test_web_motion_changed(void)
{
    s_motion_initialized = false;
    web_motion_changed(5, true);
    TEST_ASSERT_EQUAL_UINT8(5, s_motion_speed);
    TEST_ASSERT_TRUE(s_motion_forward);
    uint32_t seq = s_evlog_seq;
    web_motion_changed(5, true);
    TEST_ASSERT_EQUAL_UINT32(seq, s_evlog_seq);
    web_motion_changed(9, false);
    TEST_ASSERT_FALSE(s_motion_forward);
}

static void test_web_func_map_get_set(void)
{
    TEST_ASSERT_TRUE(web_func_map_set(1, 2, 3, SETTINGS_FUNC_OUT_F0F,
                                      SETTINGS_FUNC_DIR_FWD, SETTINGS_FUNC_SPD_MOVING));
    uint8_t sa = 0, sb = 0, dir = 0, spd = 0;
    uint16_t aux = 0;
    TEST_ASSERT_TRUE(web_func_map_get(1, &sa, &sb, &aux, &dir, &spd));
    TEST_ASSERT_EQUAL_UINT8(2, sa);
    TEST_ASSERT_EQUAL_UINT8(3, sb);
    TEST_ASSERT_EQUAL_UINT16(SETTINGS_FUNC_OUT_F0F, aux);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_FUNC_DIR_FWD, dir);
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_FUNC_SPD_MOVING, spd);
    /* Null out-params are tolerated. */
    TEST_ASSERT_TRUE(web_func_map_get(1, NULL, NULL, NULL, NULL, NULL));
    TEST_ASSERT_FALSE(web_func_map_get(SETTINGS_FUNC_MAP_COUNT, &sa, &sb, &aux, &dir, &spd));
    TEST_ASSERT_FALSE(web_func_map_set(SETTINGS_FUNC_MAP_COUNT, 1, 1, 0, 0, 0));
    /* slot_a == 0 formats the "вЂ”" placeholder. */
    TEST_ASSERT_TRUE(web_func_map_set(3, 0, 4, 0, 0, 0));
    mock_map_save_ret = ESP_FAIL;
    TEST_ASSERT_FALSE(web_func_map_set(4, 1, 0, 0, 0, 0));
}

/* ---------- IP parsing ---------- */

static void test_parse_ip4(void)
{
    esp_ip4_addr_t a;
    TEST_ASSERT_FALSE(parse_ip4(NULL, &a));
    TEST_ASSERT_FALSE(parse_ip4("", &a));
    TEST_ASSERT_FALSE(parse_ip4("10.0.0.1", NULL));
    TEST_ASSERT_FALSE(parse_ip4("0.0.0.0", &a));
    TEST_ASSERT_FALSE(parse_ip4("not-an-ip", &a));
    TEST_ASSERT_TRUE(parse_ip4("10.0.0.1", &a));
}

/* ---------- wifi_start ---------- */

static settings_config_t make_wifi_cfg(void)
{
    settings_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.wifi_mode = 1;
    snprintf(cfg.ap_ssid, sizeof(cfg.ap_ssid), "AP");
    snprintf(cfg.ap_ip, sizeof(cfg.ap_ip), "10.0.0.1");
    return cfg;
}

static void test_wifi_start_off(void)
{
    settings_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.wifi_mode = 0;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_full(void)
{
    settings_config_t cfg = make_wifi_cfg();
    snprintf(cfg.ap_password, sizeof(cfg.ap_password), "supersecret");
    mock_netif_actual_ip.addr = ESP_IP4TOADDR(10, 0, 0, 2);
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_TRUE(s_wifi_started);
    TEST_ASSERT_EQUAL_INT(1, mock_netif_dhcps_start_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_netif_set_dns_calls);
}

static void test_wifi_start_short_password(void)
{
    settings_config_t cfg = make_wifi_cfg();
    snprintf(cfg.ap_password, sizeof(cfg.ap_password), "123");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_init_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_init_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, wifi_start(&cfg));
}

static void test_wifi_start_init_invalid_state(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_init_ret = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_no_sta_netif(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_sta_netif_null = 1;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_sta_mode_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_mode_sta_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_sta_start_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_start_first_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_channel_chosen(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_scan_start_ret = ESP_OK;
    mock_scan_num_ret = ESP_OK;
    mock_scan_num = 2;
    mock_scan_records_n = 2;
    /* Channel 1 is busy, 11 is clear -> best is 6. */
    mock_scan_records[0].rssi = -40;
    mock_scan_records[0].primary = 1;
    mock_scan_records[1].rssi = -40;
    mock_scan_records[1].primary = 1;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_ap_netif_null(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_ap_netif_null = 1;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_get_ip_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_netif_get_ip_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

static void test_wifi_start_bad_ap_ip(void)
{
    settings_config_t cfg = make_wifi_cfg();
    snprintf(cfg.ap_ip, sizeof(cfg.ap_ip), "bogus");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
}

/* ---------- wifi auto-off ---------- */

static void test_wifi_auto_off_paths(void)
{
    s_autooff_iter_cap = 1;
    s_wifi_started = false;
    wifi_auto_off_task(NULL);

    s_wifi_started = true;
    s_cfg.auto_off_min = 0;
    wifi_auto_off_task(NULL);

    s_cfg.auto_off_min = 5;
    s_ap_sta_count = 1;
    wifi_auto_off_task(NULL);

    s_ap_sta_count = 0;
    s_last_client_us = mock_timer_now_us;
    wifi_auto_off_task(NULL);
    TEST_ASSERT_EQUAL_INT(0, mock_wifi_stop_calls);

    mock_timer_now_us += (int64_t)5 * 60 * 1000000;
    wifi_auto_off_task(NULL);
    TEST_ASSERT_FALSE(s_wifi_started);
    TEST_ASSERT_EQUAL_INT(1, mock_wifi_stop_calls);
}

/* ---------- captive DNS ---------- */

static int build_dns_query(uint8_t *out)
{
    memset(out, 0, 32);
    out[0] = 0x12;
    out[1] = 0x34;
    out[2] = 0x01; /* flags: standard query */
    out[5] = 0x01; /* qdcount = 1 */
    out[12] = 3;   /* "www" */
    out[13] = 'w';
    out[14] = 'w';
    out[15] = 'w';
    out[16] = 3;
    out[17] = 'c';
    out[18] = 'o';
    out[19] = 'm';
    out[20] = 0;
    out[21] = 0;
    out[22] = 1;
    out[23] = 0;
    out[24] = 1;
    return 25;
}

static void test_dns_task_socket_fail(void)
{
    mock_socket_ret = -1;
    s_dns_iter_cap = 1;
    dns_server_task(NULL);
    TEST_ASSERT_EQUAL_INT(0, mock_socket_close_calls);
}

static void test_dns_task_bind_fail(void)
{
    mock_socket_ret = 5;
    mock_bind_ret = -1;
    s_dns_iter_cap = 1;
    dns_server_task(NULL);
    TEST_ASSERT_EQUAL_INT(1, mock_socket_close_calls);
}

static void test_dns_task_serves(void)
{
    mock_socket_ret = 5;
    mock_bind_ret = 0;
    s_dns_iter_cap = 2;
    mock_recv_script_n = 2;
    mock_recv_script_len[0] = 5; /* too short: skipped */
    uint8_t q[32];
    int qlen = build_dns_query(q);
    mock_recv_script_len[1] = qlen;
    memcpy(mock_recv_script_buf[1], q, (size_t)qlen);
    dns_server_task(NULL);
    TEST_ASSERT_EQUAL_INT(1, mock_sendto_calls);
}

/* ---------- simple GET handlers ---------- */

static void test_root_handler(void)
{
    httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, root_handler(&req));
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", mock_resp_type);
    TEST_ASSERT_TRUE(mock_resp_body_len > 1000U);
}

static void test_control_source_get(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 1;
    TEST_ASSERT_EQUAL(ESP_OK, control_source_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"source\":\"web\""));
    reset_resp();
    s_cfg.control_source = 0;
    TEST_ASSERT_EQUAL(ESP_OK, control_source_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"source\":\"rails\""));
}

static void test_control_source_post(void)
{
    httpd_req_t req = make_req(0);
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("source=web");
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_EQUAL_UINT8(1, s_cfg.control_source);
    TEST_ASSERT_EQUAL_INT(1, mock_settings_save_calls);

    reset_resp();
    set_query("source=1");
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_EQUAL_UINT8(1, s_cfg.control_source);

    reset_resp();
    set_query("source=rails");
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_EQUAL_UINT8(0, s_cfg.control_source);
    TEST_ASSERT_EQUAL_INT(1, mock_cv_write_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_cv_commit_calls);
    TEST_ASSERT_EQUAL_INT(3, mock_motor_stop_calls);
}

static void test_mode_get(void)
{
    httpd_req_t req = make_req(0);
    mock_cv[29] = 0x02;
    TEST_ASSERT_EQUAL(ESP_OK, mode_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"mode\":\"dcc\""));
    reset_resp();
    mock_cv[29] = 0x06;
    TEST_ASSERT_EQUAL(ESP_OK, mode_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"mode\":\"dc\""));
}

static void test_mode_post(void)
{
    httpd_req_t req = make_req(0);
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, mode_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("mode=dc");
    TEST_ASSERT_EQUAL(ESP_OK, mode_post(&req));
    TEST_ASSERT_EQUAL_UINT8(0x04, mock_cv[29] & 0x04U);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"mode\":\"dc\""));

    reset_resp();
    set_query("mode=dcc");
    TEST_ASSERT_EQUAL(ESP_OK, mode_post(&req));
    TEST_ASSERT_EQUAL_UINT8(0, mock_cv[29] & 0x04U);
    TEST_ASSERT_EQUAL_INT(2, mock_motor_stop_calls);
}

static void test_bemf_cal_get(void)
{
    httpd_req_t req = make_req(0);
    memset(&mock_bemf_cal_info, 0, sizeof(mock_bemf_cal_info));
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"points\":[]"));
    reset_resp();
    mock_bemf_cal_info.active = true;
    mock_bemf_cal_info.valid = true;
    mock_bemf_cal_info.stored = true;
    mock_bemf_cal_info.step = 2;
    mock_bemf_cal_info.total = 16;
    mock_bemf_cal_info.count = 2;
    mock_bemf_cal_info.speed[0] = 5;
    mock_bemf_cal_info.frac[0] = 512;
    mock_bemf_cal_info.speed[1] = 6;
    mock_bemf_cal_info.frac[1] = 1024;
    mock_bemf_enabled = 1;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"speed\":5,\"frac\":50"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"use\":true"));
}

static void test_bemf_base_get(void)
{
    httpd_req_t req = make_req(0);
    memset(&mock_bemf_base_info, 0, sizeof(mock_bemf_base_info));
    mock_bemf_base_info.count = 1;
    mock_bemf_base_info.speed[0] = 3;
    mock_bemf_base_info.frac[0] = 256;
    mock_bemf_base_info.start_frac = 0;
    mock_bemf_base_info.full_frac = 1024;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_base_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"start\":0"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"speed\":3"));
}

static void test_bemf_cal_post_paths(void)
{
    httpd_req_t req = make_req(0);
    set_query("");
    mock_bemf_cal_start_ret = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "calibration running"));

    reset_resp();
    mock_bemf_cal_start_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "start failed"));

    reset_resp();
    mock_bemf_cal_start_ret = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    reset_resp();
    set_query("reset=1");
    mock_bemf_cal_clear_ret = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_bemf_cal_clear_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    reset_resp();
    mock_bemf_cal_clear_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "calibration busy"));
}

static void test_bemf_use_get_post(void)
{
    httpd_req_t req = make_req(0);
    mock_bemf_enabled = 1;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"enabled\":true"));
    reset_resp();
    set_query("enabled=0");
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_enabled);
    TEST_ASSERT_EQUAL_INT(1, mock_bemf_use_save_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"enabled\":false"));
    reset_resp();
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_enabled);
}

static void test_motor_get_post(void)
{
    httpd_req_t req = make_req(0);
    mock_motor_speed = 42;
    mock_motor_forward = false;
    TEST_ASSERT_EQUAL(ESP_OK, motor_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"speed\":42"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"forward\":false"));

    /* Rails control rejects web motor commands. */
    reset_resp();
    s_cfg.control_source = 0;
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "rails control active"));

    /* Web control: explicit speed + forward. */
    reset_resp();
    s_cfg.control_source = 1;
    mock_motor_speed = 0;
    mock_motor_forward = true;
    set_query("speed=50&forward=0");
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_motor_set_speed_calls);
    TEST_ASSERT_EQUAL_UINT8(50, mock_motor_set_speed_value);
    TEST_ASSERT_FALSE(mock_motor_set_speed_fwd);

    /* No params: reuse current status. */
    reset_resp();
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_INT(2, mock_motor_set_speed_calls);

    /* Out of range. */
    reset_resp();
    set_query("speed=127");
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
}

static void test_functions_get(void)
{
    httpd_req_t req = make_req(0);
    s_fn[0] = true;
    s_fn[WEB_FN_COUNT - 1] = true;
    TEST_ASSERT_EQUAL(ESP_OK, functions_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"states\":["));
}

static void test_function_post(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 0;
    TEST_ASSERT_EQUAL(ESP_OK, function_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "rails control active"));

    reset_resp();
    s_cfg.control_source = 1;
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, function_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("fn=29");
    TEST_ASSERT_EQUAL(ESP_OK, function_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("fn=1&state=1");
    TEST_ASSERT_EQUAL(ESP_OK, function_post(&req));
    TEST_ASSERT_TRUE(s_fn[1]);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"state\":true"));
}

static void test_aux_effect_post(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 0;
    TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "rails control active"));

    reset_resp();
    s_cfg.control_source = 1;
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("ch=99");
    TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("ch=2&on=1&pwm_on=200&pwm_off=10&mode=99&period=500");
    TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_auxio_set_effect_calls);
    TEST_ASSERT_EQUAL_INT(AUXIO_EFFECT_STEADY, mock_auxio_last_mode);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"mode\":99"));
}

static void test_audio_status_get(void)
{
    httpd_req_t req = make_req(0);
    mock_audio_is_playing = 1;
    s_cfg.active_slot = 4;
    s_cfg.master_volume = 30;
    s_cfg.engine_volume = 40;
    s_cfg.effects_volume = 50;
    TEST_ASSERT_EQUAL(ESP_OK, audio_status_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"playing\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"active_slot\":4"));
    reset_resp();
    mock_audio_is_playing = 0;
    TEST_ASSERT_EQUAL(ESP_OK, audio_status_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"playing\":false"));
}

static void test_audio_tracks_get(void)
{
    httpd_req_t req = make_req(0);
    mock_tracks_count = 2;
    mock_tracks[0].slot = 1;
    mock_tracks[0].enabled = true;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/slot1.wav");
    snprintf(mock_tracks[0].label, sizeof(mock_tracks[0].label), "Engine");
    mock_tracks[1].slot = 2;
    s_track_cat[0] = SETTINGS_TRACK_CAT_ENGINE;
    s_track_cat[1] = SETTINGS_TRACK_CAT_EFFECTS;
    TEST_ASSERT_EQUAL(ESP_OK, audio_tracks_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"label\":\"Engine\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"cats\":[0,1"));
}

static void test_audio_play_post(void)
{
    httpd_req_t req = make_req(0);
    mock_tracks_count = 1;
    mock_tracks[0].slot = 3;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/slot3.wav");
    set_query("slot=3");
    TEST_ASSERT_EQUAL(ESP_OK, audio_play_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_audio_voice_play_calls);
    TEST_ASSERT_EQUAL_UINT8(3, s_cfg.active_slot);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    reset_resp();
    mock_audio_voice_play_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, audio_play_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));

    reset_resp();
    set_query("slot=99");
    TEST_ASSERT_EQUAL(ESP_OK, audio_play_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "no file"));
}

static void test_audio_stop_post(void)
{
    httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, audio_stop_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_audio_stop_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
}

static void test_audio_volume_post(void)
{
    httpd_req_t req = make_req(0);
    set_query("master=150&engine=60&effects=70");
    TEST_ASSERT_EQUAL(ESP_OK, audio_volume_post(&req));
    TEST_ASSERT_EQUAL_UINT8(100, s_cfg.master_volume);
    TEST_ASSERT_EQUAL_UINT8(60, s_cfg.engine_volume);
    TEST_ASSERT_EQUAL_UINT8(70, s_cfg.effects_volume);
    TEST_ASSERT_EQUAL_INT(1, mock_audio_set_volume_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_settings_save_deferred_calls);

    /* Same values -> no new journal entry. */
    uint32_t seq = s_evlog_seq;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_volume_post(&req));
    TEST_ASSERT_EQUAL_UINT32(seq, s_evlog_seq);
}

/* ---------- internals ---------- */

static void test_internal_helpers(void)
{
    TEST_ASSERT_EQUAL_UINT16(570, aux_effect_period(AUXIO_EFFECT_MARS));
    TEST_ASSERT_EQUAL_UINT16(900, aux_effect_period(AUXIO_EFFECT_BEACON));
    TEST_ASSERT_EQUAL_UINT16(1000, aux_effect_period(AUXIO_EFFECT_STROBE));
    TEST_ASSERT_EQUAL_UINT16(600, aux_effect_period(AUXIO_EFFECT_DITCH));
    TEST_ASSERT_EQUAL_UINT16(800, aux_effect_period(AUXIO_EFFECT_STEADY));
    TEST_ASSERT_EQUAL_UINT8(AUXIO_CH_F0F, func_out_channel(0));
    TEST_ASSERT_EQUAL_UINT8(AUXIO_CH_F0R, func_out_channel(1));
    TEST_ASSERT_EQUAL_UINT8(AUXIO_CH_AUX1, func_out_channel(2));
    TEST_ASSERT_EQUAL_UINT8(AUXIO_CH_AUX7, func_out_channel(8));
    /* fn out of range is a no-op. */
    func_apply_output_locked(SETTINGS_FUNC_MAP_COUNT);
    /* audio slots for an out-of-range function returns zeros. */
    uint8_t sa = 9, sb = 9;
    web_func_audio_slots(SETTINGS_FUNC_MAP_COUNT, &sa, &sb);
    TEST_ASSERT_EQUAL_UINT8(0, sa);
    TEST_ASSERT_EQUAL_UINT8(0, sb);
    web_func_audio_slots(SETTINGS_FUNC_MAP_COUNT, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, web_get_voice_volume(1));
}

/* ---------- pipe writer ---------- */

static void test_pipe_writer_success(void)
{
    pipe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = fopen("web_tmp/audio/pw.bin", "wb");
    TEST_ASSERT_NOT_NULL(ctx.f);
    ctx.write_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(pipe_item_t));
    ctx.free_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(int));
    ctx.done_sem = xSemaphoreCreateBinary();
    ctx.write_err = ESP_OK;
    ctx.bufs[0] = (uint8_t *)malloc(16);
    memcpy(ctx.bufs[0], "abc", 3);

    pipe_item_t it = { .idx = 0, .len = 3, .last = false };
    xQueueSend(ctx.write_q, &it, 0);
    pipe_item_t empty = { .idx = 0, .len = 0, .last = false };
    xQueueSend(ctx.write_q, &empty, 0);
    pipe_item_t last = { .idx = 0, .len = 0, .last = true };
    xQueueSend(ctx.write_q, &last, 0);
    s_pipe_iter_cap = 8;
    pipe_writer(&ctx);
    fclose(ctx.f);
    free(ctx.bufs[0]);
    vQueueDelete(ctx.write_q);
    vQueueDelete(ctx.free_q);
    vSemaphoreDelete(ctx.done_sem);

    FILE *r = fopen("web_tmp/audio/pw.bin", "rb");
    char got[8] = { 0 };
    size_t n = fread(got, 1, sizeof(got), r);
    fclose(r);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("abc", got);
}

static void test_pipe_writer_abort(void)
{
    pipe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = fopen("web_tmp/audio/pw.bin", "wb");
    ctx.write_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(pipe_item_t));
    ctx.free_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(int));
    ctx.done_sem = xSemaphoreCreateBinary();
    ctx.write_err = ESP_OK;
    ctx.abort = true;
    s_pipe_iter_cap = 1;
    pipe_writer(&ctx);
    fclose(ctx.f);
    vQueueDelete(ctx.write_q);
    vQueueDelete(ctx.free_q);
    vSemaphoreDelete(ctx.done_sem);
    TEST_PASS();
}

static void test_pipe_writer_write_fail(void)
{
    FILE *w = fopen("web_tmp/audio/pw.bin", "wb");
    fputs("x", w);
    fclose(w);
    pipe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = fopen("web_tmp/audio/pw.bin", "rb"); /* read-only -> fwrite fails */
    TEST_ASSERT_NOT_NULL(ctx.f);
    ctx.write_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(pipe_item_t));
    ctx.free_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(int));
    ctx.done_sem = xSemaphoreCreateBinary();
    ctx.write_err = ESP_OK;
    ctx.bufs[0] = (uint8_t *)malloc(16);
    memcpy(ctx.bufs[0], "abc", 3);
    pipe_item_t it = { .idx = 0, .len = 3, .last = true };
    xQueueSend(ctx.write_q, &it, 0);
    s_pipe_iter_cap = 4;
    pipe_writer(&ctx);
    fclose(ctx.f);
    free(ctx.bufs[0]);
    vQueueDelete(ctx.write_q);
    vQueueDelete(ctx.free_q);
    vSemaphoreDelete(ctx.done_sem);
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, ctx.write_err);
}

/* ---------- audio upload ---------- */

static bool file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static void test_audio_upload_guards(void)
{
    httpd_req_t req = make_req(16);
    mock_storage_is_mounted = 0;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);

    mock_storage_is_mounted = 1;
    reset_resp();
    req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    req = make_req((size_t)UPLOAD_MAX + 1U);
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
}

static void test_audio_upload_success(void)
{
    uint8_t data[16];
    memset(data, 1, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "my file.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_STRING("audio/my_file.wav", mock_saved_tracks[0].file);
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/my_file.wav"));

    /* Empty header -> default slotN.wav name. */
    reset_resp();
    set_body(data, sizeof(data));
    mock_header_name[0] = '\0';
    set_query("slot=2");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "slot2.wav"));
}

static void test_audio_upload_replaces_and_removes_old(void)
{
    FILE *f = fopen("web_tmp/audio/old.wav", "wb");
    fputs("old", f);
    fclose(f);
    mock_tracks_count = 2;
    mock_tracks[0].slot = 1;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/old.wav");
    mock_tracks[1].slot = 3;
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/keep.wav");
    uint8_t data[16];
    memset(data, 2, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "new.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/new.wav"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)mock_saved_tracks_count);
    TEST_ASSERT_EQUAL_STRING("audio/new.wav", mock_saved_tracks[0].file);
}

static void test_audio_upload_compacts_duplicates(void)
{
    mock_tracks_count = 3;
    for (int i = 0; i < 3; ++i) {
        mock_tracks[i].slot = 1;
        snprintf(mock_tracks[i].file, sizeof(mock_tracks[i].file), "audio/old%d.wav", i);
    }
    uint8_t data[16];
    memset(data, 3, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "x.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)mock_saved_tracks_count);
}

static void test_audio_upload_slots_full(void)
{
    mock_tracks_count = SETTINGS_MAX_TRACKS;
    for (int i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        mock_tracks[i].slot = (uint8_t)(i + 1);
        snprintf(mock_tracks[i].file, sizeof(mock_tracks[i].file), "audio/t%d.wav", i);
    }
    uint8_t data[16];
    memset(data, 4, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=99");
    snprintf(mock_header_name, sizeof(mock_header_name), "y.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "track slots full"));
}

/* No slot in the query -> the first free slot is assigned (slot 0 is not
 * displayable or deletable). */
static void test_audio_upload_autoslot(void)
{
    mock_tracks_count = 0;
    memset(mock_tracks, 0, sizeof(mock_tracks));
    uint8_t data[16];
    memset(data, 3, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("");
    mock_header_name[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_UINT8(1, mock_saved_tracks[0].slot);
}

static void test_audio_upload_autoslot_full(void)
{
    mock_tracks_count = SETTINGS_MAX_TRACKS;
    for (int i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        mock_tracks[i].slot = (uint8_t)(i + 1);
        snprintf(mock_tracks[i].file, sizeof(mock_tracks[i].file), "audio/t%d.wav", i);
    }
    uint8_t data[16];
    memset(data, 4, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("");
    mock_header_name[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "track slots full"));
}

static void test_audio_upload_validate_and_save_fail(void)
{
    uint8_t data[16];
    memset(data, 5, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "v.wav");
    mock_audio_validate_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "invalid wav"));

    reset_resp();
    set_body(data, sizeof(data));
    mock_audio_validate_ret = ESP_OK;
    mock_tracks_save_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "tracks save failed"));
}

static void test_audio_upload_short_body_and_flush_fail(void)
{
    uint8_t data[16];
    memset(data, 6, sizeof(data));
    httpd_req_t req = make_req(100); /* declares more than the body provides */
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "s.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);

    reset_resp();
    req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    mock_fsync_ret = 1;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("write failed", mock_resp_send_err_msg);
}

static void test_audio_upload_open_fail(void)
{
    /* A directory at the target path makes fopen("wb") fail on all hosts. */
    MKDIR("web_tmp/audio/adir.wav");
    uint8_t data[16];
    memset(data, 7, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "adir.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("open failed", mock_resp_send_err_msg);
}

static void test_audio_upload_no_mem(void)
{
    uint8_t data[16];
    memset(data, 8, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "m.wav");
    mock_alloc_fail_at = 0; /* xQueueCreate's calloc fails */
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static void test_audio_upload_buf_alloc_fail_and_task_fail(void)
{
    uint8_t data[16];
    memset(data, 9, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "m2.wav");
    /* Queues (2 callocs) then malloc(bufs[0]) fails. */
    mock_alloc_fail_at = 2;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));

    reset_resp();
    set_body(data, sizeof(data));
    /* Fail malloc(bufs[1]) so the already-allocated bufs[0] is freed. */
    mock_alloc_fail_at = 3;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));

    reset_resp();
    set_body(data, sizeof(data));
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    mock_task_create_ok = 1;
}

/* ---------- audio delete / category ---------- */

static void test_audio_track_delete(void)
{
    FILE *f = fopen("web_tmp/audio/del.wav", "wb");
    fputs("d", f);
    fclose(f);
    mock_tracks_count = 1;
    mock_tracks[0].slot = 2;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/del.wav");
    s_cfg.active_slot = 2;
    httpd_req_t req = make_req(0);
    set_query("slot=2");
    TEST_ASSERT_EQUAL(ESP_OK, audio_track_delete_post(&req));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/del.wav"));
    TEST_ASSERT_EQUAL_UINT8(0, s_cfg.active_slot);
    TEST_ASSERT_EQUAL_INT(1, mock_audio_stop_calls);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)mock_saved_tracks_count);
}

static void test_audio_track_delete_in_use_and_load_fail(void)
{
    FILE *f = fopen("web_tmp/audio/shared.wav", "wb");
    fputs("d", f);
    fclose(f);
    mock_tracks_count = 2;
    mock_tracks[0].slot = 1;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/shared.wav");
    mock_tracks[1].slot = 2;
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/shared.wav");
    httpd_req_t req = make_req(0);
    set_query("slot=2");
    TEST_ASSERT_EQUAL(ESP_OK, audio_track_delete_post(&req));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/shared.wav"));
    remove("web_tmp/audio/shared.wav");

    reset_resp();
    mock_tracks_load_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, audio_track_delete_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    reset_resp();
    mock_tracks_load_ret = 0;
    mock_tracks_save_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, audio_track_delete_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "tracks save failed"));
}

static void test_track_category_post(void)
{
    httpd_req_t req = make_req(0);
    set_query("slot=0&cat=0");
    TEST_ASSERT_EQUAL(ESP_OK, track_category_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("slot=21&cat=0");
    TEST_ASSERT_EQUAL(ESP_OK, track_category_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("slot=1&cat=2");
    TEST_ASSERT_EQUAL(ESP_OK, track_category_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("slot=1&cat=1");
    TEST_ASSERT_EQUAL(ESP_OK, track_category_post(&req));
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_EFFECTS, s_track_cat[0]);
    TEST_ASSERT_EQUAL_INT(1, mock_cats_save_calls);

    reset_resp();
    set_query("slot=1&cat=0");
    TEST_ASSERT_EQUAL(ESP_OK, track_category_post(&req));
    TEST_ASSERT_EQUAL_UINT8(SETTINGS_TRACK_CAT_ENGINE, s_track_cat[0]);
}

/* ---------- CV handlers ---------- */

static void test_cv_read_and_all(void)
{
    httpd_req_t req = make_req(0);
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, cv_read_get(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("index=0");
    TEST_ASSERT_EQUAL(ESP_OK, cv_read_get(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_cv[5] = 42;
    set_query("index=5");
    TEST_ASSERT_EQUAL(ESP_OK, cv_read_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"value\":42"));

    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, cv_all_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"count\":512"));
}

static void test_cv_write_post(void)
{
    httpd_req_t req = make_req(0);
    set_query("index=0&value=1");
    TEST_ASSERT_EQUAL(ESP_OK, cv_write_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("index=5");
    TEST_ASSERT_EQUAL(ESP_OK, cv_write_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_cv_write_ret = ESP_FAIL;
    set_query("index=5&value=1");
    TEST_ASSERT_EQUAL(ESP_OK, cv_write_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));

    reset_resp();
    mock_cv_write_ret = 0;
    set_query("index=29&value=10&commit=1");
    TEST_ASSERT_EQUAL(ESP_OK, cv_write_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_cv_commit_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_dcc_reload_config_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    /* CV63 aliases the master volume: applied live and persisted (REV-SET1). */
    reset_resp();
    mock_audio_last_volume = 0;
    mock_settings_save_deferred_calls = 0;
    set_query("index=63&value=255");
    TEST_ASSERT_EQUAL(ESP_OK, cv_write_post(&req));
    TEST_ASSERT_EQUAL_UINT8(100, mock_audio_last_volume);
    TEST_ASSERT_EQUAL_INT(1, mock_settings_save_deferred_calls);
    TEST_ASSERT_EQUAL_UINT8(100, s_cfg.master_volume);

    /* Clamp: a >100 % request cannot exceed the audio range. */
    reset_resp();
    s_cfg.master_volume = 0;
    web_master_volume_changed(200);
    TEST_ASSERT_EQUAL_UINT8(100, s_cfg.master_volume);
    TEST_ASSERT_EQUAL_UINT8(100, mock_audio_last_volume);
}

/* ---------- AUX config ---------- */

static void test_aux_cfg_get_post(void)
{
    httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"aux\":["));

    set_query("ch=9&level=10&effect=0");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("ch=1&level=101&effect=0");
    TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("ch=1&level=10&effect=7");
    TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_aux_save_ret = ESP_FAIL;
    set_query("ch=1&level=50&effect=0");
    TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save failed"));

    reset_resp();
    mock_aux_save_ret = 0;
    TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_post(&req));
    TEST_ASSERT_EQUAL_INT(2, mock_auxio_config_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
}

static void test_aux_cfg_effect_periods(void)
{
    httpd_req_t req = make_req(0);
    for (int fx = 0; fx < AUXIO_EFFECT_COUNT; ++fx) {
        char q[48];
        snprintf(q, sizeof(q), "ch=0&level=100&effect=%d", fx);
        set_query(q);
        reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_post(&req));
        TEST_ASSERT_EQUAL_INT(fx, mock_auxio_last_mode);
    }
}

/* ---------- function map handlers ---------- */

static void test_func_map_get_post(void)
{
    httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, func_map_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"map\":["));

    const char *bad[] = {
        "fn=11&a=1&b=0&aux=0&dir=0&speed=0",
        "fn=1&a=21&b=0&aux=0&dir=0&speed=0",
        "fn=1&a=1&b=21&aux=0&dir=0&speed=0",
        "fn=1&a=1&b=0&aux=512&dir=0&speed=0",
        "fn=1&a=1&b=0&aux=0&dir=3&speed=0",
        "fn=1&a=1&b=0&aux=0&dir=0&speed=3",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset_resp();
        set_query(bad[i]);
        TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    }

    reset_resp();
    mock_map_save_ret = ESP_FAIL;
    set_query("fn=1&a=1&b=0&aux=1&dir=1&speed=1");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save failed"));

    reset_resp();
    mock_map_save_ret = 0;
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
}

/* ---- canonical function bindings (R2) ---- */

static void test_func_bind_load_and_desired(void)
{
    /* load-success path from the store */
    memset(mock_binds, 0, sizeof(mock_binds));
    mock_binds[0].used = 1;
    mock_binds[0].fn = 1;
    mock_binds[0].target_type = FUNC_TARGET_OUTPUT;
    mock_binds[0].target_id = 2;
    mock_binds_count = 1;
    mock_bind_load_ret = ESP_OK;
    func_bind_load();
    TEST_ASSERT_EQUAL_UINT32(1, s_func_bind_count);

    /* binding path drives the output mask; fn_on gates it */
    s_motion_initialized = true;
    s_motion_forward = true;
    s_motion_speed = 1;
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(1U << 2), func_desired_locked(1, true));
    TEST_ASSERT_EQUAL_UINT16(0, func_desired_locked(1, false));
    TEST_ASSERT_EQUAL_UINT16(0, func_desired_locked(0, true));
}

static void test_func_bind_get_view(void)
{
    httpd_req_t req = make_req(0);
    s_func_bind_count = 1;
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind[0].used = 1;
    s_func_bind[0].fn = 2;
    s_func_bind[0].target_type = FUNC_TARGET_SOUND;
    s_func_bind[0].target_id = 5;
    s_func_bind[0].short_ms = 400;

    reset_resp();
    set_query("view=bind");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"binds\":["));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"id\":5"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"short_ms\":400"));

    reset_resp();
    set_query("view=matrix");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"count\":1"));

    reset_resp();
    mock_alloc_fail_at = 0;
    TEST_ASSERT_EQUAL(ESP_OK, func_map_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "oom"));
}

static void test_func_map_post_binding(void)
{
    httpd_req_t req = make_req(0);

    reset_resp();
    set_query("bind=1&fn=2&type=2&id=5&dir=1&state=2&mode=3&flags=1&short=7"
              "&short_ms=400&min_ms=150&fade_ms=80");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_EQUAL_UINT32(1, s_func_bind_count);
    TEST_ASSERT_EQUAL_UINT8(2, s_func_bind[0].fn);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_SOUND, s_func_bind[0].target_type);
    TEST_ASSERT_EQUAL_UINT8(5, s_func_bind[0].target_id);
    TEST_ASSERT_EQUAL_UINT8(1, s_func_bind[0].dir);
    TEST_ASSERT_EQUAL_UINT8(2, s_func_bind[0].state);
    TEST_ASSERT_EQUAL_UINT8(3, s_func_bind[0].mode);
    TEST_ASSERT_EQUAL_UINT8(1, s_func_bind[0].flags);
    TEST_ASSERT_EQUAL_UINT8(7, s_func_bind[0].short_table);
    TEST_ASSERT_EQUAL_UINT16(400, s_func_bind[0].short_ms);
    TEST_ASSERT_EQUAL_UINT16(150, s_func_bind[0].min_ms);
    TEST_ASSERT_EQUAL_UINT16(80, s_func_bind[0].fade_ms);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    reset_resp();
    set_query("bind=1&fn=1&type=1&id=2"); /* optional params default */
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_EQUAL_UINT32(2, s_func_bind_count);

    const char *bad[] = {
        "bind=1&fn=99&type=1&id=0",
        "bind=1&fn=1&type=0&id=0",
        "bind=1&fn=1&type=9&id=0",
        "bind=1&fn=1&type=1",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset_resp();
        set_query(bad[i]);
        TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    }

    reset_resp();
    mock_bind_save_ret = ESP_FAIL;
    set_query("bind=1&fn=3&type=2&id=1");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save failed"));
    mock_bind_save_ret = 0;

    reset_resp();
    set_query("bind=1&remove=1&idx=9");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_bind_save_ret = ESP_FAIL;
    set_query("bind=1&remove=1&idx=0");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save failed"));
    mock_bind_save_ret = 0;

    reset_resp();
    set_query("bind=1&remove=1&idx=0");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_UINT32(1, s_func_bind_count);

    /* Full store: add fails. */
    reset_resp();
    s_func_bind_count = FUNC_BIND_MAX;
    set_query("bind=1&fn=3&type=2&id=1");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save failed"));

    /* Empty store: direct remove guard. */
    s_func_bind_count = 0;
    TEST_ASSERT_FALSE(web_func_bind_remove(0));
}

static void test_sound_rest_handlers(void)
{
    httpd_req_t req = make_req(0);

    mock_sound_status.type = SOUND_SCHEME_DIESEL;
    mock_sound_status.enabled = true;
    mock_sound_status.engine = true;
    mock_sound_status.table = 5;
    mock_sound_status.phase = 2;
    mock_sound_status.speed = 100;
    mock_sound_status.forward = true;
    snprintf(mock_sound_status.name, sizeof(mock_sound_status.name), "my\"sc");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, sound_state_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"type\":2"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "my\\\"sc"));

    mock_sound_scheme.type = SOUND_SCHEME_STEAM;
    mock_sound_scheme.engine.engine_start_fn = 3;
    mock_sound_scheme.tables[4].used = true;
    snprintf(mock_sound_scheme.tables[4].name, SOUND_NAME_MAX, "D1");
    mock_sound_scheme.extras[0].fn = 7;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"tables\":["));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"extras\":["));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "D1"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"drive\":["));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"brake\":{\"max\":"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"init\":\""));

    reset_resp();
    mock_alloc_fail_at = 0;
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "oom"));

    reset_resp();
    set_query("type=1&start_fn=4&sync=1&flags=3&start=2&stop=3&shutdown=4&cyl_min=1&cyl_max=3&cyl_inc=1");
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_post_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_UINT8(1, mock_sound_scheme.type);
    TEST_ASSERT_EQUAL_UINT8(4, mock_sound_scheme.engine.engine_start_fn);
    TEST_ASSERT_TRUE(mock_sound_scheme.engine.sync_motion);
    /* `flags` is mirrored to CV30 so the engine (which reads CV30) sees it. */
    TEST_ASSERT_EQUAL_UINT16(30, mock_cv_last_index);
    TEST_ASSERT_EQUAL_UINT8(3, mock_cv[30]);
    TEST_ASSERT_EQUAL_INT(1, mock_cv_commit_deferred_calls);

    /* Engine graph arrays + brake are settable through the scheme POST. */
    reset_resp();
    set_query("drive0=5&drive4=9&accel2=3&coast1=4&brake_max=20&brake_min=80");
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_post_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_UINT8(5, mock_sound_scheme.engine.drive[0]);
    TEST_ASSERT_EQUAL_UINT8(9, mock_sound_scheme.engine.drive[4]);
    TEST_ASSERT_EQUAL_UINT8(3, mock_sound_scheme.engine.accel[2]);
    TEST_ASSERT_EQUAL_UINT8(4, mock_sound_scheme.engine.coast[1]);
    TEST_ASSERT_EQUAL_UINT8(20, mock_sound_scheme.brake.max_on_speed);
    TEST_ASSERT_EQUAL_UINT8(80, mock_sound_scheme.brake.min_brake_speed);

    /* An out-of-range scheme type is rejected before touching RAM. */
    reset_resp();
    set_query("type=9");
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_post_handler(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_sound_scheme_save_ret = ESP_FAIL;
    set_query("type=2");
    TEST_ASSERT_EQUAL(ESP_OK, sound_scheme_post_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save"));
    mock_sound_scheme_save_ret = 0;

    reset_resp();
    set_query("i=5&used=1&name=Drive&min=10&max=20&rate=32&min_plays=1&max_plays=3&end=6"
              "&nacc=7&ndec=8&init=i.wav&loop=l.wav&endf=e.wav");
    TEST_ASSERT_EQUAL(ESP_OK, sound_table_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"i\":5"));
    TEST_ASSERT_EQUAL_UINT8(10, mock_sound_scheme.tables[5].min_speed);
    TEST_ASSERT_EQUAL_STRING("Drive", mock_sound_scheme.tables[5].name);
    TEST_ASSERT_EQUAL_STRING("l.wav", mock_sound_scheme.tables[5].loop[0].file);

    reset_resp();
    set_query("i=0");
    TEST_ASSERT_EQUAL(ESP_OK, sound_table_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    reset_resp();
    set_query("i=40");
    TEST_ASSERT_EQUAL(ESP_OK, sound_table_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_sound_scheme_save_ret = ESP_FAIL;
    set_query("i=5&used=1");
    TEST_ASSERT_EQUAL(ESP_OK, sound_table_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save"));
    mock_sound_scheme_save_ret = 0;

    reset_resp();
    set_query("i=3&table=5&fn=7&dir=1&state=2&mode=3&vol=80&rmin=100&rmax=900");
    TEST_ASSERT_EQUAL(ESP_OK, sound_extra_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"i\":3"));
    TEST_ASSERT_EQUAL_UINT8(5, mock_sound_scheme.extras[3].table);
    TEST_ASSERT_EQUAL_UINT16(900, mock_sound_scheme.extras[3].random_max_ms);

    reset_resp();
    set_query("i=99");
    TEST_ASSERT_EQUAL(ESP_OK, sound_extra_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_sound_scheme_save_ret = ESP_FAIL;
    set_query("i=3&fn=7");
    TEST_ASSERT_EQUAL(ESP_OK, sound_extra_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save"));
    mock_sound_scheme_save_ret = 0;

    reset_resp();
    mock_sound_lint_ret = 2;
    TEST_ASSERT_EQUAL(ESP_OK, sound_lint_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"problems\":2"));
}

static void test_sound_project_handlers(void)
{
    httpd_req_t req = make_req(0);

    snprintf(mock_sound_active_name, sizeof(mock_sound_active_name), "diesel");
    mock_sound_scheme_list_count = 2;
    snprintf(mock_sound_scheme_list_names[0], SOUND_FILE_MAX, "diesel");
    snprintf(mock_sound_scheme_list_names[1], SOUND_FILE_MAX, "steam\"x");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, sound_projects_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"active\":\"diesel\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"name\":\"diesel\",\"active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"name\":\"steam\\\"x\",\"active\":false"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"count\":2"));

    reset_resp();
    mock_alloc_calls = 0;
    mock_alloc_fail_at = 0;
    TEST_ASSERT_EQUAL(ESP_OK, sound_projects_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "oom"));

    reset_resp();
    set_query("create=my_scheme");
    TEST_ASSERT_EQUAL(ESP_OK, sound_project_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_INT(1, mock_sound_scheme_create_calls);
    TEST_ASSERT_EQUAL_STRING("my_scheme", mock_sound_last_name);
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_DIESEL, mock_sound_last_type);

    reset_resp();
    set_query("create=steam1&type=3");
    TEST_ASSERT_EQUAL(ESP_OK, sound_project_post(&req));
    TEST_ASSERT_EQUAL_UINT8(SOUND_SCHEME_STEAM, mock_sound_last_type);

    reset_resp();
    set_query("activate=steam1");
    TEST_ASSERT_EQUAL(ESP_OK, sound_project_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_sound_load_scheme_calls);
    TEST_ASSERT_EQUAL_STRING("steam1", mock_sound_last_name);

    reset_resp();
    set_query("delete=steam1");
    TEST_ASSERT_EQUAL(ESP_OK, sound_project_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_sound_scheme_delete_calls);

    reset_resp();
    mock_sound_scheme_create_ret = ESP_FAIL;
    set_query("create=x");
    TEST_ASSERT_EQUAL(ESP_OK, sound_project_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
    mock_sound_scheme_create_ret = 0;

    reset_resp();
    set_query("other=1");
    TEST_ASSERT_EQUAL(ESP_OK, sound_project_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    mock_sound_export_len = 16;
    mock_sound_export_byte = 0x5A;
    reset_resp();
    set_query("name=diesel");
    TEST_ASSERT_EQUAL(ESP_OK, sound_download_get(&req));
    TEST_ASSERT_EQUAL_STRING("application/octet-stream", mock_resp_type);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "Content-Disposition"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "diesel.mds"));
    TEST_ASSERT_EQUAL_UINT32(16, mock_resp_body_len);
    TEST_ASSERT_EQUAL_UINT8(0x5A, (uint8_t)mock_resp_body[0]);

    reset_resp();
    mock_sound_scheme_export_ret = ESP_ERR_NOT_FOUND;
    TEST_ASSERT_EQUAL(ESP_OK, sound_download_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
    mock_sound_scheme_export_ret = 0;

    reset_resp();
    set_query("other=1");
    TEST_ASSERT_EQUAL(ESP_OK, sound_download_get(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_alloc_calls = 0;
    mock_alloc_fail_at = 0;
    set_query("name=diesel");
    TEST_ASSERT_EQUAL(ESP_OK, sound_download_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "oom"));
}

static void test_sound_upload_handler(void)
{
    static uint8_t big[SOUND_STORE_MAX_BYTES];
    memset(big, 0x11, sizeof(big));
    uint8_t shorty[8];
    memset(shorty, 0x22, sizeof(shorty));
    httpd_req_t req = make_req(0);

    mock_storage_is_mounted = 0;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);
    mock_storage_is_mounted = 1;

    reset_resp();
    req = make_req(8);
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    req = make_req(SOUND_STORE_MAX_BYTES);
    set_query("other=1");
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    /* Short body: the recv loop ends before content_len -> fail. */
    reset_resp();
    set_body(shorty, sizeof(shorty));
    set_query("name=abc");
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));

    /* Success; activation defaults on. */
    reset_resp();
    set_body(big, sizeof(big));
    set_query("name=abc");
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_INT(1, mock_sound_scheme_import_calls);
    TEST_ASSERT_EQUAL_STRING("abc", mock_sound_last_name);
    TEST_ASSERT_EQUAL_UINT32(SOUND_STORE_MAX_BYTES, mock_sound_import_last_len);
    TEST_ASSERT_TRUE(mock_sound_import_last_activate);

    /* activate=0 keeps the current scheme. */
    reset_resp();
    set_body(big, sizeof(big));
    set_query("name=abc&activate=0");
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_FALSE(mock_sound_import_last_activate);

    reset_resp();
    mock_sound_scheme_import_ret = ESP_FAIL;
    set_body(big, sizeof(big));
    set_query("name=abc");
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
    mock_sound_scheme_import_ret = 0;

    reset_resp();
    mock_alloc_calls = 0;
    mock_alloc_fail_at = 0;
    set_query("name=abc");
    TEST_ASSERT_EQUAL(ESP_OK, sound_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "oom"));
}

static void test_web_func_map_set_preserves_sound_bindings(void)
{
    /* A canonical SOUND binding must survive a legacy map edit: the legacy
     * POST only rebuilds OUTPUT/SLOT records. */
    s_func_bind_count = 1;
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind[0].used = 1;
    s_func_bind[0].fn = 3;
    s_func_bind[0].target_type = FUNC_TARGET_SOUND;
    s_func_bind[0].target_id = 5;
    s_func_bind[0].mode = SOUND_MODE_SHORT_LONG;

    TEST_ASSERT_TRUE(web_func_map_set(2, 0, 0, 0x0004u, SETTINGS_FUNC_DIR_FWD,
                                      SETTINGS_FUNC_SPD_MOVING));
    TEST_ASSERT_EQUAL_UINT32(2, s_func_bind_count); /* 1 sound + 1 AUX output */

    int found = -1;
    for (size_t i = 0; i < s_func_bind_count; ++i) {
        if (s_func_bind[i].target_type == FUNC_TARGET_SOUND) {
            found = (int)i;
        }
    }
    TEST_ASSERT_TRUE(found >= 0);
    TEST_ASSERT_EQUAL_UINT8(5, s_func_bind[found].target_id);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_OUTPUT, s_func_bind[0].target_type);
}

/* ---------- small branch gaps ---------- */

static void test_utf8_four_byte_and_outputs_init(void)
{
    web_log_event("u4", "\xf0\x9f\x98\x80");
    TEST_ASSERT_EQUAL_UINT32(1, s_evlog_seq);
    TEST_ASSERT_EQUAL_STRING("\xf0\x9f\x98\x80", s_evlog[0].text);

    mock_auxio_init_ret = ESP_OK;
    outputs_init();
    mock_auxio_init_ret = ESP_FAIL;
    outputs_init();
    TEST_PASS();
}

static void test_captive_handler(void)
{
    httpd_req_t req = make_req(0);
    snprintf(s_ap_ip, sizeof(s_ap_ip), "10.1.2.3");
    TEST_ASSERT_EQUAL(ESP_OK, captive_handler(&req, HTTPD_404_NOT_FOUND));
    TEST_ASSERT_EQUAL_STRING("302 Found", mock_resp_status);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "10.1.2.3"));
}

static void test_start_dns_hijack(void)
{
    mock_task_create_ok = 1;
    start_dns_hijack();
    mock_task_create_ok = 0;
    start_dns_hijack();
    mock_task_create_ok = 1;
    TEST_PASS();
}

static void test_pipe_writer_idle_continue(void)
{
    pipe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.f = fopen("web_tmp/audio/pw.bin", "wb");
    ctx.write_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(pipe_item_t));
    ctx.free_q = xQueueCreate(PIPE_NUM_BUFS, sizeof(int));
    ctx.done_sem = xSemaphoreCreateBinary();
    ctx.write_err = ESP_OK;
    ctx.abort = false;
    s_pipe_iter_cap = 1; /* empty queue + cap -> one idle continue */
    pipe_writer(&ctx);
    fclose(ctx.f);
    vQueueDelete(ctx.write_q);
    vQueueDelete(ctx.free_q);
    vSemaphoreDelete(ctx.done_sem);
    TEST_PASS();
}

static void test_pipe_upload_freeq_send_fail(void)
{
    /* Seeding the free queue fails -> the first receive fails too. */
    uint8_t data[16];
    memset(data, 1, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "pf.wav");
    mock_queue_send_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static void test_pipe_upload_write_send_fail(void)
{
    uint8_t data[16];
    memset(data, 2, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "pw2.wav");
    mock_queue_send_fail_after = PIPE_NUM_BUFS; /* seeding ok, first write fails */
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static uint8_t pipe_big[7 * 8192];

static void test_pipe_upload_freeq_exhausted(void)
{
    memset(pipe_big, 3, sizeof(pipe_big));
    httpd_req_t req = make_req(sizeof(pipe_big));
    set_body(pipe_big, sizeof(pipe_big));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "pb.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static void test_pipe_upload_writer_error(void)
{
    uint8_t data[16];
    memset(data, 4, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "pe.wav");
    s_pipe_write_err_inject = true;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static void test_aux_effect_valid_mode(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 1;
    set_query("ch=1&on=1&mode=2");
    TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
    TEST_ASSERT_EQUAL_INT(AUXIO_EFFECT_MARS, mock_auxio_last_mode);
}

/* ---------- wifi / device / storage ---------- */

static void test_wifi_get(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.wifi_mode = 1;
    snprintf(s_cfg.ap_ssid, sizeof(s_cfg.ap_ssid), "AP");
    snprintf(s_cfg.ap_password, sizeof(s_cfg.ap_password), "secret123");
    snprintf(s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid), "STA");
    snprintf(s_cfg.sta_password, sizeof(s_cfg.sta_password), "stapass1");
    s_cfg.port = 80;
    s_cfg.auto_off_min = 5;
    s_cfg.hold = 0;
    snprintf(s_cfg.ap_ip, sizeof(s_cfg.ap_ip), "10.0.0.1");
    snprintf(s_sta_ip, sizeof(s_sta_ip), "10.0.0.9");
    s_wifi_started = true;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ap_password\":\"********\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"status\":\"ap_running\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"sta_ip\":\"10.0.0.9\""));

    reset_resp();
    s_cfg.ap_password[0] = '\0';
    s_cfg.sta_password[0] = '\0';
    s_wifi_started = false;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"status\":\"off\""));
}

static void test_wifi_post(void)
{
    httpd_req_t req = make_req(0);
    set_query("ap_ssid=NewAP&ap_password=longenough&auto_off_min=7&ap_ip=10.5.5.5");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_post(&req));
    TEST_ASSERT_EQUAL_STRING("NewAP", s_cfg.ap_ssid);
    TEST_ASSERT_EQUAL_STRING("longenough", s_cfg.ap_password);
    TEST_ASSERT_EQUAL_STRING("10.5.5.5", s_cfg.ap_ip);
    TEST_ASSERT_EQUAL_UINT8(7, s_cfg.auto_off_min);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "reboot"));

    reset_resp();
    set_query("ap_password=short");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "ap password must be >=8 chars"));

    reset_resp();
    set_query("ap_ip=bogus");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "invalid ap ip"));

    reset_resp();
    set_query("ap_ssid=&ap_password_clear=1");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_post(&req));
    TEST_ASSERT_EQUAL_STRING("", s_cfg.ap_password);
    TEST_ASSERT_EQUAL_STRING("NewAP", s_cfg.ap_ssid); /* empty value ignored */
}

static void test_wifi_and_factory_reset(void)
{
    httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, wifi_reset_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_settings_save_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_calls);

    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, factory_reset_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_factory_reset_calls);
    TEST_ASSERT_EQUAL_INT(2, mock_esp_restart_calls);
}

static void test_device_get_post(void)
{
    httpd_req_t req = make_req(0);
    snprintf(s_cfg.device_name, sizeof(s_cfg.device_name), "DEC");
    mock_timer_now_us = 3661000000LL; /* 1 h 1 min 1 s */
    TEST_ASSERT_EQUAL(ESP_OK, device_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"name\":\"DEC\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"uptime\":3661"));
    mock_timer_now_us = 0;

    set_query("name=NewName");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, device_post(&req));
    TEST_ASSERT_EQUAL_STRING("NewName", s_cfg.device_name);
    TEST_ASSERT_EQUAL_INT(1, mock_settings_save_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"name\":\"NewName\""));

    set_query("");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, device_post(&req));
    TEST_ASSERT_EQUAL_STRING("NewName", s_cfg.device_name);
}

static void test_storage_get(void)
{
    httpd_req_t req = make_req(0);
    mock_storage_free = 1234;
    TEST_ASSERT_EQUAL(ESP_OK, storage_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"free\":1234"));

    reset_resp();
    mock_storage_free = ((uint64_t)5u << 32) | 9u;
    TEST_ASSERT_EQUAL(ESP_OK, storage_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"free\":5000000009"));

    reset_resp();
    mock_storage_free_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, storage_get(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);

    reset_resp();
    mock_storage_free_ret = 0;
    mock_storage_get_time_advance_us = 20000; /* force the slow warning */
    TEST_ASSERT_EQUAL(ESP_OK, storage_get(&req));
}

/* ---------- logging / misc handlers ---------- */

static void test_task_inputs_and_clientlog(void)
{
    httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, task_inputs_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"actions\":[1,2,3]"));

    reset_resp();
    set_query("m=client+error");
    TEST_ASSERT_EQUAL(ESP_OK, clientlog_get(&req));
    TEST_ASSERT_EQUAL_STRING("204 No Content", mock_resp_status);

    reset_resp();
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, clientlog_get(&req));
}

static void test_log_get(void)
{
    httpd_req_t req = make_req(0);
    web_log_event("T1", "first");
    web_log_event("T2", "%s", "second");
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, log_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"tag\":\"T2\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "first"));

    reset_resp();
    char q[32];
    snprintf(q, sizeof(q), "since=%lu", (unsigned long)s_evlog_seq);
    set_query(q);
    TEST_ASSERT_EQUAL(ESP_OK, log_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"entries\":[]"));

    /* Fill the journal with long text so the JSON buffer closes early. */
    reset_resp();
    char longtext[120];
    memset(longtext, 'x', 90);
    longtext[90] = '\0';
    for (int i = 0; i < WEB_EVLOG_MAX; ++i) {
        web_log_event("LONGTAG", "%s", longtext);
    }
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, log_get(&req));

    /* No mutex: the lock is skipped. */
    reset_resp();
    s_evlog_mutex = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, log_get(&req));
}

static void test_web_fs_busy(void)
{
    s_up_active = false;
    TEST_ASSERT_FALSE(web_fs_busy());
    s_up_active = true;
    TEST_ASSERT_TRUE(web_fs_busy());
    s_up_active = false;
}

static void test_progress_handler(void)
{
    httpd_req_t req = make_req(0);
    s_up_slot = 7;
    s_up_total = 100;
    s_up_received = 50;
    s_up_active = true;
    s_up_ota = true;
    TEST_ASSERT_EQUAL(ESP_OK, progress_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"slot\":7"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"received\":50"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ota\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "Access-Control-Allow-Origin"));
    s_up_ota = false;
}

/* ---------- server startup ---------- */

static void test_register_route_failure(void)
{
    mock_httpd_register_err = ESP_FAIL;
    register_route("/x", HTTP_GET, root_handler);
    mock_httpd_register_err = 0;
    TEST_PASS();
}

static void test_start_servers(void)
{
    mock_httpd_start_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, start_http_server());
    TEST_ASSERT_EQUAL(ESP_FAIL, start_progress_server());

    mock_httpd_start_err = 0;
    TEST_ASSERT_EQUAL(ESP_OK, start_http_server());
    TEST_ASSERT_TRUE(mock_route_count > 10);
    TEST_ASSERT_NOT_NULL(mock_err_handler);
    TEST_ASSERT_EQUAL(ESP_OK, start_progress_server());
}

/* ---------- web_init ---------- */

static void test_web_init_wifi_off(void)
{
    mock_settings_cfg.wifi_mode = 0;
    TEST_ASSERT_EQUAL(ESP_OK, web_init());
    TEST_ASSERT_NOT_NULL(s_evlog_mutex);
    TEST_ASSERT_NOT_NULL(s_func_mutex);
    TEST_ASSERT_EQUAL_INT(SETTINGS_AUX_COUNT, mock_auxio_config_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_audio_set_volume_calls);
    TEST_ASSERT_TRUE(s_track_cat_loaded);
}

static void test_web_init_clamps_aux(void)
{
    mock_settings_cfg.wifi_mode = 0;
    for (int i = 0; i < SETTINGS_AUX_COUNT; ++i) {
        mock_aux[i].level = 200;
        mock_aux[i].effect = 99;
    }
    TEST_ASSERT_EQUAL(ESP_OK, web_init());
    for (int i = 0; i < SETTINGS_AUX_COUNT; ++i) {
        TEST_ASSERT_EQUAL_UINT8(100, s_aux_cfg[i].level);
        TEST_ASSERT_EQUAL_UINT8(AUXIO_EFFECT_STEADY, s_aux_cfg[i].effect);
    }
}

static void test_web_init_wifi_on(void)
{
    mock_settings_cfg = make_wifi_cfg();
    mock_settings_cfg.auto_off_min = 5;
    mock_settings_cfg.master_volume = 33;
    TEST_ASSERT_EQUAL(ESP_OK, web_init());
    TEST_ASSERT_TRUE(s_wifi_started);
    TEST_ASSERT_TRUE(mock_route_count > 10);
    TEST_ASSERT_EQUAL_INT(33, mock_audio_last_volume);
}

static void test_web_init_server_fail(void)
{
    mock_settings_cfg = make_wifi_cfg();
    mock_httpd_start_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, web_init());
}

/* ---------- OTA ---------- */

static size_t build_container(uint8_t *out, uint32_t fw_len, uint32_t n_files,
                              uint16_t name_len, uint16_t label_len, uint32_t data_len,
                              const char *name, const char *label)
{
    size_t o = 0;
    memcpy(out, OTA_CONTAINER_MAGIC, OTA_CONTAINER_MAGIC_LEN);
    o = OTA_CONTAINER_MAGIC_LEN;
    out[o++] = (uint8_t)(fw_len);
    out[o++] = (uint8_t)(fw_len >> 8);
    out[o++] = (uint8_t)(fw_len >> 16);
    out[o++] = (uint8_t)(fw_len >> 24);
    out[o++] = (uint8_t)(n_files);
    out[o++] = (uint8_t)(n_files >> 8);
    out[o++] = (uint8_t)(n_files >> 16);
    out[o++] = (uint8_t)(n_files >> 24);
    for (uint32_t i = 0; i < fw_len; ++i) {
        out[o++] = (uint8_t)(0xA0u + i);
    }
    out[o++] = (uint8_t)(name_len);
    out[o++] = (uint8_t)(name_len >> 8);
    out[o++] = (uint8_t)(label_len);
    out[o++] = (uint8_t)(label_len >> 8);
    out[o++] = (uint8_t)(data_len);
    out[o++] = (uint8_t)(data_len >> 8);
    out[o++] = (uint8_t)(data_len >> 16);
    out[o++] = (uint8_t)(data_len >> 24);
    if (name != NULL) {
        size_t n = strlen(name);
        memcpy(out + o, name, n);
        o += n;
    }
    if (label != NULL) {
        size_t n = strlen(label);
        memcpy(out + o, label, n);
        o += n;
    }
    for (uint32_t i = 0; i < data_len; ++i) {
        out[o++] = (uint8_t)(i & 0x7fU);
    }
    return o;
}

static void test_ota_safe_name_and_slot(void)
{
    char out[80];
    ota_safe_name("dir/sub/My File.WAV", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("My_File.WAV", out);
    ota_safe_name("name", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("name.wav", out);
    ota_safe_name("slot9.wav", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("slot9.wav", out);
    ota_safe_name("a/b", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("b.wav", out);
    char tiny[3] = { 'x', 'x', 'x' };
    ota_safe_name("abc", tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_CHAR('\0', tiny[0]);
    ota_safe_name("abc", tiny, 0);
    char cyr[16];
    ota_safe_name("\xd0\xb0\xd0\xb1", cyr, sizeof(cyr));
    TEST_ASSERT_NOT_NULL(strstr(cyr, ".wav"));

    TEST_ASSERT_EQUAL_INT(3, ota_slot_from_name("slot3.wav"));
    TEST_ASSERT_EQUAL_INT(0, ota_slot_from_name("slot0.wav"));
    TEST_ASSERT_EQUAL_INT(0, ota_slot_from_name("slot21.wav"));
    TEST_ASSERT_EQUAL_INT(0, ota_slot_from_name("slotx.wav"));
    TEST_ASSERT_EQUAL_INT(0, ota_slot_from_name("other.wav"));
    TEST_ASSERT_EQUAL_INT(0, ota_slot_from_name("slot3"));
}

static void test_ota_stream_read(void)
{
    uint8_t data[10];
    memset(data, 5, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    ota_stream_t st;
    memset(&st, 0, sizeof(st));
    st.req = &req;
    st.remaining = (int)sizeof(data);
    uint8_t dst[16];
    TEST_ASSERT_EQUAL_INT(4, ota_stream_read(&st, dst, 4));
    int r = ota_stream_read(&st, dst, 10);
    TEST_ASSERT_EQUAL_INT(6, r);

    /* Idle polling then failure when the body is short. */
    ota_stream_t st2;
    memset(&st2, 0, sizeof(st2));
    st2.req = &req;
    st2.remaining = 5;
    mock_httpd_body_pos = mock_httpd_body_len; /* nothing left to serve */
    TEST_ASSERT_EQUAL_INT(-1, ota_stream_read(&st2, dst, 4));

    ota_stream_t st3;
    memset(&st3, 0, sizeof(st3));
    st3.req = &req;
    st3.remaining = 5;
    mock_recv_fail = 1;
    TEST_ASSERT_EQUAL_INT(-1, ota_stream_read(&st3, dst, 4));
}

static void test_ota_guards(void)
{
    httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    req = make_req(8u * 1024u * 1024u + 1u);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    mock_ota_partition_absent = 1;
    req = make_req(64);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);

    reset_resp();
    mock_ota_partition_absent = 0;
    mock_alloc_fail_at = 0;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("no mem", mock_resp_send_err_msg);

    reset_resp();
    mock_alloc_fail_at = 1;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("no mem", mock_resp_send_err_msg);

    /* Short header. */
    reset_resp();
    uint8_t tiny[10];
    memset(tiny, 0, sizeof(tiny));
    req = make_req(sizeof(tiny));
    set_body(tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("short header", mock_resp_send_err_msg);

    /* Firmware larger than the partition. */
    reset_resp();
    uint8_t body[64];
    memset(body, 0, sizeof(body));
    req = make_req(sizeof(body));
    set_body(body, sizeof(body));
    mock_ota_next_size = 32;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("firmware too large", mock_resp_send_err_msg);
}

static void test_ota_plain_success_and_failures(void)
{
    uint8_t body[64];
    memset(body, 0x42, sizeof(body));
    httpd_req_t req = make_req(sizeof(body));
    set_body(body, sizeof(body));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"bytes\":64"));
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_calls);
    TEST_ASSERT_FALSE(s_up_active);

    /* ota_begin failure. */
    reset_resp();
    set_body(body, sizeof(body));
    mock_ota_begin_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("ota begin failed", mock_resp_send_err_msg);

    /* First (header) write failure. */
    reset_resp();
    mock_ota_begin_err = 0;
    mock_ota_write_err = ESP_FAIL;
    set_body(body, sizeof(body));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("ota write failed", mock_resp_send_err_msg);

    /* Failure inside the firmware copy loop. */
    reset_resp();
    mock_ota_write_err = 0;
    mock_ota_write_fail_after = 1;
    set_body(body, sizeof(body));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("ota write failed", mock_resp_send_err_msg);

    /* ota_end failure. */
    reset_resp();
    mock_ota_write_fail_after = -1;
    mock_ota_end_err = ESP_FAIL;
    set_body(body, sizeof(body));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("ota end failed", mock_resp_send_err_msg);

    /* set_boot failure. */
    reset_resp();
    mock_ota_end_err = 0;
    mock_ota_set_boot_err = ESP_FAIL;
    set_body(body, sizeof(body));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("ota boot failed", mock_resp_send_err_msg);
}

static uint8_t ota_combined[4096];

static void test_ota_combined_success(void)
{
    memset(mock_tracks, 0, sizeof(mock_tracks));
    mock_tracks_count = 0;
    const char *name = "slot1.wav";
    const char *label = "My Sound";
    size_t n = build_container(ota_combined, 32, 1, (uint16_t)strlen(name),
                               (uint16_t)strlen(label), 20, name, label);
    httpd_req_t req = make_req(n);
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":1"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/slot1.wav"));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)mock_saved_tracks_count);
    TEST_ASSERT_EQUAL_UINT8(1, mock_saved_tracks[0].slot);
    TEST_ASSERT_EQUAL_STRING("audio/slot1.wav", mock_saved_tracks[0].file);
    TEST_ASSERT_EQUAL_STRING("My Sound", mock_saved_tracks[0].label);
    remove("web_tmp/audio/slot1.wav");
}

/* A sound in a combined image that is not a valid WAV is discarded (and not
 * bound to a track). */
static void test_ota_combined_invalid_wav(void)
{
    memset(mock_tracks, 0, sizeof(mock_tracks));
    mock_tracks_count = 0;
    const char *name = "bad.wav";
    size_t n = build_container(ota_combined, 32, 1, (uint16_t)strlen(name), 0, 8, name, NULL);
    httpd_req_t req = make_req(n);
    set_body(ota_combined, n);
    mock_audio_validate_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    mock_audio_validate_ret = ESP_OK;
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":0"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/bad.wav"));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)mock_saved_tracks_count);
}

static void test_ota_combined_auto_slot_and_label(void)
{
    const char *name = "nondescript.wav";
    size_t n = build_container(ota_combined, 32, 1, (uint16_t)strlen(name), 0, 8, name, NULL);
    httpd_req_t req = make_req(n);
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)mock_saved_tracks_count);
    TEST_ASSERT_EQUAL_UINT8(1, mock_saved_tracks[0].slot);
    TEST_ASSERT_EQUAL_STRING("nondescript", mock_saved_tracks[0].label);
    remove("web_tmp/audio/nondescript.wav");
}

static void test_ota_combined_clamp_and_short(void)
{
    const char *name = "slot2.wav";
    size_t n = build_container(ota_combined, 32, 30, (uint16_t)strlen(name), 0, 4, name, NULL);
    httpd_req_t req = make_req(n);
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":1"));
    remove("web_tmp/audio/slot2.wav");
}

static void test_ota_combined_invalid_header_and_short_data(void)
{
    /* name_len beyond the limit -> ota_file_hdr_parse fails. */
    const char *name = "x";
    size_t n = build_container(ota_combined, 32, 1, 100, 0, 1, name, NULL);
    httpd_req_t req = make_req(n);
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":0"));

    /* data_len larger than the bytes actually present. */
    memset(mock_tracks, 0, sizeof(mock_tracks));
    mock_tracks_count = 0;
    const char *name2 = "slot4.wav";
    size_t full = build_container(ota_combined, 32, 1, (uint16_t)strlen(name2), 0, 100,
                                  name2, NULL);
    size_t trunc = full - 90; /* keep only 10 of 100 data bytes */
    req = make_req(trunc);
    set_body(ota_combined, trunc);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/slot4.wav"));
}

static void test_ota_combined_storage_and_write_failures(void)
{
    const char *name = "slot5.wav";
    size_t n = build_container(ota_combined, 32, 1, (uint16_t)strlen(name), 0, 4, name, NULL);
    httpd_req_t req = make_req(n);

    /* Storage not mounted: sounds are skipped. */
    mock_storage_is_mounted = 0;
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    mock_storage_is_mounted = 1;

    /* fopen fails because a directory occupies the target path. */
    reset_resp();
    MKDIR("web_tmp/audio/adir.wav");
    const char *name2 = "adir.wav";
    size_t n2 = build_container(ota_combined, 32, 1, (uint16_t)strlen(name2), 0, 4,
                                name2, NULL);
    req = make_req(n2);
    set_body(ota_combined, n2);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":0"));
    RMDIR("web_tmp/audio/adir.wav");

    /* tracks save failure: file is written but no track is recorded. */
    reset_resp();
    mock_tracks_save_ret = ESP_FAIL;
    const char *name3 = "slot6.wav";
    size_t n3 = build_container(ota_combined, 32, 1, (uint16_t)strlen(name3), 0, 4,
                                name3, NULL);
    req = make_req(n3);
    set_body(ota_combined, n3);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":1"));
    mock_tracks_save_ret = 0;
    remove("web_tmp/audio/slot6.wav");

    /* Firmware write failure inside a combined upload. */
    reset_resp();
    mock_ota_write_err = ESP_FAIL;
    size_t n4 = build_container(ota_combined, 32, 1, (uint16_t)strlen(name), 0, 4, name, NULL);
    req = make_req(n4);
    set_body(ota_combined, n4);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("ota write failed", mock_resp_send_err_msg);
    mock_ota_write_err = 0;

    /* n_files == 0: no sound processing at all. */
    reset_resp();
    size_t n5 = build_container(ota_combined, 32, 0, 0, 0, 0, NULL, NULL);
    req = make_req(n5);
    set_body(ota_combined, n5);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":0"));
}

static void test_web_apply_function_stop_both_voices(void)
{
    s_func_map[2].slot_a = 1;
    s_func_map[2].slot_b = 3;
    mock_tracks_count = 0;
    web_apply_function(2, true);
    mock_audio_voice_stop_calls = 0;
    web_apply_function(2, false); /* !state stops both voice_a and voice_b */
    TEST_ASSERT_EQUAL_INT(2, mock_audio_voice_stop_calls);
}

static void test_audio_upload_compaction_moves_entries(void)
{
    mock_tracks_count = 3;
    mock_tracks[0].slot = 1;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/a0.wav");
    mock_tracks[1].slot = 1;
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/a1.wav");
    mock_tracks[2].slot = 5;
    snprintf(mock_tracks[2].file, sizeof(mock_tracks[2].file), "audio/a5.wav");
    uint8_t data[16];
    memset(data, 1, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "c.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)mock_saved_tracks_count);
    TEST_ASSERT_EQUAL_STRING("audio/c.wav", mock_saved_tracks[0].file);
    TEST_ASSERT_EQUAL_UINT8(5, mock_saved_tracks[1].slot);
}

static void test_audio_track_delete_compaction_moves_entries(void)
{
    FILE *f = fopen("web_tmp/audio/d0.wav", "wb");
    fputs("d", f);
    fclose(f);
    remove("web_tmp/audio/d0.wav");
    mock_tracks_count = 2;
    mock_tracks[0].slot = 2; /* deleted */
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/del2.wav");
    mock_tracks[1].slot = 3; /* kept and shifted down */
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/keep3.wav");
    httpd_req_t req = make_req(0);
    set_query("slot=2");
    TEST_ASSERT_EQUAL(ESP_OK, audio_track_delete_post(&req));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)mock_saved_tracks_count);
    TEST_ASSERT_EQUAL_UINT8(3, mock_saved_tracks[0].slot);
}

static uint8_t ota_long[9000];

static void test_ota_plain_long_and_loop_fail(void)
{
    memset(ota_long, 0x55, sizeof(ota_long));
    httpd_req_t req = make_req(sizeof(ota_long));
    set_body(ota_long, sizeof(ota_long));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"bytes\":9000"));

    /* Body ends mid-firmware: the copy loop fails. */
    reset_resp();
    req = make_req(sizeof(ota_long));
    set_body(ota_long, 20);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("ota write failed", mock_resp_send_err_msg);
}

static void test_ota_combined_truncated_name_and_label(void)
{
    const char *name = "slot8.wav";
    size_t full = build_container(ota_combined, 32, 1, (uint16_t)strlen(name), 7, 4,
                                  name, "LabelXY");

    /* Truncate right after the file header: the name read fails. */
    size_t trunc = OTA_CONTAINER_HDR_LEN + 32 + OTA_FILE_HDR_LEN;
    httpd_req_t req = make_req(trunc);
    set_body(ota_combined, trunc);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":0"));

    /* Truncate after the name: the label read fails. */
    trunc = OTA_CONTAINER_HDR_LEN + 32 + OTA_FILE_HDR_LEN + strlen(name);
    reset_resp();
    req = make_req(trunc);
    set_body(ota_combined, trunc);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":0"));
    (void)full;
}

static void test_ota_combined_file_io_failures(void)
{
    const char *name = "slot9.wav";
    size_t n = build_container(ota_combined, 32, 1, (uint16_t)strlen(name), 0, 8, name, NULL);
    httpd_req_t req = make_req(n);

    int *flags[] = { &mock_ota_fwrite_fail, &mock_ota_fflush_fail, &mock_ota_fclose_fail };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        *flags[i] = 1;
        reset_resp();
        set_body(ota_combined, n);
        TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"files\":0"));
        TEST_ASSERT_FALSE(file_exists("web_tmp/audio/slot9.wav"));
        *flags[i] = 0;
    }
}

static size_t build_two_file_container(uint8_t *out)
{
    size_t o = 0;
    uint32_t fw_len = 16;
    memcpy(out, OTA_CONTAINER_MAGIC, OTA_CONTAINER_MAGIC_LEN);
    o = OTA_CONTAINER_MAGIC_LEN;
    out[o++] = (uint8_t)fw_len;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 2; /* n_files */
    out[o++] = 0;
    out[o++] = 0;
    out[o++] = 0;
    for (uint32_t i = 0; i < fw_len; ++i) {
        out[o++] = 0x11;
    }
    const char *names[2] = { "aaa.wav", "bbb.wav" };
    for (int k = 0; k < 2; ++k) {
        uint16_t nl = (uint16_t)strlen(names[k]);
        out[o++] = (uint8_t)nl;
        out[o++] = (uint8_t)(nl >> 8);
        out[o++] = 0;
        out[o++] = 0;
        uint32_t dl = 2;
        out[o++] = (uint8_t)dl;
        out[o++] = 0;
        out[o++] = 0;
        out[o++] = 0;
        memcpy(out + o, names[k], nl);
        o += nl;
        out[o++] = 'A';
        out[o++] = 'B';
    }
    return o;
}

static void test_ota_combined_auto_slot_collision(void)
{
    memset(mock_tracks, 0, sizeof(mock_tracks));
    mock_tracks_count = 0;
    size_t n = build_two_file_container(ota_combined);
    httpd_req_t req = make_req(n);
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)mock_saved_tracks_count);
    TEST_ASSERT_EQUAL_UINT8(1, mock_saved_tracks[0].slot);
    TEST_ASSERT_EQUAL_UINT8(2, mock_saved_tracks[1].slot);
    remove("web_tmp/audio/aaa.wav");
    remove("web_tmp/audio/bbb.wav");
}

static void test_web_init_wifi_start_warning(void)
{
    mock_settings_cfg = make_wifi_cfg();
    mock_wifi_init_ret = ESP_FAIL;
    /* The httpd still starts, so web_init reports the server result (the
     * Wi-Fi failure is only logged). */
    TEST_ASSERT_EQUAL(ESP_OK, web_init());

    mock_wifi_init_ret = 0;
    mock_settings_cfg = make_wifi_cfg();
    mock_settings_cfg.auto_off_min = 5;
    mock_task_create_ok = 0; /* DNS + auto-off task creation fail */
    TEST_ASSERT_EQUAL(ESP_OK, web_init());
    mock_task_create_ok = 1;
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_web_log_event_null_mutex);
    RUN_TEST(test_web_log_event_stores);
    RUN_TEST(test_web_log_event_utf8_and_truncation);
    RUN_TEST(test_get_function_state_bounds);
    RUN_TEST(test_control_is_rails);
    RUN_TEST(test_wifi_event_handler);
    RUN_TEST(test_wifi_pick_channel_scan_fail);
    RUN_TEST(test_wifi_pick_channel_no_aps);
    RUN_TEST(test_wifi_pick_channel_num_fail);
    RUN_TEST(test_wifi_pick_channel_records_fail);
    RUN_TEST(test_wifi_pick_channel_scores);
    RUN_TEST(test_web_apply_function_bounds);
    RUN_TEST(test_web_apply_function_mutex_null);
    RUN_TEST(test_web_apply_function_output_delta);
    RUN_TEST(test_web_apply_function_unchanged);
    RUN_TEST(test_web_apply_function_starts_sound);
    RUN_TEST(test_web_apply_function_sound_missing_stops);
    RUN_TEST(test_web_apply_function_tracks_load_fail);
    RUN_TEST(test_web_apply_function_double_voice);
    RUN_TEST(test_web_apply_function_b_voice_ignored_above_10);
    RUN_TEST(test_web_apply_function_out_of_audio_range);
    RUN_TEST(test_web_apply_function_scheme_routes_to_sound);
    RUN_TEST(test_web_motion_changed);
    RUN_TEST(test_web_func_map_get_set);
    RUN_TEST(test_parse_ip4);
    RUN_TEST(test_wifi_start_off);
    RUN_TEST(test_wifi_start_full);
    RUN_TEST(test_wifi_start_short_password);
    RUN_TEST(test_wifi_start_init_fail);
    RUN_TEST(test_wifi_start_init_invalid_state);
    RUN_TEST(test_wifi_start_no_sta_netif);
    RUN_TEST(test_wifi_start_sta_mode_fail);
    RUN_TEST(test_wifi_start_sta_start_fail);
    RUN_TEST(test_wifi_start_channel_chosen);
    RUN_TEST(test_wifi_start_ap_netif_null);
    RUN_TEST(test_wifi_start_get_ip_fail);
    RUN_TEST(test_wifi_start_bad_ap_ip);
    RUN_TEST(test_wifi_auto_off_paths);
    RUN_TEST(test_dns_task_socket_fail);
    RUN_TEST(test_dns_task_bind_fail);
    RUN_TEST(test_dns_task_serves);
    RUN_TEST(test_root_handler);
    RUN_TEST(test_control_source_get);
    RUN_TEST(test_control_source_post);
    RUN_TEST(test_mode_get);
    RUN_TEST(test_mode_post);
    RUN_TEST(test_bemf_cal_get);
    RUN_TEST(test_bemf_base_get);
    RUN_TEST(test_bemf_cal_post_paths);
    RUN_TEST(test_bemf_use_get_post);
    RUN_TEST(test_motor_get_post);
    RUN_TEST(test_functions_get);
    RUN_TEST(test_function_post);
    RUN_TEST(test_aux_effect_post);
    RUN_TEST(test_audio_status_get);
    RUN_TEST(test_audio_tracks_get);
    RUN_TEST(test_audio_play_post);
    RUN_TEST(test_audio_stop_post);
    RUN_TEST(test_audio_volume_post);
    RUN_TEST(test_internal_helpers);
    RUN_TEST(test_pipe_writer_success);
    RUN_TEST(test_pipe_writer_abort);
    RUN_TEST(test_pipe_writer_write_fail);
    RUN_TEST(test_audio_upload_guards);
    RUN_TEST(test_audio_upload_success);
    RUN_TEST(test_audio_upload_replaces_and_removes_old);
    RUN_TEST(test_audio_upload_compacts_duplicates);
    RUN_TEST(test_audio_upload_slots_full);
    RUN_TEST(test_audio_upload_autoslot);
    RUN_TEST(test_audio_upload_autoslot_full);
    RUN_TEST(test_audio_upload_validate_and_save_fail);
    RUN_TEST(test_audio_upload_short_body_and_flush_fail);
    RUN_TEST(test_audio_upload_open_fail);
    RUN_TEST(test_audio_upload_no_mem);
    RUN_TEST(test_audio_upload_buf_alloc_fail_and_task_fail);
    RUN_TEST(test_audio_track_delete);
    RUN_TEST(test_audio_track_delete_in_use_and_load_fail);
    RUN_TEST(test_track_category_post);
    RUN_TEST(test_cv_read_and_all);
    RUN_TEST(test_cv_write_post);
    RUN_TEST(test_aux_cfg_get_post);
    RUN_TEST(test_aux_cfg_effect_periods);
    RUN_TEST(test_func_map_get_post);
    RUN_TEST(test_func_bind_load_and_desired);
    RUN_TEST(test_func_bind_get_view);
    RUN_TEST(test_func_map_post_binding);
    RUN_TEST(test_sound_rest_handlers);
    RUN_TEST(test_sound_project_handlers);
    RUN_TEST(test_sound_upload_handler);
    RUN_TEST(test_web_func_map_set_preserves_sound_bindings);
    RUN_TEST(test_utf8_four_byte_and_outputs_init);
    RUN_TEST(test_captive_handler);
    RUN_TEST(test_start_dns_hijack);
    RUN_TEST(test_pipe_writer_idle_continue);
    RUN_TEST(test_pipe_upload_freeq_send_fail);
    RUN_TEST(test_pipe_upload_write_send_fail);
    RUN_TEST(test_pipe_upload_freeq_exhausted);
    RUN_TEST(test_pipe_upload_writer_error);
    RUN_TEST(test_aux_effect_valid_mode);
    RUN_TEST(test_wifi_get);
    RUN_TEST(test_wifi_post);
    RUN_TEST(test_wifi_and_factory_reset);
    RUN_TEST(test_device_get_post);
    RUN_TEST(test_storage_get);
    RUN_TEST(test_task_inputs_and_clientlog);
    RUN_TEST(test_log_get);
    RUN_TEST(test_progress_handler);
    RUN_TEST(test_web_fs_busy);
    RUN_TEST(test_register_route_failure);
    RUN_TEST(test_start_servers);
    RUN_TEST(test_web_init_wifi_off);
    RUN_TEST(test_web_init_clamps_aux);
    RUN_TEST(test_web_init_wifi_on);
    RUN_TEST(test_web_init_server_fail);
    RUN_TEST(test_ota_safe_name_and_slot);
    RUN_TEST(test_ota_stream_read);
    RUN_TEST(test_ota_guards);
    RUN_TEST(test_ota_plain_success_and_failures);
    RUN_TEST(test_ota_combined_success);
    RUN_TEST(test_ota_combined_invalid_wav);
    RUN_TEST(test_ota_combined_auto_slot_and_label);
    RUN_TEST(test_ota_combined_clamp_and_short);
    RUN_TEST(test_ota_combined_invalid_header_and_short_data);
    RUN_TEST(test_ota_combined_storage_and_write_failures);
    RUN_TEST(test_web_apply_function_stop_both_voices);
    RUN_TEST(test_audio_upload_compaction_moves_entries);
    RUN_TEST(test_audio_track_delete_compaction_moves_entries);
    RUN_TEST(test_ota_plain_long_and_loop_fail);
    RUN_TEST(test_ota_combined_truncated_name_and_label);
    RUN_TEST(test_ota_combined_file_io_failures);
    RUN_TEST(test_ota_combined_auto_slot_collision);
    RUN_TEST(test_web_init_wifi_start_warning);
    return UNITY_END();
}
