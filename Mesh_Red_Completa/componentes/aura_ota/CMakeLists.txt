# Actualización por la malla (receptor de hoja y concentrador) y tipo y versión del firmware
idf_component_register(SRCS "fw.c" "receptor.c"
                       INCLUDE_DIRS "include"
                       REQUIRES aura_mesh
                       PRIV_REQUIRES app_update esp_app_format bootloader_support esp_timer esp_wifi mbedtls)
