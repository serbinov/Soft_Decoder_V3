#include "web_util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool parse_query(const char *query, const char *key, char *out, size_t out_len)
{
    if (query == NULL || key == NULL || out == NULL || out_len == 0) {
        return false;
    }
    size_t key_len = strlen(key);
    const char *cur = query;
    while (*cur != '\0') {
        const char *end = strchr(cur, '&');
        size_t pair_len = end ? (size_t)(end - cur) : strlen(cur);
        const char *eq = memchr(cur, '=', pair_len);
        if (eq != NULL && (size_t)(eq - cur) == key_len && strncmp(cur, key, key_len) == 0) {
            size_t vl = pair_len - key_len - 1U;
            const char *src = eq + 1;
            size_t r = 0, w = 0;
            while (r < vl) {
                unsigned char c = (unsigned char)src[r++];
                if (c == '%') {
                    if (vl - r < 2U || !isxdigit((unsigned char)src[r]) ||
                        !isxdigit((unsigned char)src[r + 1])) {
                        out[0] = '\0';
                        return false;
                    }
                    char hex[3] = { src[r], src[r + 1], '\0' };
                    c = (unsigned char)strtoul(hex, NULL, 16);
                    r += 2U;
                } else if (c == '+') {
                    c = ' ';
                }
                if (c == 0U || w + 1U >= out_len) {
                    out[0] = '\0';
                    return false;
                }
                out[w++] = (char)c;
            }
            out[w] = '\0';
            return true;
        }
        if (end == NULL) {
            break;
        }
        cur = end + 1;
    }
    return false;
}

bool parse_u8(const char *query, const char *key, uint8_t *out)
{
    char buf[16] = { 0 };
    if (!parse_query(query, key, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(buf, &end, 10);
    if (end == buf || *end != '\0' || v > 255) {
        return false;
    }
    *out = (uint8_t)v;
    return true;
}

bool parse_u16(const char *query, const char *key, uint16_t *out)
{
    char buf[16] = { 0 };
    if (!parse_query(query, key, buf, sizeof(buf))) {
        return false;
    }
    char *end = NULL;
    unsigned long v = strtoul(buf, &end, 10);
    if (end == buf || *end != '\0' || v > 65535) {
        return false;
    }
    *out = (uint16_t)v;
    return true;
}

bool parse_bool(const char *query, const char *key, bool def)
{
    char buf[16] = { 0 };
    if (!parse_query(query, key, buf, sizeof(buf))) {
        return def;
    }
    return strcmp(buf, "1") == 0 || strcasecmp(buf, "true") == 0 || strcasecmp(buf, "yes") == 0;
}

void json_escape(const char *in, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t w = 0;
    for (size_t r = 0; in != NULL && in[r] != '\0' && w + 2 < out_len; ++r) {
        unsigned char c = (unsigned char)in[r];
        if (c == '"' || c == '\\') {
            out[w++] = '\\';
            out[w++] = (char)c;
        } else if (c == '\n') {
            out[w++] = '\\';
            out[w++] = 'n';
        } else if (c == '\r') {
            out[w++] = '\\';
            out[w++] = 'r';
        } else if (c == '\t') {
            out[w++] = '\\';
            out[w++] = 't';
        } else if (c < 0x20U) {
            /* Remaining C0 controls are not legal in a JSON string; replace
             * them instead of emitting invalid JSON the UI cannot parse. */
            out[w++] = '?';
        } else {
            out[w++] = (char)c;
        }
    }
    out[w] = '\0';
}

/* Append formatted text to a bounded JSON buffer without the classic
 * "sizeof(buf) - used" underflow when the buffer fills up. */
void buf_appendf(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    if (*used >= cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *used += (size_t)n;
        if (*used > cap) {
            *used = cap;
        }
    }
}

size_t dns_build_response(const uint8_t *q, int qlen, uint8_t *r, const uint8_t ip[4])
{
    if (q == NULL || r == NULL || ip == NULL || qlen < 17 || qlen > DNS_MSG_MAX ||
        (q[2] & 0xF8U) != 0U || q[3] != 0U || q[4] != 0U || q[5] != 1U ||
        q[6] != 0U || q[7] != 0U || q[8] != 0U || q[9] != 0U ||
        q[10] != 0U || q[11] != 0U) {
        return 0;
    }
    memcpy(r, q, 12);
    r[2] = 0x81;
    r[3] = 0x80;
    r[6] = 0x00;
    r[7] = 0x01; /* ANCOUNT = 1 */
    r[8] = 0x00;
    r[9] = 0x00; /* NSCOUNT = 0 */
    r[10] = 0x00;
    r[11] = 0x00; /* ARCOUNT = 0 */
    r[12] = 0x00;
    r[13] = 0x00;

    int i = 12;
    while (i < qlen && q[i] != 0) {
        if (q[i] > 63U || i + 1 + q[i] >= qlen || i - 12 > 253) {
            return 0;
        }
        i += 1 + q[i];
    }
    if (i >= qlen || q[i] != 0 || i + 5 > qlen) {
        return 0;
    }
    if (i - 12 > 254) return 0;
    int qend = i + 5;
    if (qend != qlen || q[i + 3] != 0U || q[i + 4] != 1U ||
        q[i + 1] != 0U || (q[i + 2] != 1U && q[i + 2] != 28U)) {
        return 0;
    }
    /* The echoed question plus the 16-byte answer must fit the response
     * buffer, otherwise a near-512-byte query would write past the end. */
    if (qend + 16 > DNS_MSG_MAX) {
        return 0;
    }
    int o = 12;
    memcpy(r + o, q + 12, (size_t)(qend - 12));
    o += qend - 12;
    r[2] = (uint8_t)(0x80U | (q[2] & 1U));
    r[3] = 0U;
    if (q[i + 2] == 28U) {
        r[7] = 0U;
        return (size_t)o;
    }

    r[o++] = 0xC0;
    r[o++] = 0x0C;
    r[o++] = 0x00;
    r[o++] = 0x01;
    r[o++] = 0x00;
    r[o++] = 0x01;
    r[o++] = 0x00;
    r[o++] = 0x00;
    r[o++] = 0x00;
    r[o++] = 0x3C;
    r[o++] = 0x00;
    r[o++] = 0x04;
    r[o++] = ip[0];
    r[o++] = ip[1];
    r[o++] = ip[2];
    r[o++] = ip[3];
    return (size_t)o;
}

void url_decode(const char *input, char *output, size_t out_len)
{
    if (output == NULL || out_len == 0) {
        return;
    }
    output[0] = '\0';
    if (input == NULL) {
        return;
    }
    size_t w = 0;
    for (size_t i = 0; input[i] != '\0' && (w + 1U) < out_len; ++i) {
        char c = input[i];
        if (c == '%' && isxdigit((unsigned char)input[i + 1]) &&
            isxdigit((unsigned char)input[i + 2])) {
            char hex[3] = { input[i + 1], input[i + 2], '\0' };
            output[w++] = (char)strtoul(hex, NULL, 16);
            i += 2;
        } else if (c == '+') {
            output[w++] = ' ';
        } else {
            output[w++] = c;
        }
    }
    output[w] = '\0';
}

void sanitize_name(const char *in, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    size_t w = 0;
    const char *src = (in != NULL) ? in : "";
    for (size_t i = 0; src[i] != '\0' && (w + 1U) < out_len; ++i) {
        char c = src[i];
        if ((isalnum((unsigned char)c) && (unsigned char)c < 0x80U) ||
            (unsigned char)c >= 0x80U || c == '_' || c == '-' || c == '.') {
            out[w++] = c;
        } else {
            out[w++] = '_';
        }
    }
    out[w] = '\0';
}

typedef struct {
    const char *host;
    size_t host_len;
    unsigned port;
    bool has_port;
} web_util_authority_t;

static char web_util_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool web_util_ascii_ieq(const char *a, size_t alen, const char *b, size_t blen)
{
    if (alen != blen) {
        return false;
    }
    for (size_t i = 0; i < alen; ++i) {
        if (web_util_ascii_lower(a[i]) != web_util_ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

static bool web_util_ws(char c)
{
    return c == ' ' || c == '\t';
}

static bool web_util_ascii_alnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool web_util_parse_port(const char *s, size_t len, unsigned *port)
{
    if (s == NULL || port == NULL || len == 0U || len > 5U) {
        return false;
    }
    unsigned value = 0U;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        value = value * 10U + (unsigned)(s[i] - '0');
    }
    if (value > 65535U) {
        return false;
    }
    *port = value;
    return true;
}

/* Parse `host[:port]` or `[ipv6][:port]` without a scheme or path. */
static bool web_util_parse_authority(const char *s, size_t len, web_util_authority_t *out)
{
    if (s == NULL || out == NULL || len == 0U) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x20U || c >= 0x7fU || c == '@' || c == '/' || c == '?' || c == '#') {
            return false;
        }
    }
    if (s[0] == '[') {
        size_t close = 1U;
        while (close < len && s[close] != ']') {
            ++close;
        }
        if (close == len || close == 1U) {
            return false;
        }
        for (size_t i = 1U; i < close; ++i) {
            char c = s[i];
            if (!(web_util_ascii_alnum(c) || c == ':' || c == '.' || c == '-' || c == '%')) {
                return false;
            }
        }
        out->host = s;
        out->host_len = close + 1U;
        size_t rest = close + 1U;
        if (rest == len) {
            return true;
        }
        if (s[rest] != ':' || !web_util_parse_port(s + rest + 1U, len - rest - 1U, &out->port)) {
            return false;
        }
        out->has_port = true;
        return true;
    }
    size_t colon = len;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == ':') {
            colon = i;
            break;
        }
    }
    if (colon == 0U) {
        return false;
    }
    for (size_t i = 0; i < colon; ++i) {
        char c = s[i];
        if (!(web_util_ascii_alnum(c) || c == '-' || c == '.' || c == '_')) {
            return false;
        }
    }
    out->host = s;
    out->host_len = colon;
    if (colon == len) {
        return true;
    }
    if (!web_util_parse_port(s + colon + 1U, len - colon - 1U, &out->port)) {
        return false;
    }
    out->has_port = true;
    return true;
}

/* Extract the authority from `scheme://authority[/path][?query]` and report
 * the scheme's default port. Only http/https are accepted. */
static bool web_util_parse_origin_url(const char *value, web_util_authority_t *out,
                                      unsigned *default_port)
{
    const char *p = value;
    while (web_util_ws(*p)) {
        ++p;
    }
    const char *end = p + strlen(p);
    while (end > p && web_util_ws(end[-1])) {
        --end;
    }
    size_t len = (size_t)(end - p);
    if (len == 0U) {
        return false;
    }
    size_t sep = 0U;
    while (sep + 2U < len && !(p[sep] == ':' && p[sep + 1U] == '/' && p[sep + 2U] == '/')) {
        ++sep;
    }
    if (sep + 2U >= len) {
        return false;
    }
    if (sep == 4U && web_util_ascii_ieq(p, 4U, "http", 4U)) {
        *default_port = 80U;
    } else if (sep == 5U && web_util_ascii_ieq(p, 5U, "https", 5U)) {
        *default_port = 443U;
    } else {
        return false;
    }
    const char *authority = p + sep + 3U;
    size_t authority_len = len - sep - 3U;
    size_t used = 0U;
    while (used < authority_len && authority[used] != '/' && authority[used] != '?' &&
           authority[used] != '#') {
        ++used;
    }
    return web_util_parse_authority(authority, used, out);
}

static bool web_util_parse_host_header(const char *value, web_util_authority_t *out)
{
    const char *p = value;
    while (web_util_ws(*p)) {
        ++p;
    }
    const char *end = p + strlen(p);
    while (end > p && web_util_ws(end[-1])) {
        --end;
    }
    return web_util_parse_authority(p, (size_t)(end - p), out);
}

bool web_util_same_origin(const char *origin, const char *referer, const char *host)
{
    const char *source = NULL;
    if (origin != NULL) {
        const char *p = origin;
        while (web_util_ws(*p)) {
            ++p;
        }
        if (*p != '\0') {
            source = origin;
        }
    }
    if (source == NULL && referer != NULL) {
        const char *p = referer;
        while (web_util_ws(*p)) {
            ++p;
        }
        if (*p != '\0') {
            source = referer;
        }
    }
    if (source == NULL) {
        return true; /* Non-browser client without Origin/Referer. */
    }
    {
        const char *p = source;
        while (web_util_ws(*p)) {
            ++p;
        }
        if (web_util_ascii_ieq(p, strlen(p), "null", 4U)) {
            return false;
        }
    }
    if (host == NULL) {
        return false;
    }
    web_util_authority_t source_authority;
    web_util_authority_t host_authority;
    unsigned default_port = 0U;
    if (!web_util_parse_origin_url(source, &source_authority, &default_port) ||
        !web_util_parse_host_header(host, &host_authority)) {
        return false;
    }
    unsigned source_port = source_authority.has_port ? source_authority.port : default_port;
    unsigned host_port = host_authority.has_port ? host_authority.port : default_port;
    return source_port == host_port &&
           web_util_ascii_ieq(source_authority.host, source_authority.host_len,
                              host_authority.host, host_authority.host_len);
}

/* Direction / speed gates: a function only drives its output while its
 * configured motion condition holds. */
static bool func_cond_ok(const settings_func_map_t *m, bool motion_forward,
                         uint8_t motion_speed)
{
    if (m->dir == SETTINGS_FUNC_DIR_FWD && !motion_forward) {
        return false;
    }
    if (m->dir == SETTINGS_FUNC_DIR_REV && motion_forward) {
        return false;
    }
    if (m->speed == SETTINGS_FUNC_SPD_MOVING && motion_speed == 0U) {
        return false;
    }
    if (m->speed == SETTINGS_FUNC_SPD_STOP && motion_speed != 0U) {
        return false;
    }
    return true;
}

uint16_t web_util_func_desired(const settings_func_map_t *m, bool fn_on,
                               bool motion_forward, uint8_t motion_speed)
{
    if (m == NULL) {
        return 0U;
    }
    bool on = fn_on && func_cond_ok(m, motion_forward, motion_speed);
    uint16_t mask = m->aux_mask;

    /* F0F and F0R together mean the head light, i.e. directional. The other
     * outputs in the same mask (AUX1..AUX7) must stay independent of the
     * driving direction instead of being dropped. */
    const uint16_t light = SETTINGS_FUNC_OUT_F0F | SETTINGS_FUNC_OUT_F0R;
    if ((mask & light) == light) {
        if (!on) {
            return 0U;
        }
        uint16_t dir_bit = motion_forward ? SETTINGS_FUNC_OUT_F0F
                                          : SETTINGS_FUNC_OUT_F0R;
        return (uint16_t)((mask & (uint16_t)~light) | dir_bit);
    }
    return on ? mask : 0U;
}

static bool func_cmp(int32_t lhs, uint8_t op, int32_t rhs)
{
    switch (op) {
        case 0: return lhs == rhs;
        case 1: return lhs != rhs;
        case 2: return lhs < rhs;
        case 3: return lhs <= rhs;
        case 4: return lhs > rhs;
        case 5: return lhs >= rhs;
        default: return false;
    }
}

static bool func_cond_value(func_var_get_t vars, void *ctx, uint8_t var, int32_t *out)
{
    if (vars == NULL || out == NULL || var >= 16U) {
        return false;
    }
    return vars(ctx, var, out);
}

static bool func_binding_cond_ok(const func_binding_t *b, func_var_get_t vars, void *ctx)
{
    if ((b->flags & FUNC_FLAG_COND) == 0U) {
        return true;
    }
    int32_t lhs = 0;
    if (!func_cond_value(vars, ctx, (uint8_t)(b->short_table & 0x0FU), &lhs)) {
        return false;
    }
    bool ok = func_cmp(lhs, (uint8_t)(b->short_ms & 0x07U), (int32_t)(int16_t)b->min_ms);
    if ((b->flags & FUNC_FLAG_COND2) != 0U) {
        int32_t lhs2 = 0;
        if (!func_cond_value(vars, ctx, (uint8_t)((b->short_ms >> 12) & 0x0FU), &lhs2)) {
            return false;
        }
        bool ok2 = func_cmp(lhs2, (uint8_t)((b->short_ms >> 3) & 0x07U), (int32_t)(int16_t)b->fade_ms);
        ok = (b->flags & FUNC_FLAG_COND_OR) != 0U ? (ok || ok2) : (ok && ok2);
    }
    return (b->flags & FUNC_FLAG_INVERT) != 0U ? !ok : ok;
}

uint16_t func_eval_ex(const func_binding_t *bind, size_t count, uint8_t fn, uint8_t state,
                      uint8_t dir, func_eval_cb_t cb, void *ctx,
                      func_var_get_t vars, void *var_ctx)
{
    if (bind == NULL) {
        return 0U;
    }
    uint16_t mask = 0U;
    for (size_t i = 0; i < count; ++i) {
        const func_binding_t *b = &bind[i];
        if (b->used == 0U || b->fn != fn) {
            continue;
        }
        if (b->dir != FUNC_DIR_ANY && b->dir != dir) {
            continue;
        }
        if (b->state != FUNC_STATE_ANY && b->state != state) {
            continue;
        }
        if (!func_binding_cond_ok(b, vars, var_ctx)) {
            continue;
        }
        switch (b->target_type) {
            case FUNC_TARGET_OUTPUT:
                if (b->target_id < 9U) {
                    mask |= (uint16_t)(1U << b->target_id);
                }
                break;
            case FUNC_TARGET_SOUND:
            case FUNC_TARGET_SLOT:
            case FUNC_TARGET_LOGIC:
                if (cb != NULL) {
                    cb(ctx, b->target_type, b->target_id, true);
                }
                break;
            default:
                break;
        }
    }
    return mask;
}

uint16_t func_eval(const func_binding_t *bind, size_t count, uint8_t fn, uint8_t state,
                   uint8_t dir, func_eval_cb_t cb, void *ctx)
{
    return func_eval_ex(bind, count, fn, state, dir, cb, ctx, NULL, NULL);
}

uint8_t web_util_voice_volume(uint8_t fn, const uint8_t *cats, bool cats_loaded,
                              uint8_t engine_vol, uint8_t effects_vol)
{
    uint8_t cat;
    if (cats_loaded && cats != NULL && fn >= 1U && fn <= SETTINGS_MAX_TRACKS) {
        cat = cats[fn - 1U];
    } else {
        /* Before the persisted categories are loaded, use the default layout
         * (slot 1 = engine) instead of a zero-initialised array. */
        cat = SETTINGS_TRACK_CAT_DEFAULT_SLOT(fn);
    }
    return cat == SETTINGS_TRACK_CAT_ENGINE ? engine_vol : effects_vol;
}

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool ota_container_parse(const uint8_t *buf, size_t len, ota_container_hdr_t *out)
{
    if (buf == NULL || out == NULL || len < OTA_CONTAINER_HDR_LEN) {
        return false;
    }
    if (memcmp(buf, OTA_CONTAINER_MAGIC, OTA_CONTAINER_MAGIC_LEN) != 0) {
        return false;
    }
    out->fw_len = rd_u32le(buf + 8);
    out->n_files = rd_u32le(buf + 12);
    /* Sanity: non-empty firmware, a sane number of files. */
    return out->fw_len > 0U && out->n_files <= 64U;
}

bool ota_file_hdr_parse(const uint8_t *buf, size_t len, ota_file_hdr_t *out)
{
    if (buf == NULL || out == NULL || len < OTA_FILE_HDR_LEN) {
        return false;
    }
    out->name_len = rd_u16le(buf);
    out->label_len = rd_u16le(buf + 2);
    out->data_len = rd_u32le(buf + 4);
    return out->name_len > 0U && out->name_len <= OTA_FILE_NAME_MAX &&
           out->label_len <= OTA_FILE_LABEL_MAX && out->data_len > 0U;
}
