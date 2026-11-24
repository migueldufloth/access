# BOM — Access N3 (Montagem Real)

Lista de materiais dos **dois dispositivos efetivamente montados** na N3:
- **Dispositivo A (`porta-01`):** Central da Porta baseada em **ESP32-S3** com OLED, reed magnético `MC-38` e áudio I²S (`MAX98357` + caixa de som).
- **Dispositivo B (`janela-01`):** Satélite de Movimento baseado em **ESP32 DevKit V1** com sensor PIR `HC-SR501` (e suporte opcional a ultrassônico `HC-SR04`) e LED de alerta remoto.

## Componentes em uso (montados e validados)

| Item | Qtd. | Dispositivo | Uso |
| :--- | :--- | :--- | :--- |
| Placa ESP32-S3 Dev Module | 1 | `porta-01` (Central) | Microcontrolador principal (Wi-Fi + MQTT WSS + ESP-NOW + I²S + OLED) |
| Placa ESP32 DevKit V1 (ESP-WROOM-32, CP2102) | 1 | `janela-01` (Satélite) | Microcontrolador satélite (Wi-Fi + MQTT WSS + ESP-NOW + PIR) |
| Display OLED SSD1306 I²C 128×64 (3,3 V) | 1 | `porta-01` | Dashboard local (`3V3`/`GND`, SDA GPIO 14, SCL GPIO 12) |
| Módulo amplificador I²S MAX98357A (3 W) | 1 | `porta-01` | Amplificação de áudio digital I²S (`5V`/`GND`, BCLK 47, LRC 48, DIN 21) |
| Alto-falante / caixa de som (4 Ω ou 8 Ω, 3 W) | 1 | `porta-01` | Sirene e bipes de abertura/fechamento via MAX98357A |
| Sensor magnético de contato MC-38 (reed switch) | 1 | `porta-01` | Detecção de abertura de porta (`GPIO 15` ou `GPIO 6` supervisionado) |
| Sensor de presença PIR HC-SR501 | 1 | `janela-01` | Detecção de movimento (`VCC` em `VIN` 5 V, `OUT` em `GPIO 4`) |
| Sensor ultrassônico HC-SR04 *(opcional)* | 1 | `janela-01` | Detecção auxiliar de passagem (`TRIG` 18, `ECHO` 19 com divisor 5 V → 3,3 V) |
| LED difuso 5 mm (vermelho) | 2 | `porta-01` e `janela-01` | Alerta visual local e remoto sincronizado (GPIO 2) |
| Resistor 220 Ω (1/4 W) | 2 | `porta-01` e `janela-01` | Limitador de corrente em série com cada LED (≈ 5,9 mA) |
| Protoboards e conjunto de jumpers | 2 conj. | Ambos | Montagem de bancada |
| Cabos USB (USB-C para S3 / micro-USB para DevKit) + fontes 5 V / 1 A | 2 | Ambos | Alimentação e programação |

## Componentes adicionais para supervisão analógica do reed (opcional / modo `REED_SUPERVISIONADO = 1`)

| Item | Qtd. | Uso | Observação |
| :--- | :--- | :--- | :--- |
| Resistor 10 kΩ (1/4 W) | 1 | R1 (pull-up para 3V3 no GPIO 6) | Divisor resistivo da supervisão de cabo rompido/curto |
| Resistor 2,2 kΩ (1/4 W) | 1 | Rs (em série junto ao sensor MC-38) | Define faixa de porta fechada (~0,60 V) |
| Resistor 4,7 kΩ (1/4 W) | 1 | Reol (em paralelo com o reed MC-38) | Define faixa de porta aberta (~1,35 V) vs fio rompido (~3,3 V) |
| Divisor resistivo 1 kΩ + 2 kΩ *(se usar HC-SR04)* | 1 conj. | Adaptação de nível 5 V → 3,3 V no pino `ECHO` | Protege o GPIO 19 do ESP32 |

## Referências
- Mapa de pinos atualizado: [`mapa-de-pinos.md`](mapa-de-pinos.md)
- Verificação elétrica e supervisão do reed: [`README.md`](README.md)
- Esquemas originais do CP12 (referência histórica): [`esquema-dispositivo-a.svg`](esquema-dispositivo-a.svg) e [`esquema-dispositivo-b.svg`](esquema-dispositivo-b.svg)
