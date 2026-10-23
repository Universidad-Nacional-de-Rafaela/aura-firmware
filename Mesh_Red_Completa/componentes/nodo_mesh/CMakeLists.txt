# Biblioteca de la hoja de la mesh de AURA
idf_component_register(SRCS "nodo_mesh.c" "cola.c"
                       INCLUDE_DIRS "include"
                       REQUIRES aura_mesh aura_ota esp_wifi
                       PRIV_REQUIRES esp_netif esp_event nvs_flash esp_timer heap esp_partition esp_rom)
