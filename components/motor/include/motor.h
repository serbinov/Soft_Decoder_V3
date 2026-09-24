#ifndef MOTOR_H
#define MOTOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t motor_init(void);
/* Drive the H-bridge inputs into a non-driving state (coast) as early as
 * possible — before the slow storage/NVS init. From reset until motor_init()
 * IN1/IN2 are inputs, so a DRV8870 without external pull-downs can briefly
 * drive the motor; calling this at the start of app_main prevents that. */
void motor_boot_safe(void);
esp_err_t motor_set_speed(uint8_t speed128, bool forward);
void motor_stop(void);
void motor_get_status(uint8_t *out_speed128, bool *out_forward);

/* Rail sense voltage (divider mV) fed by the track task; used for the BEMF
 * speed target and freewheel-clamp rejection. */
esp_err_t motor_set_rail_voltage_mv(uint32_t rail_mv);

/* Serialize ADC1 between the track task (rail sense) and the motor task
 * (BEMF sampling window). Call unlock ONLY when lock returned true. */
bool motor_bemf_lock(void);
void motor_bemf_unlock(void);

/* BEMF calibration: runs the motor without load through a set of speed steps,
 * measures the back-EMF (as a fraction of the rail voltage) at each step and
 * stores the curve in NVS. Afterwards the PID targets the calibrated curve so
 * the no-load RPM is held even under load. */
#define MOTOR_BEMF_CAL_MAX_POINTS 16

typedef struct {
    bool active;      /* calibration running */
    uint8_t step;     /* current step while active (1-based) */
    uint8_t total;    /* total steps */
    uint8_t count;    /* measured points (current run or stored) */
    uint8_t speed[MOTOR_BEMF_CAL_MAX_POINTS];
    uint16_t frac[MOTOR_BEMF_CAL_MAX_POINTS]; /* rail fraction * 1024 */
    bool valid;       /* a usable calibrated curve is loaded */
    bool stored;      /* the curve came from a stored calibration, not the base */
} motor_bemf_cal_info_t;

esp_err_t motor_bemf_cal_start(void);
void motor_bemf_cal_info(motor_bemf_cal_info_t *info);
esp_err_t motor_bemf_cal_clear(void);
/* Re-read the stored calibration (or the firmware base curve) and apply it. */
esp_err_t motor_bemf_cal_reload(void);

/* Enable/disable closed-loop BEMF regulation. When disabled the motor runs
 * open-loop on the speed-curve PWM only: no coast sampling window and no PID
 * correction. Useful when the back-EMF measurement is unreliable or the motor
 * is not the one the calibration curve was taken with. The value is loaded
 * from settings at motor_init(); the caller persists changes with
 * settings_bemf_use_save(). */
void motor_set_bemf_enabled(bool enabled);
bool motor_get_bemf_enabled(void);

/* Raw BEMF measurement diagnostics for debugging the calibration / PID. */
typedef struct {
    uint32_t rail_mv;
    uint32_t bemf1_mv;
    uint32_t bemf2_mv;
    uint32_t bemf_filtered_mv;
    bool bemf_valid;
    bool cal_valid;
    bool cal_active;
    uint8_t cal_step;
    uint8_t applied_speed;
    uint32_t duty;
    int32_t error;
    int32_t integral;
    int32_t corr;
    bool pid_ok;
    int32_t target;
} motor_bemf_diag_t;
void motor_bemf_diag(motor_bemf_diag_t *d);

/* Direct raw ADC read of the BEMF and rail sense pins (diagnostics). */
void motor_bemf_adc_dump(uint16_t *b1_raw, uint16_t *b2_raw, uint16_t *rail_raw);
/* Force a coast window and read the BEMF terminals in mV (diagnostics). */
void motor_bemf_coast_read(uint16_t *b1_mv, uint16_t *b2_mv);

/* Firmware base (factory-default) BEMF settings. */
#define MOTOR_BEMF_BASE_MAX_POINTS 16

typedef struct {
    uint8_t count;
    uint8_t speed[MOTOR_BEMF_BASE_MAX_POINTS];
    uint16_t frac[MOTOR_BEMF_BASE_MAX_POINTS];   /* rail fraction * 1024 */
    uint16_t start_frac;                          /* linear target start * 1024 */
    uint16_t full_frac;                           /* linear target full * 1024 */
} motor_bemf_base_info_t;
void motor_bemf_base_info(motor_bemf_base_info_t *info);

#endif
