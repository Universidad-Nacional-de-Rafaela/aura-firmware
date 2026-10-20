idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES aura_mesh esp_wifi esp_netif esp_event esp_timer nvs_flash)
