idf_component_register(SRCS "src/sound_store.c" "src/sound.c"
                       INCLUDE_DIRS "include"
                       REQUIRES settings storage audio motor esp_timer)
