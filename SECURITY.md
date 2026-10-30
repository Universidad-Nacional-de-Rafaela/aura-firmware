# Seguridad

## Cómo reportar un problema

Si encontrás una vulnerabilidad en el firmware o una credencial publicada en este repositorio,
**no abras un issue público**. Reportalo en privado:

- desde la pestaña **Security** del repositorio, con **Report a vulnerability**, o
- por mail a la cátedra: matias.wanzenried@unraf.edu.ar.

Contá qué encontraste, dónde (archivo, commit o dispositivo) y cómo reproducirlo. Si es una
credencial, no la copies en el reporte: alcanza con decir en qué archivo y commit está.

Es un proyecto académico: respondemos lo antes posible, sin un plazo garantizado.

## Qué cubre

El firmware de los dispositivos de AURA (`comun/`, `infraestructura/`, `Mesh_Red_Completa/`,
`dispositivos/`, `conectores/`) y las herramientas de este repositorio. La plataforma AURA vive en
otro repositorio; si el problema es de ella, reportalo igual por acá y lo derivamos.

## Credenciales

Las claves de WiFi, de la mesh, de LoRaWAN y de MQTT van en `config_local.h`, que no se versiona.
El CI rechaza cualquier PR que las incluya y GitHub bloquea el push si detecta una clave conocida.
Si una se filtra, borrarla en un commit nuevo **no alcanza**, porque queda en la historia: hay que
cambiarla.
