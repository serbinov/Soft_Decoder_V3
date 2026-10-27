/* Cached settings_config_t and its mutex. Callers use web_cfg_read() for a
 * consistent copy or hold web_cfg_lock() around read-modify-save. */

#include "web_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "settings.h"

static settings_config_t s_cfg;
static SemaphoreHandle_t s_cfg_mutex;

esp_err_t web_cfg_init(void)
{
    if (s_cfg_mutex == NULL) s_cfg_mutex = xSemaphoreCreateMutex();
    return s_cfg_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

bool web_cfg_lock(TickType_t wait)
{
    return s_cfg_mutex == NULL || xSemaphoreTake(s_cfg_mutex, wait) == pdTRUE;
}

void web_cfg_unlock(void)
{
    if (s_cfg_mutex != NULL) xSemaphoreGive(s_cfg_mutex);
}

/* Copy the cached config without ever holding the lock across I/O. */
bool web_cfg_read(settings_config_t *out)
{
    if (out == NULL || !web_cfg_lock(pdMS_TO_TICKS(100))) return false;
    *out = s_cfg;
    web_cfg_unlock();
    return true;
}

/* Persist `next` and publish it as the new cache. The config lock is held
 * across the save so concurrent read-modify-save handlers cannot overwrite
 * each other with a stale snapshot. */
esp_err_t web_cfg_save(const settings_config_t *next, bool deferred)
{
    if (next == NULL) return ESP_ERR_INVALID_ARG;
    if (!web_cfg_lock(pdMS_TO_TICKS(100))) return ESP_ERR_TIMEOUT;
    esp_err_t err = deferred ? settings_save_deferred(next) : settings_save(next);
    if (err == ESP_OK) s_cfg = *next;
    web_cfg_unlock();
    return err;
}

settings_config_t *web_cfg(void)
{
    return &s_cfg;
}
