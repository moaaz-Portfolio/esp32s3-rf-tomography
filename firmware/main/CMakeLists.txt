idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_timer esp_http_server nvs_flash lwip)
