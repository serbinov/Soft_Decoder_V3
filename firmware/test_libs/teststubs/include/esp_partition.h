#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_flash.h"

typedef struct esp_partition_t esp_partition_t;

struct esp_partition_t {
    esp_flash_t *flash;
    uint32_t address;
    uint32_t size;
    uint8_t type;
    uint8_t subtype;
    char label[17];
};

#define ESP_PARTITION_TYPE_DATA             1
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS 0x83

esp_err_t esp_partition_register_external(esp_flash_t *flash_chip, size_t offset, size_t size,
                                          const char *label, uint8_t type, uint8_t subtype,
                                          const esp_partition_t **out_partition);
