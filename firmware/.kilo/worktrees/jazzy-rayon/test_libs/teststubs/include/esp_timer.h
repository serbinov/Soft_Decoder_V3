#ifndef MOCK_ESP_TIMER_H
#define MOCK_ESP_TIMER_H

#include <stdint.h>

extern int64_t mock_timer_now_us;
int64_t esp_timer_get_time(void);

#endif /* MOCK_ESP_TIMER_H */
