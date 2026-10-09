idf_component_register(SRCS "src/audio.c" "src/ima_adpcm.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_driver_i2s pinmap esp_timer storage)
