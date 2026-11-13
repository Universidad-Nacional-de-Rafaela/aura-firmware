# Cambio de clave de firma en placas de producción

Análisis y propuesta del grupo de E1-PB-LECA-TAB01, pendiente de decisión de la cátedra. 13/11/2026.

Las tres placas instaladas el 11/11 (el gateway, el concentrador y la hoja E1-PB-LECA-TAB01) quedaron firmadas con la clave de prueba del grupo, y tienen que pasar a la clave de la cátedra. Hay dos caminos: grabarlas por USB, o actualizarlas por la malla con una imagen de transición que lleve las dos firmas. Cuál conviene depende del certificado del servidor HTTPS de la cátedra.

## La situación

- Las tres placas se grabaron por USB el 11/11, desde `main`, con imágenes firmadas con la clave de prueba del grupo.
- Una placa solo acepta actualizaciones firmadas con la clave de la imagen que está corriendo. Hoy, una imagen firmada por la cátedra se rechaza con `imagen_invalida`, igual que en la prueba P7 del banco (imagen firmada con otra clave).
- El gateway quedó con `OTA_PERMITIR_HTTP 0` y sin `OTA_CA_PEM`. En `Mesh3_Gateway/main/ota_gateway.c`, sin `OTA_CA_PEM` el gateway verifica al servidor contra las CA públicas que trae ESP-IDF: solo descarga por HTTPS desde servidores con un certificado de una CA pública.
- Según el PR #66 (13/11), el grupo entrega el código por PR y, después del merge, la cátedra compila desde `main` con las credenciales de la instalación y firma esa imagen, hoy desde la notebook del profe. No firma `.bin` compilados por otros, y la imagen firmada no se publica.

## Por qué no conviene llevar la clave de prueba al servidor

Técnicamente alcanzaría: la clave es un archivo, y las placas aceptan cualquier imagen firmada con ella, sin importar en qué máquina se firmó. No conviene, por tres razones:

1. **Una clave de producción tiene que haber estado siempre en la máquina autorizada.** Esta estuvo en la notebook de un alumno y en una copia de respaldo. No hay forma de demostrar que no quedó otra copia, y quien la tenga puede firmar firmware que esas placas instalan.
2. **Quedarían dos claves en producción:** la de la cátedra para el resto de las placas, y la del grupo para estas tres. El servidor tendría que saber con qué clave firmar para cada placa, y se rompe la regla de un tipo, un binario (`GUIA_NODOS_HOJA.md`, 2.7). Adoptar la clave del grupo para todo evita eso, pero traslada el problema 1 a todas las placas.
3. **Sienta un precedente** para que cualquier grupo pida lo mismo con su clave de prueba.

## Camino A: grabar por USB

En la máquina de la cátedra:

1. Compilar el gateway, el concentrador y la hoja desde `main`, con los archivos de configuración de la red final.
2. Firmar las tres imágenes con la clave de la cátedra, y verificarlas.
3. Grabar cada placa por USB. Sin `erase-flash`, conservan lo que tienen en la NVS: el aula de la hoja y la autorización del concentrador. Con `erase-flash` arrancan limpias, y hay que volver a asignarlas y autorizarlas en AURA.
4. Comprobar que vuelvan a la malla y que AURA confirme las mediciones.

Para la hoja hay que abrir el tablero y desconectar su fuente mientras está por USB.

## Camino B: actualizar por la malla con una imagen de dos firmas

ESP-IDF admite hasta tres firmas por imagen en el esquema de firma que usa el proyecto (Secure Boot v2, RSA-3072). Eso permite cambiar de clave sin USB:

1. **Primera OTA, con una imagen de transición** firmada con las dos claves. La placa la acepta por la firma del grupo, y queda corriendo una imagen que lleva las dos.
2. **Segunda OTA, firmada solo por la cátedra.** La placa la acepta porque la imagen que está corriendo ya trae la clave de la cátedra.
3. **Desde ahí, la clave del grupo deja de valer,** porque la imagen instalada ya no la lleva.

La imagen de transición se arma respetando el #66. La compila la cátedra desde `main`, y no sale de su máquina, porque lleva las credenciales. La firma del grupo se agrega ahí mismo: alguien del grupo trae su clave en un pendrive, firma esa imagen en la máquina de la cátedra, y se lleva la clave sin dejar copia. Después, la cátedra le agrega su firma con la opción de `espsecure sign-data` que agrega una firma a una imagen ya firmada (`--append-signatures`; conviene confirmar el nombre con `espsecure sign-data --help`).

Que la clave del grupo pase por esa máquina no deja un riesgo permanente, porque en el paso 3 deja de valer. `firmar.py` del PR #47 no sirve para agregar la segunda firma: se niega a firmar una imagen que ya tiene una.

Requisitos:

- **Subir la versión en cada OTA.** Con la misma versión, la placa contesta `aplicado` sin instalar nada.
- **Probarlo antes en el banco, con una placa de prueba.** Falta confirmar que esta versión de ESP-IDF, en el modo sin Secure Boot (`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`), toma como confiables las claves de todas las firmas de la imagen que está corriendo, y no solo la de la primera. Si toma solo la primera, el camino funciona igual, pero con una OTA más en el medio y el orden de las firmas invertido.

## Lo que decide el camino: el certificado del servidor HTTPS

El concentrador y la hoja no descargan nada por HTTPS: el gateway descarga la imagen y se la pasa por la malla, y ellos solo verifican la firma. La condición está en el gateway.

| Certificado del servidor | Gateway | Concentrador y hoja |
| --- | --- | --- |
| De una CA pública (por ejemplo, Let's Encrypt, con nombre de dominio) | Nada que cambiar: ya confía en las CA públicas. Camino B | Camino B |
| De una CA propia de la cátedra | Camino A, una vez, con `OTA_CA_PEM` = la CA de la cátedra (convertida con `pem_a_define.py`) y la clave de la cátedra | Camino B: la hoja no se toca |

Con una CA pública, la URL tiene que usar el mismo nombre que figura en el certificado, y el gateway tiene que poder resolver ese nombre y llegar al servidor desde la red del router. Con una CA propia, el certificado del servidor tiene que estar emitido para la IP o el nombre que usa la URL: en el banco, un certificado para otra IP se rechazó igual que uno de otra CA (pruebas P14 y P15, en `dispositivos/E1-PB-LECA-TAB01/pruebas/2026-11-06-ota-https/`).

Para el gateway no hay atajo con una CA propia: no puede descargar la imagen que le enseña a confiar en esa CA desde un servidor en el que todavía no confía.

**Para saber cuál es el caso:** abrir la dirección del servidor en un navegador. Si carga sin advertencias, el certificado es de una CA pública; si avisa que no es de confianza, es de una CA propia.

## Las imágenes llevan credenciales compiladas

La imagen del gateway lleva el WiFi y la clave MQTT; las del concentrador y la hoja, el ID y la clave de la mesh. Sin el cifrado de la flash, que llega con la etapa 2, esos datos se pueden leer en el `.bin`.

Por eso, las imágenes de producción no se suben a un servidor público, ni a releases de GitHub, ni a ningún servicio externo, aunque eso le resolviera al gateway la confianza en el certificado. El servidor de imágenes tiene que ser accesible solo desde la red de la instalación.

## Después de migrar

- El grupo borra de su notebook los archivos con datos de producción: el `credenciales.h` del gateway y los `config_local.h` con los datos de la red final.
- La clave de prueba del grupo queda solo para el banco.

## Pendiente de decidir o de probar

- [ ] Qué certificado tiene el servidor HTTPS de la cátedra: de una CA pública o de una propia.
- [ ] Probar en el banco la imagen de dos firmas, y confirmar si la placa confía en las claves de todas las firmas o solo en la de la primera.
- [ ] Qué camino se usa para las tres placas.
