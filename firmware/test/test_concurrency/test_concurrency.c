/* Real-concurrency harness for the FreeRTOS test mocks (G-TEST-1/G-TEST-7).
 *
 * The default stubs are deterministic no-ops so the existing single-threaded
 * suites stay reproducible. This suite is compiled with
 * -DMOCK_FREERTOS_REAL_SYNC, which switches xSemaphore* to native mutexes and
 * exposes mock_thread_* so two real threads can contend for the same lock.
 *
 * The last test models the safety-critical motor calibration flow: a worker
 * publishes calibration progress and a stop/web request cancels the run. It
 * fails if the lock is ever replaced by the no-op (void*)1 implementation. */
#include <unity.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef MOCK_FREERTOS_REAL_SYNC
#error "test_concurrency must be built with -DMOCK_FREERTOS_REAL_SYNC"
#endif

#include "../../test_libs/teststubs/stubs.c"

void setUp(void)
{
    mock_mutex_create_fail = 0;
    mock_sem_take_fail = 0;
}

void tearDown(void)
{
}

/* G-TEST-7: the harness must not silently exercise a no-op safety mock. */
static void test_real_sync_is_not_a_noop_mock(void)
{
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(lock);
    TEST_ASSERT_NOT_EQUAL((SemaphoreHandle_t)1, lock);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(lock, 0));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(lock));
    vSemaphoreDelete(lock);
}

typedef struct {
    SemaphoreHandle_t lock;
    volatile int held;
    volatile int released;
} hold_ctx_t;

static void hold_lock_worker(void *arg)
{
    hold_ctx_t *ctx = (hold_ctx_t *)arg;
    if (xSemaphoreTake(ctx->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    ctx->held = 1;
    mock_thread_sleep_ms(200);
    ctx->held = 0;
    ctx->released = 1;
    (void)xSemaphoreGive(ctx->lock);
}

/* A second thread must observe a real wait: while the lock is held, a bounded
 * take has to time out instead of returning pdTRUE like the old mock. */
static void test_real_mutex_blocks_and_times_out(void)
{
    hold_ctx_t ctx = {0};
    ctx.lock = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(ctx.lock);

    mock_thread_t thread = mock_thread_start(hold_lock_worker, &ctx);
    TEST_ASSERT_NOT_NULL(thread);
    for (int i = 0; i < 400 && !ctx.held; ++i) {
        mock_thread_sleep_ms(1);
    }
    TEST_ASSERT_EQUAL_INT(1, ctx.held);

    TEST_ASSERT_EQUAL(pdFALSE, xSemaphoreTake(ctx.lock, 20));
    mock_thread_join(thread);
    TEST_ASSERT_EQUAL_INT(1, ctx.released);

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ctx.lock, 1000));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreGive(ctx.lock));
    vSemaphoreDelete(ctx.lock);
}

typedef struct {
    SemaphoreHandle_t lock;
    volatile int inside;
    volatile int overlaps;
    volatile int runs;
    volatile int failures;
} overlap_ctx_t;

static void overlap_worker(void *arg)
{
    overlap_ctx_t *ctx = (overlap_ctx_t *)arg;
    for (int i = 0; i < 20; ++i) {
        if (xSemaphoreTake(ctx->lock, portMAX_DELAY) != pdTRUE) {
            ctx->failures++;
            return;
        }
        if (ctx->inside != 0) {
            ctx->overlaps++;
        }
        ctx->inside = 1;
        mock_thread_sleep_ms(2); /* force the other thread to wait at the lock */
        ctx->inside = 0;
        ctx->runs++;
        (void)xSemaphoreGive(ctx->lock);
    }
}

/* Two threads repeatedly enter the same critical section. Any observed overlap
 * means the mock did not provide real mutual exclusion. */
static void test_real_mutex_serializes_two_threads(void)
{
    overlap_ctx_t ctx = {0};
    ctx.lock = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(ctx.lock);

    mock_thread_t a = mock_thread_start(overlap_worker, &ctx);
    mock_thread_t b = mock_thread_start(overlap_worker, &ctx);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    mock_thread_join(a);
    mock_thread_join(b);

    TEST_ASSERT_EQUAL_INT(0, ctx.failures);
    TEST_ASSERT_EQUAL_INT(0, ctx.overlaps);
    TEST_ASSERT_EQUAL_INT(40, ctx.runs);
    vSemaphoreDelete(ctx.lock);
}

typedef struct {
    SemaphoreHandle_t lock;
    volatile int progress;   /* calibration step most recently published */
    volatile int cancelled;  /* stop/web request won */
    volatile int target;     /* commanded speed: non-zero while calibrating */
    volatile int active;
    volatile int saved;      /* a completed curve was persisted */
} cal_stop_ctx_t;

static void calibration_worker(void *arg)
{
    cal_stop_ctx_t *ctx = (cal_stop_ctx_t *)arg;
    for (int step = 1; step <= 40; ++step) {
        if (xSemaphoreTake(ctx->lock, portMAX_DELAY) != pdTRUE) {
            return;
        }
        if (ctx->cancelled) {
            ctx->target = 0;
            ctx->active = 0;
            (void)xSemaphoreGive(ctx->lock);
            return;
        }
        ctx->target = step * 6; /* partial calibration drive */
        ctx->progress = step;
        ctx->active = 1;
        (void)xSemaphoreGive(ctx->lock);
        mock_thread_sleep_ms(5);
    }

    if (xSemaphoreTake(ctx->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!ctx->cancelled) {
        ctx->target = 0;
        ctx->active = 0;
        ctx->saved = 1;
    }
    (void)xSemaphoreGive(ctx->lock);
}

static void stop_web_worker(void *arg)
{
    cal_stop_ctx_t *ctx = (cal_stop_ctx_t *)arg;
    while (ctx->progress < 3) {
        mock_thread_sleep_ms(1);
    }
    if (xSemaphoreTake(ctx->lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    ctx->cancelled = 1;
    ctx->target = 0;
    ctx->active = 0;
    ctx->saved = 0;
    (void)xSemaphoreGive(ctx->lock);
}

/* Motor calibration vs stop/web interleaving: the stop must be able to publish
 * a terminal zero-drive state while the calibration worker is mid-run, and the
 * worker must never restore drive or persist the aborted curve afterwards. */
static void test_calibration_stop_interleaving_is_terminal(void)
{
    cal_stop_ctx_t ctx = {0};
    ctx.lock = xSemaphoreCreateMutex();
    TEST_ASSERT_NOT_NULL(ctx.lock);

    mock_thread_t cal = mock_thread_start(calibration_worker, &ctx);
    mock_thread_t stop = mock_thread_start(stop_web_worker, &ctx);
    TEST_ASSERT_NOT_NULL(cal);
    TEST_ASSERT_NOT_NULL(stop);
    mock_thread_join(stop);
    mock_thread_join(cal);

    TEST_ASSERT_EQUAL_INT(1, ctx.cancelled);
    TEST_ASSERT_EQUAL_INT(0, ctx.saved);
    TEST_ASSERT_EQUAL_INT(0, ctx.target);
    TEST_ASSERT_EQUAL_INT(0, ctx.active);
    TEST_ASSERT_TRUE(ctx.progress >= 3);
    TEST_ASSERT_TRUE(ctx.progress < 40); /* the stop really interrupted the sweep */
    vSemaphoreDelete(ctx.lock);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_sync_is_not_a_noop_mock);
    RUN_TEST(test_real_mutex_blocks_and_times_out);
    RUN_TEST(test_real_mutex_serializes_two_threads);
    RUN_TEST(test_calibration_stop_interleaving_is_terminal);
    return UNITY_END();
}
