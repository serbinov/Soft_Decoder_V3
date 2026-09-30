#ifndef MOCK_ESP_EVENT_H
#define MOCK_ESP_EVENT_H

/* Host shim for the ESP-IDF event loop (only what components/web needs). */

#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;

#define WIFI_EVENT ((esp_event_base_t)"WIFI_EVENT")
#define IP_EVENT   ((esp_event_base_t)"IP_EVENT")

#define ESP_EVENT_ANY_ID (-1)

#define WIFI_EVENT_AP_START             1
#define WIFI_EVENT_AP_STOP              2
#define WIFI_EVENT_AP_STACONNECTED      3
#define WIFI_EVENT_AP_STADISCONNECTED   4
#define IP_EVENT_ASSIGNED_IP_TO_CLIENT  5

typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);
typedef void *esp_event_handler_instance_t;

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void *arg,
                                              esp_event_handler_instance_t *instance);

#endif /* MOCK_ESP_EVENT_H */
