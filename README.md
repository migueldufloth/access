# Projeto IoT - Access (Equipe 05 — Controle de Acesso)

## Integrantes
- Lucas Honorato dos Santos
- Miguel Angel Balladares Huertas
- Miguel Angelo Dufloth Filho
- Gustavo Franz
- Leonardo Lotério de Lima
- Adrian Marcio Roth

## Família Temática
Tema da equipe: **Controle de Acesso**. A N1 e a N2/N3 são estágios de escopo crescente
dentro desse mesmo tema, não temas diferentes:
- **N1 (degrau mínimo viável):** Abertura e Presença — detecção de abertura de porta, alerta
  local e notificação remota.
- **N2/N3 (escopo completo):** Controle de Acesso — evolução do mesmo protótipo para o
  controle de acesso completo.

## Problema
A ausência de monitoramento automatizado e em tempo real sobre a abertura de portas, janelas ou compartimentos restritos dificulta a detecção rápida de acessos não autorizados ou intrusões, aumentando o risco de segurança em ambientes residenciais ou corporativos.

## Usuário ou Contexto de Uso
**Contexto:** Ambientes que exigem monitoramento contínuo de acesso e presença, como salas de servidores/TI, escritórios, almoxarifados ou residências.
**Usuário:** Administradores de segurança, gestores de TI ou proprietários que necessitam receber alertas instantâneos de intrusão/abertura de portas e controlar o estado de alarme remotamente.

## Objetivo da N1
Desenvolver um protótipo com ESP32 capaz de detectar abertura de porta via sensor magnético e botão, operar em modos armado/desarmado, emitir um alerta/alarme local (buzzer, display e LED), publicar telemetria via MQTT com Wi-Fi e permitir o acionamento de comandos remotos de controle de estado com confirmação de ação.

> **Nota:** o protótipo evoluiu da Versão 1 (sensor PIR HC-SR501, botão, buzzer e LED — efetivamente montada e testada) para o hardware atual, por duas razões. Primeiro, um integrante da equipe adquiriu um kit com ESP32, display OLED e sensor magnético de contato, tornando esses componentes disponíveis para o projeto. Segundo, o sensor magnético MC-38 responde diretamente ao evento do problema definido — abertura de porta —, enquanto o PIR captava presença no ambiente: dispararia com qualquer movimento na sala e ficaria em silêncio se a porta fosse aberta fora do seu campo de visão. O display OLED, por sua vez, permitiu um dashboard local que a Versão 1 não tinha.

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
- **Comunicação (previsto, ainda não implementado):** Protocolo MQTT encapsulado em rede Wi-Fi padrão 2.4 GHz. A notificação remota atual sai por HTTP (ver [Status Atual da Implementação](#status-atual-da-implementação)).

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

### Versão 1 — concepção inicial

<img width="1138" height="501" alt="image" src="https://github.com/user-attachments/assets/2561c95c-0847-4903-b416-a3a165107052" />

Simulador: https://wokwi.com/projects/473279281421060097

> **Este circuito foi efetivamente montado e testado:** o sensor PIR, o botão e o LED de
> status funcionaram nessa montagem. O desenho e o link acima são o registro dessa concepção
> inicial e não refletem a montagem atual.

**Evolução para a Versão 2** (não uma correção — a Versão 1 funcionou como planejada):
- Um integrante da equipe adquiriu um kit com ESP32, display OLED e sensor magnético de
  contato (MC-38-NA), tornando esses componentes disponíveis para o projeto.
- A equipe migrou para esse conjunto porque o MC-38 responde diretamente ao evento do
  problema definido — abertura de porta —, enquanto o PIR captava presença no ambiente
  (dispararia com qualquer movimento na sala e ficaria em silêncio se a porta fosse aberta
  fora do seu campo de visão); e porque o display OLED permitiu um dashboard local que a
  Versão 1 não tinha.
- Buzzer remanejado para o GPIO 25, com o GPIO 18 mantido em `LOW` como terra virtual.
- LED de status e botão de armar/desarmar ainda não foram reintegrados na Versão 2.

### Versão 2 — implementação atual

Pinagem atual documentada na seção [Instruções de Execução](#instruções-de-execução).

> Diagrama a produzir — novo projeto no Wokwi ainda não criado para esta versão.

## Instruções de Execução
- **IDE:** Arduino IDE ou PlatformIO, com suporte à placa ESP32 (ESP-WROOM-32 / "ESP32 Dev Module").
- **Bibliotecas:** `WiFi.h` (built-in do core ESP32), `Wire.h`, `Adafruit_GFX` e `Adafruit_SSD1306`. O cliente MQTT (`mqtt_client.h`, do ESP-IDF) também vem no core ESP32 e não precisa de instalação. É preciso instalar manualmente a `ArduinoJson` (Benoît Blanchon) pelo Gerenciador de Bibliotecas da Arduino IDE.
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

## Backlog

A coluna **Versão** identifica em qual hardware a tarefa foi feita ou será feita. Uma linha
`v1` marcada como Feito descreve o circuito da concepção inicial, não uma funcionalidade
presente no firmware atual — só linhas `v2` correspondem ao que está em
`firmware/Access/Access.ino` hoje.

### Versão 1 (concluída)

| Tarefa | Versão | Responsável | Prazo | Status |
| :--- | :--- | :--- | :--- | :--- |
| Montar circuito com PIR, botão, buzzer e LED | v1 | Adrian Marcio Roth | — | Feito |
| Ler PIR e estado do botão com debounce no Monitor Serial | v1 | Adrian Marcio Roth | — | Feito |

### Versão 2 (em andamento)

| Tarefa | Versão | Responsável | Prazo | Status |
| :--- | :--- | :--- | :--- | :--- |
| Migrar para o kit com ESP32, OLED e sensor magnético | v2 | Lucas Honorato | — | Feito |
| Ler o MC-38 com detecção de borda | v2 | Adrian Marcio Roth | — | Feito |
| Buzzer com padrões distintos para abertura e fechamento | v2 | Lucas Honorato | — | Feito |
| Dashboard local no OLED (estado, contador, tempo, status de rede) | v2 | Miguel Angelo Dufloth | — | Feito |
| Wi-Fi com log de IP por DHCP e RSSI | v2 | Gustavo Franz | — | Feito |
| Reconexão automática não bloqueante, verificação a cada 5s | v2 | Gustavo Franz | — | Feito |
| Testar reconexão com queda provocada (evidência do CP06) | v2 | Lucas Honorato | — | Feito |
| Reintegrar botão de armar/desarmar na v2 | v2 | Adrian Marcio Roth | 15/09 | A fazer |
| Reintegrar LED vermelho de status na v2 | v2 | Adrian Marcio Roth | 15/09 | A fazer |
| Cliente MQTT (PubSubClient) e conexão ao broker | v2 | Gustavo Franz | 15/09 | A fazer |
| Publicar telemetria em `access/grupo5/sensor/presenca` | v2 | Miguel Angel Balladares | 15/09 | A fazer |
| Subscrever e tratar comandos em `access/grupo5/comando/alarme` | v2 | Leonardo Lotério | 15/09 | A fazer |
| Publicar confirmação em `access/grupo5/status/confirmacao` | v2 | Leonardo Lotério | 15/09 | A fazer |
| Mover a decisão de disparo do alarme para fora do `loop()`, passando pela mensageria | v2 | Gustavo Franz | 15/09 | A fazer |
| Reconexão do broker MQTT, separada da reconexão do Wi-Fi | v2 | Gustavo Franz | 15/09 | A fazer |
| Validar leitura do sensor antes de publicar | v2 | Miguel Angel Balladares | 15/09 | A fazer |
| Diagrama do circuito v2 e novo projeto no Wokwi | v2 | Adrian Marcio Roth | 15/09 | A fazer |
| Documentação técnica do firmware (`firmware/README.md`) | v2 | Miguel Angelo Dufloth | 20/09 | A fazer |
| Ensaio da demonstração e da validação individual | v2 | Todos | 20/09 | A fazer |

> [`docs/aprofundamento-aula04.md`](docs/aprofundamento-aula04.md) registra o planejamento e
> os status da Versão 1 no momento da Aula 04 — é um registro histórico, não atualizado desde
> então. O backlog vigente do projeto é o desta seção.

## Primeiro Risco Técnico
- **Risco:** Perda de conexão Wi-Fi/Broker MQTT durante um evento de intrusão/abertura de porta, impedindo a notificação remota do alarme.
- **Mitigação/Investigação:** Implementar rotinas de reconexão automática no firmware do ESP32 e garantir que o alarme sonoro/visual local continue operando de forma autônoma (lógica offline), registrando o evento assim que a conexão for reestabelecida.

## Dúvidas para o Professor
Nenhuma pendente no momento.
