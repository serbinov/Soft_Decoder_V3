#ifndef DCC_H
#define DCC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*dcc_speed_cb_t)(uint8_t speed128, bool forward);
typedef void (*dcc_function_cb_t)(uint8_t fn, bool state);
typedef void (*dcc_cv_write_cb_t)(uint16_t cv, uint8_t value, bool service_mode);
typedef bool (*dcc_cv_read_cb_t)(uint16_t cv, uint8_t *out_value);
typedef void (*dcc_reset_cb_t)(void);

esp_err_t dcc_init(void);
void dcc_set_address(uint16_t addr, bool long_addr);
void dcc_set_speed_step_mode(bool mode_14);
void dcc_set_consist(uint8_t address, bool reverse_dir);

/* Re-read CV1/CV17/CV18/CV19/CV29 (through the registered CV read callback)
 * and apply address / speed-step mode / consist immediately. Call after any
 * write to those CVs, from DCC or from the web UI. */
void dcc_reload_config(void);

void dcc_register_speed_cb(dcc_speed_cb_t cb);
void dcc_register_function_cb(dcc_function_cb_t cb);
void dcc_register_cv_write_cb(dcc_cv_write_cb_t cb);
void dcc_register_cv_read_cb(dcc_cv_read_cb_t cb);
void dcc_register_reset_cb(dcc_reset_cb_t cb);

/* Timestamp (us, esp_timer) of the last valid packet addressed to this
 * decoder (or broadcast/service). Used for the CV11 packet timeout. */
int64_t dcc_last_packet_us(void);

/* Service-mode acknowledgement (NMRA ~6 ms current pulse on ACK_LOAD). */
esp_err_t dcc_service_ack(void);

#endif
