# Mapa de pinos — Access N3

ESP32 DevKit V1 (ESP-WROOM-32). Projeto desenhado no CP12; a montagem física e a validação em bancada ficam para a Unidade 3. Esquemas: [`esquema-dispositivo-a.svg`](esquema-dispositivo-a.svg) e [`esquema-dispositivo-b.svg`](esquema-dispositivo-b.svg).

## Dispositivo A — porta (`porta-01`)

| Função | GPIO | Tipo | Ligação | Observação |
| :--- | :--- | :--- | :--- | :--- |
| Reed MC-38 supervisionado | 34 | Entrada analógica (ADC1_CH6) | R1 10 kΩ para 3V3; Rs 2,2 kΩ e Reol 4,7 kΩ junto ao sensor | Lê 4 estados (ver [README](README.md#supervisão-do-reed)). GPIO34 é só entrada, sem pull-up interno: o pull-up é o R1 |
| Buzzer | 25 | Saída digital (PWM por `tone`) | R2 1 kΩ para a base do Q1 NPN | O buzzer fica entre +5 V e o coletor do Q1; **não** é ligado ao GPIO |
| LED de status | 26 | Saída digital | R4 220 Ω em série com o LED, catodo no GND | |
| Botão armar/desarmar | 27 | Entrada digital (`INPUT_PULLUP`) | Botão entre GPIO27 e GND | Debounce de 50 ms em software |
| OLED SSD1306 — SDA | 21 | I²C | Direto no módulo (3,3 V, endereço 0x3C) | Pull-ups no próprio módulo |
| OLED SSD1306 — SCL | 22 | I²C | Direto no módulo | |
| 3V3 | — | Alimentação | OLED e R1 | |
| VIN (5 V) | — | Alimentação | Buzzer e diodo D1 | Vem do micro-USB |
| GND | — | Terra | Comum a todos os blocos | Terra virtual do GPIO18 (v3) **eliminado** |

## Dispositivo B — janela / ambiente (`janela-01`)

| Função | GPIO | Tipo | Ligação | Observação |
| :--- | :--- | :--- | :--- | :--- |
| Reed MC-38 supervisionado | 34 | Entrada analógica (ADC1_CH6) | Igual ao dispositivo A | |
| PIR HC-SR501 (OUT) | 13 | Entrada digital | OUT direto; VCC no +5 V (VIN) | Saída em 3,3 V (datasheet). Aquecimento de ~60 s: o firmware reporta `calibrando` e ignora o PIR nesse período |
| Buzzer | 25 | Saída digital (PWM por `tone`) | Igual ao dispositivo A | |
| LED de status | 26 | Saída digital | Igual ao dispositivo A | |
| 3V3 / VIN / GND | — | Alimentação | R1; PIR e buzzer; terra comum | Sem OLED nem botão neste dispositivo |

## Conferência das regras de pinagem

- Nenhum uso de GPIO 6 a 11 (flash).
- Nenhum atuador em pino de strapping (0, 2, 5, 12, 15): o buzzer está no GPIO25 e o LED no GPIO26.
- O único sensor analógico (reed supervisionado) está no ADC1 (GPIO34); o ADC2 não é usado, porque não lê com o Wi-Fi ativo.
- GPIO34 a 39 são só entrada e não têm pull-up interno; por isso o reed usa pull-up externo (R1).

## Mudanças em relação ao firmware atual (v3)

| Item | Hoje (`config.h`) | N3 |
| :--- | :--- | :--- |
| `REED_PIN` | 4, digital com `INPUT_PULLUP` | 34, analógico (ADC1) com supervisão |
| `BUZZER_POS` | 25, ligado direto no buzzer | 25, ligado à base do transistor Q1 |
| `BUZZER_GND` | 18, mantido em `LOW` como terra | Removido (o buzzer volta ao GND real) |
| LED de status | Não implementado | 26 |
| Botão | Não implementado | 27 (só no dispositivo A) |
| PIR | Não existe | 13 (só no dispositivo B) |
