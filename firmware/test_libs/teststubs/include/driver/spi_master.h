#pragma once

#include "esp_err.h"

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

typedef int spi_host_device_t;

#define SPI2_HOST       1
#define SPI_DMA_CH_AUTO 0

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *bus_config, int dma_chan);
