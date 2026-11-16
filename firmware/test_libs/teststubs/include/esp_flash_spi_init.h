#pragma once

#include "esp_err.h"
#include "esp_flash.h"
#include "driver/spi_master.h"

typedef int esp_flash_io_mode_t;

typedef struct {
    int host_id;
    int cs_io_num;
    esp_flash_io_mode_t io_mode;
    int freq_mhz;
    int input_delay_ns;
} esp_flash_spi_device_config_t;

#define SPI_FLASH_FASTRD 0
#define SPI_FLASH_SLOWRD 1

esp_err_t spi_bus_add_flash_device(esp_flash_t **out_chip,
                                   const esp_flash_spi_device_config_t *config);
esp_err_t spi_bus_remove_flash_device(esp_flash_t *chip);
