#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct esp_flash_t esp_flash_t;

esp_err_t esp_flash_init(esp_flash_t *chip);
esp_err_t esp_flash_get_size(esp_flash_t *chip, uint32_t *out_size);
esp_err_t esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t esp_flash_erase_region(esp_flash_t *chip, uint32_t start_address, uint32_t size);
