idf_component_register(SRCS "src/storage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES spi_flash esp_partition esp_littlefs vfs driver nvs_flash pinmap esp_timer)
