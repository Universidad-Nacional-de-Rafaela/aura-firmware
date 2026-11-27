# En modo independiente (CONFIG_PUERTA_INDEPENDIENTE) arranca otro main, sin mesh ni NVS. Los
# componentes requeridos son los mismos en los dos casos (no pueden depender de la configuracion);
# el enlazador deja afuera lo que no se llama.
if(CONFIG_PUERTA_INDEPENDIENTE)
    set(fuentes "main_independiente.c")
else()
    set(fuentes "main.c" "almacen.c")
endif()

idf_component_register(SRCS ${fuentes} "rele.c" "sonido.c" "lector.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nodo_mesh nvs_flash esp_system esp_driver_gpio esp_driver_ledc
                                     esp_driver_spi esp_driver_usb_serial_jtag esp_timer esp_event
                                     abobija__rc522)

if(CONFIG_PUERTA_INDEPENDIENTE)
    # La lista de llaveros de esta puerta (no se versiona). Si falta, falla la compilacion
    # (main_independiente.c), no la configuracion: asi menuconfig sigue abriendo para corregirlo.
    get_filename_component(llaveros "${COMPONENT_DIR}/../llaveros/${CONFIG_PUERTA_CODIGO}.h" ABSOLUTE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "PUERTA_LLAVEROS_H=\"${llaveros}\"")
endif()
