idf_component_register(SRCS "main.c" "ota_gateway.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES aura_mesh aura_ota esp_wifi esp_netif esp_event esp_timer nvs_flash
                                     app_update esp_partition esp_http_client esp_app_format
                                     bootloader_support mbedtls)
