/* Control-event journal: ring buffer, /api/log and /api/clientlog.
 * Owns the journal storage; other modules only call web_log_event(). */

#include "web.h"
#include "web_internal.h"
#include "web_util.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WEB_EVLOG_MAX      40
#define WEB_EVLOG_TAG_MAX  24
#define WEB_EVLOG_TEXT_MAX 96

static const char *TAG_LOG = "web";

typedef struct {
    uint32_t seq;
    char tag[WEB_EVLOG_TAG_MAX];
    char text[WEB_EVLOG_TEXT_MAX];
} web_evlog_t;

static web_evlog_t s_evlog[WEB_EVLOG_MAX];
static uint32_t s_evlog_head;
static uint32_t s_evlog_seq;
static SemaphoreHandle_t s_evlog_mutex;

static size_t utf8_safe_len(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        size_t clen = 1;
        if ((c & 0xE0U) == 0xC0U) {
            clen = 2;
        } else if ((c & 0xF0U) == 0xE0U) {
            clen = 3;
        } else if ((c & 0xF8U) == 0xF0U) {
            clen = 4;
        }
        if (i + clen > n) {
            break;
        }
        for (size_t j = 1; j < clen; ++j) {
            if (s[i + j] == '\0') return i;
        }
        i += clen;
    }
    return i;
}

esp_err_t web_log_init(void)
{
    if (s_evlog_mutex == NULL) s_evlog_mutex = xSemaphoreCreateMutex();
    return s_evlog_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void web_log_event(const char *tag, const char *fmt, ...)
{
    if (s_evlog_mutex == NULL) {
        return;
    }
    web_evlog_t ev;
    memset(ev.tag, 0, sizeof(ev.tag));
    memset(ev.text, 0, sizeof(ev.text));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ev.text, sizeof(ev.text), fmt, ap);
    va_end(ap);
    ev.text[utf8_safe_len(ev.text, strlen(ev.text))] = '\0';
    if (tag != NULL) {
        size_t tn = utf8_safe_len(tag, sizeof(ev.tag) - 1U);
        memcpy(ev.tag, tag, tn);
        ev.tag[tn] = '\0';
    }
    (void)xSemaphoreTake(s_evlog_mutex, portMAX_DELAY);
    s_evlog_seq++;
    ev.seq = s_evlog_seq;
    s_evlog[s_evlog_head] = ev;
    s_evlog_head = (s_evlog_head + 1U) % WEB_EVLOG_MAX;
    xSemaphoreGive(s_evlog_mutex);
}

/* GET /api/log?since=N — control events newer than sequence N. */
esp_err_t log_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char buf[16] = { 0 };
    uint32_t since = 0;
    if (parse_query(query, "since", buf, sizeof(buf)) && buf[0]) {
        since = (uint32_t)strtoul(buf, NULL, 10);
    }

    char json[4096];
    size_t used = 0;
    buf_appendf(json, sizeof(json), &used, "{\"ok\":true,\"entries\":[");
    if (s_evlog_mutex != NULL) {
        (void)xSemaphoreTake(s_evlog_mutex, portMAX_DELAY);
    }
    int emitted = 0;
    uint32_t transmitted = since;
    for (uint32_t i = 0; i < WEB_EVLOG_MAX; ++i) {
        const web_evlog_t *e = &s_evlog[(s_evlog_head + i) % WEB_EVLOG_MAX];
        if (e->seq == 0U || e->seq <= since) {
            continue;
        }
        /* Stop before starting an entry that might not fully fit: a partially
         * appended entry would leave the JSON document malformed. 400 bytes
         * covers the largest escaped tag+text entry. */
        if (used >= sizeof(json) - 400U) {
            break;
        }
        char et[WEB_EVLOG_TAG_MAX * 2];
        char ex[WEB_EVLOG_TEXT_MAX * 2];
        json_escape(e->tag, et, sizeof(et));
        json_escape(e->text, ex, sizeof(ex));
        buf_appendf(json, sizeof(json), &used,
                    "%s{\"s\":%lu,\"tag\":\"%s\",\"text\":\"%s\"}",
                    emitted++ ? "," : "", (unsigned long)e->seq, et, ex);
        transmitted = e->seq;
    }
    if (s_evlog_mutex != NULL) {
        xSemaphoreGive(s_evlog_mutex);
    }
    buf_appendf(json, sizeof(json), &used, "],\"seq\":%lu}", (unsigned long)transmitted);
    return send_json(req, json);
}

/* GET /api/clientlog?m=... — browser JS errors forwarded to the serial log. */
esp_err_t clientlog_get(httpd_req_t *req)
{
    char query[WEB_QUERY_MAX] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char msg[224] = { 0 };
    if (parse_query(query, "m", msg, sizeof(msg)) && msg[0]) {
        ESP_LOGE(TAG_LOG, "CLIENT ERROR: %s", msg);
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}
