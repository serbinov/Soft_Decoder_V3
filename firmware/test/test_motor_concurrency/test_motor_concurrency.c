/* Real-sync concurrency test against the production motor calibration worker
 * (G-TEST-1/G-MOTOR-6).
 *
 * Unlike test_concurrency (which models the lock/state interleaving), this
 * suite compiles the real components/motor/src/motor.c and runs the real
 * bemf_cal_task() on a native thread while the test thread stops the motor.
 * The real-sync stubs (see MOCK_FREERTOS_REAL_SYNC) provide actual blocking
 * mutexes; the delay hook is a deterministic barrier so the test never relies
 * on sleeps to hit the desired interleaving.
 *
 * Production C is not modified: the worker is reached through the same
 * admission/reservation API the web/console paths use, and its static entry
 * point is exposed only by the test's local `static` strip. */
#include <unity.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef MOCK_FREERTOS_REAL_SYNC

#include "esp_adc/adc_oneshot.h"
#include "driver/ledc.h"
#include "hal/ledc_ll.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pinmap.h"
#include "settings.h"
#include "bemf_cal_base.h"

static esp_err_t test_adc_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                               int *raw);
static void test_coast_delay(uint32_t us);
static void test_task_delay(TickType_t ticks);
static void test_delay_until(TickType_t *last, TickType_t period);
static TickType_t test_tick_count(void);

/* White-box include: expose motor.c internals (bemf_cal_task, s_control_mutex,
 * s_output_generation, ...). All system headers are pre-included above. */
#define static
#define adc_oneshot_read test_adc_read
#define esp_rom_delay_us test_coast_delay
#define vTaskDelay test_task_delay
#define vTaskDelayUntil test_delay_until
#define xTaskGetTickCount test_tick_count
#include "../../components/motor/src/motor.c"
#undef static
#undef adc_oneshot_read
#undef esp_rom_delay_us
#undef vTaskDelay
#undef vTaskDelayUntil
#undef xTaskGetTickCount

#include "../../test_libs/teststubs/stubs.c"

/* ---- settings.c substitute ------------------------------------------- */
static uint8_t g_cv[SETTINGS_CV_COUNT + 1];
static settings_bemf_cal_t g_cal;
static bool g_cal_valid;

esp_err_t settings_cv_write(uint16_t index, uint8_t value)
{
    if (index == 0U || index > SETTINGS_CV_COUNT) return ESP_ERR_INVALID_ARG;
    g_cv[index] = value;
    return ESP_OK;
}

esp_err_t settings_cv_commit(void)
{
    return ESP_OK;
}

esp_err_t settings_cv_snapshot(uint8_t out[SETTINGS_CV_COUNT + 1])
{
    memcpy(out, g_cv, sizeof(g_cv));
    return ESP_OK;
}

bool settings_bemf_cal_validate(const settings_bemf_cal_t *cal)
{
    if (cal == NULL || cal->count < 2U || cal->count > SETTINGS_BEMF_CAL_MAX_POINTS) {
        return false;
    }
    if (cal->speed[0] == 0U && cal->frac[0] != 0U) {
        return false;
    }
    for (uint8_t i = 0; i < cal->count; ++i) {
        if (cal->speed[i] > 126U || cal->frac[i] > 1024U ||
            (i > 0U && (cal->speed[i] <= cal->speed[i - 1U] ||
                        cal->frac[i] < cal->frac[i - 1U]))) {
            return false;
        }
    }
    return cal->frac[cal->count - 1U] >= SETTINGS_BEMF_CAL_MIN_END_FRAC;
}

esp_err_t settings_cv_read(uint16_t idx, uint8_t *out)
{
    if (out == NULL || idx < 1 || idx > SETTINGS_CV_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = g_cv[idx];
    return ESP_OK;
}

esp_err_t settings_bemf_cal_load(settings_bemf_cal_t *cal)
{
    if (cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!g_cal_valid) {
        return ESP_ERR_NOT_FOUND;
    }
    *cal = g_cal;
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save(const settings_bemf_cal_t *cal)
{
    if (cal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    g_cal = *cal;
    g_cal_valid = true;
    return ESP_OK;
}

esp_err_t settings_bemf_cal_save_guarded(const settings_bemf_cal_t *cal,
                                         settings_bemf_cal_authorize_t authorize,
                                         void *context)
{
    if (!settings_bemf_cal_validate(cal)) return ESP_ERR_INVALID_ARG;
    if (authorize != NULL && !authorize(context)) return ESP_ERR_INVALID_STATE;
    return settings_bemf_cal_save(cal);
}

esp_err_t settings_bemf_cal_clear(void)
{
    g_cal_valid = false;
    return ESP_OK;
}

static bool g_bemf_use = true;

esp_err_t settings_bemf_use_load(bool *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = g_bemf_use;
    return ESP_OK;
}

esp_err_t settings_bemf_use_save(bool enabled)
{
    g_bemf_use = enabled;
    return ESP_OK;
}

/* ---- deterministic worker barrier ------------------------------------- */
static volatile unsigned g_delay_count;
static volatile unsigned g_pause_after_delays;
static volatile int g_worker_paused;
static volatile int g_worker_release;
static volatile int g_worker_pause_timed_out;
static int g_worker_finished = 1;

static void test_task_delay(TickType_t ticks)
{
    vTaskDelay(ticks); /* stub: advances the virtual clock, no real sleep */
    ++g_delay_count;
    if (g_pause_after_delays > 0U && g_delay_count >= g_pause_after_delays &&
        !g_worker_paused) {
        g_worker_paused = 1;
        int released = 0;
        /* Bounded wait: even if the test thread dies, the worker cannot hang. */
        for (unsigned i = 0; i < 5000U; ++i) {
            if (g_worker_release) {
                released = 1;
                break;
            }
            mock_thread_sleep_ms(1);
        }
        if (!released) {
            g_worker_pause_timed_out = 1;
        }
    }
}

static void test_delay_until(TickType_t *last, TickType_t period)
{
    *last += period;
    test_task_delay(period);
}

static TickType_t test_tick_count(void)
{
    return (TickType_t)(mock_timer_now_us / 1000);
}

static esp_err_t test_adc_read(adc_oneshot_unit_handle_t unit, adc_channel_t channel,
                               int *raw)
{
    return adc_oneshot_read(unit, channel, raw);
}

static void test_coast_delay(uint32_t us)
{
    esp_rom_delay_us(us);
}

/* ---- helpers ---- */
static void cv_defaults(void)
{
    memset(g_cv, 0, sizeof(g_cv));
    g_cv[1] = 3;
    g_cv[2] = 0;
    g_cv[5] = 255;
    g_cv[6] = 128;
    g_cv[7] = 4;
    g_cv[29] = 0x02;
    g_cv[54] = 128;
    g_cv[55] = 60;
    g_cv[56] = 32;
    g_cv[66] = 128;
    g_cv[95] = 128;
    for (int i = 0; i < 28; ++i) {
        g_cv[67 + i] = (uint8_t)((i * 255) / 27);
    }
}

static void worker_thread(void *arg)
{
    bemf_cal_task(arg);
}

static void adaptive_worker_thread(void *arg)
{
    bemf_cal_task(arg);
}

static void set_cal_adc(void)
{
    memset(mock_adc_raw, 0, sizeof(mock_adc_raw));
    /* ~45 mV against a 2000 mV rail: above zero for a valid sample, below the
     * at-rest threshold (rail/16) so admission accepts the run. */
    mock_adc_raw[PIN_BEMF1 - 1] = 60;
    mock_adc_raw[PIN_BEMF2 - 1] = 0;
    mock_adc_raw[PIN_RAIL_SENSE - 1] = 0;
}

void setUp(void)
{
    mock_timer_now_us = 0;
    mock_adc_ok = 1;
    mock_task_create_ok = 1;
    mock_task_create_fail_after = -1;
    mock_task_create_calls = 0;
    mock_task_create_hook = NULL;
    mock_task_startup_hook = NULL;
    mock_mutex_create_fail = 0;
    mock_sem_take_fail = 0;
    mock_ledc_update_count = 0;
    memset(mock_ledc_duty, 0, sizeof(mock_ledc_duty));
    mock_rom_delay_us_total = 0;

    g_delay_count = 0;
    g_pause_after_delays = 0;
    g_worker_paused = 0;
    g_worker_release = 0;
    g_worker_pause_timed_out = 0;
    g_worker_finished = 1;

    g_cal_valid = false;
    memset(&g_cal, 0, sizeof(g_cal));
    g_bemf_use = true;
    cv_defaults();

    TEST_ASSERT_EQUAL(ESP_OK, motor_init());
    TEST_ASSERT_TRUE(motor_is_inhibited());
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_inhibited(false));
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(0));
}

void tearDown(void)
{
    /* If the bounded join failed, the worker may still own the production
     * mutexes; leaking them is safer than freeing an object in use. */
    if (!g_worker_finished) {
        return;
    }
    if (s_control_mutex != NULL) {
        vSemaphoreDelete(s_control_mutex);
        s_control_mutex = NULL;
    }
    if (s_adc_mutex != NULL) {
        vSemaphoreDelete(s_adc_mutex);
        s_adc_mutex = NULL;
    }
}

/* Start the production legacy calibration worker on a real thread, pause it at
 * a known delay after the first driven sample, then stop from this (real) test
 * thread while the worker is alive. The delay hook is the barrier, so the
 * interleaving is deterministic rather than sleep-timed. */
static void stop_during_calibration(bool emergency, bool adaptive)
{
    set_cal_adc();
    TEST_ASSERT_EQUAL(ESP_OK, motor_set_rail_voltage_mv(2000));
    TEST_ASSERT_FALSE(motor_bemf_cal_active());
    if (adaptive) {
        /* Same admission path/default profile as the web UI. */
        motor_bemf_cal_config_t cfg;
        motor_bemf_cal_default_config(&cfg);
        TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start_ex(&cfg));
    } else {
        TEST_ASSERT_EQUAL(ESP_OK, motor_bemf_cal_start());
    }
    TEST_ASSERT_TRUE(motor_bemf_cal_active());

    uint32_t generation = s_output_generation;
    g_delay_count = 0;
    g_worker_paused = 0;
    g_worker_release = 0;
    g_worker_pause_timed_out = 0;
    g_pause_after_delays = 1; /* first settle/boost delay after a sample */
    g_worker_finished = 0;

    mock_thread_t worker = mock_thread_start(
        adaptive ? adaptive_worker_thread : worker_thread,
        (void *)(uintptr_t)generation);
    TEST_ASSERT_NOT_NULL(worker);

    bool paused = false;
    for (unsigned i = 0; i < 5000U; ++i) {
        if (g_worker_paused) {
            paused = true;
            break;
        }
        mock_thread_sleep_ms(1);
    }

    bool lock_released = false;
    esp_err_t stop_err = ESP_OK;
    if (paused) {
        lock_released = xSemaphoreTake(s_control_mutex, 100) == pdTRUE;
        if (lock_released) {
            (void)xSemaphoreGive(s_control_mutex);
        }
        if (emergency) {
            motor_emergency_stop();
        } else {
            stop_err = motor_set_speed(0, true); /* web/API stop path */
        }
    }

    /* Always release and join before asserting so a failure cannot leave the
     * production worker parked forever. */
    g_worker_release = 1;
    int joined = mock_thread_join_timeout(worker, 5000);
    g_worker_finished = joined;

    TEST_ASSERT_TRUE_MESSAGE(paused, "calibration worker did not reach the barrier");
    TEST_ASSERT_FALSE_MESSAGE(g_worker_pause_timed_out, "worker barrier timed out");
    TEST_ASSERT_TRUE_MESSAGE(lock_released,
                             "control mutex was still held while the worker was paused");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, joined, "calibration worker did not terminate");
    TEST_ASSERT_EQUAL(ESP_OK, stop_err);

    TEST_ASSERT_FALSE(motor_bemf_cal_active());
    motor_bemf_cal_info_t info;
    motor_bemf_cal_info(&info);
    TEST_ASSERT_FALSE(info.active);
    TEST_ASSERT_TRUE(info.result == MOTOR_BEMF_CAL_CANCELLED ||
                     info.result == MOTOR_BEMF_CAL_FAILED);

    uint8_t target = 255;
    bool forward = true;
    motor_get_status(&target, &forward);
    TEST_ASSERT_EQUAL_UINT8(0, target);
    uint8_t applied = 255;
    motor_get_applied_speed(&applied, NULL);
    TEST_ASSERT_EQUAL_UINT8(0, applied);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_0]);
    TEST_ASSERT_EQUAL_UINT32(0, mock_ledc_duty[LEDC_CHANNEL_1]);
    TEST_ASSERT_EQUAL_UINT32(0, s_last_duty);
    TEST_ASSERT_FALSE(g_cal_valid); /* a cancelled run must not save a curve */

    /* The control mutex must be free after the worker finished. */
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(s_control_mutex, 100));
    (void)xSemaphoreGive(s_control_mutex);
}

static void test_calibration_emergency_stop_real_worker(void)
{
    stop_during_calibration(true, false); /* legacy FAST worker, e-stop */
}

static void test_calibration_web_stop_real_worker(void)
{
    stop_during_calibration(false, true); /* adaptive web worker, speed 0 */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_calibration_emergency_stop_real_worker);
    RUN_TEST(test_calibration_web_stop_real_worker);
    return UNITY_END();
}

#else

/* Platforms without the real-sync shim skip the production-worker test rather
 * than failing the build. The deterministic test_motor suite still runs. */
void setUp(void)
{
}

void tearDown(void)
{
}

static void test_real_sync_unavailable(void)
{
    TEST_IGNORE_MESSAGE("MOCK_FREERTOS_REAL_SYNC shim unavailable on this platform");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_sync_unavailable);
    return UNITY_END();
}

#endif /* MOCK_FREERTOS_REAL_SYNC */
