#pragma once

// Copie este arquivo para "secrets.h" (mesma pasta) e preencha com os valores
// reais. O "secrets.h" esta listado no .gitignore e nunca deve ser versionado.

#define WIFI_SSID   "nome-da-rede-2.4GHz"
#define WIFI_PASS   "senha-da-rede"

// Broker Mosquitto N3 (CONTRATOS.md v1):
// - Producao (qualquer rede): "wss://accessgrupo5.duckdns.org/mqtt" (porta 443, TLS)
// - Docker Compose local:     "mqtt://<IP_DO_NOTEBOOK>:1883"
#define MQTT_URI    "wss://accessgrupo5.duckdns.org/mqtt"
#define MQTT_USER   "porta-01"
#define MQTT_PASS   "senha-do-porta-01"
