#ifndef MOCK_FREERTOS_TASK_H
#define MOCK_FREERTOS_TASK_H

#include "FreeRTOS.h"

void vTaskDelayUntil(TickType_t *pxPreviousWakeTime, TickType_t xTimeIncrement);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
extern TaskHandle_t mock_current_task_handle;
extern void (*mock_task_startup_hook)(void (*task)(void *), void *param);
extern void (*mock_task_create_hook)(void (*task)(void *), void *param);

#endif /* MOCK_FREERTOS_TASK_H */
