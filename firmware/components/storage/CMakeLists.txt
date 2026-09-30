idf_component_register(SRCS "src/storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES spi_flash esp_partition esp_littlefs vfs esp_driver_spi nvs_flash pinmap esp_timer)
