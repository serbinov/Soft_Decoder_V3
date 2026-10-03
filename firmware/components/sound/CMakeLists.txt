idf_component_register(SRCS "src/sound_store.c" "src/sound.c"
                           "src/sound_graph.c" "src/sound_graph_runner.c" "src/sound_graph_store.c"
                       INCLUDE_DIRS "include"
                       REQUIRES settings storage audio motor esp_timer)
