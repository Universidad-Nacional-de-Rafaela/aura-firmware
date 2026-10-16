#!/bin/bash
# Herramientas para el broker Mosquitto de AURA en Docker.
# Los clientes MQTT vienen dentro de la imagen eclipse-mosquitto:2, asi que
# no hace falta instalar mosquitto-clients en la notebook.
#
# El broker de aura-app no acepta anonimos: para mirar se entra como "monitor" y para
# publicar (comando, limpiar) como "aura-backend", con las claves que el contenedor ya
# tiene en su entorno (MQTT_*_PASSWORD del .env de aura-app).
#
# Uso: ./broker.sh <comando>        (otro contenedor: AURA_BROKER=nombre ./broker.sh ...)

C=${AURA_BROKER:-aura-mosquitto-1}

# mosquitto_sub/pub dentro del contenedor, con usuario. El primer argumento es -it o -i.
sub() { t=$1; shift; docker exec "$t" $C sh -c 'exec mosquitto_sub -h localhost -u monitor -P "$MQTT_MONITOR_PASSWORD" "$@"' sh "$@"; }
pub() { docker exec -i $C sh -c 'exec mosquitto_pub -h localhost -u aura-backend -P "$MQTT_BACKEND_PASSWORD" "$@"' sh "$@"; }

case "$1" in
  ver)        # todo lo que pasa por el broker, en vivo
    sub -it -t '#' -v ;;

  datos)      # telemetria de la mesh (raiz) y de LoRaWAN (bridge), por hw_id
    sub -it -t 'hw/+/data' -v ;;

  acks)       # lo que AURA (o herramientas/ack_falso.py) confirma
    sub -it -t 'hw/+/ack' -v ;;

  estado)     # solo los status de los nodos
    sub -it -t 'hw/+/status' -v ;;

  respuestas) # transmitido / recibido / aplicado / rechazado de cada comando
    sub -it -t 'hw/+/response' -v ;;

  alertas)    # hw/<hw_id>/alerts/<tipo>
    sub -it -t 'hw/+/alerts/#' -v ;;

  retenidos)  # que quedo pegado en el broker
    echo "Mensajes retenidos:"
    sub -i -t '#' -v --retained-only -W 3 2>/dev/null | sort -u
    echo "(vacio = ninguno)" ;;

  limpiar)    # borra los retenidos: payload vacio con retain
    echo "Borrando mensajes retenidos..."
    sub -i -t '#' --retained-only -W 3 -F '%t' 2>/dev/null | sort -u | while read -r t; do
      [ -n "$t" ] && pub -t "$t" -r -n </dev/null && echo "  borrado: $t"
    done
    echo "Listo. OJO: un nodo vivo va a volver a publicar el suyo enseguida." ;;

  comando)    # baja un comando:  ./broker.sh comando mac-e072a1f7efe4 '{"command":"set_config","params":{"intervalo_s":30},"command_id":"c-1"}'
    [ -z "$2" ] || [ -z "$3" ] && { echo "uso: $0 comando <hw_id> '<json>'   (hw_id: mac-<12 hex> de la placa)"; exit 1; }
    pub -t "hw/$2/command" -q 1 -m "$3"
    echo "enviado a hw/$2/command" ;;

  log)        # log interno del broker
    docker logs --tail 40 $C 2>&1 ;;

  problemas)  # solo lo que importa cuando algo anda mal
    echo "=== desconexiones sucias y errores ==="
    docker logs $C 2>&1 | grep -E 'exceeded timeout|socket error|error|not authori' | tail -15
    echo
    echo "=== ultimas conexiones ==="
    docker logs $C 2>&1 | grep 'New client connected' | tail -8 ;;

  reiniciar)
    docker restart $C && echo "broker reiniciado" ;;

  *)
    echo "uso: $0 {ver|datos|acks|estado|respuestas|alertas|retenidos|limpiar|comando|log|problemas|reiniciar}"
    exit 1 ;;
esac
