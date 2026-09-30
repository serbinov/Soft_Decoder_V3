/* Host-side stubs and fault-injection hooks for the web component tests.
 *
 * Included by test/test_web/test_web.c after web.c and the shared stubs.c.
 * It provides:
 *   - a scripted esp_http_server implementation (requests + captured responses)
 *   - esp_wifi / esp_netif / esp_event mocks with failure flags
 *   - a scripted lwIP socket mock for the captive-portal DNS task
 *   - mock implementations of the settings/audio/motor/auxio/storage/dcc
 *     dependencies so web.c can be exercised in isolation.
 *
 * The allocation attributes are macro-renamed by test_web.c (malloc/calloc and
 * fsync) before web.c is included, so this file #undefs them and wraps the
 * real libc allocator while tracking an injectable failure point. */

#undef malloc
#undef calloc
#undef fsync
#undef WEB_FWRITE
#undef WEB_FFLUSH
#undef WEB_FCLOSE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "lwip/sockets.h"

#include "audio.h"
#include "auxio.h"
#include "dcc.h"
#include "motor.h"
#include "settings.h"
#include "storage.h"

/* ====================================================================== */
/* injectable allocation                                                   */
/* ====================================================================== */

/* -1 = never fail; N >= 0 fails the (N+1)-th allocation and then disarms. */
int mock_alloc_fail_at = -1;
int mock_alloc_calls = 0;

void *mock_web_malloc(size_t n)
{
    if (mock_alloc_fail_at >= 0 && mock_alloc_calls++ == mock_alloc_fail_at) {
        mock_alloc_fail_at = -1;
        return NULL;
    }
    return malloc(n);
}

void *mock_web_calloc(size_t n, size_t s)
{
    if (mock_alloc_fail_at >= 0 && mock_alloc_calls++ == mock_alloc_fail_at) {
        mock_alloc_fail_at = -1;
        return NULL;
    }
    return calloc(n, s);
}

int mock_fsync_ret = 0;

int web_mock_fsync(int fd)
{
    (void)fd;
    return mock_fsync_ret;
}

int mock_ota_fwrite_fail = 0;
int mock_ota_fflush_fail = 0;
int mock_ota_fclose_fail = 0;

size_t web_mock_fwrite(const void *p, size_t sz, size_t n, FILE *f)
{
    if (mock_ota_fwrite_fail) {
        return 0;
    }
    return fwrite(p, sz, n, f);
}

int web_mock_fflush(FILE *f)
{
    int rc = fflush(f);
    return mock_ota_fflush_fail ? -1 : rc;
}

int web_mock_fclose(FILE *f)
{
    int rc = fclose(f);
    return mock_ota_fclose_fail ? -1 : rc;
}

/* ====================================================================== */
/* esp_http_server                                                         */
/* ====================================================================== */

int mock_httpd_start_err = 0;
int mock_httpd_register_err = 0;
int mock_httpd_start_calls = 0;
int mock_httpd_register_calls = 0;
int mock_httpd_err_register_calls = 0;
httpd_config_t mock_httpd_last_cfg;

/* Route table (bounded), useful for assertions and for keeping the shim
 * faithful to the real server. */
#define MOCK_HTTPD_MAX_ROUTES 64
httpd_uri_t mock_routes[MOCK_HTTPD_MAX_ROUTES];
int mock_route_count = 0;
httpd_err_handler_func_t mock_err_handler;

/* Request body / query / header script. */
uint8_t mock_httpd_body[65536];
size_t mock_httpd_body_len = 0;
size_t mock_httpd_body_pos = 0;
int mock_recv_idle_n = 0;
int mock_recv_idle_val = 0;
int mock_recv_fail = 0;
int mock_recv_limit = -1;
int mock_recv_bytes = 0;
int mock_recv_calls = 0;

char mock_query[2048] = { 0 };
char mock_header_name[160] = { 0 };

/* Captured response. */
char mock_resp_body[262144];
size_t mock_resp_body_len = 0;
char mock_resp_type[96] = { 0 };
char mock_resp_status[48] = { 0 };
char mock_resp_hdr[512] = { 0 };
int mock_resp_send_err_code = 0;
char mock_resp_send_err_msg[96] = { 0 };
int mock_resp_send_calls = 0;

void mock_httpd_reset(void)
{
    mock_httpd_body_len = 0;
    mock_httpd_body_pos = 0;
    mock_recv_idle_n = 0;
    mock_recv_idle_val = 0;
    mock_recv_fail = 0;
    mock_recv_limit = -1;
    mock_recv_bytes = 0;
    mock_recv_calls = 0;
    mock_query[0] = '\0';
    mock_header_name[0] = '\0';
    mock_resp_body_len = 0;
    mock_resp_body[0] = '\0';
    mock_resp_type[0] = '\0';
    mock_resp_status[0] = '\0';
    mock_resp_hdr[0] = '\0';
    mock_resp_send_err_code = 0;
    mock_resp_send_err_msg[0] = '\0';
    mock_resp_send_calls = 0;
}

void mock_httpd_set_body(const void *data, size_t len)
{
    if (len > sizeof(mock_httpd_body)) {
        len = sizeof(mock_httpd_body);
    }
    if (data != NULL && len > 0) {
        memcpy(mock_httpd_body, data, len);
    }
    mock_httpd_body_len = len;
    mock_httpd_body_pos = 0;
    mock_recv_bytes = 0;
}

bool httpd_uri_match_wildcard(const char *ref_uri, const char *in_uri, size_t len)
{
    (void)ref_uri;
    (void)in_uri;
    (void)len;
    return true;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    mock_httpd_start_calls++;
    if (config != NULL) {
        mock_httpd_last_cfg = *config;
    }
    if (mock_httpd_start_err) {
        return (esp_err_t)mock_httpd_start_err;
    }
    if (handle != NULL) {
        *handle = (httpd_handle_t)1;
    }
    return ESP_OK;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    (void)handle;
    mock_httpd_register_calls++;
    if (mock_httpd_register_err) {
        return (esp_err_t)mock_httpd_register_err;
    }
    if (uri != NULL && mock_route_count < MOCK_HTTPD_MAX_ROUTES) {
        mock_routes[mock_route_count++] = *uri;
    }
    return ESP_OK;
}

esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error,
                                     httpd_err_handler_func_t handler)
{
    (void)handle;
    (void)error;
    mock_httpd_err_register_calls++;
    mock_err_handler = handler;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    (void)req;
    if (type != NULL) {
        snprintf(mock_resp_type, sizeof(mock_resp_type), "%s", type);
    }
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value)
{
    (void)req;
    size_t used = strlen(mock_resp_hdr);
    snprintf(mock_resp_hdr + used, sizeof(mock_resp_hdr) - used, "%s: %s;",
             field != NULL ? field : "", value != NULL ? value : "");
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    (void)req;
    if (status != NULL) {
        snprintf(mock_resp_status, sizeof(mock_resp_status), "%s", status);
    }
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, size_t buf_len)
{
    (void)req;
    mock_resp_send_calls++;
    if (buf == NULL) {
        return ESP_OK;
    }
    if (buf_len == HTTPD_RESP_USE_STRLEN) {
        buf_len = strlen(buf);
    }
    size_t room = sizeof(mock_resp_body) - 1U - mock_resp_body_len;
    if (buf_len > room) {
        buf_len = room;
    }
    memcpy(mock_resp_body + mock_resp_body_len, buf, buf_len);
    mock_resp_body_len += buf_len;
    mock_resp_body[mock_resp_body_len] = '\0';
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg)
{
    (void)req;
    mock_resp_send_err_code = (int)error;
    if (msg != NULL) {
        snprintf(mock_resp_send_err_msg, sizeof(mock_resp_send_err_msg), "%s", msg);
    }
    return ESP_OK;
}

esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len)
{
    (void)req;
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mock_query[0] == '\0') {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(buf, buf_len, "%s", mock_query);
    return ESP_OK;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val,
                                      size_t val_size)
{
    (void)req;
    (void)field;
    if (val == NULL || val_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mock_header_name[0] == '\0') {
        val[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(val, val_size, "%s", mock_header_name);
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len)
{
    (void)req;
    mock_recv_calls++;
    if (mock_recv_fail) {
        return -1;
    }
    if (mock_recv_idle_n > 0) {
        mock_recv_idle_n--;
        return mock_recv_idle_val;
    }
    if (mock_recv_limit >= 0 && mock_recv_bytes >= mock_recv_limit) {
        return -1;
    }
    size_t avail = mock_httpd_body_len - mock_httpd_body_pos;
    if (avail == 0) {
        return 0;
    }
    size_t n = buf_len < avail ? buf_len : avail;
    if (mock_recv_limit >= 0 && (int)(mock_recv_bytes + (int)n) > mock_recv_limit) {
        n = (size_t)(mock_recv_limit - mock_recv_bytes);
    }
    if (buf != NULL && n > 0) {
        memcpy(buf, mock_httpd_body + mock_httpd_body_pos, n);
    }
    mock_httpd_body_pos += n;
    mock_recv_bytes += (int)n;
    return (int)n;
}

/* ====================================================================== */
/* esp_netif                                                               */
/* ====================================================================== */

int mock_netif_init_ret = 0;
int mock_ap_netif_null = 0;
int mock_sta_netif_null = 0;
int mock_netif_get_ip_ret = 0;
esp_ip4_addr_t mock_netif_actual_ip;
int mock_netif_set_ip_calls = 0;
int mock_netif_dhcps_stop_calls = 0;
int mock_netif_dhcps_start_calls = 0;
int mock_netif_set_dns_calls = 0;

int mock_netif_str_to_ip4(const char *s, esp_ip4_addr_t *out)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    char tail = '\0';
    if (s == NULL || out == NULL) {
        return -1;
    }
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return -1;
    }
    if (a > 255u || b > 255u || c > 255u || d > 255u) {
        return -1;
    }
    out->addr = ESP_IP4TOADDR(a, b, c, d);
    return 0;
}

esp_err_t esp_netif_init(void)
{
    return (esp_err_t)mock_netif_init_ret;
}

esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    return mock_ap_netif_null ? NULL : (esp_netif_t *)1;
}

esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    return mock_sta_netif_null ? NULL : (esp_netif_t *)1;
}

esp_err_t esp_netif_dhcps_stop(esp_netif_t *netif)
{
    (void)netif;
    mock_netif_dhcps_stop_calls++;
    return ESP_OK;
}

esp_err_t esp_netif_dhcps_start(esp_netif_t *netif)
{
    (void)netif;
    mock_netif_dhcps_start_calls++;
    return ESP_OK;
}

esp_err_t esp_netif_set_ip_info(esp_netif_t *netif, const esp_netif_ip_info_t *ip_info)
{
    (void)netif;
    (void)ip_info;
    mock_netif_set_ip_calls++;
    return ESP_OK;
}

esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip_info)
{
    (void)netif;
    if (ip_info != NULL) {
        memset(ip_info, 0, sizeof(*ip_info));
        ip_info->ip = mock_netif_actual_ip;
    }
    return (esp_err_t)mock_netif_get_ip_ret;
}

esp_err_t esp_netif_set_dns_info(esp_netif_t *netif, esp_netif_dns_type_t type,
                                 esp_netif_dns_info_t *dns)
{
    (void)netif;
    (void)type;
    (void)dns;
    mock_netif_set_dns_calls++;
    return ESP_OK;
}

esp_err_t esp_netif_str_to_ip4(const char *src, esp_ip4_addr_t *dst)
{
    return mock_netif_str_to_ip4(src, dst) == 0 ? ESP_OK : ESP_FAIL;
}

/* ====================================================================== */
/* esp_event                                                               */
/* ====================================================================== */

int mock_event_loop_ret = 0;

esp_err_t esp_event_loop_create_default(void)
{
    return (esp_err_t)mock_event_loop_ret;
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance)
{
    (void)base;
    (void)id;
    (void)handler;
    (void)arg;
    (void)instance;
    return ESP_OK;
}

/* ====================================================================== */
/* esp_wifi                                                                */
/* ====================================================================== */

int mock_wifi_init_ret = 0;
int mock_wifi_init_calls = 0;
int mock_wifi_mode_sta_ret = 0;
int mock_wifi_mode_ap_ret = 0;
int mock_wifi_set_config_ret = 0;
int mock_wifi_start_first_ret = 0;
int mock_wifi_start_calls = 0;
int mock_wifi_stop_calls = 0;
int mock_wifi_set_storage_calls = 0;
int mock_wifi_set_bandwidth_calls = 0;
int mock_wifi_set_ps_calls = 0;

int mock_scan_start_ret = 0;
int mock_scan_num_ret = 0;
uint16_t mock_scan_num = 0;
int mock_scan_records_ret = 0;
uint16_t mock_scan_records_n = 0;
wifi_ap_record_t mock_scan_records[16];
int mock_clear_ap_list_calls = 0;

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config;
    mock_wifi_init_calls++;
    return (esp_err_t)mock_wifi_init_ret;
}

esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
    (void)storage;
    mock_wifi_set_storage_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    if (mode == WIFI_MODE_STA) {
        return (esp_err_t)mock_wifi_mode_sta_ret;
    }
    return (esp_err_t)mock_wifi_mode_ap_ret;
}

esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t *conf)
{
    (void)interface;
    (void)conf;
    return (esp_err_t)mock_wifi_set_config_ret;
}

esp_err_t esp_wifi_start(void)
{
    if (mock_wifi_start_calls++ == 0) {
        return (esp_err_t)mock_wifi_start_first_ret;
    }
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    mock_wifi_stop_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    (void)config;
    (void)block;
    return (esp_err_t)mock_scan_start_ret;
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t *number)
{
    if (number != NULL) {
        *number = mock_scan_num;
    }
    return (esp_err_t)mock_scan_num_ret;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *ap_records)
{
    if (mock_scan_records_ret) {
        return (esp_err_t)mock_scan_records_ret;
    }
    uint16_t want = number != NULL ? *number : 0;
    uint16_t n = want < mock_scan_records_n ? want : mock_scan_records_n;
    for (uint16_t i = 0; i < n; ++i) {
        ap_records[i] = mock_scan_records[i];
    }
    if (number != NULL) {
        *number = n;
    }
    return ESP_OK;
}

esp_err_t esp_wifi_clear_ap_list(void)
{
    mock_clear_ap_list_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_set_bandwidth(wifi_interface_t interface, wifi_bandwidth_t bandwidth)
{
    (void)interface;
    (void)bandwidth;
    mock_wifi_set_bandwidth_calls++;
    return ESP_OK;
}

esp_err_t esp_wifi_set_ps(wifi_ps_type_t type)
{
    (void)type;
    mock_wifi_set_ps_calls++;
    return ESP_OK;
}

/* ====================================================================== */
/* lwIP sockets (captive-portal DNS)                                       */
/* ====================================================================== */

int mock_socket_ret = 5;
int mock_bind_ret = 0;
int mock_socket_close_calls = 0;
int mock_sendto_calls = 0;
size_t mock_sendto_last_len = 0;

#define MOCK_RECV_SCRIPT_MAX 8
int mock_recv_script_n = 0;
int mock_recv_script_i = 0;
int mock_recv_script_len[MOCK_RECV_SCRIPT_MAX];
uint8_t mock_recv_script_buf[MOCK_RECV_SCRIPT_MAX][512];

uint16_t mock_lwip_htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

uint32_t mock_lwip_htonl(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

int mock_socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    return mock_socket_ret;
}

int mock_bind(int sockfd, const struct sockaddr *addr, mock_socklen_t addrlen)
{
    (void)sockfd;
    (void)addr;
    (void)addrlen;
    return mock_bind_ret;
}

int mock_setsockopt(int sockfd, int level, int optname, const void *optval,
                    mock_socklen_t optlen)
{
    (void)sockfd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return 0;
}

int mock_recvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, mock_socklen_t *addrlen)
{
    (void)sockfd;
    (void)flags;
    (void)src_addr;
    (void)addrlen;
    if (mock_recv_script_i >= mock_recv_script_n) {
        return -1;
    }
    int idx = mock_recv_script_i++;
    int n = mock_recv_script_len[idx];
    if (n < 0) {
        return n;
    }
    size_t take = (size_t)n < len ? (size_t)n : len;
    if (buf != NULL && take > 0) {
        memcpy(buf, mock_recv_script_buf[idx], take);
    }
    return (int)take;
}

int mock_sendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, mock_socklen_t addrlen)
{
    (void)sockfd;
    (void)buf;
    (void)flags;
    (void)dest_addr;
    (void)addrlen;
    mock_sendto_calls++;
    mock_sendto_last_len = len;
    return (int)len;
}

int mock_socket_close(int fd)
{
    (void)fd;
    mock_socket_close_calls++;
    return 0;
}

/* ====================================================================== */
/* settings                                                                */
/* ====================================================================== */

settings_config_t mock_settings_cfg;
int mock_settings_load_ret = 0;
int mock_settings_load_calls = 0;
int mock_settings_save_ret = 0;
int mock_settings_save_calls = 0;
int mock_settings_save_deferred_calls = 0;

uint8_t mock_cv[SETTINGS_CV_COUNT + 1];
int mock_cv_read_ret = 0;
int mock_cv_write_ret = 0;
int mock_cv_commit_ret = 0;
int mock_cv_write_calls = 0;
int mock_cv_commit_calls = 0;
uint16_t mock_cv_last_index = 0;
uint8_t mock_cv_last_value = 0;

settings_track_t mock_tracks[SETTINGS_MAX_TRACKS];
size_t mock_tracks_count = 0;
int mock_tracks_load_ret = 0;
settings_track_t mock_saved_tracks[SETTINGS_MAX_TRACKS];
size_t mock_saved_tracks_count = 0;
int mock_tracks_save_ret = 0;
int mock_tracks_save_calls = 0;

uint8_t mock_cats[SETTINGS_MAX_TRACKS];
size_t mock_cats_count = 0;
int mock_cats_load_ret = 0;
int mock_cats_save_ret = 0;
uint8_t mock_saved_cats[SETTINGS_MAX_TRACKS];
size_t mock_saved_cats_count = 0;
int mock_cats_save_calls = 0;

settings_func_map_t mock_map[SETTINGS_FUNC_MAP_COUNT];
size_t mock_map_count = 0;
int mock_map_load_ret = 0;
int mock_map_save_ret = 0;
int mock_map_save_calls = 0;
settings_func_map_t mock_saved_map[SETTINGS_FUNC_MAP_COUNT];

settings_aux_cfg_t mock_aux[SETTINGS_AUX_COUNT];
size_t mock_aux_count = 0;
int mock_aux_load_ret = 0;
int mock_aux_save_ret = 0;
int mock_aux_save_calls = 0;
settings_aux_cfg_t mock_saved_aux[SETTINGS_AUX_COUNT];

int mock_bemf_use_save_ret = 0;
int mock_bemf_use_save_calls = 0;
int mock_bemf_use_saved_value = -1;
int mock_factory_reset_ret = 0;
int mock_factory_reset_calls = 0;

esp_err_t settings_load(settings_config_t *cfg)
{
    mock_settings_load_calls++;
    if (cfg != NULL) {
        *cfg = mock_settings_cfg;
    }
    return (esp_err_t)mock_settings_load_ret;
}

esp_err_t settings_save(const settings_config_t *cfg)
{
    (void)cfg;
    mock_settings_save_calls++;
    return (esp_err_t)mock_settings_save_ret;
}

esp_err_t settings_save_deferred(const settings_config_t *cfg)
{
    (void)cfg;
    mock_settings_save_deferred_calls++;
    return ESP_OK;
}

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (out == NULL || idx < 1U || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = mock_cv[idx];
    return (esp_err_t)mock_cv_read_ret;
}

esp_err_t settings_cv_write(uint16_t idx, uint8_t value)
{
    mock_cv_write_calls++;
    mock_cv_last_index = idx;
    mock_cv_last_value = value;
    if (idx >= 1U && idx <= SETTINGS_CV_COUNT) {
        mock_cv[idx] = value;
    }
    return (esp_err_t)mock_cv_write_ret;
}

esp_err_t settings_cv_commit(void)
{
    mock_cv_commit_calls++;
    return (esp_err_t)mock_cv_commit_ret;
}

int mock_cv_commit_deferred_calls = 0;

void settings_cv_commit_deferred(void)
{
    mock_cv_commit_deferred_calls++;
}

esp_err_t settings_tracks_load(settings_track_t *tracks, size_t *count)
{
    if (tracks != NULL && count != NULL) {
        memcpy(tracks, mock_tracks, sizeof(mock_tracks));
        *count = mock_tracks_count;
    }
    return (esp_err_t)mock_tracks_load_ret;
}

esp_err_t settings_tracks_save(const settings_track_t *tracks, size_t count)
{
    mock_tracks_save_calls++;
    if (tracks != NULL && count <= SETTINGS_MAX_TRACKS) {
        memcpy(mock_saved_tracks, tracks, count * sizeof(settings_track_t));
        mock_saved_tracks_count = count;
    }
    return (esp_err_t)mock_tracks_save_ret;
}

esp_err_t settings_track_cats_load(uint8_t *cats, size_t *count)
{
    if (cats != NULL && count != NULL) {
        memcpy(cats, mock_cats, sizeof(mock_cats));
        *count = mock_cats_count;
    }
    return (esp_err_t)mock_cats_load_ret;
}

esp_err_t settings_track_cats_save(const uint8_t *cats, size_t count)
{
    mock_cats_save_calls++;
    if (cats != NULL && count <= SETTINGS_MAX_TRACKS) {
        memcpy(mock_saved_cats, cats, count);
        mock_saved_cats_count = count;
    }
    return (esp_err_t)mock_cats_save_ret;
}

esp_err_t settings_func_map_load(settings_func_map_t *map, size_t *count)
{
    if (map != NULL && count != NULL) {
        memcpy(map, mock_map, sizeof(mock_map));
        *count = mock_map_count;
    }
    return (esp_err_t)mock_map_load_ret;
}

esp_err_t settings_func_map_save(const settings_func_map_t *map, size_t count)
{
    mock_map_save_calls++;
    if (map != NULL && count <= SETTINGS_FUNC_MAP_COUNT) {
        memcpy(mock_saved_map, map, count * sizeof(settings_func_map_t));
    }
    return (esp_err_t)mock_map_save_ret;
}

/* ---- canonical function bindings (test doubles) ---- */

func_binding_t mock_binds[FUNC_BIND_MAX];
size_t mock_binds_count = 0;
int mock_bind_load_ret = ESP_ERR_NOT_FOUND; /* empty store by default */
int mock_bind_save_ret = 0;
int mock_bind_save_calls = 0;

esp_err_t settings_func_bind_load(func_binding_t *bind, size_t *count)
{
    if (bind != NULL && count != NULL) {
        memcpy(bind, mock_binds, sizeof(mock_binds));
        *count = mock_binds_count;
    }
    return (esp_err_t)mock_bind_load_ret;
}

esp_err_t settings_func_bind_save(const func_binding_t *bind, size_t count)
{
    mock_bind_save_calls++;
    if (bind != NULL && count <= FUNC_BIND_MAX) {
        memcpy(mock_binds, bind, count * sizeof(func_binding_t));
        mock_binds_count = count;
    }
    return (esp_err_t)mock_bind_save_ret;
}

esp_err_t settings_func_bind_legacy_convert(const settings_func_map_t *map, size_t map_count,
                                            func_binding_t *out, size_t *out_count)
{
    if (map == NULL || out == NULL || out_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = 0;
    for (size_t f = 0; f < map_count && f < SETTINGS_FUNC_MAP_COUNT; ++f) {
        uint8_t dir = FUNC_DIR_ANY;
        if (map[f].dir == SETTINGS_FUNC_DIR_FWD) {
            dir = FUNC_DIR_FWD;
        } else if (map[f].dir == SETTINGS_FUNC_DIR_REV) {
            dir = FUNC_DIR_REV;
        }
        uint8_t state = FUNC_STATE_ANY;
        if (map[f].speed == SETTINGS_FUNC_SPD_MOVING) {
            state = FUNC_STATE_MOVING;
        } else if (map[f].speed == SETTINGS_FUNC_SPD_STOP) {
            state = FUNC_STATE_STOPPED;
        }
        const uint16_t light = SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R;
        for (uint8_t bit = 0; bit < 9U && n < FUNC_BIND_MAX; ++bit) {
            if ((map[f].aux_mask & (uint16_t)(1U << bit)) == 0U) {
                continue;
            }
            if ((map[f].aux_mask & light) == light && bit <= 1U) {
                if (map[f].dir == SETTINGS_FUNC_DIR_FWD && bit != 0U) {
                    continue;
                }
                if (map[f].dir == SETTINGS_FUNC_DIR_REV && bit != 1U) {
                    continue;
                }
                memset(&out[n], 0, sizeof(out[n]));
                out[n].used = 1;
                out[n].fn = (uint8_t)f;
                out[n].target_type = FUNC_TARGET_OUTPUT;
                out[n].target_id = bit;
                out[n].dir = (bit == 0U) ? FUNC_DIR_FWD : FUNC_DIR_REV;
                out[n].state = state;
                n++;
                continue;
            }
            memset(&out[n], 0, sizeof(out[n]));
            out[n].used = 1;
            out[n].fn = (uint8_t)f;
            out[n].target_type = FUNC_TARGET_OUTPUT;
            out[n].target_id = bit;
            out[n].dir = dir;
            out[n].state = state;
            n++;
        }
        const uint8_t slots[2] = { map[f].slot_a, map[f].slot_b };
        for (int s = 0; s < 2 && n < FUNC_BIND_MAX; ++s) {
            if (slots[s] != 0U) {
                memset(&out[n], 0, sizeof(out[n]));
                out[n].used = 1;
                out[n].fn = (uint8_t)f;
                out[n].target_type = FUNC_TARGET_SLOT;
                out[n].target_id = slots[s];
                out[n].dir = dir;
                out[n].state = state;
                out[n].mode = SOUND_MODE_LATCHED;
                n++;
            }
        }
    }
    *out_count = n;
    return ESP_OK;
}

esp_err_t settings_func_bind_add(func_binding_t *bind, size_t *count, const func_binding_t *b)
{
    if (bind == NULL || count == NULL || b == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*count >= FUNC_BIND_MAX) {
        return ESP_ERR_NO_MEM;
    }
    bind[*count] = *b;
    bind[*count].used = 1;
    (*count)++;
    return ESP_OK;
}

esp_err_t settings_func_bind_remove(func_binding_t *bind, size_t *count, size_t idx)
{
    if (bind == NULL || count == NULL || idx >= *count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = idx; i + 1U < *count; ++i) {
        bind[i] = bind[i + 1U];
    }
    (*count)--;
    memset(&bind[*count], 0, sizeof(bind[0]));
    return ESP_OK;
}

esp_err_t settings_aux_cfg_load(settings_aux_cfg_t *cfg, size_t *count)
{
    if (cfg != NULL && count != NULL) {
        memcpy(cfg, mock_aux, sizeof(mock_aux));
        *count = mock_aux_count;
    }
    return (esp_err_t)mock_aux_load_ret;
}

esp_err_t settings_aux_cfg_save(const settings_aux_cfg_t *cfg, size_t count)
{
    mock_aux_save_calls++;
    if (cfg != NULL && count <= SETTINGS_AUX_COUNT) {
        memcpy(mock_saved_aux, cfg, count * sizeof(settings_aux_cfg_t));
    }
    return (esp_err_t)mock_aux_save_ret;
}

esp_err_t settings_bemf_use_save(bool enabled)
{
    mock_bemf_use_save_calls++;
    mock_bemf_use_saved_value = enabled ? 1 : 0;
    return (esp_err_t)mock_bemf_use_save_ret;
}

esp_err_t settings_factory_reset(void)
{
    mock_factory_reset_calls++;
    return (esp_err_t)mock_factory_reset_ret;
}

/* ---- sound stubs ---- */
int mock_sound_scheme_enabled = 0;
int mock_sound_function_calls = 0;
uint8_t mock_sound_last_fn = 0;
bool mock_sound_last_state = false;
int mock_sound_stop_all_calls = 0;

sound_scheme_t mock_sound_scheme;
int mock_sound_scheme_get_ret = 0;
int mock_sound_scheme_set_calls = 0;
int mock_sound_scheme_save_ret = 0;
int mock_sound_scheme_save_calls = 0;
int mock_sound_lint_ret = 0;
sound_status_t mock_sound_status;

esp_err_t sound_scheme_get(sound_scheme_t *out)
{
    if (out != NULL) {
        *out = mock_sound_scheme;
    }
    return (esp_err_t)mock_sound_scheme_get_ret;
}

esp_err_t sound_scheme_set(const sound_scheme_t *in)
{
    mock_sound_scheme_set_calls++;
    if (in != NULL) {
        mock_sound_scheme = *in;
    }
    return ESP_OK;
}

esp_err_t sound_scheme_save(void)
{
    mock_sound_scheme_save_calls++;
    return (esp_err_t)mock_sound_scheme_save_ret;
}

void sound_status_get(sound_status_t *out)
{
    if (out != NULL) {
        *out = mock_sound_status;
    }
}

int sound_lint(char *out, size_t cap)
{
    if (out != NULL && cap != 0U) {
        out[0] = '\0';
    }
    return mock_sound_lint_ret;
}

uint8_t sound_type_get(void) { return mock_sound_scheme.type; }
esp_err_t sound_type_set(uint8_t type) { mock_sound_scheme.type = type; return ESP_OK; }

esp_err_t sound_engine_get(sound_engine_t *out)
{
    if (out != NULL) {
        *out = mock_sound_scheme.engine;
    }
    return ESP_OK;
}

esp_err_t sound_engine_set(const sound_engine_t *in)
{
    if (in != NULL) {
        mock_sound_scheme.engine = *in;
    }
    return ESP_OK;
}

esp_err_t sound_table_get(uint8_t idx, sound_table_t *out)
{
    if (out != NULL && idx < SOUND_MAX_TABLES) {
        *out = mock_sound_scheme.tables[idx];
    }
    return ESP_OK;
}

esp_err_t sound_table_set(uint8_t idx, const sound_table_t *t)
{
    if (t != NULL && idx < SOUND_MAX_TABLES) {
        mock_sound_scheme.tables[idx] = *t;
    }
    return ESP_OK;
}

esp_err_t sound_extra_get(uint8_t idx, sound_extra_t *out)
{
    if (out != NULL && idx < SOUND_MAX_EXTRAS) {
        *out = mock_sound_scheme.extras[idx];
    }
    return ESP_OK;
}

esp_err_t sound_extra_set(uint8_t idx, const sound_extra_t *e)
{
    if (e != NULL && idx < SOUND_MAX_EXTRAS) {
        mock_sound_scheme.extras[idx] = *e;
    }
    return ESP_OK;
}

bool sound_scheme_enabled(void) { return mock_sound_scheme_enabled != 0; }
int mock_sound_reload_calls = 0;
void sound_reload_bindings(void) { mock_sound_reload_calls++; }
void sound_function(uint8_t fn, bool state)
{
    mock_sound_function_calls++;
    mock_sound_last_fn = fn;
    mock_sound_last_state = state;
}
void sound_stop_all(void) { mock_sound_stop_all_calls++; }

/* Scheme project files (create/select/delete/export/import/list). */
int mock_sound_last_name_calls = 0;
char mock_sound_last_name[64] = { 0 };
int mock_sound_active_name_ret = 0;
char mock_sound_active_name[64] = { 0 };
int mock_sound_load_scheme_ret = 0;
int mock_sound_load_scheme_calls = 0;

esp_err_t sound_active_name_get(char *out, size_t cap)
{
    mock_sound_last_name_calls++;
    if (out != NULL && cap > 0) {
        snprintf(out, cap, "%s", mock_sound_active_name);
    }
    return (esp_err_t)mock_sound_active_name_ret;
}

esp_err_t sound_load_scheme(const char *name)
{
    mock_sound_load_scheme_calls++;
    if (name != NULL) {
        snprintf(mock_sound_last_name, sizeof(mock_sound_last_name), "%s", name);
    }
    return (esp_err_t)mock_sound_load_scheme_ret;
}

int mock_sound_scheme_create_ret = 0;
int mock_sound_scheme_create_calls = 0;
uint8_t mock_sound_last_type = 0;

esp_err_t sound_scheme_create(const char *name, uint8_t type)
{
    mock_sound_scheme_create_calls++;
    if (name != NULL) {
        snprintf(mock_sound_last_name, sizeof(mock_sound_last_name), "%s", name);
    }
    mock_sound_last_type = type;
    return (esp_err_t)mock_sound_scheme_create_ret;
}

int mock_sound_scheme_delete_ret = 0;
int mock_sound_scheme_delete_calls = 0;

esp_err_t sound_scheme_delete(const char *name)
{
    mock_sound_scheme_delete_calls++;
    if (name != NULL) {
        snprintf(mock_sound_last_name, sizeof(mock_sound_last_name), "%s", name);
    }
    return (esp_err_t)mock_sound_scheme_delete_ret;
}

int mock_sound_scheme_export_ret = 0;
int mock_sound_scheme_export_calls = 0;
size_t mock_sound_export_len = 0;
uint8_t mock_sound_export_byte = 0;

esp_err_t sound_scheme_export(const char *name, uint8_t *buf, size_t cap, size_t *out_len)
{
    (void)name;
    mock_sound_scheme_export_calls++;
    if (mock_sound_scheme_export_ret != 0) {
        return (esp_err_t)mock_sound_scheme_export_ret;
    }
    size_t n = mock_sound_export_len < cap ? mock_sound_export_len : cap;
    if (buf != NULL) {
        memset(buf, mock_sound_export_byte, n);
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return ESP_OK;
}

int mock_sound_scheme_import_ret = 0;
int mock_sound_scheme_import_calls = 0;
size_t mock_sound_import_last_len = 0;
bool mock_sound_import_last_activate = false;

esp_err_t sound_scheme_import(const char *name, const uint8_t *buf, size_t len, bool activate)
{
    (void)buf;
    mock_sound_scheme_import_calls++;
    if (name != NULL) {
        snprintf(mock_sound_last_name, sizeof(mock_sound_last_name), "%s", name);
    }
    mock_sound_import_last_len = len;
    mock_sound_import_last_activate = activate;
    return (esp_err_t)mock_sound_scheme_import_ret;
}

int mock_sound_scheme_list_ret = 0;
int mock_sound_scheme_list_calls = 0;
int mock_sound_scheme_list_count = 0;
char mock_sound_scheme_list_names[8][SOUND_FILE_MAX];

esp_err_t sound_scheme_list(char names[][SOUND_FILE_MAX], size_t max, size_t *count)
{
    mock_sound_scheme_list_calls++;
    if (mock_sound_scheme_list_ret != 0) {
        return (esp_err_t)mock_sound_scheme_list_ret;
    }
    size_t n = (size_t)mock_sound_scheme_list_count;
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; ++i) {
        snprintf(names[i], SOUND_FILE_MAX, "%s", mock_sound_scheme_list_names[i]);
    }
    if (count != NULL) {
        *count = n;
    }
    return ESP_OK;
}

esp_err_t sound_brake_get(sound_brake_t *out)
{
    if (out != NULL) {
        *out = mock_sound_scheme.brake;
    }
    return ESP_OK;
}

esp_err_t sound_brake_set(const sound_brake_t *in)
{
    if (in != NULL) {
        mock_sound_scheme.brake = *in;
    }
    return ESP_OK;
}

/* ====================================================================== */
/* audio                                                                   */
/* ====================================================================== */

int mock_audio_is_playing = 0;
int mock_audio_validate_ret = 0;
int mock_audio_voice_play_ret = 0;
int mock_audio_voice_play_calls = 0;
int mock_audio_voice_stop_calls = 0;
int mock_audio_stop_calls = 0;
int mock_audio_set_volume_calls = 0;
uint8_t mock_audio_last_volume = 0;
uint8_t mock_audio_last_voice = 0;
char mock_audio_last_path[200];

esp_err_t audio_validate_wav(const char *path)
{
    (void)path;
    return (esp_err_t)mock_audio_validate_ret;
}

esp_err_t audio_voice_play(uint8_t voice, const char *path, bool loop, uint8_t volume)
{
    (void)loop;
    (void)volume;
    mock_audio_voice_play_calls++;
    mock_audio_last_voice = voice;
    if (path != NULL) {
        snprintf(mock_audio_last_path, sizeof(mock_audio_last_path), "%s", path);
    }
    return (esp_err_t)mock_audio_voice_play_ret;
}

esp_err_t audio_voice_stop(uint8_t voice)
{
    (void)voice;
    mock_audio_voice_stop_calls++;
    return ESP_OK;
}

void audio_stop_all(void)
{
    mock_audio_stop_calls++;
}

void audio_set_volume(uint8_t vol)
{
    mock_audio_set_volume_calls++;
    mock_audio_last_volume = vol;
}

uint8_t audio_get_volume(void)
{
    return mock_audio_last_volume;
}

bool audio_is_playing(void)
{
    return mock_audio_is_playing != 0;
}

esp_err_t audio_play(const char *path)
{
    (void)path;
    return ESP_OK;
}

esp_err_t audio_stop(void)
{
    mock_audio_stop_calls++;
    return ESP_OK;
}

/* ====================================================================== */
/* motor                                                                   */
/* ====================================================================== */

uint8_t mock_motor_speed = 0;
bool mock_motor_forward = true;
int mock_motor_stop_calls = 0;
int mock_motor_set_speed_calls = 0;
uint8_t mock_motor_set_speed_value = 0;
bool mock_motor_set_speed_fwd = true;

motor_bemf_cal_info_t mock_bemf_cal_info;
motor_bemf_base_info_t mock_bemf_base_info;
int mock_bemf_enabled = 1;
int mock_bemf_cal_start_ret = 0;
int mock_bemf_cal_clear_ret = 0;
int mock_bemf_cal_start_calls = 0;
int mock_bemf_cal_clear_calls = 0;

void motor_stop(void)
{
    mock_motor_stop_calls++;
}

int mock_motor_emergency_stop_calls = 0;

void motor_emergency_stop(void)
{
    mock_motor_emergency_stop_calls++;
}

esp_err_t motor_set_speed(uint8_t speed128, bool forward)
{
    mock_motor_set_speed_calls++;
    mock_motor_set_speed_value = speed128;
    mock_motor_set_speed_fwd = forward;
    mock_motor_speed = speed128;
    mock_motor_forward = forward;
    return ESP_OK;
}

void motor_get_status(uint8_t *out_speed128, bool *out_forward)
{
    if (out_speed128 != NULL) {
        *out_speed128 = mock_motor_speed;
    }
    if (out_forward != NULL) {
        *out_forward = mock_motor_forward;
    }
}

void motor_bemf_cal_info(motor_bemf_cal_info_t *info)
{
    if (info != NULL) {
        *info = mock_bemf_cal_info;
    }
}

void motor_bemf_base_info(motor_bemf_base_info_t *info)
{
    if (info != NULL) {
        *info = mock_bemf_base_info;
    }
}

esp_err_t motor_bemf_cal_start(void)
{
    mock_bemf_cal_start_calls++;
    return (esp_err_t)mock_bemf_cal_start_ret;
}

esp_err_t motor_bemf_cal_clear(void)
{
    mock_bemf_cal_clear_calls++;
    return (esp_err_t)mock_bemf_cal_clear_ret;
}

void motor_set_bemf_enabled(bool enabled)
{
    mock_bemf_enabled = enabled ? 1 : 0;
}

bool motor_get_bemf_enabled(void)
{
    return mock_bemf_enabled != 0;
}

/* ====================================================================== */
/* auxio                                                                   */
/* ====================================================================== */

int mock_auxio_init_ret = 0;
int mock_auxio_set_enabled_calls = 0;
int mock_auxio_set_effect_calls = 0;
int mock_auxio_config_calls = 0;
uint8_t mock_auxio_last_channel = 0;
uint8_t mock_auxio_last_pwm_on = 0;
uint8_t mock_auxio_last_pwm_off = 0;
int mock_auxio_last_mode = 0;
uint16_t mock_auxio_last_period = 0;
int mock_auxio_last_enabled = -1;

esp_err_t auxio_init(void)
{
    return (esp_err_t)mock_auxio_init_ret;
}

esp_err_t auxio_set_enabled(uint8_t channel, bool enabled)
{
    mock_auxio_set_enabled_calls++;
    mock_auxio_last_channel = channel;
    mock_auxio_last_enabled = enabled ? 1 : 0;
    return ESP_OK;
}

esp_err_t auxio_set_output(uint8_t channel, bool enabled, uint8_t pwm)
{
    (void)channel;
    (void)enabled;
    (void)pwm;
    return ESP_OK;
}

esp_err_t auxio_set_effect(uint8_t channel, bool enabled, uint8_t pwm_on, uint8_t pwm_off,
                           auxio_effect_t mode, uint16_t period_ms)
{
    mock_auxio_set_effect_calls++;
    mock_auxio_last_channel = channel;
    mock_auxio_last_enabled = enabled ? 1 : 0;
    mock_auxio_last_pwm_on = pwm_on;
    mock_auxio_last_pwm_off = pwm_off;
    mock_auxio_last_mode = (int)mode;
    mock_auxio_last_period = period_ms;
    return ESP_OK;
}

esp_err_t auxio_config(uint8_t channel, uint8_t pwm_on, uint8_t pwm_off,
                       auxio_effect_t mode, uint16_t period_ms)
{
    mock_auxio_config_calls++;
    mock_auxio_last_channel = channel;
    mock_auxio_last_pwm_on = pwm_on;
    mock_auxio_last_pwm_off = pwm_off;
    mock_auxio_last_mode = (int)mode;
    mock_auxio_last_period = period_ms;
    return ESP_OK;
}

/* ====================================================================== */
/* storage                                                                 */
/* ====================================================================== */

int mock_storage_is_mounted = 1;
int mock_storage_free_ret = 0;
uint64_t mock_storage_free = 1234u;
int64_t mock_storage_get_time_advance_us = 0;

bool storage_is_mounted(void)
{
    return mock_storage_is_mounted != 0;
}

esp_err_t storage_get_free_bytes(uint64_t *out_free_bytes)
{
    if (out_free_bytes != NULL) {
        *out_free_bytes = mock_storage_free;
    }
    extern int64_t mock_timer_now_us;
    mock_timer_now_us += mock_storage_get_time_advance_us;
    return (esp_err_t)mock_storage_free_ret;
}

/* ====================================================================== */
/* dcc                                                                     */
/* ====================================================================== */

int mock_dcc_reload_config_calls = 0;

void dcc_reload_config(void)
{
    mock_dcc_reload_config_calls++;
}

/* ====================================================================== */
/* reset                                                                   */
/* ====================================================================== */

/* Restore every mock flag/injector to its neutral default and clear the
 * web.c module state so each Unity test starts from a known baseline. */
void mock_web_reset(void)
{
    mock_httpd_reset();
    mock_alloc_fail_at = -1;
    mock_alloc_calls = 0;
    mock_fsync_ret = 0;
    mock_ota_fwrite_fail = 0;
    mock_ota_fflush_fail = 0;
    mock_ota_fclose_fail = 0;
    mock_queue_send_fail = 0;
    mock_queue_send_fail_after = -1;
    mock_queue_send_calls = 0;
    mock_ota_write_err = 0;
    mock_ota_write_fail_after = -1;
    mock_ota_write_calls = 0;
    mock_ota_begin_err = 0;
    mock_ota_end_err = 0;
    mock_ota_set_boot_err = 0;
    mock_ota_partition_absent = 0;
    mock_ota_bytes = 0;
    mock_ota_next_size = 4u * 1024u * 1024u;
    mock_esp_restart_calls = 0;

    mock_httpd_start_err = 0;
    mock_httpd_register_err = 0;
    mock_httpd_start_calls = 0;
    mock_httpd_register_calls = 0;
    mock_httpd_err_register_calls = 0;
    mock_route_count = 0;
    memset(&mock_httpd_last_cfg, 0, sizeof(mock_httpd_last_cfg));
    memset(mock_routes, 0, sizeof(mock_routes));
    mock_err_handler = NULL;

    mock_netif_init_ret = 0;
    mock_ap_netif_null = 0;
    mock_sta_netif_null = 0;
    mock_netif_get_ip_ret = 0;
    memset(&mock_netif_actual_ip, 0, sizeof(mock_netif_actual_ip));
    mock_netif_set_ip_calls = 0;
    mock_netif_dhcps_stop_calls = 0;
    mock_netif_dhcps_start_calls = 0;
    mock_netif_set_dns_calls = 0;
    mock_event_loop_ret = 0;

    mock_wifi_init_ret = 0;
    mock_wifi_init_calls = 0;
    mock_wifi_mode_sta_ret = 0;
    mock_wifi_mode_ap_ret = 0;
    mock_wifi_set_config_ret = 0;
    mock_wifi_start_first_ret = 0;
    mock_wifi_start_calls = 0;
    mock_wifi_stop_calls = 0;
    mock_wifi_set_storage_calls = 0;
    mock_wifi_set_bandwidth_calls = 0;
    mock_wifi_set_ps_calls = 0;

    mock_scan_start_ret = 0;
    mock_scan_num_ret = 0;
    mock_scan_num = 0;
    mock_scan_records_ret = 0;
    mock_scan_records_n = 0;
    memset(mock_scan_records, 0, sizeof(mock_scan_records));
    mock_clear_ap_list_calls = 0;

    mock_socket_ret = 5;
    mock_bind_ret = 0;
    mock_socket_close_calls = 0;
    mock_sendto_calls = 0;
    mock_sendto_last_len = 0;
    mock_recv_script_n = 0;
    mock_recv_script_i = 0;
    memset(mock_recv_script_len, 0, sizeof(mock_recv_script_len));
    memset(mock_recv_script_buf, 0, sizeof(mock_recv_script_buf));

    memset(&mock_settings_cfg, 0, sizeof(mock_settings_cfg));
    mock_settings_cfg.wifi_mode = 1;
    mock_settings_cfg.port = 80;
    mock_settings_load_ret = 0;
    mock_settings_load_calls = 0;
    mock_settings_save_ret = 0;
    mock_settings_save_calls = 0;
    mock_settings_save_deferred_calls = 0;

    memset(mock_cv, 0, sizeof(mock_cv));
    mock_cv[7] = 7;
    mock_cv[29] = 0x02;
    mock_cv_read_ret = 0;
    mock_cv_write_ret = 0;
    mock_cv_commit_ret = 0;
    mock_cv_write_calls = 0;
    mock_cv_commit_calls = 0;
    mock_cv_commit_deferred_calls = 0;
    mock_cv_last_index = 0;
    mock_cv_last_value = 0;

    memset(mock_tracks, 0, sizeof(mock_tracks));
    memset(mock_saved_tracks, 0, sizeof(mock_saved_tracks));
    mock_tracks_count = 0;
    mock_tracks_load_ret = 0;
    mock_tracks_save_ret = 0;
    mock_tracks_save_calls = 0;
    mock_saved_tracks_count = 0;

    memset(mock_cats, 0, sizeof(mock_cats));
    memset(mock_saved_cats, 0, sizeof(mock_saved_cats));
    mock_cats_count = 0;
    mock_cats_load_ret = 0;
    mock_cats_save_ret = 0;
    mock_cats_save_calls = 0;
    mock_saved_cats_count = 0;

    memset(mock_map, 0, sizeof(mock_map));
    memset(mock_saved_map, 0, sizeof(mock_saved_map));
    mock_map_count = 0;
    mock_map_load_ret = 0;
    mock_map_save_ret = 0;
    mock_map_save_calls = 0;

    memset(mock_binds, 0, sizeof(mock_binds));
    mock_binds_count = 0;
    mock_bind_load_ret = ESP_ERR_NOT_FOUND;
    mock_bind_save_ret = 0;
    mock_bind_save_calls = 0;

    memset(mock_aux, 0, sizeof(mock_aux));
    memset(mock_saved_aux, 0, sizeof(mock_saved_aux));
    for (int i = 0; i < SETTINGS_AUX_COUNT; ++i) {
        mock_aux[i].level = 100;
        mock_aux[i].effect = 0;
    }
    mock_aux_count = SETTINGS_AUX_COUNT;
    mock_aux_load_ret = 0;
    mock_aux_save_ret = 0;
    mock_aux_save_calls = 0;

    mock_bemf_use_save_ret = 0;
    mock_bemf_use_save_calls = 0;
    mock_bemf_use_saved_value = -1;
    mock_factory_reset_ret = 0;
    mock_factory_reset_calls = 0;

    mock_sound_scheme_enabled = 0;
    mock_sound_function_calls = 0;
    mock_sound_last_fn = 0;
    mock_sound_last_state = false;
    mock_sound_stop_all_calls = 0;
    memset(&mock_sound_scheme, 0, sizeof(mock_sound_scheme));
    memset(&mock_sound_status, 0, sizeof(mock_sound_status));
    mock_sound_scheme_get_ret = 0;
    mock_sound_scheme_set_calls = 0;
    mock_sound_scheme_save_ret = 0;
    mock_sound_scheme_save_calls = 0;
    mock_sound_lint_ret = 0;
    mock_sound_reload_calls = 0;
    mock_sound_last_name_calls = 0;
    mock_sound_last_name[0] = '\0';
    mock_sound_active_name_ret = 0;
    mock_sound_active_name[0] = '\0';
    mock_sound_load_scheme_ret = 0;
    mock_sound_load_scheme_calls = 0;
    mock_sound_scheme_create_ret = 0;
    mock_sound_scheme_create_calls = 0;
    mock_sound_last_type = 0;
    mock_sound_scheme_delete_ret = 0;
    mock_sound_scheme_delete_calls = 0;
    mock_sound_scheme_export_ret = 0;
    mock_sound_scheme_export_calls = 0;
    mock_sound_export_len = 0;
    mock_sound_export_byte = 0;
    mock_sound_scheme_import_ret = 0;
    mock_sound_scheme_import_calls = 0;
    mock_sound_import_last_len = 0;
    mock_sound_import_last_activate = false;
    mock_sound_scheme_list_ret = 0;
    mock_sound_scheme_list_calls = 0;
    mock_sound_scheme_list_count = 0;
    memset(mock_sound_scheme_list_names, 0, sizeof(mock_sound_scheme_list_names));

    mock_audio_is_playing = 0;
    mock_audio_validate_ret = 0;
    mock_audio_voice_play_ret = 0;
    mock_audio_voice_play_calls = 0;
    mock_audio_voice_stop_calls = 0;
    mock_audio_stop_calls = 0;
    mock_audio_set_volume_calls = 0;
    mock_audio_last_volume = 0;
    mock_audio_last_voice = 0;
    mock_audio_last_path[0] = '\0';

    mock_motor_speed = 0;
    mock_motor_forward = true;
    mock_motor_stop_calls = 0;
    mock_motor_set_speed_calls = 0;
    mock_motor_set_speed_value = 0;
    mock_motor_set_speed_fwd = true;
    memset(&mock_bemf_cal_info, 0, sizeof(mock_bemf_cal_info));
    memset(&mock_bemf_base_info, 0, sizeof(mock_bemf_base_info));
    mock_bemf_enabled = 1;
    mock_bemf_cal_start_ret = 0;
    mock_bemf_cal_clear_ret = 0;
    mock_bemf_cal_start_calls = 0;
    mock_bemf_cal_clear_calls = 0;

    mock_auxio_init_ret = 0;
    mock_auxio_set_enabled_calls = 0;
    mock_auxio_set_effect_calls = 0;
    mock_auxio_config_calls = 0;
    mock_auxio_last_channel = 0;
    mock_auxio_last_pwm_on = 0;
    mock_auxio_last_pwm_off = 0;
    mock_auxio_last_mode = 0;
    mock_auxio_last_period = 0;
    mock_auxio_last_enabled = -1;

    mock_storage_is_mounted = 1;
    mock_storage_free_ret = 0;
    mock_storage_free = 1234u;
    mock_storage_get_time_advance_us = 0;

    mock_dcc_reload_config_calls = 0;

    /* web.c module state. */
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(s_fn, 0, sizeof(s_fn));
    memset(s_track_cat, 0, sizeof(s_track_cat));
    memset(s_func_map, 0, sizeof(s_func_map));
    memset(s_aux_cfg, 0, sizeof(s_aux_cfg));
    memset(s_func_last_mask, 0, sizeof(s_func_last_mask));
    memset(s_func_tracks, 0, sizeof(s_func_tracks));
    s_track_cat_loaded = false;
    s_motion_initialized = false;
    s_motion_forward = true;
    s_motion_speed = 0;
    s_wifi_started = false;
    s_server = NULL;
    s_progress_server = NULL;
    s_ap_ip[0] = '\0';
    memset(s_ap_ip_bytes, 0, sizeof(s_ap_ip_bytes));
    s_sta_ip[0] = '\0';
    s_ap_sta_count = 0;
    s_ap_started_us = 0;
    s_last_client_us = 0;
    s_up_total = 0;
    s_up_received = 0;
    s_up_active = false;
    s_up_slot = 0;
    s_autooff_iter_cap = 0;
    s_dns_iter_cap = 0;
    s_pipe_iter_cap = 0;
    s_pipe_write_err_inject = false;
    s_evlog_head = 0;
    s_evlog_seq = 0;
    memset(s_evlog, 0, sizeof(s_evlog));
    /* Logger/function mutexes are created by web_init(); give the handlers a
     * usable lock by default (the NULL path is exercised explicitly). */
    s_evlog_mutex = (SemaphoreHandle_t)1;
    s_func_mutex = (SemaphoreHandle_t)1;
}
