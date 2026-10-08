#pragma once
#include "almacenamiento.h"
#include "adaptador_aura.h"
#include "pantalla.h"

bool aplicar_config(JsonObjectConst params,String& motivo) {
  MeshConfig next; const char* error;
  if(!storageOk) { motivo="nvs_no_disponible"; return false; }
  if(!leerConfig(params,journal.config,next,error)) { motivo=error; return false; }
  MeshConfig old=journal.config; journal.config=next;
  if(!commit()) { journal.config=old; motivo="fallo_nvs"; return false; }
  snapshotPending=true; updateOutputs(); return true;
}
void describir_config(JsonObject out) { describirConfig(journal.config,out); }

void beginNetwork() {
  // La catedra mantiene esta biblioteca. No se modifica comun/ en el PR del nodo.
  NodoMeshCallbacks cb={aplicar_config,describir_config,nullptr,
    ICIV_POWER_PIN<0?"desconocida":journal.powerKnown?(battery?"bateria":"red"):"desconocida"};
  radioOk=nodo_mesh_iniciar(cb);
  if(!nodo_mesh.configurada) {
    uint8_t mac[6]; esp_read_mac(mac,ESP_MAC_WIFI_STA);
    Serial.printf("[IDENTIDAD] hw_id mac-%02x%02x%02x%02x%02x%02x\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  }
  linkState=radioOk&&nodo_mesh.configurada?UNKNOWN:GATEWAY_DOWN;
}

// El adaptador conserva el mensaje completo en NVS y entrega uno por vez a la
// cola oficial. Esto permite agrupar sondas, preservar ingest_id/RTC y proteger
// la primera captura del corte sin cambiar la biblioteca compartida.
// Usa los structs expuestos por nodo_mesh.h: revisar si la catedra cambia esa API.
bool stageFlight() {
  if(!nodo_mesh.cola_ok || nodo_mesh_pendientes()) return true;
  if(!flight.activo) {
    Muestra next;
    if(!nextSample(journal,next)) return true;
    if(next.kind!=MEDICION) {
      // Eventos guardados con el firmware anterior: no se publican como values.
      JsonDocument details;
      const char* tipo=next.kind==ALERTA_ENERGIA?"energia":"sensor";
      if(next.kind==ALERTA_ENERGIA) details["alimentacion"]=next.state?"bateria":"red";
      else {
        details["campo"]=next.sensor?"temp_freezer_c":"temp_heladera_c";
        details["motivo"]=next.state==SONDA_OK?"recuperada":next.state==FUERA_RANGO?"fuera_de_rango":"sin_respuesta";
      }
      nodo_mesh_alerta(tipo,next.state==SONDA_OK?"info":"warning","Evento del firmware anterior",details.as<JsonObjectConst>());
      acknowledge(journal,next.id); snapshotPending=true; return commit();
    }
    EnvioProtegido nextFlight;
    if(!prepararEnvio(journal,nextFlight) || !flightStore.save(nextFlight)) return false;
    flight=nextFlight;
  }
  return cola_push(&nodo_mesh.cola,&flight.muestra);
}

void reportState(uint32_t now) {
  uint32_t every=journal.config.sensors[0].intervalS*1000UL;
  if(!snapshotPending && uint32_t(now-reportAt)<every) return;
  if(!nodo_mesh.configurada || !radio_conectada()) return;
  JsonDocument d; describir_config(d["config"].to<JsonObject>());
  uint32_t pending=pendingMeasurements(journal,flight);
  if(!flight.activo) pending+=nodo_mesh_pendientes();
  // La cola oficial es una copia del envio en vuelo, no otra lectura pendiente.
  d["pendientes"]=pending;
  uint64_t dropped=uint64_t(journal.dropped[0])+journal.dropped[1];
  d["descartadas"]=uint32_t(dropped>UINT32_MAX?UINT32_MAX:dropped);
  d["alimentacion"]=ICIV_POWER_PIN<0?"desconocida":journal.powerKnown?(battery?"bateria":"red"):"desconocida";
  if(nodo_mesh_encolar_evento(AURA_TIPO_REPORTE,d)) { snapshotPending=false; reportAt=now; }
}

void serviceNetwork(uint32_t now) {
  if(!radioOk) return;
  if(!storageOk) {
    // Aun con un fallo de flash se escuchan comandos y se responde con rechazo.
    nodo_mesh_procesar_recibidas();
    if(nodo_mesh.configurada && radio_conectada()) nodo_mesh_enviar_evento();
    updateOutputs(); return;
  }
  if(!stageFlight()) { storageOk=false; updateOutputs(); return; }
  uint32_t recovery=journal.config.recoveryS*1000UL;
  delivery.revisar(now,aura_espera_confirmacion_ms(nodo_mesh.intentos),recovery);
  MuestraAura before={}; bool hadHead=nodo_mesh.cola_ok && cola_peek(&nodo_mesh.cola,&before);
  uint32_t confirmed=nodo_mesh.confirmadas;
  uint32_t sentAt=nodo_mesh.enviada_en;
  bool wasInFlight=nodo_mesh.en_vuelo;
  uint32_t attempts=nodo_mesh.intentos;
  // La mesh sigue escuchando y reconectando. Se limita solo el envio de datos.
  if(delivery.lenta && nodo_mesh_pendientes()) {
    nodo_mesh.proximo_envio=delivery.proximo;
    if(delivery.corresponde(now)) { nodo_mesh.en_vuelo=false; nodo_mesh.intentos=0; }
  }
  uint32_t scheduled=nodo_mesh.proximo_envio;
  nodo_mesh_loop();
  if(nodo_mesh.confirmadas!=confirmed) {
    if(hadHead && confirmarEnvio(journal,flight,before.ingest_id)) {
      if(!commit() || !flightStore.clear(flight)) { storageOk=false; updateOutputs(); return; }
    }
    delivery.confirmar(); linkState=ONLINE; snapshotPending=true;
  } else {
    bool sent=nodo_mesh.en_vuelo && (!wasInFlight || nodo_mesh.enviada_en!=sentAt || nodo_mesh.intentos!=attempts);
    bool failed=hadHead && !nodo_mesh.en_vuelo && nodo_mesh.proximo_envio!=scheduled;
    if(sent || failed) delivery.intento(now,sent,recovery);
    if(!nodo_mesh.configurada || !radio_conectada()) linkState=GATEWAY_DOWN;
    else if(delivery.lenta) linkState=CENTRAL_DOWN;
    else if(linkState==GATEWAY_DOWN) linkState=UNKNOWN;
  }
  reportState(now); updateOutputs();
}
