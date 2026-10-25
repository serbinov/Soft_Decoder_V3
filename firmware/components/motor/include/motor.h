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
/* Fail-safe stop: drive the bridge off immediately from any task, even if the
 * motor task is stuck. Use for resets / emergency stops instead of motor_stop()
 * when the stop must not depend on the motor task running. */
void motor_emergency_stop(void);
/* Persistent startup/maintenance gate. Boot defaults to inhibited. Setting
 * true immediately coasts, clears the command and cancels calibration without
 * waiting for the control task. Release only after safety is ready; false never
 * restores an old command. Nonzero speed/calibration requests are rejected. */
esp_err_t motor_set_inhibited(bool inhibited);
typedef enum {
    MOTOR_INHIBIT_CONTROL = 1U,
    MOTOR_INHIBIT_SAFETY = 2U,
    MOTOR_INHIBIT_DCC_TIMEOUT = 4U,
} motor_inhibit_reason_t;
/* Independent owners cannot clear another owner's inhibit. */
esp_err_t motor_set_inhibit_reason(motor_inhibit_reason_t reason, bool inhibited);
/* Also reports the motor-owned feedback fault: closed-loop feedback loss holds
 * non-increasing duty for at most 100 ms, then coasts and rejects nonzero
 * commands. Only speed=0 or explicit BEMF disable clears that fault; neither
 * clears CONTROL, SAFETY or DCC_TIMEOUT. */
bool motor_is_inhibited(void);
/* Timestamp (us) of the last completed motor tick (0 before the first tick). */
int64_t motor_last_tick_us(void);
void motor_get_status(uint8_t *out_speed128, bool *out_forward);
/* Actual ramped output (not the target): what the motor is doing right now.
 * The sound engine follows this so transitions match the real motion. */
void motor_get_applied_speed(uint8_t *out_speed128, bool *out_forward);

/* Rail sense voltage (divider mV) fed by the track task; used for the BEMF
 * speed target and freewheel-clamp rejection. Closed-loop/calibration accepts
 * only adequate publications younger than 100 ms. */
esp_err_t motor_set_rail_voltage_mv(uint32_t rail_mv);

/* Serialize ADC1 between the track task (rail sense) and the motor task
 * (BEMF sampling window). Call unlock ONLY when lock returned true. */
bool motor_bemf_lock(void);
void motor_bemf_unlock(void);

/* Shared ADC1 (adc_oneshot) used by the BEMF sampling and the track rail
 * sense. Configure a channel (12-bit, 12 dB) before reading. Reads return -1
 * when the ADC unit or channel is unavailable. */
esp_err_t motor_adc_config_channel(int gpio_num);
int motor_adc_read_raw(int gpio_num);

/* BEMF calibration: runs the motor without load through a set of speed steps,
 * measures the back-EMF (as a fraction of the rail voltage) at each step and
 * stores the curve in NVS. Afterwards the PID targets the calibrated curve so
 * the no-load RPM is held even under load. */
#define MOTOR_BEMF_CAL_MAX_POINTS 16
#define MOTOR_BEMF_CAL_MIN_POINTS 10

typedef enum {
    MOTOR_BEMF_CAL_IDLE,
    MOTOR_BEMF_CAL_RUNNING,
    MOTOR_BEMF_CAL_SAVING,
    MOTOR_BEMF_CAL_SUCCEEDED,
    MOTOR_BEMF_CAL_CANCELLED,
    MOTOR_BEMF_CAL_FAILED,
} motor_bemf_cal_state_t;

/* Adaptive calibration profiles. FAST is the legacy 10-point run and is kept
 * for compatibility (and for the plain motor_bemf_cal_start() call). AUTO uses
 * the thorough adaptive plan; CUSTOM honours the caller's speed list and
 * windows. */
typedef enum {
    MOTOR_BEMF_MODE_AUTO = 0,
    MOTOR_BEMF_MODE_FAST,
    MOTOR_BEMF_MODE_BALANCED,
    MOTOR_BEMF_MODE_THOROUGH,
    MOTOR_BEMF_MODE_CUSTOM,
} motor_bemf_cal_mode_t;
/* Profile marker for a curve loaded from NVS/base without a live run. */
#define MOTOR_BEMF_MODE_STORED 0xFFU

typedef struct {
    motor_bemf_cal_mode_t mode;
    uint8_t points;          /* target total, 0 = mode default (min 10, max 16) */
    uint16_t settle_ms;      /* minimum settle, 0 = mode default */
    uint16_t settle_max_ms;  /* maximum settle, 0 = 4 x settle */
    uint16_t interval_ms;    /* sample/probe interval, 0 = 40; max 80 */
    uint8_t samples;         /* measured samples per point, 0 = mode default */
    uint8_t retries;         /* point retries; 0 = mode default for non-custom */
    uint16_t boost_ms;       /* first/stall boost, 0 = mode default */
    bool adaptive;           /* CUSTOM only: allow adaptive insertion */
    bool apply_pid;          /* apply the recommended Kp/Ki/Kd after success */
    uint8_t speed_count;     /* CUSTOM: number of explicit speeds (>=10) */
    uint8_t speed[MOTOR_BEMF_CAL_MAX_POINTS];
} motor_bemf_cal_config_t;

typedef enum {
    MOTOR_BEMF_CAL_ERR_NONE,
    MOTOR_BEMF_CAL_ERR_ADC,
    MOTOR_BEMF_CAL_ERR_RAIL,
    MOTOR_BEMF_CAL_ERR_TIMEOUT,
    MOTOR_BEMF_CAL_ERR_CONTROL,
    MOTOR_BEMF_CAL_ERR_CURVE,
    MOTOR_BEMF_CAL_ERR_STORAGE,
    MOTOR_BEMF_CAL_ERR_START,
} motor_bemf_cal_error_t;

/* Result of the last adaptive run. The suggested PID gains are derived from
 * the measured plant slope and settle time; they are informational unless
 * motor_bemf_cal_config_t.apply_pid was set. */
typedef struct {
    uint8_t mode;             /* motor_bemf_cal_mode_t */
    uint8_t planned_points;   /* points the planner aimed for */
    uint8_t measured_points;  /* successful rotating points */
    uint8_t stalled_points;   /* points that did not rotate */
    uint8_t ceiling_speed;     /* first speed above the measurement ceiling, 0=none */
    uint8_t first_move_speed;  /* lowest successful speed */
    uint16_t first_frac;       /* BEMF at first_move_speed, x1024 */
    uint16_t settle_ms_avg;    /* average accepted settle time */
    uint16_t plant_gain_x1000; /* mV per duty count (x1000) */
    uint16_t kp_x100;          /* suggested Kp x100 */
    uint16_t ki_x1000;         /* suggested Ki x1000 */
    uint16_t kd_x100;          /* suggested Kd x100 */
    uint8_t quality;           /* average sample quality, 0..100 */
    bool pid_applied;          /* suggested gains were written to CV54..56 */
} motor_bemf_profile_t;

typedef struct {
    bool active;      /* calibration running */
    uint8_t step;     /* current step while active (1-based) */
    uint8_t total;    /* total steps (can grow when adaptive points are added) */
    uint8_t count;    /* measured points (current run or stored) */
    uint8_t speed[MOTOR_BEMF_CAL_MAX_POINTS];
    uint16_t frac[MOTOR_BEMF_CAL_MAX_POINTS]; /* rail fraction * 1024 */
    bool valid;       /* a usable calibrated curve is loaded */
    bool stored;      /* the curve came from a stored calibration, not the base */
    uint32_t run_id;  /* nonzero per admitted run; clear preserves this ID */
    motor_bemf_cal_state_t result;
    motor_bemf_cal_error_t reason;
    esp_err_t error_code;
    motor_bemf_profile_t profile;
} motor_bemf_cal_info_t;

esp_err_t motor_bemf_cal_start(void);
/* Fill a configuration with AUTO defaults (adaptive, up to 16 points). */
void motor_bemf_cal_default_config(motor_bemf_cal_config_t *cfg);
/* Start an adaptive/custom calibration. cfg.mode selects the profile; CUSTOM
 * requires at least MOTOR_BEMF_CAL_MIN_POINTS explicit ascending speeds. The
 * engine settles each point until stable, retries noisy points, skips a
 * non-rotating point after a boost, inserts extra points where the curve is
 * steep/uncertain, and saves a partial curve if the ceiling is reached. */
esp_err_t motor_bemf_cal_start_ex(const motor_bemf_cal_config_t *cfg);
/* Independent 10 ms safety caller: no mutex, ADC, NVS, allocation or waits.
 * Coasts on stale/low rail or an expired 100 ms worker-only drive lease. */
void motor_bemf_cal_watchdog(void);
/* Nonblocking, idempotent; inactive cancellation never stops normal motion.
 * The active reservation survives cancellation/failure until worker cleanup. */
esp_err_t motor_bemf_cal_cancel(void);
/* True while an admitted calibration owns the bridge (including the bounded
 * cleanup window after a cancel/failure). DCC speed packets must not be
 * forwarded to motor_set_speed() during this time: a repeating speed-0 packet
 * would otherwise cancel the calibration immediately. */
bool motor_bemf_cal_active(void);
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
/* Measurement acceptance ceiling, fraction of rail. Default is the 0.85
 * rail-clamp heuristic, not a physical freewheel detector. Targets never exceed
 * this ceiling and are additionally bounded by the per-step open-loop duty.
 * The value is fixed at build time; increase it only from measured hardware
 * data. */


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
esp_err_t motor_bemf_adc_dump_checked(uint16_t *b1_raw, uint16_t *b2_raw, uint16_t *rail_raw);
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
