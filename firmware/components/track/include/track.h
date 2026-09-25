#ifndef TRACK_H
#define TRACK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* DC (analog) mode support. Enabled by CV29 bit 2 (NMRA analog bit);
 * default is DCC-only. The rail ADC task drives the motor from the rail
 * voltage only while DC mode is active and rail control is enabled. */

esp_err_t track_init(void);

bool track_is_dc_mode(void);

#endif
