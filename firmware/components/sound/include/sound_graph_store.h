#ifndef SOUND_GRAPH_STORE_H
#define SOUND_GRAPH_STORE_H

#include "sound_graph.h"

#define SG_STORE_MAX_PROJECTS 64U
#define SG_STORE_MAX_ENTRIES 1024U

typedef struct {
    char id[SG_ID_CAP], name[SG_NAME_CAP];
    uint32_t revision;
} sg_store_descriptor_t;

/* No runtime side effects. Returned JSON is NUL-terminated; caller frees it.
 * Save conflicts return ESP_ERR_INVALID_STATE; expected_revision=0 creates.
 * All functions acquire storage leases and serialize store access. */
esp_err_t sg_store_save(const char *id, uint32_t expected_revision,
                        const char *json, size_t len, uint32_t *new_revision,
                        sg_diagnostic_t *diag);
esp_err_t sg_store_read(const char *id, uint32_t revision, char **out,
                        size_t *len, uint32_t *actual_revision);
esp_err_t sg_store_list(sg_store_descriptor_t *out, size_t capacity, size_t *count);
/* Select an already committed revision. Caller validates/prepares runtime first. */
esp_err_t sg_store_select(const char *id, uint32_t revision);
/* Missing record returns OK with graph_selected=false for legacy boot fallback.
 * Corrupt/missing selected graph returns failure, never a legacy fallback. */
esp_err_t sg_store_selection(char *id, size_t capacity, uint32_t *revision,
                             bool *graph_selected);
esp_err_t sg_store_select_legacy(void);
esp_err_t sg_assets_validate(const sg_graph_t *graph, sg_diagnostic_t *diag);
esp_err_t sg_asset_inspect(const char *file, sg_asset_t *out, sg_diagnostic_t *diag);
/* Failure means caller must refuse destructive asset operations. */
esp_err_t sg_store_file_referenced(const char *file, bool *referenced);

#endif
