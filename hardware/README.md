# Hardware — Access N3

Projeto de hardware dos dois dispositivos da N3. Tudo aqui é **desenho e cálculo** (CP12): os valores vêm de datasheet e de contas, não de medição. A montagem e a medição em bancada acontecem na Unidade 3.

| Arquivo | Conteúdo |
| :--- | :--- |
| [`esquema-dispositivo-a.svg`](esquema-dispositivo-a.svg) | Esquema elétrico do dispositivo A (porta): reed, buzzer, LED, botão, OLED |
| [`esquema-dispositivo-b.svg`](esquema-dispositivo-b.svg) | Esquema elétrico do dispositivo B (janela/ambiente): reed, PIR, buzzer, LED |
| [`mapa-de-pinos.md`](mapa-de-pinos.md) | GPIO de cada função e conferência das regras de pinagem |
| [`bom.md`](bom.md) | Lista de materiais: o que já temos e o que falta |

## Dispositivos

| | A — `porta-01` | B — `janela-01` |
| :--- | :--- | :--- |
| Entradas | Reed MC-38 (supervisionado) + botão armar/desarmar | Reed MC-38 (supervisionado) + PIR HC-SR501 |
| Saídas | Buzzer (via transistor), LED de status, OLED | Buzzer (via transistor), LED de status |
| Corrente de pico estimada | ≈ 410 mA | ≈ 450 mA |
| Alimentação | Micro-USB 5 V / 1 A | Micro-USB 5 V / 1 A |

## Verificação elétrica calculada

| Verificação | Valor calculado | Como será garantido |
| :--- | :--- | :--- |
| 3V3 usado | OLED ≈ 20 mA, divisor do reed ≈ 0,27 mA; o próprio ESP32 com Wi-Fi ≈ 350 mA de pico. Total no dispositivo A ≈ 375 mA, abaixo dos 800 mA do regulador da placa | Medir a corrente total na bancada (Unidade 3) |
| 5 V / VIN usado | Buzzer ≤ 30 mA; PIR ≤ 65 mA (dispositivo B). Total USB: A ≈ 410 mA, B ≈ 450 mA | Fonte 5 V / 1 A em cada dispositivo, cabo curto e capacitor de 100 µF entre VIN e GND |
| Corrente do buzzer | ≤ 30 mA, **fora do GPIO**: o GPIO25 entrega só a corrente de base, Ib = (3,3 − 0,7) V / 1 kΩ = 2,6 mA. O transistor satura, porque basta Ib ≈ 0,3 mA para 30 mA com hFE ≥ 100 | Q1 NPN com R2 de 1 kΩ |
| Corrente do LED | (3,3 − 2,0) V / 220 Ω ≈ 5,9 mA, dentro do limite do GPIO (absoluto 40 mA) | R4 de 220 Ω em série |
| GND comum | Uma única fonte por dispositivo; todos os terras terminam nos pinos GND da mesma placa. A e B são independentes, sem terra compartilhado entre eles | Conferir continuidade na montagem |
| Maior tensão em um GPIO | **3,3 V**, em todos. O único sensor alimentado com 5 V é o PIR HC-SR501, cuja saída é 3,3 V pelo datasheet. Nenhum sinal de 5 V chega a GPIO | Medir o OUT do PIR com multímetro antes de ligar na placa |
| Proteção de carga indutiva | Diodo D1 1N4148 em antiparalelo com o buzzer (catodo no +5 V). Se o buzzer for piezoelétrico, não há bobina e o diodo é redundante; se for eletromagnético, ele é necessário | D1 previsto nos dois esquemas |
| Estado seguro do atuador no boot | Buzzer **desligado**. Durante reset e boot o GPIO25 fica em alta impedância, e o R3 de 10 kΩ mantém a base do Q1 em 0 V. O LED também fica apagado. Os GPIO25 e 26 não são pinos de strapping. No `setup()`, os dois vão para `LOW` antes de qualquer outra coisa | R3 no esquema + inicialização no firmware |

Checklist de desenho:

- [x] Nenhuma carga (buzzer, LED) alimentada diretamente por GPIO. O LED consome 5,9 mA, e o buzzer passa pelo transistor.
- [x] Diodo de roda livre previsto (D1) no buzzer.
- [x] Nenhum sinal de 5 V ligado direto a um GPIO.
- [x] Nenhum uso de GPIO 6 a 11. Nenhum atuador em pino de strapping.
- [x] Sensor analógico no ADC1 (GPIO34).

## Supervisão do reed

Na v3, um fio solto no reed lê como "aberta", igual a uma abertura real. Para distinguir, o reed ganha dois resistores **junto ao sensor** e a leitura passa a ser analógica (GPIO34, ADC1 com atenuação de 11 dB):

- R1 = 10 kΩ no ESP32, ligando o GPIO34 ao 3V3.
- Rs = 2,2 kΩ em série com o conjunto "reed em paralelo com Reol = 4,7 kΩ", na caixa do sensor.

| Condição | Cálculo | Tensão no GPIO34 | Faixa de decisão |
| :--- | :--- | :--- | :--- |
| Porta fechada (reed fechado) | 3,3 V × 2,2 k / (10 k + 2,2 k) | ≈ 0,60 V | 0,15 a 1,00 V |
| Porta aberta (reed aberto) | 3,3 V × (2,2 k + 4,7 k) / (10 k + 6,9 k) | ≈ 1,35 V | 1,00 a 2,30 V |
| Fio rompido (circuito aberto) | Só o R1 puxa para 3V3 | ≈ 3,3 V | acima de 2,30 V |
| Curto no cabo | O GPIO34 vai direto ao GND | ≈ 0 V | abaixo de 0,15 V |

Fio rompido ou curto por mais de 3 s coloca o firmware no modo SEGURO e gera o código de erro `E_SENSOR_FALHA`. As faixas são calculadas; a calibração no ADC real do ESP32 (que não é linear) é feita na Unidade 3. **Plano B**, se a supervisão não ficar confiável: voltar ao reed digital no GPIO4 e detectar falha por ausência de mudança de estado por um período configurável.

## Pendências

- Segundo ESP32 ainda não adquirido (bloqueia o R13).
- Confirmar o modelo exato do buzzer (piezoelétrico ou eletromagnético) e a corrente real no datasheet.
- Itens "da v1" do [BOM](bom.md) a confirmar.
