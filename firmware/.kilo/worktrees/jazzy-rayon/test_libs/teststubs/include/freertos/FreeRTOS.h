#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

typedef void *QueueHandle_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;

#define pdTRUE   1
#define pdFALSE  0
#define pdPASS   1
#define pdFAIL   0

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(ticks) ((uint32_t)(ticks))
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFUL)
#define portYIELD_FROM_ISR(...) do { } while (0)
#define IRAM_ATTR
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux)  do { (void)(mux); } while (0)

QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize);
BaseType_t xQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait);
BaseType_t xQueueSendFromISR(QueueHandle_t xQueue, const void *pvItemToQueue, BaseType_t *pxHigherPriorityTaskWoken);
BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);
void vQueueDelete(QueueHandle_t xQueue);

BaseType_t xTaskCreate(void (*pxTaskCode)(void *), const char *pcName,
                       uint32_t usStackDepth, void *pvParameters,
                       UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask);
BaseType_t xTaskCreatePinnedToCore(void (*pxTaskCode)(void *), const char *pcName,
                                   uint32_t usStackDepth, void *pvParameters,
                                   UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask,
                                   BaseType_t xCoreID);
void vTaskDelay(const TickType_t xTicksToDelay);
void vTaskDelete(TaskHandle_t xTaskToDelete);
TickType_t xTaskGetTickCount(void);
uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore);

#endif /* MOCK_FREERTOS_H */
