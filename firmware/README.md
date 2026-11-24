# Firmwares — Projeto Access (N3 — Contrato MQTT v1 + ESP-NOW)

Esta pasta contém os firmwares das **duas placas da N3** falando o [contrato MQTT v1](../CONTRATOS.md) com o backend FastAPI/Mosquitto (e sincronização local instantânea via ESP-NOW), além do firmware legado da N1 como referência histórica.

## Estrutura

| Pasta | Placa | `deviceId` | Papel | Versão |
| :--- | :--- | :--- | :--- | :--- |
| [`firmware-central-s3/`](firmware-central-s3/) | **ESP32-S3 Dev Module** | `porta-01` | **Central da Porta:** reed MC-38 (digital ou supervisionado ADC1), OLED SSD1306, caixa de som I²S MAX98357, LED local, regra local sem rede, NVS, OTA HTTPS com SHA-256, MQTT v1 + ESP-NOW | **N3 (`1.0.0`)** |
| [`firmware-satelite-esp32/`](firmware-satelite-esp32/) | **ESP32 DevKit V1** | `janela-01` | **Satélite de Movimento:** PIR HC-SR501 (com calibração ~60 s) e/ou HC-SR04, LED remoto sincronizado, regra local, NVS, OTA HTTPS com SHA-256, MQTT v1 + ESP-NOW | **N3 (`1.0.0`)** |
| [`Access/`](Access/) | ESP32 DevKit V1 | — | Protótipo legado de placa única com buzzer (N1) | Legado |

---

## Contrato MQTT v1 e Recursos N3 Implementados nas Duas Placas

- **Transporte:** `esp_mqtt_client` (ESP-IDF nativo) sobre `wss://accessgrupo5.duckdns.org/mqtt` (porta 443, TLS validado via `esp_crt_bundle_attach` com SNTP prévio) ou `mqtt://<IP>:1883` em ambiente Docker Compose local.
- **Tópicos por placa (`access/grupo5/<deviceId>/`):**
  - `telemetria` (QoS 0, sem retain): `abertura` (`porta-01`) ou `presenca` (`janela-01`), `aberturas` e `rssi`, a cada `intervaloTelemetria_s` e por evento.
  - `estado` (QoS 1, retido): `fw`, `modo`, `armado`, `alarme`, `sensor`, `config` e `ultimoComandoId`.
  - `comando` (assinatura QoS 1) e `confirmacao` (QoS 1): ciclo completo (`recebido`/`aceito` ou `rejeitado`, `executado`/`ok` ou `falha`, `estado` com `ultimoComandoId`), catálogo de erros e cache dos últimos 16 `commandId` para reentrega com `duplicado: true`.
  - `status` (QoS 1, retido): `online` na conexão e Last Will `offline`.
- **Persistência (NVS / `Preferences`):** `armado`, `tempoEntrada_s`, `intervaloTelemetria_s` e `timeoutManual_s` sobrevivem ao reinício.
- **Regra local sem Wi-Fi (R2 / R10):** com `armado = true`, abertura inicia `alarme = "entrada"` e após `tempoEntrada_s` dispara a sirene (`"disparado"`) e pisca os LEDs nas duas placas via ESP-NOW mesmo com roteador desligado; ao voltar o Wi-Fi, reconecta sozinha.
- **Estado seguro:** reed supervisionado (`REED_SUPERVISIONADO = 1`, GPIO 6 ADC1) entra em `modo = "seguro"` (`falha_aberto`/`falha_curto`) em 3 s e volta a `automatico` sem reiniciar; PIR do Satélite reporta `sensor = "calibrando"` nos primeiros ~60 s.
- **OTA HTTPS (R17):** comando `atualizar_firmware` baixa por HTTPS (`NetworkClientSecure` com `useBuiltinCACertBundle()` + `HTTPC_STRICT_FOLLOW_REDIRECTS`), calcula SHA-256 em streaming via `mbedtls/sha256.h`, aborta com `E_OTA_FALHA` se divergir ou grava e reinicia na nova versão.

---

## Como Compilar e Gravar na Arduino IDE

1. **Bibliotecas necessárias:**
   - **Ambas as placas:** `ArduinoJson` (v7, Benoît Blanchon).
   - **Central (`porta-01`):** `Adafruit GFX Library` e `Adafruit SSD1306`.
   - Demais (`WiFi`, `NetworkClientSecure`, `HTTPClient`, `Update`, `Preferences`, `esp_now`, `mqtt_client`, `esp_crt_bundle`, `driver/i2s`, `mbedtls/sha256`) já vêm no core **Arduino-ESP32 3.x**.
2. **Credenciais (`secrets.h`, ignorado pelo `.gitignore`):**
   ```bash
   cp firmware/firmware-central-s3/secrets.example.h firmware/firmware-central-s3/secrets.h
   cp firmware/firmware-satelite-esp32/secrets.example.h firmware/firmware-satelite-esp32/secrets.h
   ```
   Preencha `WIFI_SSID`, `WIFI_PASS`, `MQTT_URI`, `MQTT_USER` (`porta-01` / `janela-01`) e `MQTT_PASS`.
3. **Partições para OTA (obrigatório para R17):**
   - Em **Tools → Partition Scheme**, escolha um esquema com **2 slots OTA**, por exemplo **Minimal SPIFFS (1.9MB APP with OTA)** ou **Default 4MB with spiffs** (no S3: **8M with spiffs** ou **Default 4MB with spiffs**).
4. **Pinagem completa (R1 — sem alimentação/terra por GPIO):** ver [`hardware/mapa-de-pinos.md`](../hardware/mapa-de-pinos.md).
