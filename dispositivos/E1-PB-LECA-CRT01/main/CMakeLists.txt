idf_component_register(SRCS "main.c" "cortina.c" "luz.c" "banco.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nodo_mesh nvs_flash esp_driver_gpio esp_driver_i2c
                                     esp_driver_usb_serial_jtag esp_rom esp_timer)
