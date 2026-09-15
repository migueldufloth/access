# Broker MQTT

Config do broker Mosquitto e do proxy nginx que atendem o tópico `access/grupo5/#`
(ver [Arquitetura do Sistema e Comunicação MQTT](../README.md#arquitetura-do-sistema-e-comunicação-mqtt)
no README principal). Roda direto no host (sem container), em `accessgrupo5.duckdns.org`.

## Estrutura
- `mosquitto/access.conf` — listeners (`1883` MQTT puro, `9001` WebSocket em loopback), autenticação
  obrigatória, path da ACL.
- `mosquitto/acl` — permissões por usuário (`access_esp32` publica telemetria/status e lê comando;
  `access_web` é o inverso).
- `nginx/access.conf` — vhost com TLS (Let's Encrypt), redirect HTTP→HTTPS, proxy de `/mqtt` para o
  WebSocket local com rate-limit, e um listener de debug na `8081` (LAN/Tailscale).
- `www/index.html` — placeholder servido pelo nginx.

## O que NÃO está aqui (de propósito)
- `mosquitto/passwd` (hash das senhas) e os certificados TLS — ficam só no servidor.
- Senhas dos usuários `access_esp32` e `access_web` — geradas com `mosquitto_passwd`, combinadas
  fora do repo.

## Deploy (no servidor)
```bash
sudo cp broker/mosquitto/access.conf /etc/mosquitto/conf.d/access.conf
sudo cp broker/mosquitto/acl        /etc/mosquitto/acl
sudo mkdir -p /var/www/access && sudo cp broker/www/index.html /var/www/access/index.html
sudo cp broker/nginx/access.conf /etc/nginx/sites-available/access
sudo ln -sf /etc/nginx/sites-available/access /etc/nginx/sites-enabled/access

# usuários (só na primeira vez / rotação de senha)
sudo mosquitto_passwd /etc/mosquitto/passwd access_esp32
sudo mosquitto_passwd /etc/mosquitto/passwd access_web

sudo nginx -t && sudo systemctl reload nginx
sudo systemctl restart mosquitto
```

## Teste rápido
```bash
mosquitto_sub -h localhost -p 1883 -u access_web -P "$PW_WEB" -t 'access/grupo5/sensor/#' -v
mosquitto_pub -h localhost -p 1883 -u access_esp32 -P "$PW_ESP" -t 'access/grupo5/sensor/presenca' -m '{"porta":"aberta"}'
```

## Conectividade externa (ESP32 em rede diferente do broker)

A porta `1883` (MQTT puro) **não é exposta à internet** — o roteador não encaminha essa porta e o
`ufw` não tem regra liberando-a para fora. Só a `443` está aberta publicamente (via
`accessgrupo5.duckdns.org`), atendendo HTTPS e o proxy WebSocket. Essa é uma decisão deliberada,
não uma pendência: expor MQTT puro sem TLS pela internet manda usuário/senha em texto claro.

Consequência prática: um dispositivo em rede diferente da do broker (como o ESP32, que não fica na
mesma LAN/Tailscale do servidor) **não consegue** se conectar com `PubSubClient` clássico (MQTT
puro por `WiFiClient`/`WiFiClientSecure` direto na `1883`). Ele precisa falar MQTT dentro de
WebSocket sobre TLS, no mesmo endpoint que o dashboard já usa:

- **Host:** `accessgrupo5.duckdns.org`
- **Porta:** `443`
- **Path:** `/mqtt`
- **Protocolo:** `wss://`

No firmware, isso significa trocar `PubSubClient` pelo cliente MQTT nativo do ESP-IDF
(`esp_mqtt_client`, já embutido no core Arduino-ESP32), que aceita `wss://` direto na URI:

```cpp
#include "mqtt_client.h"

esp_mqtt_client_config_t mqtt_cfg = {};
mqtt_cfg.broker.address.uri = "wss://accessgrupo5.duckdns.org/mqtt";
mqtt_cfg.credentials.username = "access_esp32";
mqtt_cfg.credentials.authentication.password = MQTT_PASS; // via secrets.h, nunca hardcode
mqtt_cfg.broker.verification.certificate = letsencrypt_isrg_root_x1_pem; // valida o TLS

esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
esp_mqtt_client_start(client);
```

É preciso embutir o certificado raiz **ISRG Root X1** da Let's Encrypt no firmware (ou usar o
`crt_bundle_attach` do ESP-IDF, que já traz os CAs comuns) para validar o TLS.

Se o ESP32 algum dia estiver na mesma LAN ou na Tailscale do broker, aí sim `PubSubClient` na
`1883` funciona direto, sem WSS.
