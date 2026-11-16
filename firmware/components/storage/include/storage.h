#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    STORAGE_BACKEND_NONE = 0,
    STORAGE_BACKEND_EXTERNAL_NOR = 1,
} storage_backend_t;

/* Called once per erased 64 KiB block of the external NOR. */
typedef void (*storage_progress_cb_t)(uint32_t done, uint32_t total);

esp_err_t storage_init(void);
esp_err_t storage_mount(void);
esp_err_t storage_format(void);
/* Erase the entire physical external NOR (all blocks, not just the LittleFS
 * partition) and leave it unmounted. Owner must hold maintenance and no
 * leases. progress may be NULL. Destructive: provisioning-only. */
esp_err_t storage_erase_chip(storage_progress_cb_t progress);
/* Nonblocking admission leases. Hold one for the entire lifetime of every open
 * FILE or filesystem operation; never hold a mutex while waiting for audio. */
esp_err_t storage_access_begin(void);
void storage_access_end(void);
/* Closes admission immediately. Existing leases must drain before format.
 * Maintenance is task-owned; caller bounds its quiescence wait and aborts on
 * timeout. Only the owner may format/end maintenance. */
esp_err_t storage_maintenance_begin(void);
void storage_maintenance_end(void);
bool storage_is_quiescent(void);
storage_backend_t storage_get_backend(void);
bool storage_is_mounted(void);

esp_err_t storage_get_free_bytes(uint64_t *out_free_bytes);
const char *storage_get_root(void);

#endif
