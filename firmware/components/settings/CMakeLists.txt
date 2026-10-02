idf_component_register(SRCS "src/settings.c" "src/track_manifest.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash esp_timer storage)
