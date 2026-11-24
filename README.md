# Access — monitoramento de acessos com alarme e aviso no celular

**Equipe 05 · Internet das Coisas 2026/2 · Família temática: Controle de Acesso**

O Access avisa, em segundos, quando uma porta é aberta ou há movimento num ambiente protegido.
Com o sistema armado, a sirene local toca mesmo sem internet; se ninguém reagir, o servidor manda
um push para o celular e dispara as sirenes de todos os dispositivos do local. Tudo fica registrado:
quem armou, quando a porta abriu, quanto tempo o alarme tocou.

## Problema e usuário
- **Problema:** sem monitoramento automatizado e em tempo real, a abertura de portas, janelas ou
  compartimentos restritos passa despercebida, e intrusões são detectadas tarde.
- **Usuário:** responsáveis por salas de servidores, almoxarifados, escritórios ou residências que
  precisam ser avisados de uma intrusão e controlar o alarme à distância.
  O **administrador** arma, desarma e configura; o **operador** acompanha o painel.

## Integrantes
| Integrante | GitHub | Responsabilidade na N3 |
| :--- | :--- | :--- |
| Lucas Honorato dos Santos | `honoratoog` | Hardware A e B; firmware de rede (Wi-Fi, SNTP, MQTT) |
| Adrian Marcio Roth | `adrianrothh` | Montagem e medição em bancada; caixa de acabamento |
| Gustavo Franz | `Gusgb4` | Máquina de estados, NVS, estado seguro e OTA no firmware |
| Leonardo Lotério de Lima | `leonardosth` | Testes automatizados e CI; cenários de falha |
| Miguel Angel Balladares Huertas | `miguettohuertas` | Backend: validação, persistência, API e ciclo de comando |
| Miguel Angelo Dufloth Filho | `migueldufloth` | Compose e broker; interface, analítico, notificação e regra remota; release |

## Arquitetura

```text
 Central da Porta (ESP32-S3)          Satélite de Movimento (ESP32)
 porta-01: reed MC-38, sirene I2S     janela-01: PIR, LED de alerta
        │   ╲________ ESP-NOW (alarme local, sem rede) ________╱   │
        └──────────── MQTT (contrato v1, wss:// na 443) ───────────┘
                                   │
                   ┌──────── docker compose ────────┐
                   │  mosquitto ── backend ── db    │  ──> ntfy.sh (push no celular)
                   └───────────────┬────────────────┘
                                   │ HTTPS (API + interface)
                          Navegador: administrador / operador
```

- **Dispositivos:** cada placa tem `deviceId` próprio e usuário MQTT próprio. A regra local (sirene
  após o tempo de entrada) roda no firmware e funciona sem rede.
- **Broker (Mosquitto):** autenticação obrigatória e ACL por dispositivo: cada placa só publica nos
  próprios tópicos e só o backend publica comandos.
- **Backend (FastAPI):** assina os tópicos, valida cada mensagem contra o
  [`CONTRATOS.md`](CONTRATOS.md) (descarta e registra as inválidas), grava no PostgreSQL, aplica a
  regra remota e os agendamentos, notifica no ntfy, expõe a API e serve a interface. É o único que
  publica comandos.
- **Banco (PostgreSQL):** histórico de medições, comandos com todas as fases, alertas, eventos de
  online/offline, agendamentos e mensagens inválidas. Esquema versionado em
  [`backend/db/migrations/`](backend/db/migrations/).
- **Interface:** login, estado e último contato de cada dispositivo, alertas ativos, comandos com
  progresso até a confirmação, histórico, painel analítico e configurações. Fala só com a API.

Detalhes do backend em [`backend/README.md`](backend/README.md); contrato MQTT em
[`CONTRATOS.md`](CONTRATOS.md); hardware em [`hardware/`](hardware/README.md); firmwares em
[`firmware/`](firmware/README.md).

## Como executar

### Pré-requisitos
- Docker com Docker Compose v2.
- Para gravar o firmware: Arduino IDE com o core ESP32 (ver [`firmware/README.md`](firmware/README.md)).
- Opcional, para rodar testes e o simulador fora do Docker: Python 3.12.

### 1. Configurar
```bash
git clone https://github.com/migueldufloth/access.git && cd access
cp .env.example .env      # troque TODAS as senhas e o JWT_SECRET (openssl rand -hex 32)
```
`.env` fica fora do Git. Para receber os avisos no celular, instale o app **ntfy**, assine um
tópico e coloque o nome dele em `NTFY_TOPICO`.

### 2. Subir broker, backend e banco (um comando)
```bash
docker compose up -d --build
```
Abra **http://localhost:8000** e entre com `ADMIN_USUARIO`/`ADMIN_SENHA` do `.env`. O operador
(`OPERADOR_USUARIO`) vê o painel, mas não envia comandos.

| Serviço | Porta | Papel |
| :--- | :--- | :--- |
| `backend` | 8000 | API REST (`/api/...`), interface e binários de OTA (`/firmware/<arquivo>.bin`) |
| `mosquitto` | 1883 (LAN) · 9001 (só local, para o nginx) | Broker MQTT com ACL |
| `db` | interna | PostgreSQL 16 com volume persistente |

### 3. Ligar os dispositivos
Grave os firmwares seguindo [`firmware/README.md`](firmware/README.md), com o usuário MQTT igual ao
`deviceId` (`porta-01`, `janela-01`) e a senha correspondente do `.env`.

Sem as placas em mãos, dá para simular um dispositivo que segue o mesmo contrato:
```bash
cd backend && pip install -r requirements.txt
python -m tools.simulador porta-01 --senha <MQTT_SENHA_PORTA_01>   # digite: abrir, fechar, falha, ok
```

### 4. Verificar o sistema
```bash
cd backend
pip install -r requirements-dev.txt
docker run -d --rm --name access-pg-teste -p 55432:5432 -e POSTGRES_USER=access \
  -e POSTGRES_PASSWORD=access -e POSTGRES_DB=access_test postgres:16-alpine
python -m pytest                      # testes de contrato, regras, API e banco
python -m tools.verificar_sistema     # ponta a ponta no compose, com dois dispositivos simulados
```
O CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) roda os dois a cada push e pull request.

### Produção (servidor da equipe)
No servidor, o nginx do host termina o TLS de `accessgrupo5.duckdns.org` (Let's Encrypt) e repassa
`/` para o backend e `/mqtt` para o WebSocket do Mosquitto. Assim, as placas conectam por
`wss://accessgrupo5.duckdns.org/mqtt` de qualquer rede. Passos em [`broker/README.md`](broker/README.md).

## Requisitos da N3 (R1–R18)

Situação em 24/11/2026: **servidor pronto** (backend, banco, broker, interface, testes e CI,
verificados ponta a ponta com dispositivos simulados). Os itens marcados com **(placas)** ainda
dependem de gravar nas placas o firmware adaptado ao contrato v1. O guia está em
[`docs/firmware-contrato-v1.md`](docs/firmware-contrato-v1.md).

| Req. | Como é atendido | Onde |
| :--- | :--- | :--- |
| R1 Hardware **(placas)** | 2 placas, 3 entradas (reed, PIR, ultrassônico), sirene I2S e LEDs | [`hardware/`](hardware/), [`firmware/`](firmware/) |
| R2 Firmware **(placas)** | Regra local sem rede, reconexão, NVS, `deviceId`, `ts`, logs | [`firmware/`](firmware/); adaptação ao contrato v1 em [`docs/firmware-contrato-v1.md`](docs/firmware-contrato-v1.md) |
| R3 Contrato | Contrato v1 congelado, validado pelo backend | [`CONTRATOS.md`](CONTRATOS.md), [`backend/app/contrato.py`](backend/app/contrato.py) |
| R4 Backend | Assina, valida, descarta e registra inválidas, persiste, expõe API, único que comanda | [`backend/`](backend/README.md) |
| R5 Banco | Medições, comandos (com as fases), alertas, status, agendamentos | [`backend/db/migrations/`](backend/db/migrations/) |
| R6 Interface | Valor com unidade, histórico, último contato, online/offline, alertas, comandos | `backend/app/web/` |
| R7 Comando | enviado → recebido → executado → confirmado; rejeição com motivo; `sem_resposta` em 10 s | [`backend/app/regras.py`](backend/app/regras.py) |
| R8 Decisão remota | Escalonamento (porta armada aberta além do limite configurável) e agendamentos | [`backend/app/servico.py`](backend/app/servico.py) |
| R9 Segurança | Login (bcrypt + JWT); só administrador comanda; segredos em `.env`/`secrets.h` | [`backend/app/auth.py`](backend/app/auth.py) |
| R10 Falhas | 4 cenários executados com evidência | [`docs/falhas/`](docs/falhas/README.md) |
| R11 Entrega | Este README; release identificada por tag | Releases do GitHub |
| R12 Orquestração | `docker compose up` sobe broker, backend e banco | [`compose.yaml`](compose.yaml) |
| R13 Escala **(placas)** | `porta-01` e `janela-01`, distinguidos no backend e na interface | — |
| R14 Analítico | Eventos por hora, tempo em alarme, disponibilidade, média móvel do RSSI | `GET /api/analitico/{id}` |
| R15 Notificação | Push no ntfy.sh para invasão, falha de sensor, offline armado e agendamento rejeitado | [`backend/app/notificacao.py`](backend/app/notificacao.py) |
| R16 Segurança avançada | MQTT com TLS (`wss` na 443, Let's Encrypt validado) **e** perfis com permissões diferentes | [`broker/`](broker/README.md) |
| R17 OTA **(placas)** | Comando `atualizar_firmware` (URL HTTPS + SHA-256) e binário servido pelo backend | [`docs/firmware-contrato-v1.md`](docs/firmware-contrato-v1.md) |
| R18 Testes e CI | pytest (contrato, regras, API com banco) + verificação ponta a ponta no GitHub Actions | [`backend/tests/`](backend/tests/), [`.github/workflows/`](.github/workflows/ci.yml) |

## Estrutura do repositório
- `backend/` — serviço FastAPI, migrações do banco, interface web, testes e ferramentas
  (`tools/simulador.py`, `tools/verificar_sistema.py`)
- `broker/` — Mosquitto do compose (config, ACL do contrato v1, geração de senhas) e nginx do servidor
- `firmware/` — firmwares da Central (ESP32-S3) e do Satélite (ESP32); `firmware/ota/` recebe os
  binários servidos para o OTA; `firmware/Access/` é o firmware legado da N1
- `hardware/` — esquemas elétricos, mapa de pinos, BOM e verificação elétrica
- `docs/` — CP12, cenários de falha, guia do firmware e histórico da N1
- `compose.yaml`, `.env.example`, `CONTRATOS.md`

## Reaproveitamento declarado
- **Bibliotecas:** FastAPI, Uvicorn, paho-mqtt, psycopg, PyJWT, bcrypt, httpx e pytest (via
  `requirements*.txt`); **Chart.js 4.4.7** (licença MIT), copiado sem alteração em
  `backend/app/web/vendor/`; imagens oficiais `postgres` e `eclipse-mosquitto`.
- **Projeto Sala Inteligente:** usado como referência de arquitetura (backend como único
  publicador de comandos, ciclo de confirmação). Modelo de dados, regras, contrato e interface
  foram escritos para o problema do Access.
- O código de exemplo do `esp_mqtt_client` e do ESP-NOW nos firmwares segue a documentação da
  Espressif, adaptado aos tópicos e à lógica do projeto.

## Documentação
- [`docs/falhas/`](docs/falhas/README.md) — os 4 cenários de falha do R10, com evidências.
- [`docs/firmware-contrato-v1.md`](docs/firmware-contrato-v1.md) — o que o firmware precisa
  publicar e tratar para seguir o contrato v1 (guia para a adaptação das placas).
- [`docs/cp12/`](docs/cp12/) — proposta de upgrade (CP12), com arquitetura-alvo, hardware,
  tecnologias, cronograma e riscos.
- [`docs/historico-n1.md`](docs/historico-n1.md) — histórico da N1 (protótipo de placa única).
