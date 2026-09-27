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
            if (vl >= out_len) {
                vl = out_len - 1U;
            }
            memcpy(out, eq + 1, vl);
            out[vl] = '\0';
            size_t r = 0, w = 0;
            while (out[r] != '\0') {
                if (out[r] == '%' && isxdigit((unsigned char)out[r + 1]) &&
                    isxdigit((unsigned char)out[r + 2])) {
                    char hex[3] = { out[r + 1], out[r + 2], '\0' };
                    out[w++] = (char)strtoul(hex, NULL, 16);
                    r += 3;
                } else if (out[r] == '+') {
                    out[w++] = ' ';
                    r++;
                } else {
                    out[w++] = out[r++];
                }
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
        i += 1 + q[i];
    }
    if (i >= qlen || q[i] != 0 || i + 5 > qlen) {
        return 0;
    }
    int qend = i + 5;
    /* The echoed question plus the 16-byte answer must fit the response
     * buffer, otherwise a near-512-byte query would write past the end. */
    if (qend + 16 > DNS_MSG_MAX) {
        return 0;
    }
    int o = 12;
    memcpy(r + o, q + 12, (size_t)(qend - 12));
    o += qend - 12;

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
