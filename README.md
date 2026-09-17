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
- **Comunicação:** MQTT sobre WebSocket Secure (`wss://`, porta 443, TLS) em rede Wi-Fi padrão
  2.4 GHz, via `esp_mqtt_client` do ESP-IDF. Publica telemetria, assina comando remoto e publica
  confirmação de execução (ver [Arquitetura do Sistema e Comunicação MQTT](#arquitetura-do-sistema-e-comunicação-mqtt)).

## Estrutura do Repositório
- `/firmware/Access` — código-fonte do ESP32 (sketch da Arduino IDE)
- `/hardware` — esquemáticos, datasheets e especificação de componentes
- `/docs` — documentação do projeto (entregas por aula)
- `/broker` — config do broker MQTT (Mosquitto) e do proxy nginx que atendem
  `access/grupo5/#` em produção (ver [broker/README.md](broker/README.md))

## Status Atual da Implementação

| Elemento | Situação |
| :--- | :--- |
| Leitura do sensor magnético com detecção de borda | Implementado |
| Conexão Wi-Fi com DHCP, log de IP e RSSI | Implementado |
| Reconexão automática não bloqueante do Wi-Fi | Implementado e testado |
| Sincronização de hora via SNTP antes do TLS | Implementado |
| Conexão MQTT sobre WebSocket Secure (`wss://`), TLS, Last Will | Implementado |
| Telemetria periódica e por evento em `sensor/presenca` | Implementado |
| Comando remoto (`armar`/`desarmar`/`disparar`/`silenciar`) | Implementado |
| Confirmação de comando em `status/confirmacao` | Implementado |
| Alarme acionado pela decisão remota (`alarmeDisparado`), não mais pela leitura direta do sensor no `loop()` | Implementado |
| Modo degradado (decide localmente sem contato com o broker por 30s) | Implementado |
| Dashboard web (`broker/www/index.html`) sobre `mqtt.js`/`wss` | Implementado |
| Notificação HTTP via `ntfy.sh` | Removida (substituída pelo fluxo MQTT acima) |
| Leitura validada do sensor (debounce por tempo, `lerSensorValidado()`) | Implementado |
| Reorganização do firmware em múltiplos arquivos (hoje é `Access.ino` + `config.h`) | A fazer |
| Modo armado/desarmado por botão físico e LED de status | A fazer |
| `docs/infraestrutura-broker.md` e `docs/broker-contingencia.md` dedicados | A fazer (ver [broker/README.md](broker/README.md) enquanto isso) |

## Arquitetura do Sistema e Comunicação MQTT

`MC-38 → ESP32 → Wi-Fi → wss/443 → nginx → Mosquitto → dashboard → comando → ESP32 → buzzer → confirmação`

> **Conectividade:** o ESP32 não fica na mesma rede do broker, e o broker só expõe MQTT puro
> (`1883`) para LAN/Tailscale — a internet só tem acesso à `443`. Por isso o firmware conecta via
> **MQTT sobre WebSocket com TLS** (`wss://accessgrupo5.duckdns.org/mqtt`), usando o cliente
> `esp_mqtt_client` do ESP-IDF em vez de `PubSubClient` puro. Detalhes em
> [broker/README.md](broker/README.md#conectividade-externa-esp32-em-rede-diferente-do-broker).

> **TLS:** a validação do certificado usa o bundle de CAs embutido no ESP-IDF
> (`esp_crt_bundle_attach`), que já inclui a cadeia da Let's Encrypt. `firmware/Access/cert_isrg_root_x1.h`
> (certificado ISRG Root X1 pinado) foi preparado como alternativa mais restrita, mas não está em
> uso no firmware atual — fica como referência para quem quiser trocar a verificação por pinning.

### 1. Fluxo de Telemetria (Dispositivo ➔ Nuvem)
`[Sensor magnético MC-38]` ➔ `[ESP32]` ➔ `[Wi-Fi]` ➔ `[Broker MQTT via wss]` ➔ **Tópico:**
`access/grupo5/sensor/presenca` ➔ `[Dashboard]`
* **Quando publica:** a cada mudança de estado da porta e periodicamente a cada 5s.
* **Payload (JSON):** `{"porta":"aberta","armado":true,"disparado":false,"aberturas":12,"rssi":-52}`

---

### 2. Fluxo de Controle Remoto (Nuvem ➔ Dispositivo)
`[Dashboard]` ➔ **Tópico:** `access/grupo5/comando/alarme` ➔ `[Broker MQTT]` ➔ `[ESP32 — MQTT_EVENT_DATA]` ➔ `[sistemaArmado / alarmeDisparado]` ➔ `[Buzzer]`
* **Payloads aceitos:** `armar` \| `desarmar` \| `disparar` \| `silenciar`.
* `disparar` só é aplicado com o sistema armado; caso contrário a confirmação vem com `aplicado:false` e o buzzer não soa.
* O ESP32 confirma a execução publicando em `access/grupo5/status/confirmacao`:
  `{"comando":"disparar","aplicado":true,"armado":true,"disparado":true}`

---

### 3. Fluxo de Alerta Local (Modo Degradado)
`[Sem contato com o broker por 30s]` ➔ `[avaliarModoDegradado()]` ➔ `[Lógica local: leitura direta do sensor]` ➔ `[Buzzer + OLED]`

Fora dessa situação, o buzzer **não** reage à leitura direta do sensor — só a
`alarmeDisparado`, que só o tratamento de comando MQTT escreve. Ver [Modo Degradado](#modo-degradado).

### Contrato de Tópicos

Prefixo `access/grupo5/`, definido como constantes em `firmware/Access/config.h` e replicado em
`broker/www/index.html` e no ACL do broker (`broker/mosquitto/acl`).

| Tópico | Direção | Payload |
| :--- | :--- | :--- |
| `sensor/presenca` | ESP32 → broker | `{"porta":"aberta","armado":true,"disparado":false,"aberturas":12,"rssi":-52}` |
| `comando/alarme` | dashboard → ESP32 | `armar` \| `desarmar` \| `disparar` \| `silenciar` |
| `status/confirmacao` | ESP32 → broker | `{"comando":"disparar","aplicado":true,"armado":true,"disparado":true}` |
| `status/presenca_dispositivo` | Last Will, ESP32 → broker | `online` \| `offline`, retido |

## Modo Degradado

Sem laço de reconexão manual do broker — o cliente `esp_mqtt_client` já reconecta sozinho. O que
o firmware acompanha é o **tempo sem contato**: cada evento de conexão e cada publicação (telemetria
ou confirmação) atualiza `ultimoContatoBroker`.

- Sem contato por mais de `TIMEOUT_MODO_DEGRADADO` (30s), `avaliarModoDegradado()` liga o modo
  degradado: o alarme volta a ser decidido pela leitura direta do sensor (`alarmeDisparado =
  estadoAtual`), exatamente como na v2. O log serial registra a transição e a barra de status do
  OLED mostra `[LOCAL]`.
- Quando o broker volta a responder, a próxima leitura de `avaliarModoDegradado()` desliga o modo
  degradado automaticamente e a decisão volta a vir do comando remoto.
- É a única exceção à regra de que `alarmeDisparado` só é escrita pelo tratamento de comando MQTT.

Isso preserva a autonomia local do alarme sem transformá-la na arquitetura padrão: fora de uma
falha real de rede, quem decide acionar o buzzer é o comando recebido via MQTT, não o `loop()`.

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

### Versão 2 — hardware atual

Pinagem atual documentada na seção [Instruções de Execução](#instruções-de-execução).

> Diagrama a produzir — novo projeto no Wokwi ainda não criado para esta versão.

### Versão 3 — MQTT sobre WSS (implementação atual)

Mesmo hardware e pinagem da Versão 2 — a v3 não muda um fio, muda o firmware e adiciona
infraestrutura de servidor:

- Sai a notificação HTTP para o `ntfy.sh`; entra um cliente MQTT (`esp_mqtt_client`, ESP-IDF)
  falando `wss://` com o broker.
- A decisão de acionar o buzzer deixa de ser lida direto do sensor dentro do `loop()` e passa a
  vir de um comando recebido pela rede (`alarmeDisparado`), com exceção do [modo
  degradado](#modo-degradado).
- Broker Mosquitto + proxy nginx provisionados e versionados em [`broker/`](broker/README.md), e
  um [painel web](broker/www/index.html) substitui o acompanhamento só pelo Monitor Serial/OLED.

## Instruções de Execução
- **IDE:** Arduino IDE ou PlatformIO, com suporte à placa ESP32 (ESP-WROOM-32 / "ESP32 Dev Module").
- **Bibliotecas:** `WiFi.h`, `Wire.h`, `Adafruit_GFX` e `Adafruit_SSD1306` (built-in/instaláveis
  pelo core ESP32 de sempre). O cliente MQTT (`mqtt_client.h`/`esp_mqtt_client`) e a verificação
  TLS (`esp_crt_bundle.h`/`esp_crt_bundle_attach`), ambos do ESP-IDF, também vêm no core
  Arduino-ESP32 e não precisam de instalação — é o `esp_mqtt_client` que entra no lugar do
  `PubSubClient`, porque o broker só é alcançável de fora via `wss://`, que `PubSubClient` não
  suporta (ver [Arquitetura do Sistema e Comunicação MQTT](#arquitetura-do-sistema-e-comunicação-mqtt)).
  É preciso instalar manualmente a `ArduinoJson` (Benoît Blanchon) pelo Gerenciador de
  Bibliotecas da Arduino IDE.
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
- **Credenciais:** copie `firmware/Access/secrets.example.h` para `secrets.h` (mesma pasta) e
  preencha `WIFI_SSID`, `WIFI_PASS`, `MQTT_URI` (`wss://accessgrupo5.duckdns.org/mqtt`),
  `MQTT_USER` e `MQTT_PASS` com os valores reais. `secrets.h` está no `.gitignore` e não é
  versionado.
- **Sincronização de hora:** o firmware sincroniza a hora por SNTP logo após conectar o Wi-Fi e
  **antes** de iniciar o cliente MQTT — a verificação do certificado TLS falha se o relógio ainda
  estiver em 1970. Isso é automático (`sincronizarHora()`), não precisa de ação manual, mas é a
  causa mais comum de erro de TLS se algo for alterado nessa ordem.
- **Como rodar o firmware:** abrir `firmware/Access/Access.ino` na IDE, criar o `secrets.h`,
  selecionar a placa "ESP32 Dev Module", gravar e abrir o Monitor Serial (115200 baud) para
  acompanhar logs de leitura, Wi-Fi, hora e conexão MQTT.
- **Como abrir a dashboard:** em produção já está publicada pelo nginx no mesmo domínio do
  broker. Para rodar localmente, copie `broker/www/config.example.js` para `broker/www/config.js`
  com o usuário/senha reais do `access_web` (esse `config.js` está no `.gitignore` e nunca deve
  ser versionado) e abra `broker/www/index.html` no navegador.
- **Validação da leitura:** `lerSensorValidado()` só aceita uma mudança de estado do MC-38 como real depois que o pino ficar estável por `DEBOUNCE_SENSOR_MS` (50 ms, ajustável em `config.h`) — debounce por tempo, não por contagem de amostras, filtrando bounce mecânico do reed switch e ruído elétrico antes de contar aberturas ou publicar telemetria.

## Segurança

- **Autenticação:** `allow_anonymous false` no broker — todo cliente precisa de usuário/senha
  (`access_esp32` para o dispositivo, `access_web` para a dashboard).
- **Autorização por ACL:** cada usuário só tem as permissões que o seu papel exige (ver
  [broker/mosquitto/acl](broker/mosquitto/acl)). `access_esp32` escreve telemetria/status e lê
  comando; `access_web` é o inverso — não consegue forjar telemetria, só publicar comando (o
  mesmo que qualquer operador legítimo da dashboard já pode fazer).
- **TLS:** toda conexão externa (ESP32 e dashboard) passa por `wss://` na porta 443, com
  certificado Let's Encrypt terminado no nginx. MQTT puro (`1883`) só é alcançável em LAN/Tailscale,
  nunca pela internet.
- **Credenciais fora do repositório:** `secrets.h` (Wi-Fi e broker) e `broker/www/config.js`
  (dashboard) estão no `.gitignore`; só os arquivos `*.example.*` correspondentes, sem valor
  real, são versionados.
- **Decisão consciente de escopo:** TLS mútuo (autenticação do dispositivo por certificado
  próprio, não só usuário/senha) fica para N3 — o ganho de segurança adicional não se justifica
  ainda no volume de dispositivos deste protótipo. Detalhes da infraestrutura em
  [broker/README.md](broker/README.md).

## Enquadramento IoT

Como cada item do checklist do edital é atendido no código:

| # | Item | Onde |
| :--- | :--- | :--- |
| 1 | Sensor físico real | Sensor magnético MC-38 (`REED_PIN`, `digitalRead`) |
| 2 | Atuador físico real | Buzzer piezoelétrico (`BUZZER_POS`, `tone`/`noTone`) |
| 3 | A leitura sai do dispositivo pela rede antes da decisão de atuação | O `loop()` aciona o buzzer a partir de `alarmeDisparado`, escrita só pelo comando MQTT — nunca pela leitura direta do sensor. Única exceção **declarada**: modo degradado, sem broker por 30s (ver [Modo Degradado](#modo-degradado)) |
| 4 | Mensageria MQTT, com tópico publicado e assinado | Publica em `sensor/presenca` e `status/*`; assina `comando/alarme` (`iniciarMqtt`, `mqttEventHandler`) |
| 5 | Comando remoto altera o estado do atuador | `tratarComando` altera `sistemaArmado`/`alarmeDisparado`; o buzzer físico responde a essa mudança no `loop()`, não a um clique local |
| 6 | Confirmação da ação executada | `publicarConfirmacao` em `status/confirmacao`, com `aplicado:false` quando o comando é recusado (ex.: `disparar` desarmado) |
| 7 | Telemetria ao longo do tempo | Publicação periódica a cada 5s (`INTERVALO_TELEMETRIA`) além da publicação por evento |

## Backlog

A coluna **Versão** identifica em qual hardware/software a tarefa foi feita ou será feita. Uma
linha `v1` marcada como Feito descreve o circuito da concepção inicial, não uma funcionalidade
presente no firmware atual. As linhas `v2` e `v3` juntas correspondem ao que está em
`firmware/Access/Access.ino` hoje — a v3 não trocou hardware, só o transporte de rede e a lógica
de decisão do alarme (ver [Protótipo do Circuito](#versão-3--mqtt-sobre-wss-implementação-atual)).

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
| Cliente MQTT (`esp_mqtt_client` via `wss://`, não `PubSubClient`) e conexão ao broker | v2 | Gustavo Franz | 15/09 | Feito |
| Publicar telemetria em `access/grupo5/sensor/presenca` | v2 | Miguel Angel Balladares | 15/09 | Feito |
| Subscrever e tratar comandos em `access/grupo5/comando/alarme` | v2 | Leonardo Lotério | 15/09 | Feito |
| Publicar confirmação em `access/grupo5/status/confirmacao` | v2 | Leonardo Lotério | 15/09 | Feito |
| Mover a decisão de disparo do alarme para fora do `loop()`, passando pela mensageria | v2 | Gustavo Franz | 15/09 | Feito |
| Reconexão do broker MQTT, separada da reconexão do Wi-Fi | v2 | Gustavo Franz | 15/09 | Feito (automática, embutida no `esp_mqtt_client`) |
| Validar leitura do sensor antes de publicar | v2 | Miguel Angel Balladares | 15/09 | Feito |
| Diagrama do circuito v2 e novo projeto no Wokwi | v2 | Adrian Marcio Roth | 15/09 | A fazer |
| Documentação técnica do firmware (`firmware/README.md`) | v2 | Miguel Angelo Dufloth | 20/09 | A fazer |
| Ensaio da demonstração e da validação individual | v2 | Todos | 20/09 | A fazer |

### Versão 3 (em andamento)

| Tarefa | Versão | Responsável | Prazo | Status |
| :--- | :--- | :--- | :--- | :--- |
| Provisionar broker Mosquitto com autenticação e ACL | v3 | Miguel Angelo Dufloth Filho | — | Feito |
| Publicar o broker por `wss` com TLS e proxy reverso nginx | v3 | Miguel Angelo Dufloth Filho | — | Feito |
| Definir e aplicar o contrato de tópicos no ACL | v3 | Miguel Angelo Dufloth Filho | — | Feito |
| Sincronizar hora via SNTP antes do TLS | v3 | Lucas Honorato | — | Feito |
| Migrar o firmware de HTTP/`ntfy` para MQTT sobre `wss` | v3 | Lucas Honorato | — | Feito |
| Modo degradado (decide localmente sem broker por 30s) | v3 | Gustavo Franz | — | Feito |
| Remover `delay(30)` do fim do `loop()`, OLED com cadência própria | v3 | Gustavo Franz | — | Feito |
| Painel web de telemetria (`broker/www/index.html`) sobre `mqtt.js`/`wss` | v3 | Miguel Angelo Dufloth Filho | — | Feito |
| Reorganizar o firmware em múltiplos arquivos (rede/sensor/alarme/display) | v3 | — | 20/09 | A fazer |
| `docs/infraestrutura-broker.md` e `docs/broker-contingencia.md` dedicados | v3 | Miguel Angelo Dufloth Filho | 20/09 | A fazer |
| Atualizador de IP do DuckDNS no servidor | v3 | Miguel Angelo Dufloth Filho | 20/09 | A fazer |
| Tag `v3` | v3 | Miguel Angelo Dufloth Filho | — | A fazer |

> [`docs/aprofundamento-aula04.md`](docs/aprofundamento-aula04.md) registra o planejamento e
> os status da Versão 1 no momento da Aula 04 — é um registro histórico, não atualizado desde
> então. [`docs/roteiro-mqtt-lucas.md`](docs/roteiro-mqtt-lucas.md) registra o roteiro passado
> para a migração do firmware para MQTT. O backlog vigente do projeto é o desta seção.

## Primeiro Risco Técnico
- **Risco:** Perda de conexão Wi-Fi/Broker MQTT durante um evento de intrusão/abertura de porta, impedindo a notificação remota do alarme.
- **Mitigação:** implementada. Reconexão automática do Wi-Fi (`verificarReconexaoWiFi`) e do
  broker MQTT (embutida no `esp_mqtt_client`), e [modo degradado](#modo-degradado): sem contato
  com o broker por 30s, o alarme volta a ser decidido localmente pelo sensor, sem depender da
  rede para continuar funcionando.

## Documentação

- [`broker/README.md`](broker/README.md) — estrutura, deploy e teste do broker Mosquitto/nginx.
- [`docs/roteiro-mqtt-lucas.md`](docs/roteiro-mqtt-lucas.md) — roteiro técnico usado para migrar
  o firmware de HTTP para MQTT.
- [`docs/aprofundamento-aula04.md`](docs/aprofundamento-aula04.md) — registro histórico da Aula 04
  (Versão 1), não atualizado desde então.

## Dúvidas para o Professor
Nenhuma pendente no momento.
