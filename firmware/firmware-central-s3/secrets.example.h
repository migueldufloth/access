#pragma once

// Copie este arquivo para "secrets.h" (mesma pasta) e preencha com os valores
// reais. O "secrets.h" esta listado no .gitignore e nunca deve ser versionado.

#define WIFI_SSID   "nome-da-rede-2.4GHz"
#define WIFI_PASS   "senha-da-rede"

// Broker Mosquitto configurado neste repositorio:
// - Acesso externo (internet / hotspot / faculdade):
//     "wss://accessgrupo5.duckdns.org/mqtt"  (porta 443, WebSocket + TLS)
// - Acesso na mesma LAN ou Tailscale do servidor do broker:
//     "mqtt://<IP_DO_SERVIDOR>:1883"
#define MQTT_URI    "wss://accessgrupo5.duckdns.org/mqtt"
#define MQTT_USER   "access_esp32"
#define MQTT_PASS   "senha-do-broker"

// Topico HTTP do servico ntfy.sh para notificacao push
#define NTFY_TOPIC  "AccessPortaTeste"
