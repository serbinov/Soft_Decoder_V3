idf_component_register(SRCS "src/motor.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_ledc esp_hal_ledc esp_driver_gpio esp_adc esp_timer pinmap settings)
