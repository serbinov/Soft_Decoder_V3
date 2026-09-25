idf_component_register(SRCS "src/selftest.c"
                       INCLUDE_DIRS "include"
                       REQUIRES pinmap settings storage motor audio auxio dcc web nvs_flash esp_system)
