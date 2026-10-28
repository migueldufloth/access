# BOM — Access N3

Quantidades para os **dois** dispositivos (A: porta, B: janela/ambiente). "Já tem" é o que a equipe possui da N1 (v1 e v2); "Falta" é o que ainda precisa ser providenciado antes da primeira aula de construção (10/11/2026).

## Já tem

| Item | Qtd. em mãos | Uso |
| :--- | :--- | :--- |
| ESP32 DevKit V1 (ESP-WROOM-32) | 1 | Dispositivo A |
| Sensor magnético MC-38-NA (reed) | 1 | Reed do dispositivo A |
| Buzzer piezoelétrico 5 V | 1 | Buzzer do dispositivo A |
| Display OLED SSD1306 I²C 128×64 | 1 | Dispositivo A |
| Sensor PIR HC-SR501 | 1 (da v1) | Dispositivo B |
| Botão táctil 6×6×5 mm | 1 (da v1) | Dispositivo A |
| LED vermelho difuso | 1 (da v1) | Dispositivo A |
| Resistor 220 Ω | 1 (da v1) | R4 do dispositivo A |
| Mini protoboard e jumpers | 1 conjunto | Montagem de bancada |

Os itens marcados "da v1" precisam ser **confirmados** com quem os guardou; se estiverem faltando, entram na lista abaixo.

## Falta comprar ou providenciar

| Item | Qtd. | Uso | Observação |
| :--- | :--- | :--- | :--- |
| ESP32 DevKit V1 (ESP-WROOM-32) | 1 | Dispositivo B | **Bloqueia o R13.** Mesmo modelo do A, para reaproveitar o firmware |
| Sensor magnético MC-38 (reed) | 1 | Reed do dispositivo B | |
| Buzzer piezoelétrico 5 V | 1 | Buzzer do dispositivo B | |
| Transistor NPN 2N2222A ou BC548 | 2 | Q1 de cada dispositivo | Chaveia o buzzer; Ic ≤ 100 mA |
| Diodo 1N4148 | 2 | D1 de cada dispositivo | Roda livre no buzzer (previsto mesmo para buzzer piezoelétrico) |
| Resistor 1 kΩ | 2 | R2 (base de Q1) | |
| Resistor 10 kΩ | 4 | R3 (pull-down da base) e R1 (pull-up do reed) | 2 de cada |
| Resistor 2,2 kΩ | 2 | Rs do reed supervisionado | |
| Resistor 4,7 kΩ | 2 | Reol do reed supervisionado | |
| Resistor 220 Ω | 1 | R4 do LED do dispositivo B | |
| LED vermelho 5 mm | 1 | LED do dispositivo B | |
| Capacitor eletrolítico 100 µF / 10 V | 2 | Desacoplamento VIN/GND | |
| Fonte USB 5 V / 1 A + cabo micro-USB | 2 | Alimentação | Porta de PC (500 mA) é justa demais; usar carregador |
| Cabo de 2 vias (≈ 2 m) | 2 | Ligação placa ↔ reed | |
| Borne de 2 vias ou conector | 4 | Emenda do cabo do reed | |
| Caixa plástica para eletrônica | 2 | Acabamento (R1 e "produto, não exercício") | Com furo para o cabo e o buzzer |
| Placa perfurada ou protoboard extra | 1 | Montagem do dispositivo B | |

## Esquemas

Esquemas desenhados em [`esquema-dispositivo-a.svg`](esquema-dispositivo-a.svg) e [`esquema-dispositivo-b.svg`](esquema-dispositivo-b.svg); mapa de pinos em [`mapa-de-pinos.md`](mapa-de-pinos.md).
