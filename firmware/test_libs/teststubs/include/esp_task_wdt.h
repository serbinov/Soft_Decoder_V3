#ifndef MOCK_ESP_TASK_WDT_H
#define MOCK_ESP_TASK_WDT_H
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
esp_err_t esp_task_wdt_add(TaskHandle_t task);
esp_err_t esp_task_wdt_reset(void);
esp_err_t esp_task_wdt_delete(TaskHandle_t task);
extern int mock_task_wdt_add_err;
extern int mock_task_wdt_reset_calls;
#endif
