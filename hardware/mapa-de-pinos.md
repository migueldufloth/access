# Mapa de pinos — Access N3 (Hardware Real Montado)

Mapa de pinos atualizado conforme os **dois dispositivos efetivamente montados na N3** (Central `ESP32-S3` com amplificador I²S `MAX98357` + caixa de som + OLED + reed `MC-38`, e Satélite `ESP32 DevKit V1` com `PIR HC-SR501` / `HC-SR04` + LED remoto).

> **Conformidade R1 (sem alimentação nem terra virtual por GPIO):** todas as cargas (OLED, MAX98357, PIR/HC-SR04) são alimentadas diretamente pelos barramentos reais (`3V3`, `5V`/`VIN` e `GND`) da placa; os GPIOs transportam apenas sinais de dados/controle e o LED usa resistor limitador em série.

## Dispositivo A — Central da Porta (`porta-01`, ESP32-S3)

| Função | GPIO | Tipo | Ligação | Observação |
| :--- | :--- | :--- | :--- | :--- |
| Display OLED SSD1306 — SDA | 14 | I²C | Direto no módulo (endereço `0x3C`) | Pull-ups integrados no módulo |
| Display OLED SSD1306 — SCL | 12 | I²C | Direto no módulo | |
| Display OLED — VCC / GND | — | Alimentação | `3V3` e `GND` reais da placa | Alimentação anterior por GPIO4/GPIO5 **eliminada (R1)** |
| Amplificador I²S MAX98357 — BCLK | 47 | Saída digital (I²S) | Pino `BCLK` do módulo | Driver nativo `driver/i2s.h` (`44100 Hz`) |
| Amplificador I²S MAX98357 — LRC | 48 | Saída digital (I²S) | Pino `LRC` do módulo | |
| Amplificador I²S MAX98357 — DIN | 21 | Saída digital (I²S) | Pino `DIN` do módulo | |
| Amplificador I²S MAX98357 — VIN / GND | — | Alimentação | `5V` e `GND` reais da placa | Alto-falante ligado nas saídas `+`/`−` do amplificador (nunca em GPIO) |
| Reed MC-38 (modo digital padrão) | 15 | Entrada digital (`INPUT_PULLUP`) | Entre GPIO 15 e `GND` real | `HIGH` = porta aberta (`REED_SUPERVISIONADO = 0`); terra virtual GPIO 16 **eliminado (R1)** |
| Reed MC-38 (modo supervisionado ADC1) | 6 | Entrada analógica (`ADC1_CH5`) | Divisor R1 10 kΩ (3V3) + Rs 2,2 kΩ + Reol 4,7 kΩ junto ao sensor | Ativável via `REED_SUPERVISIONADO = 1` em `config.h`; detecta 4 estados (`fechada`, `aberta`, `falha_aberto`, `falha_curto`) |
| LED de alerta local | 2 | Saída digital | GPIO 2 → resistor 220 Ω → anodo (+) do LED → catodo (−) no `GND` real | Corrente ≈ 5,9 mA; terra virtual GPIO 1 **eliminado (R1)** |

## Dispositivo B — Satélite de Movimento (`janela-01`, ESP32 DevKit V1)

| Função | GPIO | Tipo | Ligação | Observação |
| :--- | :--- | :--- | :--- | :--- |
| Sensor PIR HC-SR501 (OUT) | 4 | Entrada digital (`INPUT_PULLDOWN`) | `OUT` direto (nível lógico 3,3 V); `VCC` no `VIN` (5 V); `GND` no `GND` | Primeiros ~60 s reporta `sensor = "calibrando"`; jumper em `H` |
| Sensor Ultrassônico HC-SR04 — TRIG | 18 | Saída digital | Pino `TRIG` (opcional, habilitável em `config.h`) | Pulso de 10 µs a cada 70 ms |
| Sensor Ultrassônico HC-SR04 — ECHO | 19 | Entrada digital (`INPUT_PULLDOWN`) | Via divisor resistivo 5 V → 3,3 V (1 kΩ + 2 kΩ) | Evita 5 V direto em GPIO; `INPUT_PULLDOWN` evita disparos falsos se desconectado |
| LED de alerta remoto | 2 | Saída digital | LED onboard ou LED externo via resistor 220 Ω para `GND` | Pisca sincronizado com o alarme da Central via ESP-NOW e com o alarme próprio |
| VIN (5 V) / GND | — | Alimentação | Alimenta PIR (e HC-SR04) + terra comum | Fonte USB 5 V / 1 A |

## Conferência das regras de pinagem

- **Nenhum uso de pinos de flash SPI** (GPIO 6–11 no ESP32 clássico; GPIO 26–32 no ESP32-S3).
- **Nenhuma carga alimentada por GPIO:** OLED no `3V3`/`GND`, MAX98357 e PIR no `5V`/`VIN` e `GND`, LEDs com resistor limitador em série.
- **Supervisão analógica em ADC1:** no ESP32-S3, o GPIO 6 pertence ao `ADC1_CH5`, que funciona simultaneamente com o Wi-Fi ativo (ao contrário do ADC2).
- **Sinal máximo em GPIO = 3,3 V:** saída `OUT` do HC-SR501 já é 3,3 V pelo datasheet; pino `ECHO` do HC-SR04 deve usar divisor resistivo quando conectado.

## Histórico de evolução em relação ao desenho inicial (CP12)

| Item | Desenho CP12 | Montagem real N3 |
| :--- | :--- | :--- |
| Microcontrolador da Central (`porta-01`) | ESP32 DevKit V1 | **ESP32-S3** |
| Atuador sonoro da Central | Buzzer 5 V via transistor Q1 | **Amplificador I²S MAX98357 + alto-falante** (GPIO 47/48/21) |
| OLED / terras virtuais | GPIO 4/5/16/1 (protótipo v2) | **Removidos** — ligados em `3V3` e `GND` reais (R1) |
| Satélite (`janela-01`) | ESP32 + PIR GPIO 13 + buzzer | **ESP32 DevKit V1 + PIR GPIO 4** (+ suporte opcional HC-SR04 GPIO 18/19) + LED GPIO 2 + ESP-NOW + MQTT |
