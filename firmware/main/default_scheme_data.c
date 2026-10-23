#include "default_scheme_data.h"

#include <stdint.h>

/* Symbols created by EMBED_FILES "data/default_scheme.json". */
extern const uint8_t default_scheme_json_start[] asm("_binary_default_scheme_json_start");
extern const uint8_t default_scheme_json_end[] asm("_binary_default_scheme_json_end");

const char *default_scheme_blob(size_t *length)
{
    size_t size = (size_t)(default_scheme_json_end - default_scheme_json_start);
    if (length) { *length = size; }
    return (const char *)default_scheme_json_start;
}
