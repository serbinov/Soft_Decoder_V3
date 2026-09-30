idf_component_register(SRCS "src/auxio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_ledc esp_driver_mcpwm pinmap)
