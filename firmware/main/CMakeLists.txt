idf_component_register(SRCS "app_main.c" "default_scheme_data.c"
                       INCLUDE_DIRS "."
                       REQUIRES pinmap storage settings dcc motor auxio audio esp_driver_gpio web track provision sound
                       EMBED_FILES "data/default_scheme.json")
