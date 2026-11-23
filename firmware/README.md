# Firmwares — Projeto Access (v2 Distribuído)

Esta pasta contém os firmwares da **v2 distribuída** (duas placas ESP32 comunicando-se via broker Mosquitto deste repositório) e mantém o firmware legado da v1/N1 como referência histórica.

## Estrutura

| Pasta | Placa | Papel | Versão |
| :--- | :--- | :--- | :--- |
| [`firmware-central-s3/`](firmware-central-s3/) | **ESP32-S3** | **Placa 1 — Central da Porta:** sensor magnético MC-38, display OLED SSD1306, caixa de som I2S MAX98357, LED local, regra combinada e notificação `ntfy.sh` | **v2 (atual)** |
| [`firmware-satelite-esp32/`](firmware-satelite-esp32/) | **ESP32 Dev Module** | **Placa 2 — Satélite de Movimento:** sensor PIR HC-SR501 e/ou Ultrassônico HC-SR04 + LED de alerta remoto sincronizado | **v2 (atual)** |
| [`Access/`](Access/) | ESP32 Dev Module | Protótipo legado de placa única com buzzer (N1/v1) | Legado |

---

## Arquitetura Híbrida: ESP-NOW + Mosquitto (v2)

A comunicação foi dividida em duas camadas para máxima velocidade e confiabilidade:

1. **Placa 1 ↔ Placa 2 (ESP-NOW direto por rádio 2.4 GHz):**
   - O Satélite envia detecção de movimento (`DETECTADO`/`LIMPO`) direto para a Central e recebe o estado de alarme (`0`/`1`) para piscar o LED sincronizado — **funciona instantaneamente mesmo sem internet, sem roteador ou sem broker MQTT**.
   - **Sincronização automática de canal:** no boot, o Satélite escaneia o canal do `WIFI_SSID` configurado no `secrets.h` e, se ficar sem heartbeat da Central por `5 s`, varre automaticamente os canais `1..13` até travar no mesmo canal da Central.
2. **Placa 1 → Nuvem (Mosquitto + `ntfy.sh`):**
   - A Central (`ESP32-S3`) conecta no Wi-Fi (com potência controlada em `WIFI_POWER_8_5dBm` para estabilidade da OLED), publica toda a telemetria (`porta`, `movimento`, `alarme`, `central`, `satelite`, `presenca_dispositivo` e JSON `presenca`) no broker Mosquitto deste repositório (`wss://accessgrupo5.duckdns.org/mqtt`) via `esp_mqtt_client` e envia notificação push via `ntfy.sh`.
   - Para uso em LAN/Tailscale do broker, basta trocar `MQTT_URI` no `secrets.h` para `"mqtt://<IP_DO_SERVIDOR>:1883"`.

### Mapa de Tópicos (`access/grupo5/...`)

| Tópico | Publica | Assina | Payload | Retain |
| :--- | :--- | :--- | :--- | :--- |
| `access/grupo5/sensor/porta` | Placa 1 (Central) | Dashboard | `"ABERTA"` \| `"FECHADA"` | Sim |
| `access/grupo5/sensor/movimento` | Placa 2 (Satélite) | Placa 1 (Central), Dashboard | `"DETECTADO"` \| `"LIMPO"` | Sim |
| `access/grupo5/sensor/presenca` | Placa 1 (Central) | Dashboard | JSON (`porta`, `movimento`, `disparado`, `aberturas`, `rssi`) | Não |
| `access/grupo5/status/alarme` | Placa 1 (Central) | Placa 2 (Satélite), Dashboard | `"1"` \| `"0"` | Sim |
| `access/grupo5/status/central` | Placa 1 (Central + LWT) | Dashboard | `"online"` \| `"offline"` | Sim |
| `access/grupo5/status/satelite` | Placa 2 (Satélite + LWT) | Dashboard | `"online"` \| `"offline"` | Sim |

> **Importante (servidor Mosquitto):** atualize `/etc/mosquitto/acl` com o arquivo [`broker/mosquitto/acl`](../broker/mosquitto/acl) deste repositório e reinicie o Mosquitto (`sudo systemctl restart mosquitto`), para que o usuário `access_esp32` tenha permissão `readwrite` em `sensor/#` e `status/#`.

---

## Pinagem

### Placa 1 — Central da Porta (`ESP32-S3`)
| Componente | Pinos | Observações |
| :--- | :--- | :--- |
| OLED SSD1306 (128x64 I2C) | `SDA = 14`, `SCL = 12`, `VCC = 4`, `GND = 5` | `VCC`/`GND` com `GPIO_DRIVE_CAP_3`; Wi-Fi fixado em `WIFI_POWER_5dBm` para evitar queda de tensão na tela |
| Amplificador I2S MAX98357 | `BCLK = 47`, `LRC = 48`, `DIN = 21` | `SAMPLE_RATE = 44100`, driver nativo `driver/i2s.h` |
| Sensor Magnético MC-38 | `Sinal = 15` (`INPUT_PULLUP`), `GND = 16` (`LOW`) | `HIGH` = porta aberta |
| LED de Alerta Local | `Positivo = 2`, `GND = 1` (`LOW`) | Pisca sincronizado com a sirene I2S |

### Placa 2 — Satélite de Movimento (`ESP32 Dev Module`)
| Componente | Pinos | Observações |
| :--- | :--- | :--- |
| Sensor PIR HC-SR501 | `GPIO 4` (`INPUT_PULLDOWN`) | `HIGH` = movimento detectado |
| Sensor Ultrassônico HC-SR04 | `TRIG = 18`, `ECHO = 19` | Dispara quando distância `< 50 cm` |
| LED de Alerta Remoto | `GPIO 2` | Pisca a cada `120 ms` enquanto `status/alarme == "1"` |

---

## Como Compilar e Gravar

1. **Criar `secrets.h` em cada pasta** (o arquivo está no `.gitignore` e nunca deve ser commitado):
   ```bash
   cp firmware/firmware-central-s3/secrets.example.h firmware/firmware-central-s3/secrets.h
   cp firmware/firmware-satelite-esp32/secrets.example.h firmware/firmware-satelite-esp32/secrets.h
   ```
   Preencha `WIFI_SSID`, `WIFI_PASS`, `MQTT_URI`, `MQTT_USER` (`access_esp32`), `MQTT_PASS` (e `NTFY_TOPIC` na Central).
2. **Bibliotecas necessárias (Arduino IDE):**
   - **Placa 1 (`firmware-central-s3`):** `Adafruit GFX Library` e `Adafruit SSD1306` (demais bibliotecas — `WiFi`, `HTTPClient`, `driver/i2s.h`, `mqtt_client.h`, `esp_crt_bundle.h` — já vêm no core Arduino-ESP32).
   - **Placa 2 (`firmware-satelite-esp32`):** nenhuma biblioteca externa necessária (tudo embutido no core Arduino-ESP32).
3. **Placas na Arduino IDE:**
   - Placa 1: `ESP32S3 Dev Module` (abrir `firmware/firmware-central-s3/firmware-central-s3.ino`).
   - Placa 2: `ESP32 Dev Module` (abrir `firmware/firmware-satelite-esp32/firmware-satelite-esp32.ino`).
