# Aprofundamento do Projeto — Pós Aula 04
## Continuação do README da Aula 02 | Projeto IoT - Access (Grupo 5)

## Repositório
- **Link:** https://github.com/migueldufloth/access
- **Estrutura inicial criada:**
  - `/firmware` — código-fonte do ESP32 (a ser preenchido a partir da Aula 05)
  - `/hardware` — esquemáticos, datasheets e especificação de componentes
  - `/docs` — documentação do projeto (este documento)
  - `README.md` — visão geral do projeto

## Sensor e Atuador Reais
- **Sensor principal:** Sensor Piroelétrico PIR **HC-SR501** — detecção de presença/movimento por infravermelho, saída digital (HIGH/LOW).
- **Sensor secundário / rearme manual:** Botão Pushbutton táctil 4 terminais 6x6x5 mm — usado para armar/desarmar e confirmar rearme local do sistema.
- **Atuador/alerta sonoro:** Buzzer Piezoelétrico 5V, acionado via PWM em ~1.2kHz (GPIO 23).
- **Atuador/alerta visual:** LED Difuso Vermelho (GPIO 4), indicando estado Ativado/Desativado/Disparado.

Essas escolhas são definitivas para a N1 — não há mais alternativas em aberto.

## Hardware Detalhado
| Componente | Especificação | Função |
| :--- | :--- | :--- |
| Microcontrolador | ESP32 Kit V1 ESP-WROOM-32, Dual-Core 240MHz, Wi-Fi/BLE | Processamento local e comunicação |
| Resistor limitador | Filme de carbono, 220Ω | Proteção do LED (em série com o catodo) |
| Protoboard | Mini protoboard 400 pontos | Prototipagem do circuito |
| Cabos jumper | Macho-Macho / Macho-Fêmea | Conexões entre ESP32, sensores e atuadores |
| Alimentação | 5V DC via micro-USB (fonte externa/computador) | Alimentação da placa e periféricos |
| Estrutura/caixa | A definir | Acondicionamento físico do protótipo — decisão pendente para a fase de integração (Aula 05) |

## Protótipo do Produto
Este projeto não possui aplicativo/painel próprio (a interface é um dashboard MQTT genérico), então o protótipo visual é o arranjo físico do hardware:

- Esquema do circuito: ver imagem em `README.md` (ESP32 + PIR + botão + buzzer + LED em protoboard).
- Simulador funcional: https://wokwi.com/projects/473279281421060097

*(Pendente: foto real do protótipo montado, a incluir após a fase de integração física da Aula 05.)*

## Arquitetura Específica e Tópicos MQTT

### 1. Telemetria (Dispositivo → Nuvem)
`[PIR HC-SR501]` → `[ESP32]` → `[Wi-Fi]` → `[Broker MQTT]` → **Tópico:** `access/grupo5/sensor/presenca`
- Payload: `{"armado": true, "presenca": false, "disparado": false}`

### 2. Comando Remoto (Nuvem → Dispositivo)
`[Painel/Cliente MQTT]` → `[Broker MQTT]` → **Tópico:** `access/grupo5/comando/alarme` → `[ESP32 - subscrição/callback]`
- Payloads aceitos: `"ativar"` ou `"desativar"`

### 3. Confirmação de Comando (Dispositivo → Nuvem)
`[ESP32 executa o comando]` → **Tópico:** `access/grupo5/status/confirmacao`
- Payload: `{"comando": "ativar", "executado": true}`
- Garante que o painel saiba que o estado remoto foi de fato aplicado no dispositivo (requisito mínimo de N1).

### 4. Alerta Local Autônomo (Offline)
`[PIR = HIGH, sistema armado]` → `[Lógica local ESP32]` → `[Buzzer GPIO23 + LED GPIO4]`
- Funciona independentemente da conexão Wi-Fi/MQTT; o evento é registrado localmente e publicado assim que a conexão for restabelecida (ver reconexão no backlog).

## Backlog Reescrito

| Tarefa | Responsável | Status |
| :--- | :--- | :--- |
| Ler PIR HC-SR501 no Monitor Serial (HIGH/LOW) | Adrian | Feito |
| Ler estado do botão (armar/desarmar) com debounce | Adrian | Feito |
| Montar circuito funcional em protoboard (ESP32 + PIR + botão + buzzer + LED) | Adrian | Feito |
| Acionar buzzer (PWM 1.2kHz) e LED conforme estado local | Lucas | A fazer |
| Conectar ESP32 ao Wi-Fi com rotina de reconexão automática | Gustavo | A fazer |
| Publicar telemetria no tópico `access/grupo5/sensor/presenca` | Miguel Angel Huertas | A fazer |
| Subscrever e tratar comandos no tópico `access/grupo5/comando/alarme` | Leonardo Lotério | A fazer |
| Publicar confirmação de execução no tópico `access/grupo5/status/confirmacao` | Leonardo Lotério | A fazer |
| Implementar buffer/registro local de eventos durante perda de conexão | Gustavo | A fazer |
| Testar reconexão automática (desligar/religar Wi-Fi durante disparo) | Lucas | A fazer |
| Escrever documentação técnica do firmware (`/firmware/README.md`) | Miguel Dufloth | A fazer |
| Gravar demonstração funcional do protótipo (vídeo/apresentação) | Todos | A fazer |
| Criar estrutura inicial de pastas no repositório GitHub | Miguel Dufloth | Feito |
| Preencher documento de aprofundamento (Aula 04) | Miguel Dufloth | Feito |

## Primeiro Risco Técnico (Revisitado)
- **Risco original (Aula 02):** Perda de conexão Wi-Fi/Broker MQTT durante um evento de intrusão/abertura, impedindo a notificação remota do alarme.
- **Status:** **Confirmado e detalhado** à luz da Aula 03 (eletrônica) e Aula 04 (ADC):
  - **Eletrônica (Aula 03):** o botão pushbutton precisa de tratamento de debounce por software (ou hardware, com capacitor), já que leituras instáveis podem gerar falsos comandos de armar/desarmar — risco adicional identificado além do original.
  - **ADC (Aula 04):** o HC-SR501 tem saída digital (não analógica), então não há leitura via ADC neste sensor. Ainda assim, o conteúdo de ADC é relevante caso o projeto evolua para leitura de sensores analógicos (ex: potenciômetro de sensibilidade do PIR) na fase N2/N3.
- **Mitigação mantida:** rotina de reconexão automática no firmware + alarme sonoro/visual local operando de forma autônoma (offline) + registro do evento para reenvio assim que a conexão for reestabelecida.

## Feedback da Aula 04

*(seção intencionalmente não preenchida nesta entrega)*

---
*Documento elaborado a partir do README da Aula 02, com decisões técnicas consolidadas para a Aula 05.*
