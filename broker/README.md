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
