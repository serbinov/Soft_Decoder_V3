#ifndef MOCK_FREERTOS_IDF_ADDITIONS_H
#define MOCK_FREERTOS_IDF_ADDITIONS_H
#include "FreeRTOS.h"
#include "esp_heap_caps.h"
QueueHandle_t xQueueCreateWithCaps(UBaseType_t length, UBaseType_t item_size, UBaseType_t caps);
void vQueueDeleteWithCaps(QueueHandle_t queue);
#endif
