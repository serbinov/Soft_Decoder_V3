idf_component_register(SRCS "src/provision.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer esp_system app_update settings storage motor)
