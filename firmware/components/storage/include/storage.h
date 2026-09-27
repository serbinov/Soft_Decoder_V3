#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    STORAGE_BACKEND_NONE = 0,
    STORAGE_BACKEND_EXTERNAL_NOR = 1,
} storage_backend_t;

esp_err_t storage_init(void);
esp_err_t storage_mount(void);
esp_err_t storage_format(void);
storage_backend_t storage_get_backend(void);
bool storage_is_mounted(void);
bool storage_ext_available(void);
esp_err_t storage_get_free_bytes(uint64_t *out_free_bytes);
const char *storage_get_root(void);

#endif
