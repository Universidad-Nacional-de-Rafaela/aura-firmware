#!/bin/bash
# Herramientas para el broker Mosquitto de AURA en Docker.
# Los clientes MQTT vienen dentro de la imagen eclipse-mosquitto:2, asi que
# no hace falta instalar mosquitto-clients en la notebook.
#
# Uso: ./broker.sh <comando>

C=aura-mosquitto

case "$1" in
  ver)        # todo lo que pasa por el broker, en vivo
    docker exec -it $C mosquitto_sub -h localhost -t '#' -v ;;

  datos)      # solo telemetria
    docker exec -it $C mosquitto_sub -h localhost -t 'devices/+/data' -v ;;

  estado)     # solo los status de los nodos
    docker exec -it $C mosquitto_sub -h localhost -t 'devices/+/status' -v ;;

  retenidos)  # que quedo pegado en el broker
    echo "Mensajes retenidos:"
    timeout 5 docker exec $C mosquitto_sub -h localhost -t '#' -v --retained-only -W 3 2>/dev/null | sort -u
    echo "(vacio = ninguno)" ;;

  limpiar)    # borra los retenidos: payload vacio con retain
    echo "Borrando mensajes retenidos..."
    timeout 5 docker exec $C mosquitto_sub -h localhost -t '#' --retained-only -W 3 -F '%t' 2>/dev/null | sort -u | while read -r t; do
      [ -n "$t" ] && docker exec $C mosquitto_pub -h localhost -t "$t" -r -n && echo "  borrado: $t"
    done
    echo "Listo. OJO: un nodo vivo va a volver a publicar el suyo enseguida." ;;

  comando)    # baja un comando a la mesh:  ./broker.sh comando <uuid> '<json>'
    [ -z "$2" ] || [ -z "$3" ] && { echo "uso: $0 comando <device-uuid> '<json>'"; exit 1; }
    docker exec $C mosquitto_pub -h localhost -t "devices/$2/command" -m "$3"
    echo "enviado a devices/$2/command" ;;

  log)        # log interno del broker
    docker exec $C sh -c 'tail -40 /mosquitto/log/mosquitto.log' ;;

  problemas)  # solo lo que importa cuando algo anda mal
    echo "=== desconexiones sucias y errores ==="
    docker exec $C sh -c "grep -E 'exceeded timeout|socket error|error' /mosquitto/log/mosquitto.log | tail -15"
    echo
    echo "=== ultimas conexiones ==="
    docker exec $C sh -c "grep 'New client connected' /mosquitto/log/mosquitto.log | tail -8" ;;

  reiniciar)
    docker restart $C && echo "broker reiniciado" ;;

  *)
    echo "uso: $0 {ver|datos|estado|retenidos|limpiar|comando|log|problemas|reiniciar}"
    exit 1 ;;
esac
