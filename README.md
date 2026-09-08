# Projeto IoT - Access (Grupo 5)

## Integrantes
- Lucas Honorato dos Santos
- Miguel Angel Huertas
- Miguel Dufloth
- Gustavo Franz
- Leonardo Lotério
- Adrian Roth

## Família Temática
- **Projeto Médio (N1):** Abertura e Presença
- **Projeto Avançado (N2/N3):** Controle de Acesso

## Problema
A ausência de monitoramento automatizado e em tempo real sobre a abertura de portas, janelas ou compartimentos restritos dificulta a detecção rápida de acessos não autorizados ou intrusões, aumentando o risco de segurança em ambientes residenciais ou corporativos.

## Usuário ou Contexto de Uso
**Contexto:** Ambientes que exigem monitoramento contínuo de acesso e presença, como salas de servidores/TI, escritórios, almoxarifados ou residências.
**Usuário:** Administradores de segurança, gestores de TI ou proprietários que necessitam receber alertas instantâneos de intrusão/abertura de portas e controlar o estado de alarme remotamente.

## Objetivo da N1
Desenvolver um protótipo com ESP32 capaz de detectar abertura de porta via sensor magnético e botão, operar em modos armado/desarmado, emitir um alerta/alarme local (buzzer, display e LED), publicar telemetria via MQTT com Wi-Fi e permitir o acionamento de comandos remotos de controle de estado com confirmação de ação.

> **Nota:** o sensor de presença PIR HC-SR501, previsto na concepção inicial, foi substituído pelo sensor magnético de contato MC-38. O problema definido é abertura de porta, e presença no ambiente não é o mesmo evento: o PIR dispararia com qualquer movimento na sala e ficaria em silêncio se a porta fosse aberta fora do seu campo de visão.

## Especificação de Hardware
- **Microcontrolador:** Placa de desenvolvimento ESP32 Kit V1 ESP-WROOM-32, Dual-Core 240 MHz, Wi-Fi 802.11 b/g/n e Bluetooth BLE integrados.
- **Sensores:**
  - **Sensor de Abertura:** Sensor magnético de contato (reed switch) MC-38-NA.
  - **Controle Manual para Ligar/Desligar Sistema:** Botão Pushbutton táctil 4 terminais 6x6x5 mm.
- **Atuadores e Alertas:**
  - **Alerta Sonoro:** Buzzer Piezoelétrico 5V (geração de frequências e tons sonoros de alarme via PWM, ~1.2kHz).
  - **Alerta Visual:** LED Difuso Vermelho (indicação visual de status: aceso para Ativado/Desativado).
  - **Display:** Display OLED I2C SSD1306 128x64 (dashboard local: estado da porta, contador de aberturas, tempo desde a última mudança e status da conexão Wi-Fi).
- **Componentes Complementares e Proteção:**
  - **Resistor Limitador:** Resistor de filme de carbono de 220 Ω em série com o terminal catodo do LED para controle de corrente.
  - **Conexões e Prototipagem:** Mini Protoboard e conjunto de cabos jumpers (Macho-Macho / Macho-Fêmea).
- **Alimentação:**
  - Entrada primária de 5V DC via conector micro-USB (fonte externa/computador).
- **Comunicação:** Protocolo MQTT encapsulado em rede Wi-Fi padrão 2.4 GHz.

## Estrutura do Repositório
- `/firmware/Access` — código-fonte do ESP32 (sketch da Arduino IDE)
- `/hardware` — esquemáticos, datasheets e especificação de componentes
- `/docs` — documentação do projeto (entregas por aula)

## Status Atual da Implementação

| Elemento | Situação |
| :--- | :--- |
| Leitura do sensor magnético com detecção de borda | Implementado |
| Alarme local (buzzer) e dashboard no OLED | Implementado |
| Conexão Wi-Fi com DHCP, log de IP e RSSI | Implementado |
| Reconexão automática não bloqueante | Implementado e testado |
| Notificação remota de abertura | Implementado via HTTP (`ntfy.sh`), provisório |
| Telemetria via MQTT | A implementar |
| Tópico de comando e confirmação de estado | A implementar |
| Modo armado/desarmado e botão físico | A implementar |
| LED de status | A implementar |

## Arquitetura do Sistema e Comunicação MQTT

> **Esta seção descreve a arquitetura-alvo, ainda não implementada.** O firmware atual não possui cliente MQTT: a notificação de abertura sai por HTTP para o `ntfy.sh`. Os contratos de tópicos abaixo são o desenho a ser implementado.

### 1. Fluxo de Telemetria e Detecção (Dispositivo ➔ Nuvem)
`[Sensor Magnético / Botão]` ➔ `[ESP32 (Processamento Local)]` ➔ `[Wi-Fi]` ➔ `[Broker MQTT]` ➔ **Tópico:** `access/grupo5/sensor/presenca` ➔ `[Dashboard / Painel]`
* **Payload Publicado (JSON):** `{"armado": true, "presenca": false, "disparado": false}`

---

### 2. Fluxo de Controle Remoto (Nuvem ➔ Dispositivo)
`[Painel / Cliente MQTT]` ➔ `[Broker MQTT]` ➔ **Tópico:** `access/grupo5/comando/alarme` ➔ `[ESP32 (Subscrição / Callback)]` ➔ `[Alterar Estado: Ativado / Desativado]`
* **Payloads Aceitos:** `"ativar"` ou `"desativar"`
* O ESP32 confirma a execução publicando em `access/grupo5/status/confirmacao`.

---

### 3. Fluxo de Alerta Local (Autonomia Offline)
`[Evento de Abertura (reed switch aberto)]` ➔ `[Lógica Local ESP32]` ➔ `[Atuadores Locais: Buzzer (GPIO 25) + Display OLED]`

## Protótipo do Circuito

<img width="1138" height="501" alt="image" src="https://github.com/user-attachments/assets/2561c95c-0847-4903-b416-a3a165107052" />

Simulador: https://wokwi.com/projects/473279281421060097

## Instruções de Execução
- **IDE:** Arduino IDE ou PlatformIO, com suporte à placa ESP32 (ESP-WROOM-32 / "ESP32 Dev Module").
- **Bibliotecas:** `WiFi.h` e `HTTPClient.h` (built-in do core ESP32), `Wire.h`, `Adafruit_GFX` e `Adafruit_SSD1306`. `PubSubClient` entra quando o MQTT for implementado.
- **Pinagem (conforme `firmware/Access/Access.ino`):**
  | Componente | Pino |
  | :--- | :--- |
  | Sensor magnético MC-38 | GPIO 4 (`INPUT_PULLUP`) |
  | Buzzer (+) | GPIO 25 |
  | Buzzer (terra virtual) | GPIO 18 (mantido em `LOW`) |
  | Display OLED — SDA | GPIO 21 |
  | Display OLED — SCL | GPIO 22 |
  | LED de status | não implementado |
  | Botão armar/desarmar | não implementado |
- **Credenciais:** o firmware espera um arquivo `secrets.h` ao lado do sketch, definindo `WIFI_SSID`, `WIFI_PASS` e `NTFY_TOPIC`. Esse arquivo está no `.gitignore` e não é versionado.
- **Como rodar:** abrir `firmware/Access/Access.ino` na IDE, criar o `secrets.h`, selecionar a placa "ESP32 Dev Module", gravar e abrir o Monitor Serial (115200 baud) para acompanhar logs de leitura e de conexão Wi-Fi.
- **Validação da leitura:** o firmware deve descartar/ignorar leituras implausíveis do sensor antes de publicar telemetria (ver backlog).

## Backlog Inicial

| Tarefa | Responsável | Status |
| :--- | :--- | :--- |
| Criar repositório no GitHub | Miguel Dufloth | Feito |
| Montar circuito (ESP32 + MC-38 + buzzer + OLED) | Adrian | Feito |
| Conectar Wi-Fi com reconexão automática | Gustavo, Leonardo Lotério, Miguel Angel Huertas | Feito |
| Comunicação MQTT (telemetria, comando, confirmação) | Gustavo, Leonardo Lotério, Miguel Angel Huertas | A fazer |
| Testes de alarme local e reconexão | Lucas | Feito |
| Documentação técnica do firmware | Miguel Dufloth | A fazer |
| Demonstração funcional final | Todos | A fazer |

> Backlog detalhado, tarefa a tarefa, na entrega da Aula 04: [`docs/aprofundamento-aula04.md`](docs/aprofundamento-aula04.md).

## Primeiro Risco Técnico
- **Risco:** Perda de conexão Wi-Fi/Broker MQTT durante um evento de intrusão/abertura de porta, impedindo a notificação remota do alarme.
- **Mitigação/Investigação:** Implementar rotinas de reconexão automática no firmware do ESP32 e garantir que o alarme sonoro/visual local continue operando de forma autônoma (lógica offline), registrando o evento assim que a conexão for reestabelecida.

## Dúvidas para o Professor
Nenhuma pendente no momento.
