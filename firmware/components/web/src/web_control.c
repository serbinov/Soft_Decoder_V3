/* Control-source, actuation-readiness and maintenance lease.
 * Owns the control mutex, the short critical section that guards readiness,
 * maintenance ownership and safety epochs. */

#include "web.h"
#include "web_internal.h"

#include "audio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "motor.h"
#include "sound.h"
#include "storage.h"

static SemaphoreHandle_t s_control_mutex;
static portMUX_TYPE s_control_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_actuation_ready;
static TaskHandle_t s_maintenance_owner;
static bool s_maintenance_exclusive_fs;
static uint32_t s_control_generation;
static uint32_t s_safety_epoch;
static unsigned s_mutation_count;
static TaskHandle_t s_mutation_owner;

esp_err_t web_control_init(void)
{
    if (s_control_mutex != NULL) return ESP_OK;
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_control_state_mux);
    bool adopted = s_control_mutex == NULL;
    if (adopted) s_control_mutex = created;
    portEXIT_CRITICAL(&s_control_state_mux);
    if (!adopted) vSemaphoreDelete(created);
    return ESP_OK;
}

bool web_control_take(TickType_t wait)
{
    return s_control_mutex != NULL && xSemaphoreTake(s_control_mutex, wait) == pdTRUE;
}

void web_control_give(void)
{
    if (s_control_mutex != NULL) xSemaphoreGive(s_control_mutex);
}

void web_control_state_enter(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
}

void web_control_state_exit(void)
{
    portEXIT_CRITICAL(&s_control_state_mux);
}

void web_control_bump_generation(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    ++s_control_generation;
    portEXIT_CRITICAL(&s_control_state_mux);
}

void web_control_force_not_ready(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    s_actuation_ready = false;
    portEXIT_CRITICAL(&s_control_state_mux);
}

bool web_control_admit_mutation(bool *ready_out)
{
    portENTER_CRITICAL(&s_control_state_mux);
    bool admitted = s_maintenance_owner == NULL;
    if (admitted) { ++s_mutation_count; s_mutation_owner = xTaskGetCurrentTaskHandle(); }
    if (ready_out != NULL) *ready_out = s_actuation_ready;
    portEXIT_CRITICAL(&s_control_state_mux);
    return admitted;
}

void web_control_finish_mutation(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    --s_mutation_count;
    if (s_mutation_count == 0U) s_mutation_owner = NULL;
    portEXIT_CRITICAL(&s_control_state_mux);
}

void web_control_set_maintenance_owner(TaskHandle_t owner)
{
    portENTER_CRITICAL(&s_control_state_mux);
    s_maintenance_owner = owner;
    portEXIT_CRITICAL(&s_control_state_mux);
}

uint32_t web_control_safety_epoch_get(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    uint32_t epoch = s_safety_epoch;
    portEXIT_CRITICAL(&s_control_state_mux);
    return epoch;
}

void web_control_safety_epoch_bump(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    ++s_safety_epoch;
    portEXIT_CRITICAL(&s_control_state_mux);
}

bool web_control_is_rails(void)
{
    bool rails = false;
    if (web_control_take(pdMS_TO_TICKS(100))) {
        rails = web_cfg()->control_source == 0 && web_actuation_ready() && !web_maintenance_active();
        web_control_give();
    }
    return rails;
}

bool web_control_source_is_rails(void)
{
    /* Deliberately lock-free: DCC emergency/reset must be able to stop motion
     * even while an HTTP handler holds the control mutex across NVS work. */
    bool rails;
    portENTER_CRITICAL(&s_control_state_mux);
    rails = web_cfg()->control_source == 0 && s_actuation_ready;
    portEXIT_CRITICAL(&s_control_state_mux);
    return rails;
}

bool web_actuation_ready(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    bool ready = s_actuation_ready;
    portEXIT_CRITICAL(&s_control_state_mux);
    return ready;
}

bool web_maintenance_active(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    bool active = s_maintenance_owner != NULL;
    portEXIT_CRITICAL(&s_control_state_mux);
    return active;
}

bool web_control_rails_begin(uint32_t *generation)
{
    if (!web_control_take(pdMS_TO_TICKS(100))) return false;
    if (!web_actuation_ready() || web_maintenance_active() || web_cfg()->control_source != 0) {
        web_control_give();
        return false;
    }
    if (generation != NULL) *generation = s_control_generation;
    return true;
}

void web_control_rails_end(void)
{
    web_control_give();
}

esp_err_t web_set_actuation_ready(bool ready)
{
    if (!ready) {
        portENTER_CRITICAL(&s_control_state_mux);
        s_actuation_ready = false;
        portEXIT_CRITICAL(&s_control_state_mux);
    }
    if (!ready) (void)motor_set_inhibited(true);
    esp_err_t err = web_control_init();
    if (err != ESP_OK) return err;
    if (!web_control_take(pdMS_TO_TICKS(100))) return ESP_ERR_TIMEOUT;
    if (ready && web_maintenance_active()) {
        web_control_give();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t sound_err = sound_set_inhibited(!ready);
    esp_err_t audio_err = audio_set_inhibited(!ready);
    /* INVALID_STATE means the optional audio/sound subsystem was never
     * initialised (absent hardware, or import failure). It must neither block
     * DCC/motor readiness nor block OTA confirmation. */
    if (sound_err == ESP_ERR_INVALID_STATE) sound_err = ESP_OK;
    if (audio_err == ESP_ERR_INVALID_STATE) audio_err = ESP_OK;
    err = sound_err != ESP_OK ? sound_err : audio_err;
    if (err != ESP_OK) {
        ready = false;
        (void)motor_set_inhibited(true);
        (void)sound_set_inhibited(true);
        (void)audio_set_inhibited(true);
    }
    portENTER_CRITICAL(&s_control_state_mux);
    s_actuation_ready = ready;
    portEXIT_CRITICAL(&s_control_state_mux);
    ++s_control_generation;
    bool inhibited = !ready || web_maintenance_active();
    (void)motor_set_inhibited(inhibited);
    web_control_give();
    return err;
}

bool web_control_motor_stopped(void)
{
    uint8_t target = 0, applied = 0;
    motor_get_status(&target, NULL);
    motor_get_applied_speed(&applied, NULL);
    return target == 0U && applied == 0U;
}

esp_err_t web_control_maintenance_reserve(bool stopped)
{
    esp_err_t init_err = web_control_init();
    if (init_err != ESP_OK) return init_err;
    if (!web_control_take(pdMS_TO_TICKS(100))) return ESP_ERR_TIMEOUT;
    if (web_maintenance_active()) {
        web_control_give();
        return ESP_ERR_INVALID_STATE;
    }
    /* Check before inhibit can erase the target/applied-motion evidence. */
    if (stopped && (!web_actuation_ready() || !web_control_motor_stopped())) {
        web_control_give();
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_control_state_mux);
    s_maintenance_owner = xTaskGetCurrentTaskHandle();
    s_maintenance_exclusive_fs = false;
    portEXIT_CRITICAL(&s_control_state_mux);
    ++s_control_generation;
    esp_err_t err = motor_set_inhibited(true);
    esp_err_t sound_err = sound_set_inhibited(true);
    esp_err_t audio_err = audio_set_inhibited(true);
    web_control_give();
    if (err == ESP_OK) err = sound_err;
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
    if (audio_err == ESP_ERR_INVALID_STATE) audio_err = ESP_OK;
    if (err == ESP_OK) err = audio_err;
    if (err != ESP_OK) {
        /* The reserve failed before quiescence, so nothing was mutated. Abort
         * the maintenance and let web_maintenance_end() apply the existing
         * re-inhibit/readiness rule (a transient timeout can recover there).
         * Do not force readiness closed here: a one-shot timeout would
         * otherwise latch the decoder unusable until an explicit recovery. */
        web_maintenance_end();
    }
    return err;
}

esp_err_t web_control_maintenance_quiesce(bool exclusive_fs)
{
    esp_err_t err = ESP_OK;
    if (exclusive_fs) {
        err = storage_maintenance_begin();
        if (err == ESP_OK) s_maintenance_exclusive_fs = true;
    }
    int64_t deadline = esp_timer_get_time() + 2000000;
    while (err == ESP_OK) {
        portENTER_CRITICAL(&s_control_state_mux);
        bool mutations_drained = s_mutation_count == 0U ||
            (s_mutation_count == 1U && s_mutation_owner == xTaskGetCurrentTaskHandle());
        portEXIT_CRITICAL(&s_control_state_mux);
        if (audio_is_quiescent() && storage_is_quiescent() && mutations_drained) break;
        if (esp_timer_get_time() >= deadline) { err = ESP_ERR_TIMEOUT; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return err;
}

esp_err_t web_maintenance_begin(bool exclusive_fs)
{
    esp_err_t err = web_control_maintenance_reserve(false);
    if (err != ESP_OK) return err;
    err = web_control_maintenance_quiesce(exclusive_fs);
    if (err != ESP_OK) web_maintenance_end();
    return err;
}

/* Releasing readiness is a short, bounded operation: a transient runtime
 * timeout must not permanently latch actuation off. Only ESP_ERR_TIMEOUT is
 * retried (INVALID_STATE means the subsystem is not initialised at all). */
static esp_err_t set_inhibited_retry(esp_err_t (*set)(bool), bool inhibited)
{
    esp_err_t err = set(inhibited);
    for (unsigned attempt = 1U; attempt < 3U && err == ESP_ERR_TIMEOUT; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
        err = set(inhibited);
    }
    return err;
}

/* Shared maintenance release. Only the task that owns the maintenance may
 * release it: force-releasing another task's lease while it is still writing
 * the filesystem is unsafe (it re-enables actuation under a live mutation).
 * A truly stuck worker is handled by the task watchdog, not by stealing its
 * locks. */
static void maintenance_end_impl(void)
{
    portENTER_CRITICAL(&s_control_state_mux);
    TaskHandle_t owner = s_maintenance_owner;
    portEXIT_CRITICAL(&s_control_state_mux);
    if (owner != xTaskGetCurrentTaskHandle()) {
        return;
    }
    if (s_maintenance_exclusive_fs) storage_maintenance_end();
    if (web_control_init() != ESP_OK) {
        portENTER_CRITICAL(&s_control_state_mux);
        s_maintenance_owner = NULL;
        s_maintenance_exclusive_fs = false;
        s_actuation_ready = false;
        portEXIT_CRITICAL(&s_control_state_mux);
        return;
    }
    if (!web_control_take(portMAX_DELAY)) {
        return;
    }
    bool ready = web_actuation_ready();
    esp_err_t sound_err;
    esp_err_t audio_err;
    if (ready) {
        /* Un-inhibit attempts happen before the final state update; the
         * retries are bounded to two 10 ms waits. */
        sound_err = set_inhibited_retry(sound_set_inhibited, false);
        audio_err = set_inhibited_retry(audio_set_inhibited, false);
    } else {
        sound_err = sound_set_inhibited(true);
        audio_err = audio_set_inhibited(true);
    }
    if (sound_err == ESP_ERR_INVALID_STATE) sound_err = ESP_OK;
    if (audio_err == ESP_ERR_INVALID_STATE) audio_err = ESP_OK;
    if (ready && (sound_err != ESP_OK || audio_err != ESP_OK)) {
        ready = false;
        (void)sound_set_inhibited(true);
        (void)audio_set_inhibited(true);
    }
    (void)motor_set_inhibited(!ready);
    portENTER_CRITICAL(&s_control_state_mux);
    s_actuation_ready = ready;
    s_maintenance_owner = NULL;
    s_maintenance_exclusive_fs = false;
    portEXIT_CRITICAL(&s_control_state_mux);
    ++s_control_generation;
    web_control_give();
}

void web_maintenance_end(void)
{
    maintenance_end_impl();
}
