#ifndef SOUND_STORE_H
#define SOUND_STORE_H

#include <stddef.h>

#include "esp_err.h"
#include "sound_types.h"

/* Binary scheme storage (SOUND_ENGINE_IMPLEMENTATION.md section 20.6):
 *   [magic "MDS1"][u16 ver][u16 size][u32 crc32][sound_scheme_t]
 * One fread/fwrite of the whole struct; no textual parser. The active scheme
 * is a file under <storage_root>/projects, its name kept in NVS. */

#define SOUND_STORE_EXT ".mds"
/* Raw file layout for import/export validation and size checks. */
#define SOUND_STORE_HDR_LEN   12   /* magic[4] + ver[2] + size[2] + crc[4] */
#define SOUND_STORE_MAX_BYTES (SOUND_STORE_HDR_LEN + sizeof(sound_scheme_t))

/* Directory (under the storage root) that holds the scheme files. */
#define SOUND_PROJECTS_DIR    "projects"

/* Fill `out` with the disabled default scheme (type NONE, start key F1). */
esp_err_t sound_store_default(sound_scheme_t *out);

/* On any header/size/CRC mismatch the loader fills `out` with the default
 * scheme and returns a non-OK error. */
esp_err_t sound_store_load(const char *path, sound_scheme_t *out);
esp_err_t sound_store_save(const char *path, const sound_scheme_t *in);

/* Absolute path of a scheme (<root>/projects/<name>.mds). Fails when the
 * external storage is not mounted. */
esp_err_t sound_store_path(char *out, size_t cap, const char *name);

/* Create <root>/projects if missing. No-op (INVALID_STATE) when unmounted. */
esp_err_t sound_store_ensure_dir(void);

/* Validate raw `.mds` bytes (magic/version/size/CRC). On success copies the
 * scheme into `out`; on any mismatch returns ESP_FAIL. */
esp_err_t sound_store_verify(const uint8_t *buf, size_t len, sound_scheme_t *out);

/* Read a whole `.mds` file into `buf`; requires exactly SOUND_STORE_MAX_BYTES
 * bytes in the file and `cap >= SOUND_STORE_MAX_BYTES`. */
esp_err_t sound_store_read(const char *path, uint8_t *buf, size_t cap, size_t *out_len);

/* True when `name` is a safe scheme name: 1..(SOUND_FILE_MAX-1) characters of
 * [A-Za-z0-9_-] (no path separators, dots, spaces or control bytes). */
bool sound_store_name_ok(const char *name);

#endif
