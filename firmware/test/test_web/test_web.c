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
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#define MKDIR(p) mkdir((p), 0777)
#define RMDIR(p) rmdir(p)
#endif
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "freertos/task.h"
#include "audio.h"
#include "sound.h"
#include "auxio.h"
#include "esp_wifi.h"
#include "esp_netif.h"

/* SDK 6 values used by the DHCP fault scripts when host headers omit them. */
#ifndef ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 0x5004
#endif
#ifndef ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 0x5005
#endif

BaseType_t test_capture_task(void (*task)(void *), const char *name, uint32_t stack,
                            void *arg, UBaseType_t priority, TaskHandle_t *handle);
esp_err_t test_ordered_boot(const esp_partition_t *partition);
bool test_audio_quiet(void);
esp_err_t test_sound_inhibit(bool inhibited);
esp_err_t test_aux_enabled(uint8_t channel, bool enabled);
esp_err_t test_async_complete(httpd_req_t *req);
esp_err_t test_wifi_config(wifi_interface_t interface, wifi_config_t *config);
esp_err_t test_netif_set_ip(esp_netif_t *netif, const esp_netif_ip_info_t *ip);
esp_err_t test_netif_dhcps_stop(esp_netif_t *netif);
esp_err_t test_netif_dhcps_start(esp_netif_t *netif);
esp_err_t test_netif_dns(esp_netif_t *netif, esp_netif_dns_type_t type, esp_netif_dns_info_t *dns);
esp_err_t test_metadata_sync(void);
int test_atomic_rename(const char *from, const char *to);
int test_binary_open(const char *path, int flags, int mode);
esp_err_t httpd_resp_send_chunk(httpd_req_t *req, const char *buf, ssize_t len);

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

#include "../../components/sound/src/sound_graph.c"
#include "../../components/audio/src/ima_adpcm.c"
#include "../../components/web/src/audio_pack.c"

#define static
#include "../../components/web/src/web_util.c"

/* Header injection for CSRF tests. web_stubs.c exposes a single generic mock
 * header, so route Origin/Referer/Host through dedicated buffers while
 * delegating every existing header (Content-Type, X-File-Name, ...) to it. */
esp_err_t test_web_hdr_value_str(httpd_req_t *req, const char *field, char *val,
                                 size_t val_size);
#define httpd_req_get_hdr_value_str test_web_hdr_value_str
#define xTaskCreate test_capture_task
#define esp_ota_set_boot_partition test_ordered_boot
#define audio_is_quiescent test_audio_quiet
#define sound_set_inhibited test_sound_inhibit
#define auxio_set_enabled test_aux_enabled
#define httpd_req_async_handler_complete test_async_complete
#define esp_wifi_set_config test_wifi_config
#define esp_netif_set_ip_info test_netif_set_ip
#define esp_netif_dhcps_stop test_netif_dhcps_stop
#define esp_netif_dhcps_start test_netif_dhcps_start
#define esp_netif_set_dns_info test_netif_dns
#define settings_manifest_sync test_metadata_sync
#define rename test_atomic_rename
#define open test_binary_open
#include "../../components/web/src/web.c"
#include "../../components/web/src/web_wifi.c"
#undef httpd_req_get_hdr_value_str
#undef xTaskCreate
#undef esp_ota_set_boot_partition
#undef audio_is_quiescent
#undef sound_set_inhibited
#undef auxio_set_enabled
#undef httpd_req_async_handler_complete
#undef esp_wifi_set_config
#undef esp_netif_set_ip_info
#undef esp_netif_dhcps_stop
#undef esp_netif_dhcps_start
#undef esp_netif_set_dns_info
#undef settings_manifest_sync
#undef rename
#undef open
#include "../../test_libs/teststubs/stubs.c"
#define sg_store_list sg_store_list_stub
#include "../../test_libs/teststubs/web_stubs.c"
#undef sg_store_list

/* web_stubs.c lists a single fixed "demo" project. The reference action needs
 * the built-in project to be present, so retain the shared mock implementation
 * and add a controllable wrapper rather than duplicating the store mocks. */
static bool test_list_default_project;
static uint32_t test_list_default_revision;

esp_err_t sg_store_list(sg_store_descriptor_t *out, size_t capacity, size_t *count)
{
    esp_err_t err = sg_store_list_stub(out, capacity, count);
    if (err != ESP_OK || !test_list_default_project) return err;
    if (*count >= capacity || *count >= SG_STORE_MAX_PROJECTS) return ESP_ERR_INVALID_SIZE;
    snprintf(out[*count].id, sizeof(out[*count].id), "%s", SG_DEFAULT_PROJECT_ID);
    snprintf(out[*count].name, sizeof(out[*count].name), "ChME3 Locomotive");
    out[*count].revision = test_list_default_revision;
    ++*count;
    return ESP_OK;
}

static void *test_pending_transfer;
static unsigned test_boot_calls, test_complete_calls;
static bool test_complete_gate_closed;
static bool test_quiet_block, test_require_metadata, test_boot_order_error;
static esp_err_t test_sound_inhibit_err, test_aux_enable_err;
static int test_sound_inhibit_fail_remaining;
static int test_sound_inhibit_calls;
static bool test_sound_inhibit_last;
static char test_origin_hdr[256], test_referer_hdr[512], test_host_hdr[256];
static wifi_config_t test_ap_config;
static esp_err_t test_ip_err, test_dhcp_stop_err, test_dhcp_start_err, test_dns_err;
static esp_err_t test_manifest_err;
static unsigned test_manifest_calls, test_rename_calls;
static unsigned test_rename_fail_at, test_rename_fail_at_second;

BaseType_t test_capture_task(void (*task)(void *), const char *name, uint32_t stack,
                            void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    BaseType_t ret = xTaskCreate(task, name, stack, arg, priority, handle);
    if (ret == pdPASS && task == transfer_worker) test_pending_transfer = arg;
    return ret;
}

esp_err_t test_ordered_boot(const esp_partition_t *partition)
{
    ++test_boot_calls;
    if (test_require_metadata && mock_tracks_save_calls == 0) test_boot_order_error = true;
    return esp_ota_set_boot_partition(partition);
}

bool test_audio_quiet(void) { return !test_quiet_block && audio_is_quiescent(); }
esp_err_t test_sound_inhibit(bool inhibited)
{
    ++test_sound_inhibit_calls;
    test_sound_inhibit_last = inhibited;
    if (test_sound_inhibit_err != ESP_OK) {
        esp_err_t err = test_sound_inhibit_err;
        /* fail_remaining == 0 keeps the historical persistent injection;
         * a positive count clears the injected error after N calls. */
        if (test_sound_inhibit_fail_remaining > 0 &&
            --test_sound_inhibit_fail_remaining == 0) {
            test_sound_inhibit_err = ESP_OK;
        }
        return err;
    }
    return sound_set_inhibited(inhibited);
}
esp_err_t test_aux_enabled(uint8_t channel, bool enabled)
{
    return test_aux_enable_err != ESP_OK ? test_aux_enable_err : auxio_set_enabled(channel, enabled);
}
esp_err_t test_async_complete(httpd_req_t *req)
{
    ++test_complete_calls;
    test_complete_gate_closed = web_maintenance_active();
    return httpd_req_async_handler_complete(req);
}
/* Simulated upload handler that stalls past its deadline and asks the
 * supervisor to perform the timeout action from the worker's own context. */
static bool test_stalled_saw_abort;
static esp_err_t test_stalled_handler_rc;
esp_err_t test_stalled_transfer_handler(httpd_req_t *req)
{
    /* No bytes moved: simulate the deadline expiring while blocked. */
    s_transfer_deadline = mock_timer_now_us - 1;
    bool aborted = transfer_supervisor_check();
    test_stalled_saw_abort = aborted && transfer_aborted();
    test_stalled_handler_rc = aborted ? ESP_FAIL : ESP_OK;
    if (aborted) {
        (void)httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "stalled transfer");
    }
    return test_stalled_handler_rc;
}

static int test_progress_total;
esp_err_t test_progress_transfer_handler(httpd_req_t *req)
{
    FILE *f = fopen("web_tmp/audio/wdt_progress.tmp", "wb");
    if (f == NULL) return ESP_FAIL;
    esp_err_t err = pipe_upload(req, f, &test_progress_total);
    fclose(f);
    return err;
}

esp_err_t test_wifi_config(wifi_interface_t interface, wifi_config_t *config)
{
    if (interface == WIFI_IF_AP) test_ap_config = *config;
    return esp_wifi_set_config(interface, config);
}
esp_err_t test_netif_set_ip(esp_netif_t *netif, const esp_netif_ip_info_t *ip)
{
    return test_ip_err != ESP_OK ? test_ip_err : esp_netif_set_ip_info(netif, ip);
}
esp_err_t test_netif_dhcps_stop(esp_netif_t *netif)
{
    return test_dhcp_stop_err != ESP_OK ? test_dhcp_stop_err : esp_netif_dhcps_stop(netif);
}
esp_err_t test_netif_dhcps_start(esp_netif_t *netif)
{
    return test_dhcp_start_err != ESP_OK ? test_dhcp_start_err : esp_netif_dhcps_start(netif);
}
esp_err_t test_netif_dns(esp_netif_t *netif, esp_netif_dns_type_t type, esp_netif_dns_info_t *dns)
{
    return test_dns_err != ESP_OK ? test_dns_err : esp_netif_set_dns_info(netif, type, dns);
}
esp_err_t test_metadata_sync(void)
{
    ++test_manifest_calls;
    return test_manifest_err;
}
static unsigned test_manifest_load_calls;
esp_err_t settings_manifest_load(void)
{
    ++test_manifest_load_calls;
    return ESP_OK;
}
int test_atomic_rename(const char *from, const char *to)
{
    ++test_rename_calls;
    if (test_rename_calls == test_rename_fail_at || test_rename_calls == test_rename_fail_at_second) {
        errno = EIO;
        return -1;
    }
#ifdef _WIN32
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 0;
    errno = EIO;
    return -1;
#else
    return rename(from, to);
#endif
}
int test_binary_open(const char *path, int flags, int mode)
{
#ifdef _WIN32
    /* ESP/newlib is byte-preserving; CRT fdopen("wb") alone is not. */
    flags |= O_BINARY;
#endif
    return open(path, flags, mode);
}

esp_err_t test_web_hdr_value_str(httpd_req_t *req, const char *field, char *val,
                                 size_t val_size)
{
    const char *injected = NULL;
    if (strcmp(field, "Origin") == 0) {
        injected = test_origin_hdr;
    } else if (strcmp(field, "Referer") == 0) {
        injected = test_referer_hdr;
    } else if (strcmp(field, "Host") == 0) {
        injected = test_host_hdr;
    }
    if (injected == NULL) {
        return httpd_req_get_hdr_value_str(req, field, val, val_size);
    }
    if (val == NULL || val_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (injected[0] == '\0') {
        val[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    if (strlen(injected) >= val_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(val, val_size, "%s", injected);
    return ESP_OK;
}

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

static void set_csrf_headers(const char *origin, const char *referer, const char *host)
{
    snprintf(test_origin_hdr, sizeof(test_origin_hdr), "%s", origin != NULL ? origin : "");
    snprintf(test_referer_hdr, sizeof(test_referer_hdr), "%s", referer != NULL ? referer : "");
    snprintf(test_host_hdr, sizeof(test_host_hdr), "%s", host != NULL ? host : "");
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
    test_list_default_project = false;
    test_list_default_revision = 0;
    mock_nvs_reset();
    clear_files();
    MKDIR("web_tmp");
    MKDIR("web_tmp/audio");
    /* Bindings are per-suite state in web.c; keep the legacy fallback path
     * active unless a test populates them explicitly. */
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind_count = 0;
    s_func_bind_present = false;
    s_outputs_applied = 0;
    s_logic_active = 0;
    s_logic_notch = 0;
    s_control_mutex = (SemaphoreHandle_t)1;
    s_actuation_ready = true;
    func_tracks_cache_refresh();
    s_maintenance_owner = NULL;
    s_maintenance_exclusive_fs = false;
    s_mutation_count = 0;
    s_mutation_owner = NULL;
    s_control_generation = 0;
    s_transfer_deadline = 0;
    s_transfer_reboot = false;
    s_file_sequence = 0;
    s_route_count = 0;
    memset(s_legacy_voice, 0, sizeof(s_legacy_voice));
    memset(&s_preview_voice, 0, sizeof(s_preview_voice));
    s_preview_name[0] = '\0';
    test_pending_transfer = NULL;
    test_boot_calls = test_complete_calls = 0;
    test_complete_gate_closed = false;
    test_quiet_block = test_require_metadata = test_boot_order_error = false;
    test_sound_inhibit_err = test_aux_enable_err = ESP_OK;
    test_sound_inhibit_fail_remaining = 0;
    test_sound_inhibit_calls = 0;
    test_sound_inhibit_last = false;
    set_csrf_headers(NULL, NULL, NULL);
    mock_current_task_handle = (TaskHandle_t)1;
    mock_task_create_ok = 1;
    mock_task_create_fail_after = -1;
    mock_task_create_calls = 0;
    mock_task_startup_hook = NULL;
    mock_sem_take_fail = 0;
    mock_mutex_create_fail = 0;
    mock_timer_now_us = 0;
    mock_motor_emergency_stop_calls = 0;
    memset(&test_ap_config, 0, sizeof(test_ap_config));
    test_ip_err = test_dhcp_stop_err = test_dhcp_start_err = test_dns_err = ESP_OK;
    test_manifest_err = ESP_OK;
    test_manifest_calls = test_rename_calls = 0;
    test_manifest_load_calls = 0;
    test_rename_fail_at = test_rename_fail_at_second = 0;
    s_cfg_mutex = (SemaphoreHandle_t)1;
    s_up_req = NULL;
    s_transfer_supervisor = NULL;
    s_up_worker = NULL;
    s_up_active = false;
    s_up_aborted = false;
    s_up_lease_held = false;
    s_transfer_deadline = 0;
    s_transfer_wdt_subscribed = false;
    s_maintenance_owner = NULL;
    s_maintenance_exclusive_fs = false;
    mock_storage_free = 64U * 1024U * 1024U;
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

/* ---------- control source ---------- */

static void test_control_is_rails(void)
{
    s_cfg.control_source = 0;
    s_actuation_ready = true;
    TEST_ASSERT_TRUE(web_control_is_rails());
    TEST_ASSERT_TRUE(web_control_source_is_rails());
    s_actuation_ready = false;
    TEST_ASSERT_FALSE(web_control_source_is_rails());
    s_actuation_ready = true;
    s_cfg.control_source = 1;
    TEST_ASSERT_FALSE(web_control_is_rails());
    TEST_ASSERT_FALSE(web_control_source_is_rails());
    s_cfg.control_source = 0;
}

static void test_emergency_post_cuts_all_and_clears_functions(void)
{
    httpd_req_t req = make_req(0);
    s_fn[1] = true;
    s_fn[2] = true;
    mock_motor_emergency_stop_calls = 0;
    mock_audio_stop_calls = 0;
    mock_sound_stop_all_calls = 0;
    uint32_t generation = s_control_generation;
    reset_resp();

    TEST_ASSERT_EQUAL(ESP_OK, emergency_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_motor_emergency_stop_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_audio_stop_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_sound_stop_all_calls);
    TEST_ASSERT_EQUAL_UINT32(generation + 1U, s_control_generation);
    TEST_ASSERT_FALSE(s_fn[1]);
    TEST_ASSERT_FALSE(s_fn[2]);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    /* A blocked control mutex must not refuse the emergency action. */
    s_fn[1] = true;
    generation = s_control_generation;
    mock_sem_take_fail = 1;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, emergency_post(&req));
    TEST_ASSERT_EQUAL_UINT32(generation + 1U, s_control_generation);
    /* A failed function-mutex take must still clear the function state. */
    TEST_ASSERT_FALSE(s_fn[1]);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    mock_sem_take_fail = 0;
    s_fn[1] = false;
    s_actuation_ready = true;
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

static void test_wifi_pick_channel_adjacent_overlap(void)
{
    mock_scan_start_ret = ESP_OK;
    mock_scan_num_ret = ESP_OK;
    mock_scan_num = 2;
    mock_scan_records_n = 2;
    /* Strong APs on 2 and 10 bury channels 1 and 11; 6 is clear. Counting
     * exact matches only (the old behaviour) would have picked channel 1. */
    mock_scan_records[0].rssi = -40;
    mock_scan_records[0].primary = 2;
    mock_scan_records[1].rssi = -40;
    mock_scan_records[1].primary = 10;
    TEST_ASSERT_EQUAL_UINT8(6, wifi_pick_channel());
}

static void test_wifi_pick_channel_ht40(void)
{
    mock_scan_start_ret = ESP_OK;
    mock_scan_num_ret = ESP_OK;
    mock_scan_num = 1;
    mock_scan_records_n = 1;
    /* 40 MHz AP on 1+5 also occupies the lower half of channel 6. */
    mock_scan_records[0].rssi = -40;
    mock_scan_records[0].primary = 1;
    mock_scan_records[0].second = WIFI_SECOND_CHAN_ABOVE;
    TEST_ASSERT_EQUAL_UINT8(11, wifi_pick_channel());
}

/* ---------- function outputs / mapping ---------- */

static void test_web_apply_function_bounds(void)
{
    /* Out-of-range function keys must be ignored without touching s_fn[]. */
    web_apply_function(SETTINGS_FUNC_MAP_COUNT, true);
    for (size_t i = 0; i < WEB_FN_COUNT; ++i) {
        TEST_ASSERT_FALSE(s_fn[i]);
    }
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
    func_tracks_cache_refresh();
    int loads = mock_tracks_load_calls;
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_INT(loads, mock_tracks_load_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_audio_voice_play_calls);
    TEST_ASSERT_EQUAL_STRING("web_tmp/audio/slot3.wav", mock_audio_last_path);
}

static void test_web_apply_function_sound_missing_stops(void)
{
    s_func_map[1].slot_a = 9;
    s_func_map[1].slot_b = 0;
    mock_tracks_count = 0;
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_INT(0, mock_audio_voice_stop_calls);
}

static void test_web_apply_function_tracks_load_fail(void)
{
    s_func_map[1].slot_a = 4;
    mock_tracks_load_ret = ESP_FAIL;
    func_tracks_cache_refresh();
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
    func_tracks_cache_refresh();
    web_apply_function(2, true);
    TEST_ASSERT_EQUAL_INT(2, mock_audio_voice_play_calls);
}

static void test_web_apply_function_secondary_above_10(void)
{
    s_func_map[11].slot_a = 1;
    s_func_map[11].slot_b = 2;
    mock_tracks_count = 2;
    mock_tracks[0].slot = 1;
    mock_tracks[0].enabled = true;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/a.wav");
    mock_tracks[1].slot = 2;
    mock_tracks[1].enabled = true;
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/b.wav");
    func_tracks_cache_refresh();
    web_apply_function(11, true);
    TEST_ASSERT_EQUAL_INT(2, mock_audio_voice_play_calls);
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

static void set_one_binding(uint8_t fn, uint8_t type, uint8_t id, uint8_t dir, uint8_t state)
{
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind[0].used = 1;
    s_func_bind[0].fn = fn;
    s_func_bind[0].target_type = type;
    s_func_bind[0].target_id = id;
    s_func_bind[0].dir = dir;
    s_func_bind[0].state = state;
    s_func_bind_count = 1;
    s_func_bind_present = true;
}

static void test_function_post_all_f_to_all_aux(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 1;
    s_func_bind_present = true;
    s_func_bind_count = WEB_FN_COUNT;
    memset(s_func_bind, 0, sizeof(s_func_bind));
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) {
        s_func_bind[fn].used = 1;
        s_func_bind[fn].fn = fn;
        s_func_bind[fn].target_type = FUNC_TARGET_OUTPUT;
        s_func_bind[fn].target_id = (uint8_t)(fn % AUXIO_CH_COUNT);
        s_func_bind[fn].dir = FUNC_DIR_ANY;
        s_func_bind[fn].state = FUNC_STATE_ANY;
    }
    memset(s_fn, 0, sizeof(s_fn));
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    s_outputs_applied = 0;
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) {
        char q[32];
        uint8_t ch = (uint8_t)(fn % AUXIO_CH_COUNT);
        snprintf(q, sizeof(q), "fn=%u&state=1", (unsigned)fn);
        set_query(q); reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, function_post(&req));
        TEST_ASSERT_EQUAL_INT(1, mock_auxio_enabled_state[ch]);
        for (uint8_t other = 0; other < AUXIO_CH_COUNT; ++other) {
            if (other != ch) TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[other]);
        }
        snprintf(q, sizeof(q), "fn=%u&state=0", (unsigned)fn);
        set_query(q); reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, function_post(&req));
        TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[ch]);
    }
}

static void test_legacy_map_all_f_to_all_aux(void)
{
    s_func_bind_present = false;
    s_func_bind_count = 0;
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) {
        uint8_t ch = (uint8_t)(fn % AUXIO_CH_COUNT);
        uint16_t mask = (uint16_t)(1U << ch);
        memset(s_func_map, 0, sizeof(s_func_map));
        s_func_map[fn].aux_mask = mask;
        s_func_map[fn].dir = SETTINGS_FUNC_DIR_NONE;
        s_func_map[fn].speed = SETTINGS_FUNC_SPD_NONE;
        memset(s_fn, 0, sizeof(s_fn));
        memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
        s_outputs_applied = 0;

        web_apply_function(fn, true);
        TEST_ASSERT_EQUAL_INT(1, mock_auxio_enabled_state[ch]);
        for (uint8_t other = 0; other < AUXIO_CH_COUNT; ++other) {
            if (other != ch) TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[other]);
        }
        web_apply_function(fn, false);
        TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[ch]);
    }
}

static void test_headlight_direction_bindings(void)
{
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind[0].used = 1;
    s_func_bind[0].fn = 1;
    s_func_bind[0].target_type = FUNC_TARGET_OUTPUT;
    s_func_bind[0].target_id = AUXIO_CH_F0F;
    s_func_bind[0].dir = FUNC_DIR_FWD;
    s_func_bind[0].state = FUNC_STATE_ANY;
    s_func_bind[1].used = 1;
    s_func_bind[1].fn = 2;
    s_func_bind[1].target_type = FUNC_TARGET_OUTPUT;
    s_func_bind[1].target_id = AUXIO_CH_F0R;
    s_func_bind[1].dir = FUNC_DIR_REV;
    s_func_bind[1].state = FUNC_STATE_ANY;
    s_func_bind_count = 2;
    s_func_bind_present = true;
    memset(s_fn, 0, sizeof(s_fn));
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    s_outputs_applied = 0;
    s_motion_initialized = true;
    s_motion_forward = true;
    s_motion_speed = 0;

    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_INT(1, mock_auxio_enabled_state[AUXIO_CH_F0F]);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[AUXIO_CH_F0R]);
    web_apply_function(2, true);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[AUXIO_CH_F0R]);

    web_motion_changed(0, false);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[AUXIO_CH_F0F]);
    TEST_ASSERT_EQUAL_INT(1, mock_auxio_enabled_state[AUXIO_CH_F0R]);
    web_apply_function(1, false);
    TEST_ASSERT_EQUAL_INT(1, mock_auxio_enabled_state[AUXIO_CH_F0R]);
    web_apply_function(2, false);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[AUXIO_CH_F0R]);
}

static void test_graph_state_reports_dcc_control_masks(void)
{
    httpd_req_t req = make_req(0);
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind[0].used = 1;
    s_func_bind[0].fn = 1;
    s_func_bind[0].target_type = FUNC_TARGET_OUTPUT;
    s_func_bind[0].target_id = AUXIO_CH_F0F;
    s_func_bind[0].dir = FUNC_DIR_FWD;
    s_func_bind[1].used = 1;
    s_func_bind[1].fn = 2;
    s_func_bind[1].target_type = FUNC_TARGET_OUTPUT;
    s_func_bind[1].target_id = AUXIO_CH_F0R;
    s_func_bind[1].dir = FUNC_DIR_REV;
    s_func_bind[2].used = 1;
    s_func_bind[2].fn = 3;
    s_func_bind[2].target_type = FUNC_TARGET_OUTPUT;
    s_func_bind[2].target_id = AUXIO_CH_AUX1;
    s_func_bind[2].dir = FUNC_DIR_ANY;
    s_func_bind_count = 3;
    s_func_bind_present = true;
    memset(s_fn, 0, sizeof(s_fn));
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    s_outputs_applied = 0;
    s_motion_initialized = true;
    s_motion_forward = true;
    s_motion_speed = 0;

    /* This is exactly the path used by app_main on_dcc_function(). */
    web_apply_function(1, true);
    web_apply_function(2, true);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_state_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"functions\":6"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"outputs\":1"));

    /* DCC direction change must re-gate the headlight pair for Live. */
    web_motion_changed(30, false);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_state_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"functions\":6"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"outputs\":2"));

    web_apply_function(3, true);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_state_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"functions\":14"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"outputs\":6"));
}

static void test_motor_post_updates_motion_and_headlights(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 1;
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind[0].used = 1;
    s_func_bind[0].fn = 1;
    s_func_bind[0].target_type = FUNC_TARGET_OUTPUT;
    s_func_bind[0].target_id = AUXIO_CH_F0F;
    s_func_bind[0].dir = FUNC_DIR_FWD;
    s_func_bind[0].state = FUNC_STATE_ANY;
    s_func_bind[1].used = 1;
    s_func_bind[1].fn = 2;
    s_func_bind[1].target_type = FUNC_TARGET_OUTPUT;
    s_func_bind[1].target_id = AUXIO_CH_F0R;
    s_func_bind[1].dir = FUNC_DIR_REV;
    s_func_bind[1].state = FUNC_STATE_ANY;
    s_func_bind_count = 2;
    s_func_bind_present = true;
    memset(s_fn, 0, sizeof(s_fn));
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    s_outputs_applied = 0;
    s_motion_initialized = false;
    s_motion_forward = true;
    s_motion_speed = 0;

    set_query("speed=40&forward=0");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_UINT8(40, s_motion_speed);
    TEST_ASSERT_FALSE(s_motion_forward);

    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[AUXIO_CH_F0F]);
    web_apply_function(2, true);
    TEST_ASSERT_EQUAL_INT(1, mock_auxio_enabled_state[AUXIO_CH_F0R]);

    set_query("speed=40&forward=1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_auxio_enabled_state[AUXIO_CH_F0F]);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[AUXIO_CH_F0R]);

    set_query("speed=0&forward=1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_UINT8(0, s_motion_speed);
    web_apply_function(1, false);
    web_apply_function(2, false);
}

static void test_web_emergency_reset_clears_outputs(void)
{
    memset(s_fn, 0, sizeof(s_fn));
    s_fn[1] = true;
    s_fn[2] = true;
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    mock_auxio_enabled_state[AUXIO_CH_F0F] = 1;
    mock_auxio_enabled_state[AUXIO_CH_AUX7] = 1;
    s_outputs_applied = (uint16_t)((1U << AUXIO_CH_F0F) | (1U << AUXIO_CH_AUX7));
    s_logic_active = (uint16_t)(1U << (FUNC_LOGIC_DRIVE_HOLD - 1U));
    s_motion_speed = 30;
    s_motion_initialized = false;

    web_emergency_reset();

    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) TEST_ASSERT_FALSE(s_fn[fn]);
    for (uint8_t ch = 0; ch < AUXIO_CH_COUNT; ++ch) {
        TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[ch]);
    }
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
    TEST_ASSERT_EQUAL_UINT16(0, s_logic_active);
    TEST_ASSERT_EQUAL_UINT8(0, s_motion_speed);
    TEST_ASSERT_TRUE(s_motion_initialized);
    TEST_ASSERT_FALSE(mock_sound_logic_drive_hold);

    /* Even when the function mutex cannot be taken, emergency reset must
     * clear the latched outputs instead of returning early. */
    s_fn[1] = true;
    s_fn[2] = true;
    mock_auxio_enabled_state[AUXIO_CH_F0F] = 1;
    mock_auxio_enabled_state[AUXIO_CH_AUX7] = 1;
    s_outputs_applied = (uint16_t)((1U << AUXIO_CH_F0F) | (1U << AUXIO_CH_AUX7));
    s_logic_active = (uint16_t)(1U << (FUNC_LOGIC_COAST - 1U));
    s_motion_speed = 30;
    mock_sem_take_fail = 1;
    web_emergency_reset();
    mock_sem_take_fail = 0;
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) TEST_ASSERT_FALSE(s_fn[fn]);
    for (uint8_t ch = 0; ch < AUXIO_CH_COUNT; ++ch) {
        TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[ch]);
    }
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
    TEST_ASSERT_EQUAL_UINT16(0, s_logic_active);
    TEST_ASSERT_EQUAL_UINT8(0, s_motion_speed);

    /* A NULL mutex follows the same unlocked fallback. */
    s_fn[3] = true;
    s_outputs_applied = (uint16_t)(1U << AUXIO_CH_AUX7);
    s_func_mutex = NULL;
    web_emergency_reset();
    s_func_mutex = (SemaphoreHandle_t)1;
    TEST_ASSERT_FALSE(s_fn[3]);
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
}

static void test_all_logic_actions(void)
{
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    memset(s_fn, 0, sizeof(s_fn));
    s_motion_initialized = true;
    s_motion_forward = true;
    s_motion_speed = 0;
    s_logic_active = 0;
    s_logic_notch = 0;

    set_one_binding(1, FUNC_TARGET_LOGIC, FUNC_LOGIC_MUTE_STOP, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(1, true);
    TEST_ASSERT_TRUE(mock_sound_logic_mute);
    web_apply_function(1, false);
    TEST_ASSERT_FALSE(mock_sound_logic_mute);

    s_motion_speed = 30;
    set_one_binding(2, FUNC_TARGET_LOGIC, FUNC_LOGIC_MUTE_MOVE, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(2, true);
    TEST_ASSERT_TRUE(mock_sound_logic_mute);
    web_apply_function(2, false);
    TEST_ASSERT_FALSE(mock_sound_logic_mute);
    s_motion_speed = 0;

    mock_auxio_enabled_state[AUXIO_CH_F0F] = 1;
    set_one_binding(3, FUNC_TARGET_LOGIC, FUNC_LOGIC_MUTE_LIGHT, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(3, true);
    TEST_ASSERT_TRUE(mock_sound_logic_mute);
    web_apply_function(3, false);
    TEST_ASSERT_FALSE(mock_sound_logic_mute);
    mock_auxio_enabled_state[AUXIO_CH_F0F] = 0;

    set_one_binding(4, FUNC_TARGET_LOGIC, FUNC_LOGIC_DRIVE_HOLD, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(4, true);
    TEST_ASSERT_TRUE(mock_sound_logic_drive_hold);
    web_apply_function(4, false);
    TEST_ASSERT_FALSE(mock_sound_logic_drive_hold);

    set_one_binding(5, FUNC_TARGET_LOGIC, FUNC_LOGIC_COAST, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(5, true);
    TEST_ASSERT_TRUE(mock_sound_logic_coast);
    web_apply_function(5, false);
    TEST_ASSERT_FALSE(mock_sound_logic_coast);

    set_one_binding(6, FUNC_TARGET_LOGIC, FUNC_LOGIC_DYNAMIC_BRAKE, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(6, true);
    TEST_ASSERT_TRUE(mock_sound_logic_dynamic_brake);
    web_apply_function(6, false);
    TEST_ASSERT_FALSE(mock_sound_logic_dynamic_brake);

    s_logic_notch = 0;
    s_logic_active = 0;
    set_one_binding(7, FUNC_TARGET_LOGIC, FUNC_LOGIC_NOTCH_UP, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(7, true);
    TEST_ASSERT_EQUAL_INT(1, mock_sound_logic_notch);
    web_apply_function(7, false);
    web_apply_function(7, true);
    TEST_ASSERT_EQUAL_INT(2, mock_sound_logic_notch);
    web_apply_function(7, false);

    s_logic_notch = 0;
    s_logic_active = 0;
    set_one_binding(8, FUNC_TARGET_LOGIC, FUNC_LOGIC_NOTCH_DOWN, FUNC_DIR_ANY, FUNC_STATE_ANY);
    web_apply_function(8, true);
    TEST_ASSERT_EQUAL_INT(-1, mock_sound_logic_notch);
    web_apply_function(8, false);
    web_apply_function(8, true);
    TEST_ASSERT_EQUAL_INT(-2, mock_sound_logic_notch);
    web_apply_function(8, false);
}

static void test_graph_mode_routes_all_functions_to_sound(void)
{
    mock_sound_scheme_enabled = 1;
    mock_sound_function_calls = 0;
    mock_audio_voice_play_calls = 0;
    s_func_bind_present = false;
    s_func_bind_count = 0;
    memset(s_func_map, 0, sizeof(s_func_map));
    memset(s_fn, 0, sizeof(s_fn));
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) {
        web_apply_function(fn, true);
        TEST_ASSERT_EQUAL_INT((int)fn * 2 + 1, mock_sound_function_calls);
        TEST_ASSERT_EQUAL_UINT8(fn, mock_sound_last_fn);
        TEST_ASSERT_TRUE(mock_sound_last_state);
        web_apply_function(fn, false);
        TEST_ASSERT_EQUAL_INT((int)fn * 2 + 2, mock_sound_function_calls);
        TEST_ASSERT_EQUAL_UINT8(fn, mock_sound_last_fn);
        TEST_ASSERT_FALSE(mock_sound_last_state);
    }
    TEST_ASSERT_EQUAL_INT(0, mock_audio_voice_play_calls);
}

static void test_aux_cfg_all_channels(void)
{
    httpd_req_t req = make_req(0);
    mock_aux_save_ret = ESP_OK;
    for (uint8_t ch = 0; ch < AUXIO_CH_COUNT; ++ch) {
        uint8_t level = (uint8_t)(10U + ch * 9U);
        uint8_t fx = (uint8_t)(ch % AUXIO_EFFECT_COUNT);
        char q[64];
        int before = mock_auxio_config_calls;
        snprintf(q, sizeof(q), "ch=%u&level=%u&effect=%u",
                 (unsigned)ch, (unsigned)level, (unsigned)fx);
        set_query(q);
        reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, aux_cfg_post(&req));
        TEST_ASSERT_EQUAL_INT(before + 1, mock_auxio_config_calls);
        TEST_ASSERT_EQUAL_UINT8(ch, mock_auxio_last_channel);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((uint16_t)level * 255U / 100U), mock_auxio_last_pwm_on);
        TEST_ASSERT_EQUAL_INT(fx, mock_auxio_last_mode);
        TEST_ASSERT_EQUAL_UINT16(aux_effect_period(fx), mock_auxio_last_period);
    }
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
    TEST_ASSERT_FALSE(s_wifi_started);
    TEST_ASSERT_EQUAL_INT(0, mock_wifi_init_calls);
}

static void test_wifi_start_full(void)
{
    settings_config_t cfg = make_wifi_cfg();
    snprintf(cfg.ap_password, sizeof(cfg.ap_password), "supersecret");
    mock_netif_actual_ip.addr = ESP_IP4TOADDR(10, 0, 0, 2);
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_TRUE(s_wifi_started);
    TEST_ASSERT_EQUAL_INT(1, mock_netif_dhcps_start_calls);
    TEST_ASSERT_EQUAL(WIFI_AUTH_WPA2_PSK, test_ap_config.ap.authmode);
    TEST_ASSERT_EQUAL_STRING("supersecret", (const char *)test_ap_config.ap.password);
    TEST_ASSERT_EQUAL_INT(1, mock_netif_set_dns_calls);
}

static void test_wifi_start_short_password(void)
{
    settings_config_t cfg = make_wifi_cfg();
    snprintf(cfg.ap_password, sizeof(cfg.ap_password), "123");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_EQUAL(WIFI_AUTH_OPEN, test_ap_config.ap.authmode);
    TEST_ASSERT_EQUAL_UINT8(0, test_ap_config.ap.password[0]);
}

static void test_wifi_start_init_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_init_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, wifi_start(&cfg));
    TEST_ASSERT_FALSE(s_wifi_started);
    TEST_ASSERT_EQUAL_INT(1, mock_wifi_init_calls);
}

static void test_wifi_start_init_invalid_state(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_init_ret = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_TRUE(s_wifi_started);
    TEST_ASSERT_EQUAL_INT(1, mock_wifi_init_calls);
}

static void test_wifi_start_no_sta_netif(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_sta_netif_null = 1;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_TRUE(s_wifi_started);
}

static void test_wifi_start_sta_mode_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_mode_sta_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_TRUE(s_wifi_started);
}

static void test_wifi_start_sta_start_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_wifi_start_first_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_TRUE(s_wifi_started);
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
    TEST_ASSERT_EQUAL_UINT8(6, test_ap_config.ap.channel);
}

static void test_wifi_start_max_tx_power(void)
{
    settings_config_t cfg = make_wifi_cfg();
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_EQUAL_INT(1, mock_wifi_set_max_tx_power_calls);
    TEST_ASSERT_EQUAL_INT8(80, mock_wifi_last_tx_power);

    /* A rejected TX power must not keep the access point from starting. */
    mock_wifi_set_max_tx_power_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_EQUAL_INT(2, mock_wifi_set_max_tx_power_calls);
}

static void test_wifi_start_client_timeout(void)
{
    settings_config_t cfg = make_wifi_cfg();
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_EQUAL_INT(1, mock_wifi_set_inactive_time_calls);
    TEST_ASSERT_EQUAL_UINT16(120, mock_wifi_inactive_time_sec);

    /* A rejected timeout must not keep the access point from starting. */
    mock_wifi_set_inactive_time_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_EQUAL_INT(2, mock_wifi_set_inactive_time_calls);
}

static void test_wifi_start_ap_netif_null(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_ap_netif_null = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, wifi_start(&cfg));
    TEST_ASSERT_FALSE(s_wifi_started);
}

static void test_wifi_start_get_ip_fail(void)
{
    settings_config_t cfg = make_wifi_cfg();
    mock_netif_get_ip_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_EQUAL_STRING(cfg.ap_ip, s_ap_ip);
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
    s_auto_off_min = 0;
    wifi_auto_off_task(NULL);

    s_auto_off_min = 5;
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
    s_fn[1] = true;
    mock_auxio_enabled_state[AUXIO_CH_F0F] = 1;
    set_query("");
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    reset_resp();
    set_query("source=web");
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_EQUAL_UINT8(1, s_cfg.control_source);
    TEST_ASSERT_EQUAL_INT(1, mock_settings_save_calls);
    TEST_ASSERT_FALSE(s_fn[1]);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[AUXIO_CH_F0F]);

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
    TEST_ASSERT_EQUAL_INT(3, mock_motor_emergency_stop_calls);
}

static void test_control_source_post_errors(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 0;
    s_maintenance_owner = (TaskHandle_t)2;
    set_query("source=web");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "maintenance"));
    TEST_ASSERT_EQUAL_UINT8(0, s_cfg.control_source);
    s_maintenance_owner = NULL;

    mock_sem_take_fail = 1;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "control busy"));
    TEST_ASSERT_EQUAL_UINT8(0, s_cfg.control_source);
    mock_sem_take_fail = 0;

    mock_settings_save_ret = ESP_FAIL;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save failed"));
    TEST_ASSERT_EQUAL_UINT8(0, s_cfg.control_source);
    mock_settings_save_ret = 0;

    mock_cv_write_ret = ESP_FAIL;
    set_query("source=rails");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "mode save failed"));
    TEST_ASSERT_EQUAL_UINT8(0, s_cfg.control_source);
    mock_cv_write_ret = 0;
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
    TEST_ASSERT_EQUAL_INT(2, mock_motor_emergency_stop_calls);
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
    mock_bemf_cal_info.profile.mode = MOTOR_BEMF_MODE_THOROUGH;
    mock_bemf_cal_info.profile.planned_points = 16;
    mock_bemf_cal_info.profile.measured_points = 14;
    mock_bemf_cal_info.profile.stalled_points = 2;
    mock_bemf_cal_info.profile.ceiling_speed = 120;
    mock_bemf_cal_info.profile.first_move_speed = 5;
    mock_bemf_cal_info.profile.first_frac = 100;
    mock_bemf_cal_info.profile.settle_ms_avg = 600;
    mock_bemf_cal_info.profile.plant_gain_x1000 = 2400;
    mock_bemf_cal_info.profile.kp_x100 = 42;
    mock_bemf_cal_info.profile.ki_x1000 = 21;
    mock_bemf_cal_info.profile.kd_x100 = 5;
    mock_bemf_cal_info.profile.quality = 88;
    mock_bemf_cal_info.profile.pid_applied = true;
    mock_bemf_cal_info.phase = MOTOR_BEMF_CAL_PHASE_CURVE;
    mock_bemf_cal_info.profile.phase = MOTOR_BEMF_CAL_PHASE_CURVE;
    mock_bemf_cal_info.profile.sensor_ok = true;
    mock_bemf_cal_info.profile.optimal_us = 340;
    mock_bemf_cal_info.profile.dwell_ms = 1000;
    mock_bemf_enabled = 1;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"speed\":5,\"frac\":50"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"use\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"phase\":\"curve\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"sensorOk\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"optimalUs\":340"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"dwellMs\":1000"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"firstFrac\":100"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"quality\":88"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"pidApplied\":true"));
}

static void test_bemf_phase_names(void)
{
    httpd_req_t req = make_req(0);
    const char *phases[] = {"idle", "preflight", "window", "curve", "saving"};
    for (unsigned i = 0; i < 5; ++i) {
        reset_resp();
        memset(&mock_bemf_cal_info, 0, sizeof(mock_bemf_cal_info));
        mock_bemf_cal_info.phase = (uint8_t)i;
        mock_bemf_cal_info.profile.phase = (uint8_t)i;
        TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_get(&req));
        char expected[48];
        snprintf(expected, sizeof(expected), "\"phase\":\"%s\"", phases[i]);
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, expected));
    }
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

    reset_resp();
    set_query("start=1&mode=thorough&points=16&settle=600&interval=50&samples=10&retries=2&apply_pid=1");
    int before_ex = mock_bemf_cal_start_ex_calls;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_EQUAL_INT(before_ex + 1, mock_bemf_cal_start_ex_calls);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_MODE_THOROUGH, mock_bemf_cal_config.mode);
    TEST_ASSERT_EQUAL_UINT8(16, mock_bemf_cal_config.points);
    TEST_ASSERT_EQUAL_UINT16(600, mock_bemf_cal_config.settle_ms);
    TEST_ASSERT_EQUAL_UINT16(50, mock_bemf_cal_config.interval_ms);
    TEST_ASSERT_EQUAL_UINT8(10, mock_bemf_cal_config.samples);
    TEST_ASSERT_EQUAL_UINT8(2, mock_bemf_cal_config.retries);
    TEST_ASSERT_TRUE(mock_bemf_cal_config.apply_pid);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));

    reset_resp();
    set_query("start=1&speeds=12,18,24,32,40,52,66,80,100,126");
    before_ex = mock_bemf_cal_start_ex_calls;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_EQUAL_INT(before_ex + 1, mock_bemf_cal_start_ex_calls);
    TEST_ASSERT_EQUAL(MOTOR_BEMF_MODE_CUSTOM, mock_bemf_cal_config.mode);
    TEST_ASSERT_EQUAL_UINT8(10, mock_bemf_cal_config.speed_count);
    TEST_ASSERT_EQUAL_UINT8(12, mock_bemf_cal_config.speed[0]);
    TEST_ASSERT_EQUAL_UINT8(126, mock_bemf_cal_config.speed[9]);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
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
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_enabled);
}

static void test_bemf_status_names_and_previous_curve(void)
{
    httpd_req_t req = make_req(0);
    const char *results[] = {"idle", "running", "saving", "succeeded", "cancelled", "failed"};
    const char *errors[] = {"none", "adc", "rail", "timeout", "control", "curve",
                            "storage", "start", "sensor", "no-rise"};
    mock_bemf_cal_info.run_id = 42;
    mock_bemf_cal_info.valid = mock_bemf_cal_info.stored = true;
    mock_bemf_cal_info.count = 1;
    mock_bemf_cal_info.speed[0] = 5;
    mock_bemf_cal_info.frac[0] = 512;
    mock_bemf_cal_info.error_code = ESP_FAIL;
    for (unsigned i = 0; i < 10; ++i) {
        reset_resp();
        mock_bemf_cal_info.result = (motor_bemf_cal_state_t)(i % 6);
        mock_bemf_cal_info.reason = (motor_bemf_cal_error_t)i;
        TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_get(&req));
        char expected[64];
        snprintf(expected, sizeof(expected), "\"result\":\"%s\"", results[i % 6]);
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, expected));
        snprintf(expected, sizeof(expected), "\"error\":\"%s\"", errors[i]);
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, expected));
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"runId\":42"));
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"errorCode\":-1"));
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"valid\":true,\"stored\":true"));
        TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"speed\":5,\"frac\":50"));
    }
}

static void test_bemf_cancel_bypasses_admission_without_motor_control(void)
{
    httpd_req_t req = make_req(0);
    esp_err_t (*handler)(httpd_req_t *) = bemf_cal_post;
    req.user_ctx = &handler;
    s_cfg.control_source = 0;
    s_actuation_ready = false;
    s_maintenance_owner = (TaskHandle_t)2;
    mock_sem_take_fail = 1;
    mock_motor_speed = 55;
    mock_bemf_cal_info.active = true;
    set_query("cancel=1");
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_bemf_cal_cancel_calls);
    TEST_ASSERT_EQUAL_INT(MOTOR_BEMF_CAL_CANCELLED, mock_bemf_cal_info.result);
    TEST_ASSERT_TRUE(mock_bemf_cal_info.active); /* Worker retains its reservation. */
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_cal_start_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_motor_emergency_stop_calls);
    TEST_ASSERT_EQUAL_INT(55, mock_motor_speed);
    TEST_ASSERT_EQUAL_INT(0, s_mutation_count);
    mock_bemf_cal_info.active = false;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(2, mock_bemf_cal_cancel_calls);
    TEST_ASSERT_EQUAL_INT(55, mock_motor_speed);
    req.content_len = 1;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_INT(2, mock_bemf_cal_cancel_calls);
}

static void test_bemf_action_queries_strict(void)
{
    httpd_req_t req = make_req(0);
    const char *bad[] = {"cancel=", "cancel=2", "cancel=1&start=1", "reset=1&cancel=0",
        "start=1&start=1", "start=0", "reset=maybe", "cancel=%", "cancel=%00", "other=1"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        set_query(bad[i]); reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    }
    memset(mock_query, 'a', WEB_QUERY_MAX);
    mock_query[WEB_QUERY_MAX] = '\0'; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_cal_cancel_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_cal_start_calls);
    set_query("cancel=%31"); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_bemf_cal_cancel_calls);
}

static void test_bemf_start_reset_still_gated(void)
{
    httpd_req_t req = make_req(0);
    set_query("start=1");
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "control busy"));
    mock_sem_take_fail = 0; s_actuation_ready = false; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "actuation unavailable"));
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_cal_start_calls);
    s_actuation_ready = true; set_query("reset=1"); reset_resp();
    mock_bemf_cal_clear_ret = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "calibration busy"));
}

static void test_bemf_mode_durable_before_runtime(void)
{
    httpd_req_t req = make_req(0);
    set_query("enabled=0");
    mock_bemf_use_save_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false,\"enabled\":true"));
    TEST_ASSERT_EQUAL_INT(1, mock_bemf_enabled);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_set_enabled_calls);
    mock_bemf_use_save_ret = ESP_OK; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_enabled);
    TEST_ASSERT_EQUAL_INT(1, mock_bemf_set_enabled_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_set_before_save);
}

static void test_bemf_mode_queries_and_actuation_gates(void)
{
    httpd_req_t req = make_req(0);
    const char *bad[] = {"", "enabled=", "enabled=2", "enabled=1&enabled=0", "enabled=%00", "enabled=%gg"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        set_query(bad[i]); reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    }
    set_query("enabled=0"); reset_resp(); mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "control busy"));
    mock_sem_take_fail = 0; s_actuation_ready = false; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "actuation unavailable"));
    s_actuation_ready = true; s_maintenance_owner = (TaskHandle_t)2; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, bemf_use_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "actuation unavailable"));
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_use_save_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_set_enabled_calls);
    esp_err_t (*handler)(httpd_req_t *) = bemf_use_post;
    req.user_ctx = &handler;
    set_query("enabled=invalid"); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    handler = bemf_cal_post;
    set_query("cancel=1&reset=1"); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_cal_cancel_calls);
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
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "control unavailable"));

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
    set_query("fn=1&state=1");
    TEST_ASSERT_EQUAL(ESP_OK, function_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "control unavailable"));

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
    set_query("ch=1&on=1");
    TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "control unavailable"));

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

static void test_aux_effect_all_channels(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 1;
    for (uint8_t ch = 0; ch < AUXIO_CH_COUNT; ++ch) {
        char q[96];
        snprintf(q, sizeof(q), "ch=%u&on=1&pwm_on=200&pwm_off=10&mode=%u&period=500",
                 (unsigned)ch, (unsigned)(ch % AUXIO_EFFECT_COUNT));
        set_query(q);
        reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
        TEST_ASSERT_EQUAL_UINT8(ch, mock_auxio_last_channel);
        TEST_ASSERT_EQUAL_INT(1, mock_auxio_last_enabled);

        snprintf(q, sizeof(q), "ch=%u&on=0&pwm_on=200&pwm_off=10&mode=0&period=500",
                 (unsigned)ch);
        set_query(q);
        reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, aux_effect_post(&req));
        TEST_ASSERT_EQUAL_UINT8(ch, mock_auxio_last_channel);
        TEST_ASSERT_EQUAL_INT(0, mock_auxio_last_enabled);
    }
}

static void test_aux_state_get(void)
{
    httpd_req_t req = make_req(0);
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    mock_auxio_enabled_state[AUXIO_CH_F0F] = 1;
    mock_auxio_enabled_state[AUXIO_CH_AUX7] = 1;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, aux_state_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"aux\":["));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body,
                                 "{\"ch\":0,\"on\":true,\"error\":false}"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body,
                                 "{\"ch\":8,\"on\":true,\"error\":false}"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body,
                                 "{\"ch\":2,\"on\":false,\"error\":false}"));
}

static void test_dcc_status_get(void)
{
    httpd_req_t req = make_req(0);
    mock_timer_now_us = 1000000;
    mock_dcc_last_packet_us = 950000;
    mock_dcc_overruns = 7;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, dcc_status_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"overruns\":7"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"lastPacketMs\":50"));
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
    /* A finished preview must not stay in the status just because another
     * voice (engine/graph) keeps audio_is_playing() true. */
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&s_preview_voice));
    uint8_t preview_voice = s_preview_voice.voice;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_owned(&s_preview_voice, "horn.wav", false, 80));
    snprintf(s_preview_name, sizeof(s_preview_name), "horn");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_status_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"preview\":\"horn\""));
    mock_audio_owned_state[preview_voice] = AUDIO_VOICE_FINISHED;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_status_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"preview\":\"\""));
    TEST_ASSERT_EQUAL_STRING("", s_preview_name);
    TEST_ASSERT_FALSE(mock_audio_owned_busy[preview_voice]);
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
    TEST_ASSERT_EQUAL_INT(0, mock_audio_stop_calls);
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

static void test_stream_write_success(void)
{
    FILE *f = fopen("web_tmp/audio/pw.bin", "wb");
    httpd_req_t req = make_req(3);
    set_body("abc", 3);
    int total = 0;
    TEST_ASSERT_EQUAL(ESP_OK, pipe_upload(&req, f, &total));
    TEST_ASSERT_EQUAL_INT(3, total);
    fclose(f);
    FILE *r = fopen("web_tmp/audio/pw.bin", "rb");
    char got[8] = { 0 };
    size_t n = fread(got, 1, sizeof(got), r);
    fclose(r);
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)n);
    TEST_ASSERT_EQUAL_STRING("abc", got);
}

static void test_stream_short_body(void)
{
    FILE *f = fopen("web_tmp/audio/pw.bin", "wb");
    httpd_req_t req = make_req(4);
    set_body("abc", 3);
    int total = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, pipe_upload(&req, f, &total));
    fclose(f);
}

static void test_stream_file_write_failure(void)
{
    FILE *w = fopen("web_tmp/audio/pw.bin", "wb");
    fputs("x", w);
    fclose(w);
    FILE *f = fopen("web_tmp/audio/pw.bin", "rb");
    httpd_req_t req = make_req(3);
    set_body("abc", 3);
    int total = 0;
    TEST_ASSERT_EQUAL(ESP_FAIL, pipe_upload(&req, f, &total));
    fclose(f);
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

static bool dir_exists(const char *p)
{
    DIR *d = opendir(p);
    if (d == NULL) return false;
    closedir(d);
    return true;
}

static bool saved_track_exists(size_t index)
{
    char path[160];
    snprintf(path, sizeof(path), "web_tmp/%s", mock_saved_tracks[index].file);
    return file_exists(path);
}

static void pack_test_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static void pack_test_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static size_t pack_test_build(uint8_t *buf, const char *name,
                              const uint8_t *pcm, size_t pcm_len)
{
    size_t nlen = strlen(name);
    uint32_t data_off = (uint32_t)(AUDIO_PACK_HDR_LEN + AUDIO_PACK_ENTRY_HDR_LEN + nlen);
    memset(buf, 0, data_off + pcm_len);
    memcpy(buf, AUDIO_PACK_MAGIC, AUDIO_PACK_MAGIC_LEN);
    pack_test_put16(buf + 8, AUDIO_PACK_VERSION);
    pack_test_put16(buf + 10, 1);
    pack_test_put32(buf + 12, data_off);
    uint8_t *e = buf + AUDIO_PACK_HDR_LEN;
    e[0] = (uint8_t)nlen;
    e[1] = 100;
    e[2] = 1;
    e[3] = AUDIO_PACK_BITS_PCM;
    pack_test_put32(e + 4, 22050);
    pack_test_put32(e + 8, data_off);
    pack_test_put32(e + 12, (uint32_t)pcm_len);
    memcpy(buf + AUDIO_PACK_HDR_LEN + AUDIO_PACK_ENTRY_HDR_LEN, name, nlen);
    memcpy(buf + data_off, pcm, pcm_len);
    return data_off + pcm_len;
}

static const char pack_scheme_fixture[] =
    "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"demo\",\"name\":\"Demo\","
    "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":3,"
    "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
    "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"zoom\":1}}";

static size_t pack_test_build_scheme(uint8_t *buf, const char *name,
                                     const uint8_t *pcm, size_t pcm_len,
                                     const char *scheme, size_t scheme_len)
{
    size_t nlen = strlen(name);
    size_t snlen = strlen(AUDIO_PACK_SCHEME_NAME);
    uint32_t entry1 = AUDIO_PACK_HDR_LEN;
    uint32_t entry2 = entry1 + AUDIO_PACK_ENTRY_HDR_LEN + (uint32_t)nlen;
    uint32_t data_off = entry2 + AUDIO_PACK_ENTRY_HDR_LEN + (uint32_t)snlen;
    memset(buf, 0, data_off + pcm_len + scheme_len);
    memcpy(buf, AUDIO_PACK_MAGIC, AUDIO_PACK_MAGIC_LEN);
    pack_test_put16(buf + 8, AUDIO_PACK_VERSION);
    pack_test_put16(buf + 10, 2);
    pack_test_put32(buf + 12, data_off);
    uint8_t *e = buf + entry1;
    e[0] = (uint8_t)nlen; e[1] = 100; e[2] = 1; e[3] = AUDIO_PACK_BITS_PCM;
    pack_test_put32(e + 4, 22050);
    pack_test_put32(e + 8, data_off);
    pack_test_put32(e + 12, (uint32_t)pcm_len);
    memcpy(buf + entry1 + AUDIO_PACK_ENTRY_HDR_LEN, name, nlen);
    e = buf + entry2;
    e[0] = (uint8_t)snlen; e[1] = 0; e[2] = 0; e[3] = AUDIO_PACK_BITS_SCHEME;
    pack_test_put32(e + 4, 0);
    pack_test_put32(e + 8, data_off + (uint32_t)pcm_len);
    pack_test_put32(e + 12, (uint32_t)scheme_len);
    memcpy(buf + entry2 + AUDIO_PACK_ENTRY_HDR_LEN, AUDIO_PACK_SCHEME_NAME, snlen);
    memcpy(buf + data_off, pcm, pcm_len);
    memcpy(buf + data_off + pcm_len, scheme, scheme_len);
    return data_off + pcm_len + scheme_len;
}

static void test_audio_pack_upload_replaces_library(void)
{
    FILE *old = fopen("web_tmp/audio/old.wav", "wb");
    TEST_ASSERT_NOT_NULL(old);
    TEST_ASSERT_EQUAL_INT(3, (int)fwrite("OLD", 1, 3, old));
    fclose(old);

    uint8_t pcm[16] = { 0 };
    uint8_t pack[128];
    size_t n = pack_test_build(pack, "new", pcm, sizeof(pcm));
    set_body(pack, n);
    httpd_req_t req = make_req(n);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_pack_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/new.wav"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/volumes.txt"));
}

static void test_audio_pack_upload_rolls_back_on_publish_failure(void)
{
    FILE *old = fopen("web_tmp/audio/old.wav", "wb");
    TEST_ASSERT_NOT_NULL(old);
    TEST_ASSERT_EQUAL_INT(3, (int)fwrite("OLD", 1, 3, old));
    fclose(old);

    uint8_t pcm[16] = { 0 };
    uint8_t pack[128];
    size_t n = pack_test_build(pack, "new", pcm, sizeof(pcm));
    set_body(pack, n);
    httpd_req_t req = make_req(n);
    test_rename_fail_at = 2; /* backup succeeds, publication fails */
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_pack_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/new.wav"));
}

static void test_audio_pack_upload_rejects_graph_reference(void)
{
    FILE *old = fopen("web_tmp/audio/old.wav", "wb");
    TEST_ASSERT_NOT_NULL(old);
    TEST_ASSERT_EQUAL_INT(3, (int)fwrite("OLD", 1, 3, old));
    fclose(old);

    uint8_t pcm[16] = { 0 };
    uint8_t pack[128];
    size_t n = pack_test_build(pack, "new", pcm, sizeof(pcm));
    set_body(pack, n);
    httpd_req_t req = make_req(n);
    mock_graph_referenced = true;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_pack_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "graph asset referenced"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/new.wav"));
}

static void test_audio_pack_upload_applies_bundled_scheme(void)
{
    FILE *old = fopen("web_tmp/audio/old.wav", "wb");
    TEST_ASSERT_NOT_NULL(old);
    TEST_ASSERT_EQUAL_INT(3, (int)fwrite("OLD", 1, 3, old));
    fclose(old);

    uint8_t pcm[16] = { 0 };
    uint8_t pack[4096];
    size_t n = pack_test_build_scheme(pack, "new", pcm, sizeof(pcm),
                                      pack_scheme_fixture, strlen(pack_scheme_fixture));
    set_body(pack, n);
    httpd_req_t req = make_req(n);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_pack_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"scheme\":"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/new.wav"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_EQUAL_INT(1, mock_graph_save_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_graph_commit_calls);
}

static void test_audio_pack_upload_rolls_back_when_scheme_fails(void)
{
    FILE *old = fopen("web_tmp/audio/old.wav", "wb");
    TEST_ASSERT_NOT_NULL(old);
    TEST_ASSERT_EQUAL_INT(3, (int)fwrite("OLD", 1, 3, old));
    fclose(old);

    uint8_t pcm[16] = { 0 };
    uint8_t pack[4096];
    size_t n = pack_test_build_scheme(pack, "new", pcm, sizeof(pcm),
                                      pack_scheme_fixture, strlen(pack_scheme_fixture));
    set_body(pack, n);
    httpd_req_t req = make_req(n);

    mock_graph_assets_err = ESP_ERR_INVALID_ARG;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_pack_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_UINT32(0, mock_graph_save_calls);
    TEST_ASSERT_EQUAL_UINT32(0, mock_graph_select_calls);
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/new.wav"));

    mock_graph_assets_err = ESP_OK;
    mock_graph_select_err = ESP_FAIL;
    set_body(pack, n);
    req = make_req(n);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_pack_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_UINT32(1, mock_graph_save_calls);
    TEST_ASSERT_EQUAL_UINT32(1, mock_graph_select_calls);
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/new.wav"));
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
    TEST_ASSERT_EQUAL_STRING("my_file.wav", mock_saved_tracks[0].label);
    TEST_ASSERT_TRUE(saved_track_exists(0));
    TEST_ASSERT_EQUAL_STRING("audio/my_file.wav", mock_saved_tracks[0].file);

    /* Empty header -> default slotN.wav name. */
    reset_resp();
    set_body(data, sizeof(data));
    mock_header_name[0] = '\0';
    set_query("slot=2");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "slot2.wav"));
}

static void test_audio_upload_replaces_and_retains_old(void)
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
    TEST_ASSERT_TRUE(saved_track_exists(0));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_EQUAL_UINT32(2, (uint32_t)mock_saved_tracks_count);
    TEST_ASSERT_EQUAL_STRING("new.wav", mock_saved_tracks[0].label);
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
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
}

static void test_audio_upload_asset_only_over_slots(void)
{
    mock_tracks_count = SETTINGS_MAX_TRACKS;
    for (int i = 0; i < SETTINGS_MAX_TRACKS; ++i) {
        mock_tracks[i].slot = (uint8_t)(i + 1);
        snprintf(mock_tracks[i].file, sizeof(mock_tracks[i].file), "audio/t%d.wav", i);
    }
    uint8_t data[16];
    memset(data, 5, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("asset=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "asset.wav");
    int saves = mock_tracks_save_calls;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/asset.wav"));
    TEST_ASSERT_EQUAL_INT(saves, mock_tracks_save_calls);
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
    clear_files();
    RMDIR("web_tmp/audio");
    uint8_t data[16];
    memset(data, 7, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "adir.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("open failed", mock_resp_send_err_msg);
    MKDIR("web_tmp/audio");
}

static void test_audio_upload_no_mem(void)
{
    uint8_t data[16];
    memset(data, 8, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "m.wav");
    mock_alloc_fail_at = 0; /* Transaction allocation fails before filesystem mutation. */
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("no transaction memory", mock_resp_send_err_msg);
}

static void test_audio_upload_buf_alloc_fail_and_task_fail(void)
{
    uint8_t data[16];
    memset(data, 9, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "m2.wav");
    mock_alloc_fail_at = 1;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    reset_resp();
    set_body(data, sizeof(data));
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, audio_upload_post));
    TEST_ASSERT_EQUAL_STRING("worker creation failed", mock_resp_send_err_msg);
    TEST_ASSERT_EQUAL_UINT32(1, test_complete_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_httpd_async_live);
    TEST_ASSERT_FALSE(web_maintenance_active());
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
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/del.wav"));
    TEST_ASSERT_EQUAL_UINT8(0, s_cfg.active_slot);
    TEST_ASSERT_EQUAL_INT(0, mock_audio_stop_calls);
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
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "tracks load failed"));

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
        "fn=29&a=1&b=0&aux=0&dir=0&speed=0",
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

static void test_logic_bindings_apply_runtime_actions(void)
{
    s_motion_initialized = true;
    s_motion_speed = 30;
    s_motion_forward = true;
    s_func_bind_present = true;
    s_func_bind_count = 3;
    memset(s_func_bind, 0, sizeof(s_func_bind));
    s_func_bind[0].used = 1;
    s_func_bind[0].fn = 1;
    s_func_bind[0].target_type = FUNC_TARGET_LOGIC;
    s_func_bind[0].target_id = FUNC_LOGIC_MUTE_MOVE;
    s_func_bind[1].used = 1;
    s_func_bind[1].fn = 2;
    s_func_bind[1].target_type = FUNC_TARGET_LOGIC;
    s_func_bind[1].target_id = FUNC_LOGIC_DRIVE_HOLD;
    s_func_bind[2].used = 1;
    s_func_bind[2].fn = 3;
    s_func_bind[2].target_type = FUNC_TARGET_LOGIC;
    s_func_bind[2].target_id = FUNC_LOGIC_NOTCH_UP;

    web_apply_function(1, true);
    TEST_ASSERT_TRUE(mock_sound_logic_mute);
    web_apply_function(1, false);
    TEST_ASSERT_FALSE(mock_sound_logic_mute);

    web_apply_function(2, true);
    TEST_ASSERT_TRUE(mock_sound_logic_drive_hold);
    web_apply_function(2, false);
    TEST_ASSERT_FALSE(mock_sound_logic_drive_hold);

    web_apply_function(3, true);
    TEST_ASSERT_EQUAL_INT(1, mock_sound_logic_notch);
    web_apply_function(3, false);
    web_apply_function(3, true);
    TEST_ASSERT_EQUAL_INT(2, mock_sound_logic_notch);
}

static void test_func_map_post_binding(void)
{
    httpd_req_t req = make_req(0);

    reset_resp();
    set_query("bind=1&fn=2&type=1&id=3&dir=1&state=2&mode=3&flags=1&short=7"
              "&short_ms=400&min_ms=150&fade_ms=80");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_EQUAL_UINT32(1, s_func_bind_count);
    TEST_ASSERT_EQUAL_UINT8(2, s_func_bind[0].fn);
    TEST_ASSERT_EQUAL_UINT8(FUNC_TARGET_OUTPUT, s_func_bind[0].target_type);
    TEST_ASSERT_EQUAL_UINT8(3, s_func_bind[0].target_id);
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
        "bind=1&fn=1&type=2&id=5",
        "bind=1&fn=1&type=3&id=1",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        reset_resp();
        set_query(bad[i]);
        TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    }

    reset_resp();
    mock_bind_save_ret = ESP_FAIL;
    set_query("bind=1&fn=3&type=1&id=1");
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
    set_query("bind=1&fn=3&type=1&id=3");
    TEST_ASSERT_EQUAL(ESP_OK, func_map_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "save failed"));

    /* Empty store: direct remove guard. */
    s_func_bind_count = 0;
    TEST_ASSERT_FALSE(web_func_bind_remove(0));
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
    s_func_bind[0].mode = FUNC_MODE_SHORT_LONG;

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
    int before = mock_task_create_calls;
    mock_task_create_ok = 1;
    start_dns_hijack();
    TEST_ASSERT_EQUAL_INT(before + 1, mock_task_create_calls);
    mock_task_create_ok = 0;
    start_dns_hijack();
    TEST_ASSERT_EQUAL_INT(before + 2, mock_task_create_calls); /* failed create still attempted */
    mock_task_create_ok = 1;
}

static void test_stream_idle_retry(void)
{
    FILE *f = fopen("web_tmp/audio/pw.bin", "wb");
    httpd_req_t req = make_req(3);
    set_body("abc", 3);
    mock_recv_idle_n = 1;
    mock_recv_idle_val = HTTPD_SOCK_ERR_TIMEOUT;
    int total = 0;
    TEST_ASSERT_EQUAL(ESP_OK, pipe_upload(&req, f, &total));
    fclose(f);
}

static void test_stream_write_fault(void)
{
    /* A write failure aborts the unpublished staged file. */
    uint8_t data[16];
    memset(data, 1, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "pf.wav");
    mock_ota_fwrite_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static void test_stream_read_failure(void)
{
    uint8_t data[16];
    memset(data, 2, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "pw2.wav");
    mock_recv_fail = 1;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static uint8_t pipe_big[7 * 8192];

static void test_stream_receive_limit(void)
{
    memset(pipe_big, 3, sizeof(pipe_big));
    httpd_req_t req = make_req(sizeof(pipe_big));
    set_body(pipe_big, sizeof(pipe_big));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "pb.wav");
    mock_recv_limit = 0;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("upload failed", mock_resp_send_err_msg);
}

static void test_stream_injected_write_error(void)
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
    s_cfg.auto_off_min = 5;
    snprintf(s_cfg.ap_ip, sizeof(s_cfg.ap_ip), "10.0.0.1");
    s_wifi_started = true;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ap_password\":\"********\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"status\":\"ap_running\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ap_ip\":\"10.0.0.1\""));

    reset_resp();
    s_cfg.ap_password[0] = '\0';
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
    web_maintenance_end(); /* esp_restart does not return on the target. */

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
    web_maintenance_end();

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
    int before_calls = mock_httpd_register_calls;
    uint32_t before_routes = s_route_count;
    mock_httpd_register_err = ESP_FAIL;
    register_route("/x", HTTP_GET, root_handler);
    TEST_ASSERT_EQUAL_INT(before_calls + 1, mock_httpd_register_calls);
    TEST_ASSERT_EQUAL_UINT32(before_routes, s_route_count); /* failed route not counted */
    mock_httpd_register_err = 0;
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
    if (n_files == 0U) return o;
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
    TEST_ASSERT_EQUAL_STRING("My_File.wav", out);
    ota_safe_name("dir/sub/Other.Wav", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Other.wav", out);
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
    body[0] = 0xE9;
    httpd_req_t req = make_req(sizeof(body));
    set_body(body, sizeof(body));
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"bytes\":64"));
    TEST_ASSERT_TRUE(s_transfer_reboot);
    TEST_ASSERT_EQUAL_INT(0, mock_esp_restart_calls);
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
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "ota boot failed"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"boot_selection_uncertain\":true"));
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
    TEST_ASSERT_TRUE(saved_track_exists(0));
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
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
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
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
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
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

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

    /* Unique staging never opens a caller-named directory. */
    reset_resp();
    MKDIR("web_tmp/audio/adir.wav");
    const char *name2 = "adir.wav";
    size_t n2 = build_container(ota_combined, 32, 1, (uint16_t)strlen(name2), 0, 4,
                                name2, NULL);
    req = make_req(n2);
    set_body(ota_combined, n2);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "backup/space preflight failed"));
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
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "metadata incomplete"));
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
    TEST_ASSERT_EQUAL_INT(0, mock_audio_voice_stop_calls);
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
    TEST_ASSERT_EQUAL_STRING("c.wav", mock_saved_tracks[0].label);
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
    ota_long[0] = 0xE9;
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
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);

    /* Truncate after the name: the label read fails. */
    trunc = OTA_CONTAINER_HDR_LEN + 32 + OTA_FILE_HDR_LEN + strlen(name);
    reset_resp();
    req = make_req(trunc);
    set_body(ota_combined, trunc);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
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
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
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

static void test_startup_and_source_leases(void)
{
    s_actuation_ready = false;
    s_func_map[1].aux_mask = 4;
    web_apply_function(1, true);
    web_motion_changed(10, true);
    TEST_ASSERT_FALSE(s_fn[1]);
    TEST_ASSERT_EQUAL_INT(0, mock_auxio_set_enabled_calls);
    uint32_t generation = 99;
    TEST_ASSERT_FALSE(web_control_rails_begin(&generation));
    TEST_ASSERT_EQUAL(ESP_OK, web_set_actuation_ready(true));
    TEST_ASSERT_TRUE(web_control_rails_begin(&generation));
    web_control_rails_end();
    httpd_req_t req = make_req(0);
    set_query("source=web");
    TEST_ASSERT_EQUAL(ESP_OK, control_source_post(&req));
    TEST_ASSERT_TRUE(s_control_generation > generation);
    TEST_ASSERT_FALSE(web_control_rails_begin(NULL));
    set_query("speed=30&forward=1");
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_INT(1, mock_motor_set_speed_calls);
    mock_sem_take_fail = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, web_set_actuation_ready(false));
    TEST_ASSERT_FALSE(actuation_ready());
    mock_sem_take_fail = 0;
}

static void test_maintenance_owner_quiescence_and_timeout(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, web_maintenance_begin(true));
    TEST_ASSERT_TRUE(web_maintenance_active());
    TEST_ASSERT_FALSE(web_control_rails_begin(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, storage_access_begin());
    mock_current_task_handle = (TaskHandle_t)2;
    web_maintenance_end();
    TEST_ASSERT_TRUE(web_maintenance_active());
    mock_current_task_handle = (TaskHandle_t)1;
    web_maintenance_end();
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL(ESP_OK, storage_access_begin());
    storage_access_end();
    test_quiet_block = true;
    int64_t start = mock_timer_now_us;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, web_maintenance_begin(true));
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_TRUE(mock_timer_now_us - start >= 2000000);
    TEST_ASSERT_TRUE(mock_timer_now_us - start <= 2010000);
    test_quiet_block = false;
    TEST_ASSERT_EQUAL(ESP_OK, storage_access_begin());
    storage_access_end();
    test_sound_inhibit_err = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, web_maintenance_begin(false));
    TEST_ASSERT_FALSE(web_maintenance_active());
}

static void test_maintenance_transient_inhibit_timeout_keeps_ready(void)
{
    s_actuation_ready = true;
    /* One-shot ESP_ERR_TIMEOUT from the reserve inhibit. */
    test_sound_inhibit_err = ESP_ERR_TIMEOUT;
    test_sound_inhibit_fail_remaining = 1;
    test_sound_inhibit_calls = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, web_maintenance_begin(false));
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_TRUE(actuation_ready());
    TEST_ASSERT_FALSE(motor_is_inhibited());
    TEST_ASSERT_FALSE(mock_sound_inhibited);
    TEST_ASSERT_FALSE(mock_audio_inhibited);
    TEST_ASSERT_FALSE(test_sound_inhibit_last);
}

static void test_maintenance_persistent_inhibit_timeout_closes_ready(void)
{
    s_actuation_ready = true;
    /* Persistent timeout: the bounded retries still fail, so readiness must
     * close and motor/sound/audio must be re-inhibited. */
    test_sound_inhibit_err = ESP_ERR_TIMEOUT;
    test_sound_inhibit_fail_remaining = 0;
    test_sound_inhibit_calls = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, web_maintenance_begin(false));
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_FALSE(actuation_ready());
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_TRUE(mock_audio_inhibited);
    TEST_ASSERT_TRUE(test_sound_inhibit_last);
    TEST_ASSERT_TRUE(test_sound_inhibit_calls >= 5);
}

static void test_preinit_maintenance_and_readiness(void)
{
    s_control_mutex = NULL;
    s_actuation_ready = false;
    test_sound_inhibit_err = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, web_maintenance_begin(true));
    TEST_ASSERT_NOT_NULL(s_control_mutex);
    web_maintenance_end();
    TEST_ASSERT_FALSE(actuation_ready());
    TEST_ASSERT_FALSE(web_control_rails_begin(NULL));
    test_sound_inhibit_err = ESP_OK;
}

static void test_shared_aux_and_retry_failed_apply(void)
{
    s_func_map[1].aux_mask = s_func_map[2].aux_mask = 4;
    web_apply_function(1, true);
    web_apply_function(2, true);
    int applied = mock_auxio_set_enabled_calls;
    web_apply_function(1, false);
    TEST_ASSERT_EQUAL_INT(applied, mock_auxio_set_enabled_calls);
    TEST_ASSERT_EQUAL_UINT16(4, s_outputs_applied);
    web_apply_function(2, false);
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
    test_aux_enable_err = ESP_ERR_TIMEOUT;
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
    test_aux_enable_err = ESP_OK;
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_UINT16(4, s_outputs_applied);
}

static void test_empty_canonical_store_and_live_remove(void)
{
    s_func_map[1].aux_mask = 4;
    s_func_map[1].slot_a = 1;
    mock_tracks_count = 1;
    mock_tracks[0].slot = 1;
    mock_tracks[0].enabled = true;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/legacy.wav");
    mock_bind_load_ret = ESP_OK;
    mock_binds_count = 0;
    func_bind_load();
    TEST_ASSERT_TRUE(s_func_bind_present);
    TEST_ASSERT_EQUAL_INT(0, mock_bind_save_calls);
    web_apply_function(1, true);
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
    TEST_ASSERT_EQUAL_INT(0, mock_audio_voice_play_calls);
    func_binding_t b = {0};
    b.used = 1; b.fn = 1; b.target_type = FUNC_TARGET_OUTPUT; b.target_id = 2;
    b.dir = FUNC_DIR_ANY; b.state = FUNC_STATE_ANY;
    TEST_ASSERT_TRUE(web_func_bind_add(&b));
    TEST_ASSERT_EQUAL_UINT16(4, s_outputs_applied);
    TEST_ASSERT_TRUE(web_func_bind_remove(0));
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
    TEST_ASSERT_TRUE(s_func_bind_present);
}

static void test_legacy_voices_do_not_collide(void)
{
    mock_tracks_count = 3;
    for (size_t i = 0; i < 3; ++i) {
        mock_tracks[i].slot = (uint8_t)(i + 1);
        mock_tracks[i].enabled = true;
        snprintf(mock_tracks[i].file, sizeof(mock_tracks[i].file), "audio/v%u.wav", (unsigned)i);
    }
    s_func_map[1].slot_a = 1; s_func_map[1].slot_b = 2;
    s_func_map[11].slot_a = 3;
    func_tracks_cache_refresh();
    web_apply_function(1, true);
    web_apply_function(11, true);
    TEST_ASSERT_NOT_EQUAL(s_legacy_voice[1][1].voice, s_legacy_voice[11][0].voice);
    audio_voice_handle_t held = s_legacy_voice[1][1];
    web_apply_function(11, false);
    TEST_ASSERT_NOT_EQUAL(AUDIO_VOICE_FINISHED, audio_voice_get_state(held));
    httpd_req_t req = make_req(0);
    set_query("slot=3");
    TEST_ASSERT_EQUAL(ESP_OK, audio_play_post(&req));
    TEST_ASSERT_NOT_EQUAL(held.voice, s_preview_voice.voice);
    TEST_ASSERT_EQUAL(ESP_OK, audio_stop_post(&req));
    TEST_ASSERT_NOT_EQUAL(AUDIO_VOICE_FINISHED, audio_voice_get_state(held));
}

static void test_same_filename_failure_preserves_original(void)
{
    FILE *f = fopen("web_tmp/audio/shared.wav", "wb");
    fputs("original", f); fclose(f);
    mock_tracks_count = 2;
    for (size_t i = 0; i < 2; ++i) {
        mock_tracks[i].slot = (uint8_t)(i + 1);
        snprintf(mock_tracks[i].file, sizeof(mock_tracks[i].file), "audio/shared.wav");
    }
    const char data[] = "replacement";
    httpd_req_t req = make_req(sizeof(data));
    set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "shared.wav");
    set_body(data, sizeof(data));
    mock_fsync_ret = -1;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    char original[16] = {0};
    f = fopen("web_tmp/audio/shared.wav", "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT32(8, fread(original, 1, sizeof(original), f)); fclose(f);
    TEST_ASSERT_EQUAL_STRING("original", original);
    mock_fsync_ret = 0;
    set_body(data, sizeof(data)); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_TRUE(saved_track_exists(0));
    TEST_ASSERT_EQUAL_STRING("audio/shared.wav", mock_saved_tracks[1].file);
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/shared.wav"));
}

static void test_async_transfer_admission_completion_and_readonly(void)
{
    const char body[] = "staged wave";
    httpd_req_t req = make_req(sizeof(body));
    set_body(body, sizeof(body)); set_query("slot=1");
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, audio_upload_post));
    TEST_ASSERT_NOT_NULL(test_pending_transfer);
    TEST_ASSERT_TRUE(web_maintenance_active());
    TEST_ASSERT_EQUAL_INT(0, mock_recv_calls);
    TEST_ASSERT_EQUAL(ESP_OK, progress_handler(&req));
    TEST_ASSERT_EQUAL(ESP_OK, device_get(&req));
    TEST_ASSERT_FALSE(web_control_rails_begin(NULL));
    int before = mock_motor_set_speed_calls;
    set_query("speed=60"); s_cfg.control_source = 1;
    TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
    TEST_ASSERT_EQUAL_INT(before, mock_motor_set_speed_calls);
    set_query("slot=1");
    void *job = test_pending_transfer; test_pending_transfer = NULL;
    mock_current_task_handle = (TaskHandle_t)2;
    transfer_worker(job);
    TEST_ASSERT_EQUAL_UINT32(1, test_complete_calls);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_FALSE(s_up_active);
    TEST_ASSERT_TRUE(saved_track_exists(0));
    mock_current_task_handle = (TaskHandle_t)1;
    mock_task_create_ok = 0;
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, audio_upload_post));
    TEST_ASSERT_EQUAL_UINT32(2, test_complete_calls);
    TEST_ASSERT_FALSE(web_maintenance_active());
}

static void test_ota_whole_container_before_boot(void)
{
    const char *name = "slot1.wav";
    size_t n = build_container(ota_combined, 32, 1, (uint16_t)strlen(name), 0, 8, name, NULL);
    httpd_req_t req = make_req(n);
    test_require_metadata = true;
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(1, test_boot_calls);
    TEST_ASSERT_FALSE(test_boot_order_error);
    test_boot_calls = 0; s_transfer_reboot = false;
    req.content_len = n + 1U; ota_combined[n] = 0xAA;
    set_body(ota_combined, n + 1U); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_FALSE(s_transfer_reboot);
    req.content_len = n;
    set_body(ota_combined, n); reset_resp();
    mock_tracks_save_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"metadata_uncertain\":true"));
}

static void test_wifi_exact_lengths_and_safe_addresses(void)
{
    settings_config_t cfg = make_wifi_cfg();
    memset(cfg.ap_ssid, 'S', 32); cfg.ap_ssid[32] = '\0';
    memset(cfg.ap_password, 'a', 64); cfg.ap_password[64] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_EQUAL_UINT8(32, test_ap_config.ap.ssid_len);
    TEST_ASSERT_EQUAL_MEMORY(cfg.ap_ssid, test_ap_config.ap.ssid, 32);
    TEST_ASSERT_EQUAL_MEMORY(cfg.ap_password, test_ap_config.ap.password, 64);
    cfg.ap_password[63] = 'g';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_start(&cfg));
    esp_ip4_addr_t ip;
    const char *invalid[] = {"10.0.0.0", "10.0.0.255", "127.0.0.1", "224.1.2.3", "0.1.2.3", "255.255.255.255",
                             "10.1", "0x0A000001", "10.0.0.1x", "10.0.0.999", "10.0.0.", "010.0.0.1", "10.00.0.1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        TEST_ASSERT_FALSE(parse_ip4(invalid[i], &ip));
    TEST_ASSERT_TRUE(parse_ip4("10.0.0.1", &ip));
    httpd_req_t req = make_req(0);
    set_query("ap_ssid=%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0%D0%B0");
    TEST_ASSERT_EQUAL(ESP_OK, wifi_post(&req));
    TEST_ASSERT_EQUAL_UINT32(32, strlen(s_cfg.ap_ssid));
    web_maintenance_end();
    int saved = mock_settings_save_calls;
    const char *bad[] = {"ap_ssid=%00evil", "ap_ssid=%A", "ap_password=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaag",
                         "ap_ssid=123456789012345678901234567890123", "ap_ip=10.0.0.255"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        set_query(bad[i]); reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, wifi_post(&req));
        TEST_ASSERT_EQUAL_INT(saved, mock_settings_save_calls);
    }
}

static void test_log_cursor_tracks_last_transmitted_entry(void)
{
    char text[WEB_EVLOG_TEXT_MAX];
    memset(text, '"', sizeof(text) - 1); text[sizeof(text) - 1] = '\0';
    for (unsigned i = 0; i < WEB_EVLOG_MAX; ++i) web_log_event("entry", "%s", text);
    httpd_req_t req = make_req(0);
    set_query("since=0");
    TEST_ASSERT_EQUAL(ESP_OK, log_get(&req));
    const char *cursor = strstr(mock_resp_body, "\"seq\":");
    TEST_ASSERT_NOT_NULL(cursor);
    unsigned long sent = strtoul(cursor + 6, NULL, 10);
    TEST_ASSERT_TRUE(sent > 0 && sent < WEB_EVLOG_MAX);
    char next[32]; snprintf(next, sizeof(next), "since=%lu", sent);
    set_query(next); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, log_get(&req));
    char first[32]; snprintf(first, sizeof(first), "\"s\":%lu,", sent + 1);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, first));
}

static void test_async_begin_failure_and_exclusive_admission(void)
{
    httpd_req_t req = make_req(16);
    mock_httpd_async_begin_err = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, audio_upload_post));
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_INT(0, mock_httpd_async_live);
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_shutdown_calls);
    mock_httpd_async_begin_err = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, web_maintenance_begin(false));
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, audio_upload_post));
    TEST_ASSERT_NULL(test_pending_transfer);
    TEST_ASSERT_TRUE(web_maintenance_active());
    TEST_ASSERT_EQUAL_INT(0, mock_httpd_async_live);
    web_maintenance_end();
    s_transfer_deadline = mock_timer_now_us + 1;
    ++mock_timer_now_us;
    FILE *f = fopen("web_tmp/audio/deadline.bin", "wb");
    int total = -1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, pipe_upload(&req, f, &total)); fclose(f);
    TEST_ASSERT_EQUAL_INT(0, total);
}

static unsigned staged_file_count(void)
{
    unsigned count = 0;
    DIR *dir = opendir("web_tmp/audio");
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *extension = strrchr(entry->d_name, '.');
            if (extension != NULL && (strcmp(extension, ".tmp") == 0 || strcmp(extension, ".bak") == 0)) ++count;
        }
        closedir(dir);
    }
    return count;
}

static void test_ota_invalid_subset_never_publishes(void)
{
    size_t n = build_two_file_container(ota_combined);
    httpd_req_t req = make_req(n - 1U);
    set_body(ota_combined, n - 1U);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
    reset_resp();
    req.content_len = n;
    mock_tracks_load_ret = ESP_FAIL;
    set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    mock_tracks_load_ret = ESP_OK;
    uint8_t invalid[64] = {0};
    req.content_len = sizeof(invalid); set_body(invalid, sizeof(invalid)); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_STRING("invalid firmware header", mock_resp_send_err_msg);
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
}

static void test_ota_boot_failure_restores_metadata_and_preserves_files(void)
{
    FILE *f = fopen("web_tmp/audio/original.wav", "wb");
    fputs("original", f); fclose(f);
    mock_tracks_count = 1;
    mock_tracks[0].slot = 1;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/original.wav");
    size_t n = build_container(ota_combined, 32, 1, 9, 0, 8, "slot1.wav", NULL);
    httpd_req_t req = make_req(n);
    set_body(ota_combined, n);
    mock_ota_set_boot_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(1, test_boot_calls);
    TEST_ASSERT_EQUAL_INT(2, mock_tracks_save_calls);
    TEST_ASSERT_EQUAL_STRING("audio/original.wav", mock_saved_tracks[0].file);
    TEST_ASSERT_EQUAL_UINT32(1, staged_file_count());
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/original.wav"));
    TEST_ASSERT_FALSE(s_transfer_reboot);
    mock_tracks_save_ret = ESP_FAIL;
    set_body(ota_combined, n); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(2, staged_file_count());
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/original.wav"));
    TEST_ASSERT_EQUAL_UINT32(1, test_boot_calls);
}

static void test_maintenance_existing_leases_and_no_resurrection(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, storage_access_begin());
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, web_maintenance_begin(true));
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_INT(1, mock_storage_access_leases);
    TEST_ASSERT_NULL(mock_storage_maintenance_owner);
    storage_access_end();
    mock_motor_speed = 45;
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_alloc_owned(&s_preview_voice));
    TEST_ASSERT_EQUAL(ESP_OK, audio_voice_play_owned(&s_preview_voice, "sample.wav", true, 50));
    TEST_ASSERT_EQUAL(ESP_OK, web_maintenance_begin(false));
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_TRUE(mock_audio_inhibited && mock_sound_inhibited);
    TEST_ASSERT_EQUAL(ESP_OK, storage_access_begin());
    storage_access_end();
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, true));
    web_maintenance_end();
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL_UINT8(0, mock_motor_speed);
    TEST_ASSERT_EQUAL(AUDIO_VOICE_FINISHED, audio_voice_get_state(s_preview_voice));
    TEST_ASSERT_FALSE(mock_audio_inhibited || mock_sound_inhibited);
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibit_reason(MOTOR_INHIBIT_SAFETY, false));
}

static void test_async_quiescence_failure_completes_and_closes(void)
{
    httpd_req_t req = make_req(16);
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, audio_upload_post));
    TEST_ASSERT_NOT_NULL(test_pending_transfer);
    void *job = test_pending_transfer; test_pending_transfer = NULL;
    test_quiet_block = true;
    transfer_worker(job);
    TEST_ASSERT_EQUAL_UINT32(1, test_complete_calls);
    TEST_ASSERT_TRUE(test_complete_gate_closed);
    TEST_ASSERT_EQUAL_INT(0, mock_httpd_async_live);
    TEST_ASSERT_EQUAL_INT(0, mock_recv_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_httpd_shutdown_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_httpd_trigger_close_calls);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_INT(0, mock_storage_access_leases);
}

static void test_web_init_required_maps_and_outputs_fail_closed(void)
{
    s_actuation_ready = false;
    mock_settings_load_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, web_init());
    TEST_ASSERT_FALSE(web_control_rails_begin(NULL));
    mock_settings_load_ret = ESP_OK;
    mock_map_load_ret = ESP_ERR_INVALID_SIZE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, web_init());
    TEST_ASSERT_FALSE(web_control_rails_begin(NULL));
    mock_map_load_ret = ESP_OK;
    mock_auxio_init_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, web_init());
    TEST_ASSERT_FALSE(web_control_rails_begin(NULL));
    TEST_ASSERT_FALSE(actuation_ready());
}

static void test_bad_actuation_parameters_do_not_fall_back(void)
{
    httpd_req_t req = make_req(0);
    s_cfg.control_source = 1;
    const char *bad[] = {"speed=126000000000000000000&forward=1", "speed=12&forward=truejunk", "forward=true%00junk"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        set_query(bad[i]); reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, motor_post(&req));
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
        TEST_ASSERT_EQUAL_INT(0, mock_motor_set_speed_calls);
    }
    set_query("reset=garbage"); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, bemf_cal_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_INT(0, mock_bemf_cal_start_calls);
    req.content_len = 16;
    snprintf(mock_header_name, sizeof(mock_header_name), "bad%%00.wav");
    set_query("slot=1"); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
}

static void test_ap_configuration_and_dhcp_errors_propagate(void)
{
    settings_config_t cfg = make_wifi_cfg();
    esp_err_t *failures[] = {&test_ip_err, &test_dhcp_stop_err, &test_dhcp_start_err, &test_dns_err};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
        *failures[i] = ESP_FAIL;
        TEST_ASSERT_EQUAL(ESP_FAIL, wifi_start(&cfg));
        TEST_ASSERT_FALSE(s_wifi_started);
        *failures[i] = ESP_OK;
    }
    test_dhcp_start_err = ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED;
    test_dhcp_stop_err = ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    TEST_ASSERT_EQUAL(ESP_OK, wifi_start(&cfg));
    TEST_ASSERT_TRUE(s_wifi_started);
}

static void wr_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8);
}

static void wr_le32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4U; ++i) out[i] = (uint8_t)(value >> (8U * i));
}

static void make_pcm_wave(uint8_t out[48], uint8_t sample)
{
    memset(out, 0, 48);
    memcpy(out, "RIFF", 4); wr_le32(out + 4, 40);
    memcpy(out + 8, "WAVEfmt ", 8); wr_le32(out + 16, 16);
    wr_le16(out + 20, 1); wr_le16(out + 22, 1);
    wr_le32(out + 24, 44100); wr_le32(out + 28, 88200);
    wr_le16(out + 32, 2); wr_le16(out + 34, 16);
    memcpy(out + 36, "data", 4); wr_le32(out + 40, 4);
    memset(out + 44, sample, 4);
}

static void write_fixture(const char *path, const void *bytes, size_t len)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    size_t written = fwrite(bytes, 1, len, f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT32(len, written);
}

static void assert_fixture(const char *path, const void *bytes, size_t len)
{
    uint8_t actual[128];
    TEST_ASSERT_TRUE(len <= sizeof(actual));
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    size_t got = fread(actual, 1, sizeof(actual), f);
    fclose(f);
    TEST_ASSERT_EQUAL_UINT32(len, got);
    TEST_ASSERT_EQUAL_MEMORY(bytes, actual, len);
}

static size_t build_named_pair(uint8_t *out, const char *first, const char *second)
{
    memcpy(out, OTA_CONTAINER_MAGIC, 8);
    wr_le32(out + 8, 32); wr_le32(out + 12, 2);
    memset(out + 16, 0, 32); out[16] = 0xE9;
    size_t offset = 48;
    const char *names[2] = {first, second};
    for (size_t i = 0; i < 2U; ++i) {
        size_t n = strlen(names[i]);
        wr_le16(out + offset, (uint16_t)n);
        wr_le16(out + offset + 2, 0); wr_le32(out + offset + 4, 48);
        offset += 8;
        memcpy(out + offset, names[i], n); offset += n;
        make_pcm_wave(out + offset, (uint8_t)(20 + i)); offset += 48;
    }
    return offset;
}

static void seed_named_assets(void)
{
    uint8_t old[48];
    make_pcm_wave(old, 1); write_fixture("web_tmp/audio/idle.wav", old, sizeof(old));
    make_pcm_wave(old, 2); write_fixture("web_tmp/audio/brake.wav", old, sizeof(old));
    mock_tracks_count = 2;
    mock_tracks[0].slot = 1; mock_tracks[1].slot = 2;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/idle.wav");
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/brake.wav");
}

static void test_named_upload_preserves_persisted_mds_reference(void)
{
    uint8_t wave[48]; make_pcm_wave(wave, 10);
    const char *imported_file = "audio/idle.wav";
    char resolved[160];
    snprintf(resolved, sizeof(resolved), "web_tmp/%s", imported_file);
    httpd_req_t req = make_req(sizeof(wave));
    set_query("slot=1"); snprintf(mock_header_name, sizeof(mock_header_name), "idle.wav");
    set_body(wave, sizeof(wave));
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("audio/idle.wav", mock_saved_tracks[0].file);
    assert_fixture(resolved, wave, sizeof(wave));
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
    make_pcm_wave(wave, 11); set_body(wave, sizeof(wave)); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    assert_fixture(resolved, wave, sizeof(wave));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
}

static void test_named_replacement_failure_preserves_original(void)
{
    seed_named_assets();
    uint8_t old[48], next[48]; make_pcm_wave(old, 1); make_pcm_wave(next, 9);
    httpd_req_t req = make_req(sizeof(next));
    set_query("slot=1"); snprintf(mock_header_name, sizeof(mock_header_name), "idle.wav");
    set_body(next, sizeof(next) - 1);
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    assert_fixture("web_tmp/audio/idle.wav", old, sizeof(old));
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    set_body(next, sizeof(next)); reset_resp(); mock_audio_validate_ret = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    assert_fixture("web_tmp/audio/idle.wav", old, sizeof(old));
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    mock_audio_validate_ret = ESP_OK; test_manifest_err = ESP_FAIL;
    set_body(next, sizeof(next)); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    assert_fixture("web_tmp/audio/idle.wav", old, sizeof(old));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"metadata_uncertain\":true"));
    TEST_ASSERT_TRUE(staged_file_count() > 0U);
}

static void test_named_slot_rebind_retains_other_project_assets(void)
{
    seed_named_assets();
    uint8_t old[48], next[48]; make_pcm_wave(old, 1); make_pcm_wave(next, 12);
    httpd_req_t req = make_req(sizeof(next));
    set_query("slot=1"); snprintf(mock_header_name, sizeof(mock_header_name), "new_idle.wav");
    set_body(next, sizeof(next));
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("audio/new_idle.wav", mock_saved_tracks[0].file);
    assert_fixture("web_tmp/audio/new_idle.wav", next, sizeof(next));
    assert_fixture("web_tmp/audio/idle.wav", old, sizeof(old));
    make_pcm_wave(old, 2); assert_fixture("web_tmp/audio/brake.wav", old, sizeof(old));
    TEST_ASSERT_EQUAL_STRING("audio/brake.wav", mock_saved_tracks[1].file);
}

static void test_combined_ota_named_assets_and_metadata_before_boot(void)
{
    seed_named_assets();
    size_t n = build_named_pair(ota_combined, "idle.wav", "brake.wav");
    httpd_req_t req = make_req(n); set_body(ota_combined, n);
    test_require_metadata = true;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    uint8_t wave[48]; make_pcm_wave(wave, 20); assert_fixture("web_tmp/audio/idle.wav", wave, sizeof(wave));
    make_pcm_wave(wave, 21); assert_fixture("web_tmp/audio/brake.wav", wave, sizeof(wave));
    TEST_ASSERT_EQUAL_STRING("audio/idle.wav", mock_saved_tracks[0].file);
    TEST_ASSERT_EQUAL_STRING("audio/brake.wav", mock_saved_tracks[1].file);
    TEST_ASSERT_TRUE(test_manifest_calls > 0U);
    TEST_ASSERT_EQUAL_UINT32(1, test_boot_calls);
    TEST_ASSERT_FALSE(test_boot_order_error);
    TEST_ASSERT_TRUE(s_transfer_reboot);
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
}

static void test_combined_ota_duplicate_canonical_names_preserve_originals(void)
{
    seed_named_assets();
    size_t n = build_named_pair(ota_combined, "idle.wav", "idle.wav");
    httpd_req_t req = make_req(n); set_body(ota_combined, n);
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    uint8_t wave[48]; make_pcm_wave(wave, 1); assert_fixture("web_tmp/audio/idle.wav", wave, sizeof(wave));
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
}

static void test_combined_ota_publication_failure_rolls_back_all_files(void)
{
    seed_named_assets();
    size_t n = build_named_pair(ota_combined, "idle.wav", "brake.wav");
    httpd_req_t req = make_req(n); set_body(ota_combined, n);
    test_rename_fail_at = 2;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    uint8_t wave[48]; make_pcm_wave(wave, 1); assert_fixture("web_tmp/audio/idle.wav", wave, sizeof(wave));
    make_pcm_wave(wave, 2); assert_fixture("web_tmp/audio/brake.wav", wave, sizeof(wave));
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"partial\":false"));
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
}

static void test_combined_ota_rollback_failure_retains_recovery_evidence(void)
{
    seed_named_assets();
    size_t n = build_named_pair(ota_combined, "idle.wav", "brake.wav");
    httpd_req_t req = make_req(n); set_body(ota_combined, n);
    mock_ota_set_boot_err = ESP_FAIL; test_rename_fail_at = 3;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_FALSE(s_transfer_reboot);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"partial\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"boot_selection_uncertain\":true"));
    const char *value = strstr(mock_resp_body, "\"recovery\":\"");
    TEST_ASSERT_NOT_NULL(value); value += strlen("\"recovery\":\"");
    const char *end = strchr(value, '"'); TEST_ASSERT_NOT_NULL(end);
    char path[160]; size_t len = (size_t)(end - value);
    TEST_ASSERT_TRUE(len < sizeof(path)); memcpy(path, value, len); path[len] = '\0';
    TEST_ASSERT_TRUE(file_exists(path)); TEST_ASSERT_EQUAL_STRING(".bak", strrchr(path, '.'));
    FILE *f = fopen(path, "rb"); TEST_ASSERT_NOT_NULL(f);
    char magic[8]; uint32_t counts[2];
    size_t magic_n = fread(magic, 1, sizeof(magic), f);
    size_t counts_n = fread(counts, 1, sizeof(counts), f);
    TEST_ASSERT_EQUAL_UINT32(8, magic_n); TEST_ASSERT_EQUAL_UINT32(8, counts_n);
    TEST_ASSERT_EQUAL_MEMORY("WAVTXN1", magic, 8);
    TEST_ASSERT_EQUAL_UINT32(2, counts[0]); TEST_ASSERT_EQUAL_UINT32(2, counts[1]);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, (long)(sizeof(settings_config_t) + 2 * sizeof(settings_track_t)), SEEK_CUR));
    wav_file_tx_t records[2]; size_t got = fread(records, sizeof(records[0]), 2, f); fclose(f);
    TEST_ASSERT_EQUAL_UINT32(2, got);
    uint8_t wave[48]; make_pcm_wave(wave, 2); assert_fixture(records[1].backup, wave, sizeof(wave));
    make_pcm_wave(wave, 1); assert_fixture("web_tmp/audio/idle.wav", wave, sizeof(wave));
}

static void test_combined_ota_backup_space_preflight_preserves_originals(void)
{
    seed_named_assets();
    size_t n = build_named_pair(ota_combined, "idle.wav", "brake.wav");
    httpd_req_t req = make_req(n); set_body(ota_combined, n);
    mock_storage_free = 65536U + 48U;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "backup/space preflight failed"));
    uint8_t wave[48]; make_pcm_wave(wave, 1); assert_fixture("web_tmp/audio/idle.wav", wave, sizeof(wave));
    make_pcm_wave(wave, 2); assert_fixture("web_tmp/audio/brake.wav", wave, sizeof(wave));
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
}

static void test_named_shared_asset_overwrite_updates_all_references(void)
{
    seed_named_assets();
    snprintf(mock_tracks[1].file, sizeof(mock_tracks[1].file), "audio/idle.wav");
    uint8_t wave[48]; make_pcm_wave(wave, 30);
    httpd_req_t req = make_req(sizeof(wave)); set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "idle.wav"); set_body(wave, sizeof(wave));
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_STRING("audio/idle.wav", mock_saved_tracks[0].file);
    TEST_ASSERT_EQUAL_STRING("audio/idle.wav", mock_saved_tracks[1].file);
    assert_fixture("web_tmp/audio/idle.wav", wave, sizeof(wave));
}

static void test_combined_ota_transaction_heap_failure_leaves_originals(void)
{
    seed_named_assets();
    size_t n = build_named_pair(ota_combined, "idle.wav", "brake.wav");
    httpd_req_t req = make_req(n); set_body(ota_combined, n);
    mock_alloc_fail_at = 2;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_send_err_msg, "no transaction memory"));
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_tracks_save_calls);
    uint8_t wave[48]; make_pcm_wave(wave, 1); assert_fixture("web_tmp/audio/idle.wav", wave, sizeof(wave));
    TEST_ASSERT_EQUAL_UINT32(0, staged_file_count());
}

static const char graph_fixture[] =
    "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"demo\",\"name\":\"Demo\","
    "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":3,"
    "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
    "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"zoom\":1}}";

static const char default_graph_fixture[] =
    "{\"format\":\"sound-graph\",\"schemaVersion\":1,\"id\":\"" SG_DEFAULT_PROJECT_ID "\",\"name\":\"ChME3\","
    "\"engine\":{\"entry\":\"off\",\"fn\":1},\"hysteresis\":3,"
    "\"states\":[{\"id\":\"off\",\"name\":\"Off\",\"file\":\"\",\"loop\":false,\"volume\":100,\"rate\":1000}],"
    "\"transitions\":[],\"effects\":[],\"assets\":[],\"editor\":{\"zoom\":1}}";

static httpd_req_t graph_request(esp_err_t (*handler)(httpd_req_t *), const char *body)
{
    httpd_req_t req = make_req(body ? strlen(body) : 0);
    snprintf(mock_content_type, sizeof(mock_content_type), "application/json; charset=utf-8");
    if (body) set_body(body, req.content_len);
    s_route_handlers[63] = handler; req.user_ctx = &s_route_handlers[63];
    return req;
}

static int test_mutation_calls;
static esp_err_t test_mutation_handler(httpd_req_t *req)
{
    (void)req;
    ++test_mutation_calls;
    return ESP_OK;
}

static void test_mutation_dispatch_csrf_guard(void)
{
    httpd_req_t req = make_req(0);
    s_route_handlers[63] = test_mutation_handler;
    req.user_ctx = &s_route_handlers[63];
    test_mutation_calls = 0;

    /* Cross-origin POST is rejected before the handler runs. */
    set_csrf_headers("http://evil.example", NULL, "192.168.4.1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(403, mock_resp_send_err_code);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_send_err_msg, "cross-origin"));
    TEST_ASSERT_EQUAL_INT(0, test_mutation_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_mutation_count);

    /* A matching Origin (default port normalised) is accepted. */
    set_csrf_headers("http://192.168.4.1:80", NULL, "192.168.4.1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(1, test_mutation_calls);
    TEST_ASSERT_EQUAL_UINT32(0, s_mutation_count);
    TEST_ASSERT_EQUAL_INT(0, mock_resp_send_err_code);

    /* A matching Referer is also accepted. */
    set_csrf_headers(NULL, "http://192.168.4.1/index.html", "192.168.4.1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(2, test_mutation_calls);

    /* Non-browser clients (no Origin/Referer) keep working. */
    set_csrf_headers(NULL, NULL, NULL);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(3, test_mutation_calls);

    /* Origin: null is rejected. */
    set_csrf_headers("null", NULL, "192.168.4.1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_INT(403, mock_resp_send_err_code);
    TEST_ASSERT_EQUAL_INT(3, test_mutation_calls);
}

static void seed_graph(void)
{
    snprintf(mock_graph_json, sizeof(mock_graph_json), "%s", graph_fixture);
    mock_graph_revision = 1;
    mock_graph_status.active = true;
    snprintf(mock_graph_status.id, sizeof(mock_graph_status.id), "previous");
    mock_graph_status.revision = 7;
    set_query("id=demo&revision=1");
}

static void test_graph_save_validate_do_not_actuate(void)
{
    set_query("id=demo&expectedRevision=0");
    httpd_req_t req = graph_request(graph_save_post, graph_fixture);
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"revision\":1"));
    TEST_ASSERT_EQUAL_STRING(graph_fixture, mock_graph_json);
    TEST_ASSERT_EQUAL_UINT32(0, mock_motor_emergency_stop_calls);
    TEST_ASSERT_FALSE(mock_audio_inhibited);
    TEST_ASSERT_FALSE(mock_sound_inhibited);
    TEST_ASSERT_EQUAL_UINT32(0, s_mutation_count);
    reset_resp();
    req = graph_request(graph_validate_post, graph_fixture);
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"valid\":true"));
    TEST_ASSERT_EQUAL_UINT32(0, mock_motor_emergency_stop_calls);
    reset_resp();
    req = graph_request(graph_save_post, graph_fixture);
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("409 Conflict", mock_resp_status);
    TEST_ASSERT_EQUAL_UINT32(1, mock_graph_revision);
}

static void test_graph_body_bounds_mime_and_eof(void)
{
    httpd_req_t req = graph_request(graph_validate_post, "{}");
    req.content_len = SG_MAX_JSON + 1U;
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("413 Payload Too Large", mock_resp_status);
    TEST_ASSERT_EQUAL_UINT32(0, mock_recv_calls);
    TEST_ASSERT_EQUAL_UINT32(1, mock_httpd_shutdown_calls);
    reset_resp(); req = graph_request(graph_validate_post, "{}");
    snprintf(mock_content_type, sizeof(mock_content_type), "text/plain");
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("415 Unsupported Media Type", mock_resp_status);
    reset_resp(); req = graph_request(graph_validate_post, "{}");
    req.content_len = 3;
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", mock_resp_status);
    reset_resp(); req = graph_request(graph_validate_post, "{}");
    const char nul[] = {'{', 0}; set_body(nul, sizeof(nul));
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", mock_resp_status);
    reset_resp(); req = graph_request(graph_validate_post, "{}");
    mock_recv_idle_n = 4; mock_recv_idle_val = HTTPD_SOCK_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", mock_resp_status);
    reset_resp(); req = graph_request(graph_validate_post, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", mock_resp_status);
}

static void test_graph_schema_and_allocation_failures(void)
{
    httpd_req_t req = graph_request(graph_validate_post, "{\"schemaVersion\":2}");
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"valid\":false"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"diagnostics\":[{"));
    reset_resp(); req = graph_request(graph_validate_post, graph_fixture);
    mock_alloc_fail_at = 0;
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", mock_resp_status);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "Connection: close"));
    reset_resp(); req = graph_request(graph_save_post, "{}"); set_query("id=demo&expectedRevision=4294967296");
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("400 Bad Request", mock_resp_status);
}

static void test_graph_exact_max_body_and_trailing_json(void)
{
    char *body = malloc(SG_MAX_JSON + 1U);
    TEST_ASSERT_NOT_NULL(body);
    memset(body, ' ', SG_MAX_JSON);
    memcpy(body, graph_fixture, strlen(graph_fixture)); body[SG_MAX_JSON] = '\0';
    httpd_req_t req = graph_request(graph_validate_post, body);
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"valid\":true"));
    TEST_ASSERT_EQUAL_UINT32(SG_MAX_JSON, mock_recv_bytes);
    TEST_ASSERT_EQUAL_UINT32(32, mock_recv_calls);
    reset_resp(); body[SG_MAX_JSON - 1U] = '{';
    req = graph_request(graph_validate_post, body);
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"valid\":false"));
    free(body);
}

static void test_graph_maintenance_and_binding_mutation_guards(void)
{
    httpd_req_t req = graph_request(graph_save_post, graph_fixture);
    set_query("id=demo&expectedRevision=0");
    s_maintenance_owner = (TaskHandle_t)2;
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", mock_resp_status);
    TEST_ASSERT_EQUAL_UINT32(0, mock_recv_calls);
    TEST_ASSERT_EQUAL_UINT32(0, mock_graph_save_calls);
    s_maintenance_owner = NULL; mock_graph_status.active = true;
    s_func_bind_count = 1; s_func_bind[0].target_type = FUNC_TARGET_SOUND;
    reset_resp(); TEST_ASSERT_EQUAL(ESP_OK, func_map_post_binding(&req, "remove=1&idx=0"));
    TEST_ASSERT_EQUAL_STRING("409 Conflict", mock_resp_status);
    TEST_ASSERT_EQUAL_UINT32(1, s_func_bind_count);
    reset_resp(); TEST_ASSERT_EQUAL(ESP_OK, func_map_post_binding(&req, "fn=2&type=1&id=0"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_UINT32(2, s_func_bind_count);
}

static void test_graph_apply_motion_and_transaction_failures(void)
{
    seed_graph();
    httpd_req_t req = graph_request(graph_apply_post, NULL);
    mock_motor_speed = 1;
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("409 Conflict", mock_resp_status);
    TEST_ASSERT_EQUAL_UINT32(0, mock_motor_emergency_stop_calls);
    mock_motor_speed = 0; mock_motor_applied_speed = 1; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_UINT32(0, mock_motor_emergency_stop_calls);
    mock_motor_applied_speed = 0; mock_graph_prepare_err = ESP_ERR_NO_MEM; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("503 Service Unavailable", mock_resp_status);
    TEST_ASSERT_EQUAL_UINT32(0, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_UINT32(0, mock_motor_emergency_stop_calls);
    mock_graph_prepare_err = ESP_OK; mock_graph_assets_err = ESP_ERR_INVALID_ARG; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_UINT32(0, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_UINT32(0, mock_graph_commit_calls);
    TEST_ASSERT_EQUAL_STRING("previous", mock_graph_status.id);
    mock_graph_assets_err = ESP_OK; mock_graph_select_err = ESP_FAIL; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_UINT32(0, mock_graph_commit_calls);
    TEST_ASSERT_EQUAL_STRING("previous", mock_graph_status.id);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_UINT32(0, mock_storage_access_leases);
    mock_graph_select_err = ESP_OK; s_fn[2] = true; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_UINT32(1, mock_graph_commit_calls);
    TEST_ASSERT_EQUAL_STRING("demo", mock_graph_status.id);
    TEST_ASSERT_TRUE((mock_graph_primed & (1U << 2)) != 0);
}

static void test_graph_delete_project(void)
{
    httpd_req_t req = make_req(0);
    set_query("id=demo");
    mock_graph_delete_selected = true;
    mock_graph_status.active = true;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_project_delete_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_INT(1, mock_graph_delete_calls);
    TEST_ASSERT_FALSE(mock_graph_status.active);

    mock_graph_delete_err = ESP_ERR_INVALID_STATE;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_project_delete_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
}

static void test_graph_reference_post(void)
{
    httpd_req_t req = make_req(0);
    uint32_t leases = mock_storage_access_leases;
    mock_motor_speed = 0; mock_motor_applied_speed = 0;

    /* Success: the latest built-in revision is selected and installed. */
    snprintf(mock_graph_json, sizeof(mock_graph_json), "%s", default_graph_fixture);
    mock_graph_revision = 4;
    test_list_default_project = true;
    test_list_default_revision = 4;
    mock_graph_select_err = ESP_OK; mock_graph_select_calls = 0;
    mock_graph_commit_calls = 0; mock_graph_primed = 0;
    memset(&mock_graph_status, 0, sizeof(mock_graph_status));
    mock_graph_status.active = false;
    snprintf(mock_graph_status.id, sizeof(mock_graph_status.id), "previous");
    mock_graph_status.revision = 7;
    s_fn[2] = true;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_reference_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_EQUAL_INT(1, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_graph_commit_calls);
    TEST_ASSERT_TRUE(mock_graph_status.active);
    TEST_ASSERT_EQUAL_STRING(SG_DEFAULT_PROJECT_ID, mock_graph_status.id);
    TEST_ASSERT_EQUAL_UINT32(4, mock_graph_status.revision);
    TEST_ASSERT_TRUE((mock_graph_primed & (1U << 2)) != 0U);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_UINT32(leases, mock_storage_access_leases);
    s_fn[2] = false;

    /* Motor stopped admission still rejects before any selection. */
    mock_motor_speed = 1;
    mock_graph_select_calls = 0; mock_graph_commit_calls = 0;
    mock_graph_status.active = true;
    snprintf(mock_graph_status.id, sizeof(mock_graph_status.id), "previous");
    mock_graph_status.revision = 7;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_reference_post(&req));
    TEST_ASSERT_EQUAL_STRING("409 Conflict", mock_resp_status);
    TEST_ASSERT_EQUAL_INT(0, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_graph_commit_calls);
    TEST_ASSERT_TRUE(mock_graph_status.active);
    TEST_ASSERT_EQUAL_STRING("previous", mock_graph_status.id);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_UINT32(leases, mock_storage_access_leases);

    /* Missing built-in project is a clear error and keeps the live graph. */
    mock_motor_speed = 0;
    test_list_default_project = false;
    mock_graph_revision = 0;
    mock_graph_select_calls = 0; mock_graph_commit_calls = 0;
    mock_graph_status.active = true;
    snprintf(mock_graph_status.id, sizeof(mock_graph_status.id), "previous");
    mock_graph_status.revision = 7;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_reference_post(&req));
    TEST_ASSERT_EQUAL_STRING("404 Not Found", mock_resp_status);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "default project not installed"));
    TEST_ASSERT_EQUAL_INT(0, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_graph_commit_calls);
    TEST_ASSERT_TRUE(mock_graph_status.active);
    TEST_ASSERT_EQUAL_STRING("previous", mock_graph_status.id);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_UINT32(leases, mock_storage_access_leases);

    /* Selector failure keeps the previous graph active and releases admission. */
    test_list_default_project = true;
    mock_graph_revision = 4;
    mock_graph_select_err = ESP_FAIL;
    mock_graph_select_calls = 0; mock_graph_commit_calls = 0;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_reference_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
    TEST_ASSERT_EQUAL_INT(1, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_graph_commit_calls);
    TEST_ASSERT_TRUE(mock_graph_status.active);
    TEST_ASSERT_EQUAL_STRING("previous", mock_graph_status.id);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_UINT32(leases, mock_storage_access_leases);

    /* Install preparation failure does not select or replace the live graph. */
    mock_graph_select_err = ESP_OK; mock_graph_prepare_err = ESP_FAIL;
    mock_graph_select_calls = 0; mock_graph_commit_calls = 0;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_reference_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
    TEST_ASSERT_EQUAL_INT(0, mock_graph_select_calls);
    TEST_ASSERT_EQUAL_INT(0, mock_graph_commit_calls);
    TEST_ASSERT_TRUE(mock_graph_status.active);
    TEST_ASSERT_EQUAL_STRING("previous", mock_graph_status.id);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_UINT32(leases, mock_storage_access_leases);

    mock_graph_prepare_err = ESP_OK;
    test_list_default_project = false;
    test_list_default_revision = 0;
}

static void test_graph_capabilities_match_firmware_limits(void)
{
    httpd_req_t req = make_req(0);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_capabilities_get(&req));

    char expected[96];
    const struct { const char *name; unsigned value; } limits[] = {
        { "states", SG_MAX_STATES },
        { "soundStates", SG_MAX_SOUND_STATES },
        { "transitions", SG_MAX_TRANSITIONS },
        { "effects", SG_MAX_EFFECTS },
        { "assets", SG_MAX_ASSETS },
        { "variables", SG_MAX_VARIABLES },
        { "jsonBytes", SG_MAX_JSON },
    };
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        snprintf(expected, sizeof(expected), "\"%s\":%u", limits[i].name, limits[i].value);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_resp_body, expected), expected);
    }

    static const char *const conditions[] = {
        "fn_press", "fn_release", "fn_on", "fn_off", "engine_on", "engine_off",
        "speed", "accel", "decel", "sample_done", "dir_fwd", "dir_rev",
        "random", "timeout", "var_cmp", "state_enter",
    };
    for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); ++i) {
        snprintf(expected, sizeof(expected), "\"%s\"", conditions[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(mock_resp_body, expected), expected);
    }
}

static void test_graph_get_roundtrip_assets_and_routes(void)
{
    seed_graph(); httpd_req_t req = make_req(0);
    TEST_ASSERT_EQUAL(ESP_OK, graph_project_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, graph_fixture));
    reset_resp(); TEST_ASSERT_EQUAL(ESP_OK, graph_capabilities_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"jsonBytes\":131072"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"engine_on\""));
    reset_resp(); set_query("file=idle.wav"); TEST_ASSERT_EQUAL(ESP_OK, graph_asset_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"crc32\":\"1234abcd\""));
    reset_resp(); req.uri = "/sound-editor/?v=3";
    TEST_ASSERT_EQUAL(ESP_OK, sound_editor_get(&req));
    TEST_ASSERT_EQUAL_STRING("text/html; charset=utf-8", mock_resp_type);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "Content-Encoding: gzip"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "Cache-Control: no-cache"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "nosniff"));
    reset_resp(); req.uri = "/sound-editor/assets/editor-ab12.js?x=1";
    TEST_ASSERT_EQUAL(ESP_OK, sound_editor_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "immutable"));
    reset_resp(); req.uri = "/sound-editor/THIRD_PARTY_NOTICES.txt";
    TEST_ASSERT_EQUAL(ESP_OK, sound_editor_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "no-cache"));
    reset_resp(); req.uri = "/sound-editor/unknown.js";
    TEST_ASSERT_EQUAL(ESP_OK, sound_editor_get(&req));
    TEST_ASSERT_EQUAL_INT(HTTPD_404_NOT_FOUND, mock_resp_send_err_code);
    reset_resp(); TEST_ASSERT_EQUAL(ESP_OK, captive_handler(&req, HTTPD_404_NOT_FOUND));
    TEST_ASSERT_EQUAL_INT(HTTPD_404_NOT_FOUND, mock_resp_send_err_code);
    TEST_ASSERT_NULL(strstr(mock_resp_hdr, "Location:"));
    reset_resp(); TEST_ASSERT_EQUAL(ESP_OK, start_http_server());
    TEST_ASSERT_EQUAL_UINT32(63, s_route_count);
    TEST_ASSERT_EQUAL_UINT32(63, mock_route_count);
}

static void test_graph_referenced_uploads_fail_closed(void)
{
    mock_graph_referenced = true;
    uint8_t data[16] = {1}; httpd_req_t req = make_req(sizeof(data));
    set_body(data, sizeof(data)); set_query("slot=1");
    snprintf(mock_header_name, sizeof(mock_header_name), "idle.wav");
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    TEST_ASSERT_EQUAL_UINT32(0, mock_tracks_save_calls);
    mock_graph_referenced = false; mock_graph_reference_err = ESP_FAIL;
    set_body(data, sizeof(data)); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    TEST_ASSERT_EQUAL_UINT32(0, mock_tracks_save_calls);
    mock_graph_reference_err = ESP_OK; mock_graph_referenced = true;
    size_t n = build_named_pair(ota_combined, "idle.wav", "brake.wav");
    req = make_req(n); set_body(ota_combined, n); reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_EQUAL_UINT32(0, test_rename_calls);
    TEST_ASSERT_EQUAL_UINT32(0, test_boot_calls);
}

static void test_graph_factory_reset_selection_and_failure(void)
{
    seed_graph();
    httpd_req_t req = graph_request(factory_reset_post, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_UINT32(1, mock_graph_select_calls);
    TEST_ASSERT_FALSE(mock_graph_status.active);
    TEST_ASSERT_EQUAL_UINT32(1, mock_esp_restart_calls);
    TEST_ASSERT_EQUAL_STRING(graph_fixture, mock_graph_json);
    s_maintenance_owner = NULL; s_mutation_count = 0;
    mock_graph_status.active = true; mock_graph_select_err = ESP_FAIL;
    mock_esp_restart_calls = 0; reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, mutation_dispatch(&req));
    TEST_ASSERT_EQUAL_STRING("500 Internal Server Error", mock_resp_status);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "selector reset failed"));
    TEST_ASSERT_EQUAL_UINT32(0, mock_esp_restart_calls);
    TEST_ASSERT_TRUE(mock_graph_status.active);
    TEST_ASSERT_FALSE(s_actuation_ready);
    TEST_ASSERT_TRUE(mock_motor_inhibit_reasons != 0);
    TEST_ASSERT_TRUE(mock_sound_inhibited);
    TEST_ASSERT_TRUE(mock_audio_inhibited);
}

static void test_graph_projects_and_state_get(void)
{
    httpd_req_t req = make_req(0);
    mock_graph_revision = 0;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_projects_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"projects\":[]"));

    mock_graph_revision = 7;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_projects_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"id\":\"demo\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"name\":\"Demo\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"revision\":7"));

    memset(&mock_graph_status, 0, sizeof(mock_graph_status));
    mock_graph_status.active = true;
    mock_graph_status.engine = true;
    mock_graph_status.armed = true;
    mock_graph_status.fault = true;
    snprintf(mock_graph_status.id, sizeof(mock_graph_status.id), "demo");
    mock_graph_status.revision = 7;
    mock_graph_status.speed = 42;
    mock_graph_status.states[0] = 3;
    mock_graph_status.states[1] = 4;
    mock_graph_status.failed_channels = 2;
    mock_graph_status.retry_channels = 4;
    s_outputs_applied = 0x0005;
    s_logic_active = 0x0002;
    memset(s_fn, 0, sizeof(s_fn));
    s_fn[0] = true;
    s_fn[2] = true;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_state_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"active\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"id\":\"demo\""));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"revision\":7"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"engine\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"armed\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"failedChannels\":2"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"retryChannels\":4"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"states\":[3,4"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"speed\":42"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"fault\":true"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"outputs\":5"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"logic\":2"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"functions\":5"));
    memset(s_fn, 0, sizeof(s_fn));
}

static void test_graph_projects_get_error(void)
{
    httpd_req_t req = make_req(0);
    mock_graph_list_err = ESP_FAIL;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, graph_projects_get(&req));
    TEST_ASSERT_EQUAL_STRING("500 Internal Server Error", mock_resp_status);
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":false"));
    mock_graph_list_err = ESP_OK;
}

static void test_graph_query_values_are_not_truncated_or_ambiguous(void)
{
    char value[SG_ID_CAP];
    uint32_t revision = 0;
    TEST_ASSERT_TRUE(graph_param("id=valid_graph&revision=1", "id", value, sizeof(value)));
    TEST_ASSERT_EQUAL_STRING("valid_graph", value);
    TEST_ASSERT_FALSE(graph_param("id=abcdefghijklmnopqrstuvwxyz0123456789", "id", value, sizeof(value)));
    TEST_ASSERT_FALSE(graph_param("id=first&id=second", "id", value, sizeof(value)));
    TEST_ASSERT_FALSE(graph_revision("revision=1&revision=2", "revision", &revision));
    TEST_ASSERT_FALSE(graph_revision("revision=0000000000000000000001", "revision", &revision));
    TEST_ASSERT_TRUE(graph_revision("revision=4294967295", "revision", &revision));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, revision);
    TEST_ASSERT_FALSE(graph_revision("revision=4294967296", "revision", &revision));
}

/* ---------- regression tests for the safety/storage hardening batch ---------- */

static void test_emergency_reset_lock_busy_never_blocks(void)
{
    memset(s_fn, 0, sizeof(s_fn));
    s_fn[1] = true;
    s_fn[2] = true;
    memset(mock_auxio_enabled_state, 0, sizeof(mock_auxio_enabled_state));
    mock_auxio_enabled_state[AUXIO_CH_AUX1] = 1;
    s_outputs_applied = (uint16_t)(1U << AUXIO_CH_AUX1);
    s_logic_active = (uint16_t)(1U << (FUNC_LOGIC_COAST - 1U));
    s_motion_speed = 20;
    /* Simulated HTTP handler holding s_func_mutex across filesystem/NVS work. */
    mock_sem_take_fail = 1;
    web_emergency_reset();
    mock_sem_take_fail = 0;
    for (uint8_t fn = 0; fn < WEB_FN_COUNT; ++fn) TEST_ASSERT_FALSE(s_fn[fn]);
    for (uint8_t ch = 0; ch < AUXIO_CH_COUNT; ++ch) {
        TEST_ASSERT_EQUAL_INT(0, mock_auxio_enabled_state[ch]);
    }
    TEST_ASSERT_EQUAL_UINT16(0, s_outputs_applied);
    TEST_ASSERT_EQUAL_UINT16(0, s_logic_active);
    TEST_ASSERT_EQUAL_UINT8(0, s_motion_speed);
}

static void test_root_and_editor_deny_framing(void)
{
    httpd_req_t req = make_req(0);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, root_handler(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "X-Frame-Options: DENY;"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "Content-Security-Policy: frame-ancestors 'none';"));

    reset_resp();
    req.uri = "/sound-editor";
    TEST_ASSERT_EQUAL(ESP_OK, sound_editor_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "X-Frame-Options: DENY;"));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_hdr, "Content-Security-Policy: frame-ancestors 'none';"));
}

static void test_upload_name_validation_and_lowercase(void)
{
    uint8_t data[16];
    memset(data, 1, sizeof(data));
    httpd_req_t req = make_req(sizeof(data));

    static const char *const bad[] = { "noext", "tracks.txt", "volumes.txt", "x.TMP", ".pbdir" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        set_body(data, sizeof(data));
        set_query("slot=5");
        snprintf(mock_header_name, sizeof(mock_header_name), "%s", bad[i]);
        reset_resp();
        TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
        TEST_ASSERT_EQUAL_INT(HTTPD_400_BAD_REQUEST, mock_resp_send_err_code);
        TEST_ASSERT_FALSE(file_exists("web_tmp/audio/noext"));
        TEST_ASSERT_FALSE(file_exists("web_tmp/audio/tracks.txt"));
        TEST_ASSERT_FALSE(file_exists("web_tmp/audio/volumes.txt"));
    }

    /* Mixed-case extension is accepted and stored as the canonical .wav. */
    set_body(data, sizeof(data));
    set_query("slot=6");
    snprintf(mock_header_name, sizeof(mock_header_name), "Clip.WAV");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/Clip.wav"));
    TEST_ASSERT_EQUAL_STRING("Clip.wav", mock_saved_tracks[0].label);
}

static void test_library_delete_refuses_track_and_binding(void)
{
    FILE *f = fopen("web_tmp/audio/lib.wav", "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("LIB", f);
    fclose(f);
    mock_tracks_count = 1;
    mock_tracks[0].slot = 4;
    snprintf(mock_tracks[0].file, sizeof(mock_tracks[0].file), "audio/lib.wav");
    memset(s_func_map, 0, sizeof(s_func_map));
    set_one_binding(1, FUNC_TARGET_SLOT, 4, FUNC_DIR_ANY, FUNC_STATE_ANY);

    httpd_req_t req = make_req(0);
    set_query("name=lib");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_library_delete_post(&req));
    TEST_ASSERT_EQUAL_INT(409, atoi(mock_resp_status));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "function sound binding"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/lib.wav"));

    /* Without a function binding the track slot itself still blocks unlink. */
    s_func_bind_count = 0;
    s_func_bind_present = true;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_library_delete_post(&req));
    TEST_ASSERT_EQUAL_INT(409, atoi(mock_resp_status));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "track slot"));
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/lib.wav"));

    /* After the slot is unbound the delete succeeds. */
    mock_tracks_count = 0;
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_library_delete_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/lib.wav"));
}

static void test_pack_journal_prepared_rolls_back_at_boot(void)
{
    char old_names[1][WEB_LIB_NAME_MAX];
    char new_names[1][WEB_LIB_NAME_MAX];
    memset(old_names, 0, sizeof(old_names));
    memset(new_names, 0, sizeof(new_names));
    snprintf(old_names[0], sizeof(old_names[0]), "old.wav");
    snprintf(new_names[0], sizeof(new_names[0]), "new.wav");
    MKDIR("web_tmp/audio/.pbtest");
    FILE *f = fopen("web_tmp/audio/.pbtest/old.wav", "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("OLD", f);
    fclose(f);
    f = fopen("web_tmp/audio/new.wav", "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("NEW", f);
    fclose(f);

    pack_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    snprintf(tx.backup_dir, sizeof(tx.backup_dir), "%s", "web_tmp/audio/.pbtest");
    snprintf(tx.journal, sizeof(tx.journal), "%s", PACK_JOURNAL_PATH);
    tx.old_names = old_names;
    tx.old_count = 1;
    tx.new_names = new_names;
    tx.new_count = 1;
    TEST_ASSERT_EQUAL(ESP_OK, pack_journal_write(&tx, PACK_JOURNAL_PREPARED));
    test_manifest_load_calls = 0;

    TEST_ASSERT_EQUAL(ESP_OK, web_recover_pack_journal());
    TEST_ASSERT_TRUE(file_exists("web_tmp/audio/old.wav"));
    TEST_ASSERT_FALSE(file_exists("web_tmp/audio/new.wav"));
    TEST_ASSERT_FALSE(file_exists(PACK_JOURNAL_PATH));
    TEST_ASSERT_FALSE(dir_exists("web_tmp/audio/.pbtest"));
    TEST_ASSERT_EQUAL_UINT32(1, test_manifest_load_calls);
}

static void test_pack_journal_committed_cleans_backups(void)
{
    char old_names[1][WEB_LIB_NAME_MAX];
    memset(old_names, 0, sizeof(old_names));
    snprintf(old_names[0], sizeof(old_names[0]), "old.wav");
    MKDIR("web_tmp/audio/.pbcommitted");
    FILE *f = fopen("web_tmp/audio/.pbcommitted/old.wav", "wb");
    TEST_ASSERT_NOT_NULL(f);
    fputs("OLD", f);
    fclose(f);

    pack_tx_t tx;
    memset(&tx, 0, sizeof(tx));
    snprintf(tx.backup_dir, sizeof(tx.backup_dir), "%s", "web_tmp/audio/.pbcommitted");
    snprintf(tx.journal, sizeof(tx.journal), "%s", PACK_JOURNAL_PATH);
    tx.old_names = old_names;
    tx.old_count = 1;
    tx.new_count = 0;
    TEST_ASSERT_EQUAL(ESP_OK, pack_journal_write(&tx, PACK_JOURNAL_COMMITTED));
    test_manifest_load_calls = 0;

    TEST_ASSERT_EQUAL(ESP_OK, web_recover_pack_journal());
    TEST_ASSERT_FALSE(file_exists(PACK_JOURNAL_PATH));
    TEST_ASSERT_FALSE(dir_exists("web_tmp/audio/.pbcommitted"));
    TEST_ASSERT_EQUAL_UINT32(0, test_manifest_load_calls);
}

static void test_pack_success_removes_journal(void)
{
    uint8_t pcm[16] = { 0 };
    uint8_t pack[128];
    size_t n = pack_test_build(pack, "journalnew", pcm, sizeof(pcm));
    set_body(pack, n);
    httpd_req_t req = make_req(n);
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_pack_upload_post(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"ok\":true"));
    TEST_ASSERT_FALSE(file_exists(PACK_JOURNAL_PATH));
}

static void test_cv_write_reloads_multifunction_cvs(void)
{
    httpd_req_t req = make_req(0);
    int before = mock_dcc_reload_config_calls;
    set_query("index=21&value=1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, cv_write_post(&req));
    TEST_ASSERT_EQUAL_INT(before + 1, mock_dcc_reload_config_calls);
    set_query("index=22&value=1");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, cv_write_post(&req));
    TEST_ASSERT_EQUAL_INT(before + 2, mock_dcc_reload_config_calls);
}

static void test_binding_json_grows_with_count(void)
{
    s_func_bind_present = true;
    s_func_bind_count = FUNC_BIND_MAX;
    memset(s_func_bind, 0, sizeof(s_func_bind));
    for (size_t i = 0; i < FUNC_BIND_MAX; ++i) {
        s_func_bind[i].used = 1;
        s_func_bind[i].fn = (uint8_t)(i % WEB_FN_COUNT);
        s_func_bind[i].target_type = FUNC_TARGET_OUTPUT;
        s_func_bind[i].target_id = (uint8_t)(i % 9U);
    }
    httpd_req_t req = make_req(0);
    set_query("view=bind");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, func_bind_get(&req));
    TEST_ASSERT_NOT_NULL(strstr(mock_resp_body, "\"count\":64"));
    TEST_ASSERT_TRUE(strlen(mock_resp_body) > 4096U);
    size_t occurrences = 0;
    const char *p = mock_resp_body;
    while ((p = strstr(p, "\"fn\":")) != NULL) { ++occurrences; ++p; }
    TEST_ASSERT_EQUAL_UINT32(FUNC_BIND_MAX, occurrences);
    TEST_ASSERT_EQUAL_STRING("]}", mock_resp_body + strlen(mock_resp_body) - 2U);
}

static void test_ota_deadline_scales_with_length(void)
{
    mock_timer_now_us = 1000000;
    int64_t small = ota_deadline_from_len(0);
    TEST_ASSERT_TRUE(small - mock_timer_now_us >= 300000000LL);
    int64_t large = ota_deadline_from_len(8 * 1024 * 1024);
    TEST_ASSERT_TRUE(large > small);

    httpd_req_t req = make_req(16);
    reset_resp();
    s_transfer_deadline = 0;
    TEST_ASSERT_EQUAL(ESP_OK, ota_update_post(&req));
    TEST_ASSERT_TRUE(s_transfer_deadline >= mock_timer_now_us + 300000000LL);
}

static void test_volume_partial_update_keeps_other_fields(void)
{
    s_cfg.master_volume = 21;
    s_cfg.engine_volume = 34;
    s_cfg.effects_volume = 45;
    httpd_req_t req = make_req(0);
    set_query("master=50");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, audio_volume_post(&req));
    TEST_ASSERT_EQUAL_UINT8(50, s_cfg.master_volume);
    TEST_ASSERT_EQUAL_UINT8(34, s_cfg.engine_volume);
    TEST_ASSERT_EQUAL_UINT8(45, s_cfg.effects_volume);
    TEST_ASSERT_EQUAL_UINT8(50, mock_audio_last_volume);
}

static void test_wifi_reset_preserves_non_wifi_settings(void)
{
    snprintf(s_cfg.device_name, sizeof(s_cfg.device_name), "KEEP");
    s_cfg.master_volume = 66;
    s_cfg.engine_volume = 55;
    s_cfg.effects_volume = 44;
    s_cfg.control_source = 1;
    s_cfg.active_slot = 3;
    s_cfg.ap_ssid[0] = '\0';
    snprintf(s_cfg.ap_ip, sizeof(s_cfg.ap_ip), "10.9.9.9");
    mock_settings_save_calls = 0;
    mock_esp_restart_calls = 0;
    httpd_req_t req = make_req(0);
    set_query("");
    reset_resp();
    TEST_ASSERT_EQUAL(ESP_OK, wifi_reset_post(&req));
    TEST_ASSERT_EQUAL_STRING("KEEP", s_cfg.device_name);
    TEST_ASSERT_EQUAL_UINT8(66, s_cfg.master_volume);
    TEST_ASSERT_EQUAL_UINT8(55, s_cfg.engine_volume);
    TEST_ASSERT_EQUAL_UINT8(44, s_cfg.effects_volume);
    TEST_ASSERT_EQUAL_UINT8(1, s_cfg.control_source);
    TEST_ASSERT_EQUAL_UINT8(3, s_cfg.active_slot);
    TEST_ASSERT_EQUAL_UINT8(1, s_cfg.wifi_mode);
    TEST_ASSERT_EQUAL_STRING("ADDITIPUS AURA-X", s_cfg.ap_ssid);
    TEST_ASSERT_EQUAL_STRING(AP_IP_DEFAULT, s_cfg.ap_ip);
    TEST_ASSERT_EQUAL_INT(1, mock_settings_save_calls);
    TEST_ASSERT_EQUAL_INT(1, mock_esp_restart_calls);
    web_maintenance_end(); /* esp_restart does not return on the target. */
}

static void test_absent_audio_does_not_block_ready_or_maintenance(void)
{
    s_actuation_ready = false;
    test_sound_inhibit_err = ESP_ERR_INVALID_STATE;
    mock_audio_inhibit_ret = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_OK, web_set_actuation_ready(true));
    TEST_ASSERT_TRUE(actuation_ready());
    TEST_ASSERT_EQUAL(ESP_OK, web_maintenance_begin(false));
    web_maintenance_end();
    TEST_ASSERT_TRUE(actuation_ready());
    test_sound_inhibit_err = ESP_OK;
    mock_audio_inhibit_ret = ESP_OK;
}

static void test_stalled_worker_supervisor_releases_gate(void)
{
    uint8_t body[16] = { 0 };
    httpd_req_t req = make_req(sizeof(body));
    set_body(body, sizeof(body));
    set_query("slot=1");
    test_stalled_saw_abort = false;
    test_stalled_handler_rc = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, test_stalled_transfer_handler));
    TEST_ASSERT_NOT_NULL(test_pending_transfer);
    TEST_ASSERT_TRUE(web_maintenance_active());
    void *job = test_pending_transfer;
    test_pending_transfer = NULL;
    mock_current_task_handle = (TaskHandle_t)2;
    reset_resp();
    mock_httpd_shutdown_calls = 0;
    transfer_worker(job);
    mock_current_task_handle = (TaskHandle_t)1;

    TEST_ASSERT_TRUE(test_stalled_saw_abort);
    TEST_ASSERT_EQUAL(ESP_FAIL, test_stalled_handler_rc);
    TEST_ASSERT_EQUAL_INT(HTTPD_500_INTERNAL_SERVER_ERROR, mock_resp_send_err_code);
    TEST_ASSERT_TRUE(mock_httpd_shutdown_calls >= 1);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_INT(0, mock_storage_access_leases);
    TEST_ASSERT_FALSE(s_up_active);
    TEST_ASSERT_EQUAL_UINT32(1, test_complete_calls);
}

static void test_transfer_wdt_feeds_only_on_bytes(void)
{
    int before = mock_task_wdt_reset_calls;
    s_transfer_wdt_subscribed = false;
    transfer_wdt_feed(1);
    TEST_ASSERT_EQUAL_INT(before, mock_task_wdt_reset_calls);
    s_transfer_wdt_subscribed = true;
    transfer_wdt_feed(0);
    TEST_ASSERT_EQUAL_INT(before, mock_task_wdt_reset_calls);
    transfer_wdt_feed(1);
    TEST_ASSERT_EQUAL_INT(before + 1, mock_task_wdt_reset_calls);
    s_transfer_wdt_subscribed = false;
}

static void test_transfer_worker_feeds_twdt_on_received_bytes(void)
{
    uint8_t body[16];
    memset(body, 0x5A, sizeof(body));
    httpd_req_t req = make_req(sizeof(body));
    set_body(body, sizeof(body));
    test_progress_total = -1;
    mock_task_wdt_reset_calls = 0;
    TEST_ASSERT_EQUAL(ESP_OK, transfer_start(&req, test_progress_transfer_handler));
    TEST_ASSERT_NOT_NULL(test_pending_transfer);
    void *job = test_pending_transfer;
    test_pending_transfer = NULL;
    mock_current_task_handle = (TaskHandle_t)2;
    transfer_worker(job);
    mock_current_task_handle = (TaskHandle_t)1;
    TEST_ASSERT_EQUAL_INT((int)sizeof(body), test_progress_total);
    TEST_ASSERT_GREATER_THAN(0, mock_task_wdt_reset_calls);
    TEST_ASSERT_FALSE(web_maintenance_active());
    TEST_ASSERT_EQUAL_INT(0, mock_storage_access_leases);
    TEST_ASSERT_FALSE(s_up_active);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    UNITY_BEGIN();
    RUN_TEST(test_graph_projects_and_state_get);
    RUN_TEST(test_graph_projects_get_error);
    RUN_TEST(test_graph_query_values_are_not_truncated_or_ambiguous);
    RUN_TEST(test_graph_save_validate_do_not_actuate);
    RUN_TEST(test_graph_body_bounds_mime_and_eof);
    RUN_TEST(test_graph_schema_and_allocation_failures);
    RUN_TEST(test_graph_exact_max_body_and_trailing_json);
    RUN_TEST(test_graph_maintenance_and_binding_mutation_guards);
    RUN_TEST(test_graph_apply_motion_and_transaction_failures);
    RUN_TEST(test_graph_delete_project);
    RUN_TEST(test_graph_reference_post);
    RUN_TEST(test_graph_capabilities_match_firmware_limits);
    RUN_TEST(test_graph_get_roundtrip_assets_and_routes);
    RUN_TEST(test_graph_referenced_uploads_fail_closed);
    RUN_TEST(test_graph_factory_reset_selection_and_failure);
    RUN_TEST(test_mutation_dispatch_csrf_guard);
    RUN_TEST(test_named_upload_preserves_persisted_mds_reference);
    RUN_TEST(test_named_replacement_failure_preserves_original);
    RUN_TEST(test_named_slot_rebind_retains_other_project_assets);
    RUN_TEST(test_combined_ota_named_assets_and_metadata_before_boot);
    RUN_TEST(test_combined_ota_duplicate_canonical_names_preserve_originals);
    RUN_TEST(test_combined_ota_publication_failure_rolls_back_all_files);
    RUN_TEST(test_combined_ota_rollback_failure_retains_recovery_evidence);
    RUN_TEST(test_combined_ota_backup_space_preflight_preserves_originals);
    RUN_TEST(test_named_shared_asset_overwrite_updates_all_references);
    RUN_TEST(test_combined_ota_transaction_heap_failure_leaves_originals);
    RUN_TEST(test_startup_and_source_leases);
    RUN_TEST(test_maintenance_owner_quiescence_and_timeout);
    RUN_TEST(test_maintenance_transient_inhibit_timeout_keeps_ready);
    RUN_TEST(test_maintenance_persistent_inhibit_timeout_closes_ready);
    RUN_TEST(test_preinit_maintenance_and_readiness);
    RUN_TEST(test_shared_aux_and_retry_failed_apply);
    RUN_TEST(test_empty_canonical_store_and_live_remove);
    RUN_TEST(test_legacy_voices_do_not_collide);
    RUN_TEST(test_same_filename_failure_preserves_original);
    RUN_TEST(test_async_transfer_admission_completion_and_readonly);
    RUN_TEST(test_ota_whole_container_before_boot);
    RUN_TEST(test_wifi_exact_lengths_and_safe_addresses);
    RUN_TEST(test_log_cursor_tracks_last_transmitted_entry);
    RUN_TEST(test_async_begin_failure_and_exclusive_admission);
    RUN_TEST(test_ota_invalid_subset_never_publishes);
    RUN_TEST(test_ota_boot_failure_restores_metadata_and_preserves_files);
    RUN_TEST(test_maintenance_existing_leases_and_no_resurrection);
    RUN_TEST(test_async_quiescence_failure_completes_and_closes);
    RUN_TEST(test_web_init_required_maps_and_outputs_fail_closed);
    RUN_TEST(test_bad_actuation_parameters_do_not_fall_back);
    RUN_TEST(test_ap_configuration_and_dhcp_errors_propagate);
    RUN_TEST(test_web_log_event_null_mutex);
    RUN_TEST(test_web_log_event_stores);
    RUN_TEST(test_web_log_event_utf8_and_truncation);
    RUN_TEST(test_control_is_rails);
    RUN_TEST(test_emergency_post_cuts_all_and_clears_functions);
    RUN_TEST(test_wifi_event_handler);
    RUN_TEST(test_wifi_pick_channel_scan_fail);
    RUN_TEST(test_wifi_pick_channel_no_aps);
    RUN_TEST(test_wifi_pick_channel_num_fail);
    RUN_TEST(test_wifi_pick_channel_records_fail);
    RUN_TEST(test_wifi_pick_channel_scores);
    RUN_TEST(test_wifi_pick_channel_adjacent_overlap);
    RUN_TEST(test_wifi_pick_channel_ht40);
    RUN_TEST(test_web_apply_function_bounds);
    RUN_TEST(test_web_apply_function_mutex_null);
    RUN_TEST(test_web_apply_function_output_delta);
    RUN_TEST(test_web_apply_function_unchanged);
    RUN_TEST(test_web_apply_function_starts_sound);
    RUN_TEST(test_web_apply_function_sound_missing_stops);
    RUN_TEST(test_web_apply_function_tracks_load_fail);
    RUN_TEST(test_web_apply_function_double_voice);
    RUN_TEST(test_web_apply_function_secondary_above_10);
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
    RUN_TEST(test_wifi_start_max_tx_power);
    RUN_TEST(test_wifi_start_client_timeout);
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
    RUN_TEST(test_control_source_post_errors);
    RUN_TEST(test_mode_get);
    RUN_TEST(test_mode_post);
    RUN_TEST(test_bemf_cal_get);
    RUN_TEST(test_bemf_phase_names);
    RUN_TEST(test_bemf_base_get);
    RUN_TEST(test_bemf_cal_post_paths);
    RUN_TEST(test_bemf_use_get_post);
    RUN_TEST(test_bemf_status_names_and_previous_curve);
    RUN_TEST(test_bemf_cancel_bypasses_admission_without_motor_control);
    RUN_TEST(test_bemf_action_queries_strict);
    RUN_TEST(test_bemf_start_reset_still_gated);
    RUN_TEST(test_bemf_mode_durable_before_runtime);
    RUN_TEST(test_bemf_mode_queries_and_actuation_gates);
    RUN_TEST(test_motor_get_post);
    RUN_TEST(test_functions_get);
    RUN_TEST(test_function_post);
    RUN_TEST(test_function_post_all_f_to_all_aux);
    RUN_TEST(test_legacy_map_all_f_to_all_aux);
    RUN_TEST(test_headlight_direction_bindings);
    RUN_TEST(test_graph_state_reports_dcc_control_masks);
    RUN_TEST(test_motor_post_updates_motion_and_headlights);
    RUN_TEST(test_web_emergency_reset_clears_outputs);
    RUN_TEST(test_all_logic_actions);
    RUN_TEST(test_graph_mode_routes_all_functions_to_sound);
    RUN_TEST(test_aux_effect_post);
    RUN_TEST(test_aux_effect_all_channels);
    RUN_TEST(test_audio_status_get);
    RUN_TEST(test_audio_tracks_get);
    RUN_TEST(test_audio_play_post);
    RUN_TEST(test_audio_stop_post);
    RUN_TEST(test_audio_volume_post);
    RUN_TEST(test_internal_helpers);
    RUN_TEST(test_stream_write_success);
    RUN_TEST(test_stream_short_body);
    RUN_TEST(test_stream_file_write_failure);
    RUN_TEST(test_audio_upload_guards);
    RUN_TEST(test_audio_pack_upload_replaces_library);
    RUN_TEST(test_audio_pack_upload_rolls_back_on_publish_failure);
    RUN_TEST(test_audio_pack_upload_rejects_graph_reference);
    RUN_TEST(test_audio_pack_upload_applies_bundled_scheme);
    RUN_TEST(test_audio_pack_upload_rolls_back_when_scheme_fails);
    RUN_TEST(test_audio_upload_success);
    RUN_TEST(test_audio_upload_replaces_and_retains_old);
    RUN_TEST(test_audio_upload_compacts_duplicates);
    RUN_TEST(test_audio_upload_slots_full);
    RUN_TEST(test_audio_upload_asset_only_over_slots);
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
    RUN_TEST(test_aux_cfg_all_channels);
    RUN_TEST(test_aux_state_get);
    RUN_TEST(test_dcc_status_get);
    RUN_TEST(test_func_map_get_post);
    RUN_TEST(test_func_bind_load_and_desired);
    RUN_TEST(test_func_bind_get_view);
    RUN_TEST(test_logic_bindings_apply_runtime_actions);
    RUN_TEST(test_func_map_post_binding);
    RUN_TEST(test_web_func_map_set_preserves_sound_bindings);
    RUN_TEST(test_utf8_four_byte_and_outputs_init);
    RUN_TEST(test_captive_handler);
    RUN_TEST(test_start_dns_hijack);
    RUN_TEST(test_stream_idle_retry);
    RUN_TEST(test_stream_write_fault);
    RUN_TEST(test_stream_read_failure);
    RUN_TEST(test_stream_receive_limit);
    RUN_TEST(test_stream_injected_write_error);
    RUN_TEST(test_aux_effect_valid_mode);
    RUN_TEST(test_wifi_get);
    RUN_TEST(test_wifi_post);
    RUN_TEST(test_wifi_and_factory_reset);
    RUN_TEST(test_device_get_post);
    RUN_TEST(test_storage_get);
    RUN_TEST(test_task_inputs_and_clientlog);
    RUN_TEST(test_log_get);
    RUN_TEST(test_progress_handler);
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
    RUN_TEST(test_emergency_reset_lock_busy_never_blocks);
    RUN_TEST(test_root_and_editor_deny_framing);
    RUN_TEST(test_upload_name_validation_and_lowercase);
    RUN_TEST(test_library_delete_refuses_track_and_binding);
    RUN_TEST(test_pack_journal_prepared_rolls_back_at_boot);
    RUN_TEST(test_pack_journal_committed_cleans_backups);
    RUN_TEST(test_pack_success_removes_journal);
    RUN_TEST(test_cv_write_reloads_multifunction_cvs);
    RUN_TEST(test_binding_json_grows_with_count);
    RUN_TEST(test_ota_deadline_scales_with_length);
    RUN_TEST(test_volume_partial_update_keeps_other_fields);
    RUN_TEST(test_wifi_reset_preserves_non_wifi_settings);
    RUN_TEST(test_absent_audio_does_not_block_ready_or_maintenance);
    RUN_TEST(test_stalled_worker_supervisor_releases_gate);
    RUN_TEST(test_transfer_wdt_feeds_only_on_bytes);
    RUN_TEST(test_transfer_worker_feeds_twdt_on_received_bytes);
    return UNITY_END();
}
