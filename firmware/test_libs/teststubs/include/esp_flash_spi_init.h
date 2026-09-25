#pragma once

#include "esp_err.h"
#include "esp_flash.h"
#include "driver/spi_master.h"

typedef struct {
    int host_id;
    int cs_io_num;
    int io_mode;
    int freq_mhz;
    int input_delay_ns;
} esp_flash_spi_device_config_t;

#define SPI_FLASH_FASTRD 0

esp_err_t spi_bus_add_flash_device(esp_flash_t **out_chip,
                                   const esp_flash_spi_device_config_t *config);
