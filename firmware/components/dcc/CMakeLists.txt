idf_component_register(SRCS "src/dcc.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_timer pinmap)
