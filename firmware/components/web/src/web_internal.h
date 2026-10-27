#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "audio_pack.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "settings.h"

#define AP_IP_DEFAULT "192.168.100.1"
#define WEB_QUERY_MAX 1400
#define WEB_TRACKS_JSON_MAX 8192
#define WEB_FN_COUNT 29
#ifndef WEB_USERDATA_DIR
#define WEB_USERDATA_DIR "/userdata"
#endif
#ifndef WEB_AUDIO_DIR
#define WEB_AUDIO_DIR WEB_USERDATA_DIR "/audio"
#endif
#define UPLOAD_MAX (2 * 1024 * 1024)
#define WEB_PATH_MAX 640
#define WEB_LIB_NAME_MAX (AUDIO_PACK_NAME_MAX + 1)
#define WEB_LIB_JSON_MAX (96U * 1024U)
#define AUDIO_VOL_FILE WEB_AUDIO_DIR "/volumes.txt"
#define AUDIO_PACK_MAX_BYTES (15U * 1024U * 1024U)
#define PIPE_BUF_SIZE 8192
#ifndef WEB_UP_PROGRESS_STEP
#define WEB_UP_PROGRESS_STEP 262144
#endif

#ifndef WEB_FWRITE
#define WEB_FWRITE(p, sz, n, f) fwrite((p), (sz), (n), (f))
#endif
#ifndef WEB_FFLUSH
#define WEB_FFLUSH(f) fflush(f)
#endif
#ifndef WEB_FCLOSE
#define WEB_FCLOSE(f) fclose(f)
#endif
#ifdef _WIN32
#define WEB_MKDIR(p) _mkdir(p)
#define WEB_RMDIR(p) _rmdir(p)
#else
#define WEB_MKDIR(p) mkdir((p), 0755)
#define WEB_RMDIR(p) rmdir(p)
#endif

typedef struct {
    char name[WEB_LIB_NAME_MAX];
    uint8_t vol;
} web_vol_t;

typedef enum {
    WEB_ADMIT_PLAIN = 0,
    WEB_ADMIT_ACTUATION,
    WEB_ADMIT_STORAGE,
    WEB_ADMIT_MAINTENANCE,
    WEB_ADMIT_TRANSFER,
    WEB_ADMIT_JSON,
    WEB_ADMIT_BEMF_CAL,
    WEB_ADMIT_BEMF_USE,
} web_admit_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
    web_admit_t admit;
} web_route_t;

#define WEB_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

void web_server_register_routes(const web_route_t *routes, size_t count);

/* Shared HTTP response and query helpers (web_http.c). */
esp_err_t send_json(httpd_req_t *req, const char *json);
bool request_accepts_gzip(httpd_req_t *req);
esp_err_t send_root_html(httpd_req_t *req);
bool query_encoding_valid(const char *query);
bool query_has_key(const char *query, const char *key);
bool query_bool_valid(const char *query, const char *key);
esp_err_t close_rejected_body(httpd_req_t *req, esp_err_t response);

/* Event journal and its endpoints (web_log.c). */
esp_err_t web_log_init(void);
esp_err_t log_get(httpd_req_t *req);
esp_err_t clientlog_get(httpd_req_t *req);

/* Control, readiness, maintenance and safety epoch (web_control.c). */
esp_err_t web_control_init(void);
bool web_control_take(TickType_t wait);
void web_control_give(void);
void web_control_state_enter(void);
void web_control_state_exit(void);
void web_control_bump_generation(void);
void web_control_force_not_ready(void);
bool web_control_admit_mutation(bool *ready_out);
void web_control_finish_mutation(void);
void web_control_set_maintenance_owner(TaskHandle_t owner);
uint32_t web_control_safety_epoch_get(void);
void web_control_safety_epoch_bump(void);
bool web_control_motor_stopped(void);
esp_err_t web_control_maintenance_reserve(bool stopped);
esp_err_t web_control_maintenance_quiesce(bool exclusive_fs);

/* Function bindings, AUX configuration and light/logic apply (web_func.c). */
esp_err_t web_func_init(void);
void web_func_refresh_tracks(void);
uint32_t web_func_levels(void);
void web_func_io_state(uint16_t *outputs, uint16_t *logic, uint32_t *functions);
uint8_t web_func_track_cat(size_t index);
void web_func_track_cat_set(size_t index, uint8_t cat);
esp_err_t web_func_reconcile_slots(const bool *slots);
bool web_func_slots_referenced(const bool *slot_used);
esp_err_t functions_get(httpd_req_t *req);
esp_err_t function_post(httpd_req_t *req);
esp_err_t aux_effect_post(httpd_req_t *req);
esp_err_t aux_state_get(httpd_req_t *req);
esp_err_t aux_cfg_get(httpd_req_t *req);
esp_err_t aux_cfg_post(httpd_req_t *req);
esp_err_t func_map_get(httpd_req_t *req);
esp_err_t func_map_post(httpd_req_t *req);

/* Core accessors used by the function/AUX endpoint module. */
void web_func_states(bool out[WEB_FN_COUNT]);
const char *web_func_output_name(uint8_t ch);
size_t web_func_bind_count_get(void);
bool web_func_bind_at(size_t idx, func_binding_t *out);
bool web_func_bind_add(const func_binding_t *b);
bool web_func_bind_remove(size_t idx);
void web_func_aux_snapshot(settings_aux_cfg_t out[SETTINGS_AUX_COUNT]);
typedef enum {
    WEB_AUX_UPDATE_OK = 0,
    WEB_AUX_UPDATE_SAVE_FAIL,
    WEB_AUX_UPDATE_APPLY_FAIL,
} web_aux_update_t;
web_aux_update_t web_func_aux_update(uint8_t ch, uint8_t level, uint8_t fx);

/* Graph endpoints (web_graph_api.c). */
esp_err_t graph_error(httpd_req_t *req, const char *status, const char *message);
esp_err_t graph_capabilities_get(httpd_req_t *req);
esp_err_t graph_projects_get(httpd_req_t *req);
esp_err_t graph_project_get(httpd_req_t *req);
esp_err_t graph_state_get(httpd_req_t *req);
esp_err_t graph_asset_get(httpd_req_t *req);
esp_err_t graph_validate_post(httpd_req_t *req);
esp_err_t graph_save_post(httpd_req_t *req);
esp_err_t graph_apply_post(httpd_req_t *req);
esp_err_t graph_project_delete_post(httpd_req_t *req);
esp_err_t graph_reference_post(httpd_req_t *req);

/* Motor and BEMF endpoints (web_motor_api.c, web_bemf_api.c). */
esp_err_t motor_get(httpd_req_t *req);
esp_err_t motor_post(httpd_req_t *req);
esp_err_t emergency_post(httpd_req_t *req);
esp_err_t bemf_cal_get(httpd_req_t *req);
esp_err_t bemf_base_get(httpd_req_t *req);
esp_err_t bemf_cal_post(httpd_req_t *req);
esp_err_t bemf_use_get(httpd_req_t *req);
esp_err_t bemf_use_post(httpd_req_t *req);
int bemf_cal_action(const char *query);
bool bemf_bool_query(const char *query, const char *key, bool *value);

/* Settings, CV/DCC, Wi-Fi and device endpoints (web_settings_api.c). */
esp_err_t control_source_get(httpd_req_t *req);
esp_err_t control_source_post(httpd_req_t *req);
esp_err_t mode_get(httpd_req_t *req);
esp_err_t mode_post(httpd_req_t *req);
esp_err_t track_category_post(httpd_req_t *req);
esp_err_t cv_read_get(httpd_req_t *req);
esp_err_t dcc_status_get(httpd_req_t *req);
esp_err_t cv_all_get(httpd_req_t *req);
esp_err_t cv_write_post(httpd_req_t *req);
esp_err_t wifi_get(httpd_req_t *req);
esp_err_t wifi_post(httpd_req_t *req);
esp_err_t wifi_reset_post(httpd_req_t *req);
esp_err_t factory_reset_post(httpd_req_t *req);
esp_err_t device_get(httpd_req_t *req);
esp_err_t device_post(httpd_req_t *req);
esp_err_t storage_get(httpd_req_t *req);
esp_err_t task_inputs_get(httpd_req_t *req);

/* Sound-library helpers (web_audio_lib.c). */
bool web_name_is_wav(const char *n);
bool web_name_reserved(const char *name);
void web_name_lower_wav(char *name, size_t cap);
int web_name_cmp(const void *a, const void *b);
uint32_t web_file_size(const char *path);
bool web_lib_base_name(const char *in, char *out, size_t out_len);
bool web_lib_resolve_path(const char *base, char *path, size_t cap);
size_t web_vol_load(web_vol_t *out, size_t cap);
bool web_vol_get(const web_vol_t *v, size_t n, const char *name, uint8_t *out);
esp_err_t web_vol_save(const web_vol_t *v, size_t n);
uint8_t audio_library_volume_lookup(const char *name);
esp_err_t audio_library_volume_set(const char *name, uint8_t vol);
bool web_wav_probe(FILE *f, uint16_t *format, uint16_t *channels,
                   uint32_t *sample_rate, uint16_t *block_align,
                   uint16_t *bits, uint16_t *spb,
                   uint32_t *data_off, uint32_t *data_len);

/* Audio preview and library endpoints (web_audio_api.c). */
void web_audio_preview_stop(void);
void web_audio_preview_clear_if(const char *base);
esp_err_t sound_files_list_get(httpd_req_t *req);
esp_err_t sound_file_get(httpd_req_t *req);
esp_err_t audio_status_get(httpd_req_t *req);
esp_err_t audio_tracks_get(httpd_req_t *req);
esp_err_t audio_play_post(httpd_req_t *req);
esp_err_t audio_stop_post(httpd_req_t *req);
esp_err_t audio_volume_post(httpd_req_t *req);
esp_err_t audio_library_get(httpd_req_t *req);
esp_err_t audio_library_volume_post(httpd_req_t *req);
esp_err_t audio_library_delete_post(httpd_req_t *req);
esp_err_t sound_file_raw_get(httpd_req_t *req);
esp_err_t audio_upload_post(httpd_req_t *req);
esp_err_t audio_track_delete_post(httpd_req_t *req);
esp_err_t audio_pack_upload_post(httpd_req_t *req);
esp_err_t ota_update_post(httpd_req_t *req);

/* Asynchronous transfer worker and upload progress (web_transfer.c). */
int64_t web_transfer_deadline_get(void);
void web_transfer_deadline_set(int64_t value);
uint32_t web_transfer_next_sequence(void);
void web_transfer_progress_begin(int total, bool ota, uint8_t slot);
void web_transfer_progress_slot(uint8_t slot);
void web_transfer_progress_bytes(int received);
void web_transfer_progress_end(void);
void web_transfer_reboot_set(bool reboot);
esp_err_t web_transfer_start_supervisor(void);
esp_err_t web_transfer_start_progress_server(void);
bool web_transfer_expired(void);
int64_t web_transfer_ota_deadline(int content_len);
esp_err_t web_transfer_pipe_upload(httpd_req_t *req, FILE *f, int *out_total);
bool web_transfer_aborted(void);
void web_transfer_wdt_feed(size_t bytes);
esp_err_t web_transfer_start(httpd_req_t *req, esp_err_t (*handler)(httpd_req_t *),
                             bool is_ota);

/* Shared request-body stream (web_stream.c). */
typedef struct {
    httpd_req_t *req;
    uint8_t buf[4096];
    size_t len;
    size_t pos;
    int remaining;
    int idle;
} ota_stream_t;

int ota_stream_read(ota_stream_t *s, uint8_t *dst, size_t n);

/* WAV transaction shared by upload and pack (web_upload.c). */
typedef struct {
    char staged[160];
    char canonical[160];
    char backup[160];
    bool original;
    bool published;
} wav_file_tx_t;

typedef struct {
    wav_file_tx_t files[SETTINGS_MAX_TRACKS];
    settings_track_t tracks[SETTINGS_MAX_TRACKS];
    settings_track_t previous[SETTINGS_MAX_TRACKS];
    settings_config_t previous_cfg;
    size_t count, previous_count, file_count;
    char journal[160];
    bool graph_asset_conflict;
} wav_tx_t;

FILE *create_private_file(char *path, size_t path_len, char *rel, size_t rel_len,
                          const char *extension);
FILE *create_staged_wav(char *path, size_t path_len, char *rel, size_t rel_len);
bool wav_artifacts_clean(wav_tx_t *tx);
esp_err_t wav_files_prepare(wav_tx_t *tx);
esp_err_t wav_files_publish(wav_tx_t *tx);
esp_err_t wav_tx_abort(httpd_req_t *req, wav_tx_t *tx, bool metadata_attempted,
                       bool boot_attempted, const char *reason);

/* Cached settings snapshot and its lock (web_config.c). */
esp_err_t web_cfg_init(void);
bool web_cfg_lock(TickType_t wait);
void web_cfg_unlock(void);
bool web_cfg_read(settings_config_t *out);
esp_err_t web_cfg_save(const settings_config_t *next, bool deferred);
settings_config_t *web_cfg(void);

/* WiFi/AP + DNS captive portal, split out of the HTTP router. */
bool parse_ip4(const char *s, esp_ip4_addr_t *out);
esp_err_t wifi_start(const settings_config_t *cfg);
void start_dns_hijack(void);
void wifi_start_auto_off(void);
esp_err_t captive_handler(httpd_req_t *req, httpd_err_code_t err);
bool web_wifi_is_started(void);
