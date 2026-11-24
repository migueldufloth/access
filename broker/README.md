# Broker MQTT e proxy

O Mosquitto sobe pelo [`compose.yaml`](../compose.yaml) da raiz, junto com o backend e o banco.
Em produção, o nginx do servidor (`accessgrupo5.duckdns.org`) termina o TLS e repassa o tráfego
para os containers.

## Estrutura
- `mosquitto/mosquitto.conf`: listeners `1883` (MQTT puro: backend e LAN/Tailscale) e
  `9001` (WebSocket, publicado só em `127.0.0.1` para o nginx), autenticação obrigatória e ACL.
- `mosquitto/acl`: ACL do contrato v1. O `access_backend` lê telemetria, estado,
  confirmação e status de todos e é o único que escreve em `comando`. Cada dispositivo
  (usuário = `deviceId`) só escreve nos próprios tópicos e só lê o próprio `comando`.
- `mosquitto/entrypoint.sh`: gera o `passwd` na subida a partir das senhas do `.env`
  (`MQTT_SENHA_BACKEND`, `MQTT_SENHA_PORTA_01`, `MQTT_SENHA_JANELA_01`). Nenhum hash fica no Git.
- `nginx/access.conf`: vhost do servidor com TLS (Let's Encrypt), redirect HTTP→HTTPS, `/` →
  backend (`127.0.0.1:8000`) e `/mqtt` → WebSocket do Mosquitto (`127.0.0.1:9001`), com rate-limit.

Novo dispositivo: acrescente a senha no `.env`, uma linha `adicionar <deviceId> ...` no
`entrypoint.sh` e reinicie o `mosquitto`. A ACL usa `%u`, então não muda.

## Deploy no servidor
```bash
git pull
cp .env.example .env            # só na primeira vez; preencher com as senhas reais
docker compose up -d --build

# nginx do host (TLS na 443)
sudo cp broker/nginx/access.conf /etc/nginx/sites-available/access
sudo ln -sf /etc/nginx/sites-available/access /etc/nginx/sites-enabled/access
sudo nginx -t && sudo systemctl reload nginx
```
O Mosquitto antigo, instalado direto no host (usuários `access_esp32`/`access_web`, contrato v0),
precisa ser parado antes (`sudo systemctl disable --now mosquitto`), porque o do compose usa as
mesmas portas `1883` e `9001`.

## Teste rápido
```bash
# Tudo o que passa no broker (com o usuário do backend, que lê todos os dispositivos)
docker compose exec mosquitto mosquitto_sub -u access_backend -P "$MQTT_SENHA_BACKEND" -t 'access/grupo5/#' -v

# Um dispositivo publicando no próprio tópico (aceito) e no de outro (descartado pela ACL)
docker compose exec mosquitto mosquitto_pub -u porta-01 -P "$MQTT_SENHA_PORTA_01" -t access/grupo5/porta-01/status \
  -m '{"v":1,"deviceId":"porta-01","status":"online","ts":null,"uptime_s":1,"fw":"1.0.0"}' -r
```

## Conectividade externa (placa em rede diferente do servidor)
A `1883` não é exposta à internet: o roteador não encaminha a porta e MQTT puro mandaria a senha
em texto claro. Fora da LAN/Tailscale, as placas usam MQTT sobre WebSocket com TLS na `443`:

- URI: `wss://accessgrupo5.duckdns.org/mqtt`
- Cliente: `esp_mqtt_client` do ESP-IDF (já no core Arduino-ESP32), com `esp_crt_bundle_attach`
  para validar o certificado Let's Encrypt.
- Na LAN/Tailscale do servidor, `mqtt://<ip-do-servidor>:1883` também funciona.
