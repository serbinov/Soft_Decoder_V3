/* Shared HTTP response, query and root-page helpers. No component state. */

#include "web_internal.h"
#include "web_util.h"
#include "web_html.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "lwip/sockets.h"

esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

bool request_accepts_gzip(httpd_req_t *req)
{
    char enc[64];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) != ESP_OK) {
        return false;
    }
    return strstr(enc, "gzip") != NULL;
}

/* Root page: gzip for browsers, ETag lets a repeat open use a tiny 304
 * instead of downloading the 150 KB page again. */
esp_err_t send_root_html(httpd_req_t *req)
{
    const bool gz = request_accepts_gzip(req);
    httpd_resp_set_hdr(req, "ETag", WEB_HTML_ETAG);
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");

    char inm[96];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK &&
        (strcmp(inm, "*") == 0 || strstr(inm, WEB_HTML_HASH) != NULL)) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)s_page_html_gz, sizeof(s_page_html_gz));
    }
    return httpd_resp_send(req, s_page_html, HTTPD_RESP_USE_STRLEN);
}

bool query_encoding_valid(const char *query)
{
    for (size_t i = 0; query[i] != '\0'; ++i) {
        if (query[i] == '%') {
            if (query[i + 1] == '\0' || query[i + 2] == '\0' ||
                !isxdigit((unsigned char)query[i + 1]) || !isxdigit((unsigned char)query[i + 2]) ||
                (query[i + 1] == '0' && query[i + 2] == '0')) return false;
            i += 2;
        }
    }
    return true;
}

bool query_has_key(const char *query, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = query; p != NULL && *p != '\0'; p = strchr(p, '&')) {
        if (*p == '&') ++p;
        if (strncmp(p, key, n) == 0 && p[n] == '=') return true;
    }
    return false;
}

bool query_bool_valid(const char *query, const char *key)
{
    if (!query_has_key(query, key)) return true;
    char value[16];
    if (!parse_query(query, key, value, sizeof(value))) return false;
    return strcmp(value, "1") == 0 || strcmp(value, "0") == 0 ||
           strcasecmp(value, "true") == 0 || strcasecmp(value, "false") == 0 ||
           strcasecmp(value, "yes") == 0 || strcasecmp(value, "no") == 0;
}

esp_err_t close_rejected_body(httpd_req_t *req, esp_err_t response)
{
    /* IDF otherwise purges an unread body synchronously after the handler. */
    if (req->content_len != 0U) {
        httpd_resp_set_hdr(req, "Connection", "close");
        int fd = httpd_req_to_sockfd(req);
        if (fd >= 0) (void)shutdown(fd, SHUT_RDWR);
    }
    return response;
}
