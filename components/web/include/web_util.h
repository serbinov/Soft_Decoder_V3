#ifndef WEB_UTIL_H
#define WEB_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "settings.h"

/* Pure request-parsing / JSON / DNS / function-mapping helpers used by the web
 * component. They carry no hardware or FreeRTOS dependency so they can be
 * unit-tested on the host. */

#define DNS_MSG_MAX 512

/* Extract `key` from an "a=1&b=2" query string, percent- and plus-decoding the
 * value into `out`. Returns false when the key is absent. */
bool parse_query(const char *query, const char *key, char *out, size_t out_len);

/* Typed query parsers: reject empty, trailing garbage and out-of-range values. */
bool parse_u8(const char *query, const char *key, uint8_t *out);
bool parse_u16(const char *query, const char *key, uint16_t *out);
bool parse_bool(const char *query, const char *key, bool def);

/* Escape `"`, `\` and newline for embedding in a JSON string. */
void json_escape(const char *in, char *out, size_t out_len);

/* Bounded append: never overflows `buf` and never underflows on a full buffer. */
void buf_appendf(char *buf, size_t cap, size_t *used, const char *fmt, ...);

/* Build a DNS A response for a query, pointing at `ip` (4 bytes, network
 * order). Returns 0 when malformed or when the answer would not fit the
 * DNS_MSG_MAX buffer. */
size_t dns_build_response(const uint8_t *q, int qlen, uint8_t *r, const uint8_t ip[4]);

/* Percent-decode into `output`. */
void url_decode(const char *input, char *output, size_t out_len);

/* Keep [A-Za-z0-9_.-] and UTF-8 bytes (>= 0x80); replace the rest with '_'. */
void sanitize_name(const char *in, char *out, size_t out_len);

/* Output mask (SETTINGS_FUNC_OUT_*) a function must drive given its mapping,
 * its on/off state and the current motion. F0F+F0R together mean the
 * directional head light. */
uint16_t web_util_func_desired(const settings_func_map_t *m, bool fn_on,
                               bool motion_forward, uint8_t motion_speed);

/* Per-slot volume: slot 1 defaults to the engine volume, the rest to the
 * effects volume; `cats` may override the category when `cats_loaded`. */
uint8_t web_util_voice_volume(uint8_t fn, const uint8_t *cats, bool cats_loaded,
                              uint8_t engine_vol, uint8_t effects_vol);

/* --- Combined OTA container (firmware + sound files in one upload) ---
 * Little-endian layout:
 *   [0..7]   magic "AURAOTA2"
 *   [8..11]  firmware length in bytes
 *   [12..15] number of sound files
 *   firmware payload (fw_len bytes)
 *   repeated per file:
 *     [0..1] name length  [2..3] label length  [4..7] data length
 *     name bytes, label bytes, data bytes
 * The firmware part is written to the OTA partition, the files to /userdata. */
#define OTA_CONTAINER_MAGIC     "AURAOTA2"
#define OTA_CONTAINER_MAGIC_LEN 8
#define OTA_CONTAINER_HDR_LEN   16
#define OTA_FILE_HDR_LEN        8
#define OTA_FILE_NAME_MAX       63
#define OTA_FILE_LABEL_MAX      63

typedef struct {
    uint32_t fw_len;
    uint32_t n_files;
} ota_container_hdr_t;

typedef struct {
    uint16_t name_len;
    uint16_t label_len;
    uint32_t data_len;
} ota_file_hdr_t;

bool ota_container_parse(const uint8_t *buf, size_t len, ota_container_hdr_t *out);
bool ota_file_hdr_parse(const uint8_t *buf, size_t len, ota_file_hdr_t *out);

#endif /* WEB_UTIL_H */
