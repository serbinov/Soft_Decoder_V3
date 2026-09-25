#ifndef MOCK_ESP_LOG_H
#define MOCK_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGI(tag, ...)  do { printf("[%s] ", tag); printf(__VA_ARGS__); printf("\n"); } while (0)
#define ESP_LOGW(tag, ...)  ESP_LOGI(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...)  ESP_LOGI(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...)  ESP_LOGI(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...)  ESP_LOGI(tag, __VA_ARGS__)

#endif /* MOCK_ESP_LOG_H */
