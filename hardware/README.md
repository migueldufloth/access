# Hardware — Access N3

Documentação de hardware dos **dois dispositivos montados na N3** (`porta-01` e `janela-01`), atualizada para refletir a montagem física real após a evolução em relação ao desenho inicial do CP12.

| Arquivo | Conteúdo |
| :--- | :--- |
| [`mapa-de-pinos.md`](mapa-de-pinos.md) | Pinagem real das duas placas (`ESP32-S3` + `ESP32 DevKit V1`) e conferência das regras R1 |
| [`bom.md`](bom.md) | Lista de materiais efetivamente montados e opcionais de supervisão |
| [`esquema-dispositivo-a.svg`](esquema-dispositivo-a.svg) | Esquema inicial do CP12 (referência histórica — substituído na Central por ESP32-S3 + MAX98357) |
| [`esquema-dispositivo-b.svg`](esquema-dispositivo-b.svg) | Esquema inicial do CP12 (referência histórica — no Satélite montado usa PIR no GPIO 4) |

## Dispositivos montados

| | A — `porta-01` (Central) | B — `janela-01` (Satélite) |
| :--- | :--- | :--- |
| Microcontrolador | **ESP32-S3 Dev Module** | **ESP32 DevKit V1 (ESP-WROOM-32)** |
| Entradas | Sensor magnético MC-38 (GPIO 15 digital ou GPIO 6 ADC1 supervisionado) | Sensor PIR HC-SR501 (GPIO 4) + suporte opcional a HC-SR04 (GPIO 18/19) |
| Saídas | Amplificador I²S MAX98357A + alto-falante (GPIO 47/48/21), OLED SSD1306 (GPIO 14/12), LED local (GPIO 2 + 220 Ω) | LED de alerta remoto sincronizado (GPIO 2 + 220 Ω ou onboard) |
| Comunicação | Wi-Fi + MQTT (`wss://`, contrato v1) + ESP-NOW (sincronização local com Satélite) | Wi-Fi + MQTT (`wss://`, contrato v1) + ESP-NOW (sincronização local com Central) |
| Alimentação | USB-C 5 V / 1 A | Micro-USB 5 V / 1 A |

## Verificação elétrica (R1)

| Verificação | Valor / regra | Como é garantido na montagem N3 |
| :--- | :--- | :--- |
| Alimentação de cargas fora dos GPIOs | **Nenhuma carga alimentada por GPIO** | OLED alimentada no pino `3V3` e `GND` reais; MAX98357A e PIR alimentados no `5V`/`VIN` e `GND` reais. Os pinos virtuais do protótipo v2 (GPIO 1, 4, 5, 16) foram removidos do firmware |
| Corrente no barramento `3V3` (`porta-01`) | OLED ≈ 20 mA + ESP32-S3 Wi-Fi (TX limitado a `WIFI_POWER_8_5dBm`) ≈ 220 mA pico → total ≈ 240 mA (bem abaixo do regulador de 500–800 mA) | Potência de TX controlada em `config.h` evita quedas de tensão na OLED |
| Corrente no barramento `5V`/`VIN` | MAX98357A (~200–350 mA em pico de sirene); PIR HC-SR501 ≤ 65 mA | Alimentação 5 V dedicada por placa + geração senoidal no driver I²S nativo (`driver/i2s.h`) |
| Corrente nos LEDs | `(3,3 − 2,0) V / 220 Ω ≈ 5,9 mA`, bem abaixo do limite do GPIO | Resistor de 220 Ω em série com cada LED, catodo no `GND` real |
| Tensão máxima nos GPIOs | **3,3 V** em todos os pinos | Saída `OUT` do PIR HC-SR501 é 3,3 V pelo datasheet; `ECHO` do HC-SR04 (se ligado) usa divisor 1 kΩ / 2 kΩ |
| Estado seguro no boot | Sirene muda e LEDs em `LOW` | `digitalWrite(LED_POS_PIN, LOW)` / `digitalWrite(LED_ALARME_PIN, LOW)` e `i2s_zero_dma_buffer()` no `setup()` |

Checklist R1:
- [x] Nenhuma carga (OLED, amplificador, PIR, LED) alimentada diretamente por GPIO.
- [x] Terras virtuais em GPIO eliminados — todos os retornos no `GND` real.
- [x] LEDs com resistor limitador de 220 Ω em série.
- [x] Nenhum sinal de 5 V ligado diretamente a um GPIO.
- [x] Nenhum uso de pinos de flash SPI; supervisão analógica do reed em `ADC1` (GPIO 6 no ESP32-S3).

## Supervisão do reed (`REED_SUPERVISIONADO = 1`)

Para detectar fisicamente fio rompido (`falha_aberto`) ou curto-circuito (`falha_curto`) no sensor magnético MC-38 (Cenário 3 de [`docs/falhas/README.md`](../docs/falhas/README.md)), o firmware da Central suporta leitura analógica supervisionada no **GPIO 6 (`ADC1_CH5` no ESP32-S3)** com três resistores:

- `R1 = 10 kΩ` entre `GPIO 6` e `3V3` (junto à placa);
- `Rs = 2,2 kΩ` em série com o paralelo `MC-38 || Reol (4,7 kΩ)` (junto ao sensor).

| Condição | Tensão esperada no GPIO 6 | Faixa no firmware (`config.h`) | Estado reportado |
| :--- | :--- | :--- | :--- |
| Curto no cabo | ≈ 0,00 V | `< 150 mV` por 3 s | `modo = "seguro"`, `sensor = "falha_curto"` |
| Porta fechada | ≈ 0,60 V | `150 a 1000 mV` | `sensor = "ok"`, `abertura = 0` |
| Porta aberta | ≈ 1,35 V | `1000 a 2300 mV` | `sensor = "ok"`, `abertura = 1` |
| Fio rompido / desconectado | ≈ 3,30 V | `> 2300 mV` por 3 s | `modo = "seguro"`, `sensor = "falha_aberto"` |

Ao normalizar a leitura (religar o fio), o firmware retorna automaticamente para `sensor = "ok"` e `modo = "automatico"` **sem reiniciar**. Sem os três resistores instalados (`REED_SUPERVISIONADO = 0`), a Central opera no modo digital clássico no GPIO 15 (`INPUT_PULLUP` para `GND` real) e aceita `falha`/`curto`/`ok` pelo Monitor Serial para validação de bancada.
