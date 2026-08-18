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

## Componentes Previstos
- **Microcontrolador:** ESP32 (Wi-Fi integrado)
- **Sensores:** Sensor Magnético Reed Switch (ou botão de simulação) para detectar abertura/fechamento; Sensor de presença PIR (opcional/complementar)
- **Atuadores/Alertas:** Buzzer sonoro e LED indicador de status (Armado/Desarmado/Disparado)
- **Comunicação:** Protocolo MQTT e Wi-Fi

## Arquitetura Inicial
`[Sensor (Reed Switch / Botão)]` ➔ `[ESP32 (Firmware/Lógica Local)]` ➔ `[Wi-Fi / Broker MQTT]` ➔ `[Cliente/Painel MQTT (Telemetria/Comandos)]`
`[Comando Remoto MQTT]` ➔ `[ESP32]` ➔ `[Atuador/Alerta (Buzzer/LED)]`

## Backlog Inicial

| Tarefa | Responsável | Status |
| :--- | :--- | :--- |
| Criar repositório no GitHub | Miguel Dufloth | Feito |
| Preencher README inicial | [NA] | A fazer |
| Testar ESP32 com LED e Buzzer | Gustavo | A fazer |
| Validar a leitura do Reed Switch / Botão no Monitor Serial | Gustavo | A fazer |
| Configurar conexão Wi-Fi e publicação/subscrição MQTT | [NA] | A fazer |
| Desenhar diagrama de circuito e arquitetura inicial | [NA] | A fazer |
| Registrar e investigar o primeiro risco técnico | Lucas | A fazer |

## Primeiro Risco Técnico
- **Risco:** Perda de conexão Wi-Fi/Broker MQTT durante um evento de intrusão/abertura de porta, impedindo a notificação remota do alarme.
- **Mitigação/Investigação:** Implementar rotinas de reconexão automática no firmware do ESP32 e garantir que o alarme sonoro/visual local continue operando de forma autônoma (lógica offline), registrando o evento assim que a conexão for reestabelecida.

## Dúvidas para o Professor
1. Para a simulação da N1, o uso do Reed Switch combinado com um botão simples é suficiente para simular abertura de porta e rearme do sistema?
2. Há alguma recomendação específica de Broker MQTT para utilizarmos nos testes de laboratório da disciplina?
