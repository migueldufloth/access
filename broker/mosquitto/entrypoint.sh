#!/bin/sh
# Gera o arquivo de senhas do Mosquitto a partir das variáveis do .env e sobe o broker.
# Um usuário por dispositivo (usuário = deviceId) e o access_backend (CONTRATOS.md, seção 2).
set -eu

PASSWD=/mosquitto/data/passwd
rm -f "$PASSWD"
touch "$PASSWD"
chmod 0700 "$PASSWD"

adicionar() {
    [ -n "$2" ] || { echo "senha MQTT de $1 não definida no .env" >&2; exit 1; }
    mosquitto_passwd -b "$PASSWD" "$1" "$2"
}

adicionar access_backend "${MQTT_SENHA_BACKEND:-}"
adicionar porta-01 "${MQTT_SENHA_PORTA_01:-}"
adicionar janela-01 "${MQTT_SENHA_JANELA_01:-}"

# A ACL vem do repositório (montado só leitura); a cópia ganha a permissão que o Mosquitto exige.
install -m 0700 /mosquitto/config/acl /mosquitto/data/acl
chown mosquitto:mosquitto "$PASSWD" /mosquitto/data/acl
exec mosquitto -c /mosquitto/config/mosquitto.conf
