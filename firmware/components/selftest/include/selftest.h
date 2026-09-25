#ifndef SELFTEST_H
#define SELFTEST_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Built-in boot self-test, driven from the serial console ("SELFTEST").
 * It only probes state and exercises read paths: it never moves the motor,
 * plays a sound, changes persisted settings or formats storage, so it is safe
 * to run on a live decoder (including one standing on the rails). */

#define SELFTEST_MAX_CHECKS 12

typedef enum {
    SELFTEST_PASS = 0,
    SELFTEST_FAIL = 1,
    SELFTEST_SKIP = 2,
} selftest_state_t;

typedef struct {
    const char *name;
    selftest_state_t state;
} selftest_item_t;

typedef struct {
    uint8_t count;
    selftest_item_t items[SELFTEST_MAX_CHECKS];
} selftest_report_t;

/* Run all checks and fill `report`. Returns the number of checks that passed.
 * Passing a NULL report is allowed and returns 0. */
uint8_t selftest_run(selftest_report_t *report);

/* "PASS" / "FAIL" / "SKIP" for console output. */
const char *selftest_state_name(selftest_state_t state);

/* --- Actuating HIL commands -------------------------------------------------
 * These DO drive hardware (light outputs, sound, motor) for a bounded time and
 * then restore/stop, so they are only used from the explicit HIL commands.
 * `ms` is clamped by the caller-visible bounds below. */

#define SELFTEST_ACT_MS_MIN 20U
#define SELFTEST_ACT_MS_MAX 5000U
/* Motor spins for at most this many steps so a bench test cannot run away. */
#define SELFTEST_MOTOR_SPEED_MAX 60U

/* Turn an AUX output on for `ms` then restore its previous on/off state. */
esp_err_t selftest_act_aux(uint8_t channel, uint16_t ms);

/* Play a sound slot for `ms` then stop the test voice. ESP_ERR_NOT_FOUND when
 * the slot has no enabled track. */
esp_err_t selftest_act_sound(uint8_t slot, uint16_t ms);

/* Drive the motor at `speed` for `ms` then stop. */
esp_err_t selftest_act_motor(uint8_t speed, uint16_t ms);

/* Apply a function state (F0..F28) exactly like a DCC/UI function press. */
esp_err_t selftest_act_function(uint8_t fn, bool on);

/* Sweep bounds: 29 functions x ms stays short. */
#define SELFTEST_SWEEP_MS_MAX 1000U

/* Press every function F0..F28 on for `ms`, then release it. Returns the number
 * of functions swept, or 0 on a bad `ms`. */
uint8_t selftest_act_fn_sweep(uint16_t ms);

/* Turn each of the 9 AUX channels on for `ms`, restoring its state. Returns the
 * number of channels swept, or 0 on a bad `ms`. */
uint8_t selftest_act_aux_sweep(uint16_t ms);

#endif /* SELFTEST_H */
