#ifndef MOCK_ESP_HTTP_SERVER_H
#define MOCK_ESP_HTTP_SERVER_H

/* Host shim for the ESP-IDF HTTP server used by components/web/src/web.c.
 * Requests are served from a scripted body/query buffer and responses are
 * captured so the white-box tests can assert on status/headers/JSON. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void *httpd_handle_t;

typedef enum {
    HTTP_GET = 0,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
} httpd_method_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    void *user_ctx;
    httpd_method_t method;
    const char *uri;
    size_t content_len;
} httpd_req_t;

typedef esp_err_t (*httpd_uri_handler_t)(httpd_req_t *req);
typedef bool (*httpd_uri_match_fn_t)(const char *ref_uri, const char *in_uri, size_t len);

typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_uri_handler_t handler;
    void *user_ctx;
} httpd_uri_t;

typedef enum {
    HTTPD_404_NOT_FOUND = 104,
    HTTPD_400_BAD_REQUEST = 400,
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
    HTTPD_503_SERVICE_UNAVAILABLE = 503,
} httpd_err_code_t;

typedef esp_err_t (*httpd_err_handler_func_t)(httpd_req_t *req, httpd_err_code_t err);

typedef struct {
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t stack_size;
    uint16_t max_uri_handlers;
    uint16_t max_open_sockets;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    bool lru_purge_enable;
    httpd_uri_match_fn_t uri_match_fn;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG()                          \
    {                                                   \
        .server_port = 80,                              \
        .ctrl_port = 32768,                             \
        .stack_size = 4096,                             \
        .max_uri_handlers = 8,                          \
        .max_open_sockets = 7,                          \
        .recv_wait_timeout = 5,                         \
        .send_wait_timeout = 5,                         \
        .lru_purge_enable = false,                      \
        .uri_match_fn = NULL,                           \
    }

#define HTTPD_RESP_USE_STRLEN ((size_t)-1)
#define HTTPD_SOCK_ERR_TIMEOUT (-3)

bool httpd_uri_match_wildcard(const char *ref_uri, const char *in_uri, size_t len);

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config);
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri);
esp_err_t httpd_register_err_handler(httpd_handle_t handle, httpd_err_code_t error,
                                     httpd_err_handler_func_t handler);

esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *field, const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *req, const char *status);
esp_err_t httpd_resp_send(httpd_req_t *req, const char *buf, size_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *req, httpd_err_code_t error, const char *msg);

esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *buf, size_t buf_len);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *val,
                                      size_t val_size);
int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len);
size_t httpd_req_get_url_query_len(httpd_req_t *req);
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field);
esp_err_t httpd_req_async_handler_begin(httpd_req_t *req, httpd_req_t **out);
esp_err_t httpd_req_async_handler_complete(httpd_req_t *req);
int httpd_req_to_sockfd(httpd_req_t *req);
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int fd);

#endif /* MOCK_ESP_HTTP_SERVER_H */
