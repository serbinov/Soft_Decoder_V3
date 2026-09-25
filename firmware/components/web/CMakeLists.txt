idf_component_register(SRCS "src/web.c" "src/web_util.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_wifi esp_netif esp_event esp_timer app_update
                                 nvs_flash driver audio auxio motor settings storage pinmap dcc)
