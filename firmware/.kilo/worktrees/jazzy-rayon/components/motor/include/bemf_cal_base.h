/* Default BEMF calibration curve used when no calibration is stored in NVS.
 *
 * Points are measured on the reference motor running WITHOUT load: pairs of
 * applied speed (0..126) and back-EMF as a fraction of the rail voltage,
 * scaled by 1024 (e.g. 717 = 70.0% of the rail voltage).
 *
 * To bake the coefficients of YOUR motor into the firmware as the new base:
 *   1. calibrate from the web UI ("Калибровка BEMF"),
 *   2. send "BEMF?" over the COM port (UART/USB, 115200 baud) to read them,
 *   3. overwrite this file with the output of "BEMF-HDR", rebuild and flash.
 */
#ifndef BEMF_CAL_BASE_H
#define BEMF_CAL_BASE_H

#include "settings.h"

static const settings_bemf_cal_t BEMF_CAL_BASE = {
    .count = 10,
    .speed = { 12, 24, 36, 48, 60, 72, 84, 96, 108, 126 },
    .frac  = { 62, 160, 230, 300, 373, 483, 574, 661, 687, 725 },
};

#endif /* BEMF_CAL_BASE_H */
