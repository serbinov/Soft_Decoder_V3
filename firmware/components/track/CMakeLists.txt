idf_component_register(SRCS "src/track.c" "src/track_recover.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio settings motor web dcc)
