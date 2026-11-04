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

/* Bench-determined factory curve for the AURA-X reference motor:
 * notch 1 = 9.0% of rail (duty 92), notch 126 = 84.0% of rail.
 * The low-speed floor is CV2 = 23 (start duty 92); below base duty 160 the
 * controller stays open-loop because the BEMF sense reads flyback there. */
static const settings_bemf_cal_t BEMF_CAL_BASE = {
    .count = 2,
    .speed = { 1, 126 },
    .frac  = { 92, 860 },
};

#endif /* BEMF_CAL_BASE_H */
