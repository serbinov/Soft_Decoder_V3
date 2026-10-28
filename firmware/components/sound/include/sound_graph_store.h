#ifndef SOUND_GRAPH_STORE_H
#define SOUND_GRAPH_STORE_H

#include "sound_graph.h"

#define SG_STORE_MAX_PROJECTS 64U
#define SG_STORE_MAX_ENTRIES 1024U
/* Newest revisions kept per project. Older files are pruned after a save;
 * the selected revision is always retained so boot/apply stays valid. */
#define SG_STORE_KEEP_REVISIONS 8U
/* Project id of the built-in "ChME3 Locomotive" scheme. */
#define SG_DEFAULT_PROJECT_ID "locomotive_default"

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
/* Save a structurally valid but semantically invalid draft (editor errors) so
 * no work is lost. Drafts are never selected or applied; apply/boot still run
 * sg_validate and can only load a valid revision. */
esp_err_t sg_store_save_draft(const char *id, uint32_t expected_revision,
                              const char *json, size_t len, uint32_t *new_revision,
                              sg_diagnostic_t *diag);
/* Save a graph revision unconditionally (creating or advancing past any
 * existing latest revision), used when unpacking a bundled .asp sound pack. */
esp_err_t sg_store_save_force(const char *id, const char *json, size_t len,
                              uint32_t *new_revision, sg_diagnostic_t *diag);
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
/* Install the built-in scheme when no graph is selected (factory state).
 * `json` is a sound-graph document whose `"assets":[]` manifest is empty: the
 * real manifest is built from the WAV files installed on the device, so the
 * copy matches the sound pack (ADPCM/PCM, any resampling).
 * A selected graph is never replaced: that returns ESP_ERR_INVALID_STATE.
 * If `id` is already stored (e.g. the selection record was lost) its latest
 * saved revision is selected instead, so user edits win over the built-in copy.
 * `seeded` reports whether a new revision was written. */
esp_err_t sg_store_seed_default(const char *id, const char *json, size_t len,
                                uint32_t *revision, bool *seeded);
/* Delete every revision of a project. If it was selected, selection is
 * switched to legacy first. was_selected may be NULL. */
esp_err_t sg_store_delete(const char *id, bool *was_selected);
esp_err_t sg_assets_validate(const sg_graph_t *graph, sg_diagnostic_t *diag);
esp_err_t sg_asset_inspect(const char *file, sg_asset_t *out, sg_diagnostic_t *diag);
/* Failure means caller must refuse destructive asset operations. Only the
 * latest revision of each project and the selected revision are inspected;
 * historical revisions are not allowed to block asset replacement. */
esp_err_t sg_store_file_referenced(const char *file, bool *referenced);
/* Same as sg_store_file_referenced(), but ignores the project `except_id`.
 * Used when an import replaces that project together with its assets. */
esp_err_t sg_store_file_referenced_except(const char *except_id, const char *file, bool *referenced);

#endif
