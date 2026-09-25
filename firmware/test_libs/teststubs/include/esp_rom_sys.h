#ifndef MOCK_ESP_ROM_SYS_H
#define MOCK_ESP_ROM_SYS_H

#include <stdint.h>

extern uint32_t mock_rom_delay_us_total;

void esp_rom_delay_us(uint32_t us);

#endif /* MOCK_ESP_ROM_SYS_H */
