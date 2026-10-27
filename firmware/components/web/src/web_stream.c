/* Buffered HTTP request-body reader shared by pack and OTA uploads. */

#include "web.h"
#include "web_internal.h"

#include <stdint.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"

int ota_stream_read(ota_stream_t *s, uint8_t *dst, size_t n)
{
    size_t got = 0;
    while (got < n) {
        if (web_transfer_aborted()) return -1;
        int64_t deadline = web_transfer_deadline_get();
        if (deadline != 0 && esp_timer_get_time() >= deadline) return -1;
        if (s->pos < s->len) {
            size_t avail = s->len - s->pos;
            size_t take = (avail < n - got) ? avail : n - got;
            memcpy(dst + got, s->buf + s->pos, take);
            s->pos += take;
            got += take;
            continue;
        }
        if (s->remaining <= 0) {
            break;
        }
        int want = s->remaining > (int)sizeof(s->buf) ? (int)sizeof(s->buf) : s->remaining;
        int r = httpd_req_recv(s->req, (char *)s->buf, want);
        if (r > 0) {
            s->len = (size_t)r;
            s->pos = 0;
            s->remaining -= r;
            s->idle = 0;
            web_transfer_wdt_feed((size_t)r);
            continue;
        }
        if ((r == 0 || r == HTTPD_SOCK_ERR_TIMEOUT) && ++s->idle < 4) {
            continue;
        }
        return -1;
    }
    return (int)got;
}

