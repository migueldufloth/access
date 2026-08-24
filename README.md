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
Desenvolver um protótipo com ESP32 capaz de detectar o estado de abertura (aberto/fechado) via sensor (Reed Switch ou Botão), operar em modos armado/desarmado, emitir um alerta/alarme local (Buzzer/LED), publicar telemetria via MQTT com Wi-Fi e permitir o acionamento de comandos remotos de controle de estado com confirmação de ação.

## Especificação de Hardware
- **Microcontrolador:** Placa de desenvolvimento ESP32 Kit V1 ESP-WROOM-32, Dual-Core 240 MHz, Wi-Fi 802.11 b/g/n e Bluetooth BLE integrados.
- **Sensores:**
  - **Sensor de Presença:** Sensor Piroelétrico PIR modelo HC-SR501.
  - **Sensor/Controle Manual:** Botão Pushbutton táctil 4 terminais 6x6x5 mm.
- **Atuadores e Alertas:**
  - **Alerta Sonoro:** Buzzer Piezoelétrico 5V (geração de frequências e tons sonoros de alarme via PWM).
  - **Alerta Visual:** LED Difuso Vermelho (indicação visual de status: aceso para Ativado/Desativado).
- **Componentes Complementares e Proteção:**
  - **Resistor Limitador:** Resistor de filme de carbono de 220 Ω em série com o terminal catodo do LED para controle de corrente.
  - **Conexões e Prototipagem:** Mini Protoboard e conjunto de cabos jumpers (Macho-Macho / Macho-Fêmea).
- **Alimentação:**
  - Entrada primária de 5V DC via conector micro-USB (fonte externa/computador).
- **Comunicação:** Protocolo MQTT encapsulado em rede Wi-Fi padrão 2.4 GHz.

## Arquitetura 

Fluxo de Telemetria e Detecção (Entrada ➔ Nuvem):
`[Sensores (Botão / PIR)]` ➔ `[ESP32 (Leitura GPIO / Lógica Local)]` ➔ `[Rede Wi-Fi]` ➔ `[Broker MQTT (Publicação de Status)]` ➔ `[Dashboard / Painel do Usuário]`

Fluxo de Comando Remoto e Controle (Nuvem ➔ Ação):
`Painel / Cliente MQTT]` ➔ `[Broker MQTT (Tópico de Comando)]` ➔ `[ESP32 (Subscrição / Callback)]` ➔ `[Alteração de Estado (Armar / Desarmar)]`

Fluxo de Alerta Local (Autonomia / Segurança Offline):
`[Evento de Abertura / Intrusão]` ➔ `[ESP32 (Processamento Local)]` ➔ `[Atuadores / Alertas Locais (Buzzer Sonoro / LED de Status)]`

## Protótipo do Circuito

![Esquema do Circuito]

<img width="1138" height="501" alt="image" src="https://github.com/user-attachments/assets/2561c95c-0847-4903-b416-a3a165107052" />

Simulador: https://wokwi.com/projects/473279281421060097

## Backlog Inicial

| Tarefa | Responsável | Status |
| :--- | :--- | :--- |
| Criar repositório no GitHub | Miguel Dufloth | Feito |
| Preencher README inicial | [NA] | A fazer |
| Testar ESP32 com LED e Buzzer | Gustavo | A fazer |
| Validar a leitura do Reed Switch / Botão no Monitor Serial | [NA] | A fazer |
| Configurar conexão Wi-Fi e publicação/subscrição MQTT | [NA] | A fazer |
| Desenhar diagrama de circuito e arquitetura inicial | Adrian | Feito |
| Registrar e investigar o primeiro risco técnico | Lucas | A fazer |

## Primeiro Risco Técnico
- **Risco:** Perda de conexão Wi-Fi/Broker MQTT durante um evento de intrusão/abertura de porta, impedindo a notificação remota do alarme.
- **Mitigação/Investigação:** Implementar rotinas de reconexão automática no firmware do ESP32 e garantir que o alarme sonoro/visual local continue operando de forma autônoma (lógica offline), registrando o evento assim que a conexão for reestabelecida.

## Dúvidas para o Professor
1. Para a simulação da N1, o uso do Reed Switch combinado com um botão simples é suficiente para simular abertura de porta e rearme do sistema?
2. Há alguma recomendação específica de Broker MQTT para utilizarmos nos testes de laboratório da disciplina?
