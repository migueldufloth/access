# Backend do Access

Python 3.12 + FastAPI + PostgreSQL + paho-mqtt. Sobe pelo [`compose.yaml`](../compose.yaml) da raiz.

## Módulos
| Arquivo | Papel |
| :--- | :--- |
| `app/contrato.py` | Contrato v1: valida as mensagens dos dispositivos (seção 6 do `CONTRATOS.md`) e monta os comandos |
| `app/servico.py` | Ingestão (valida, grava, descarta e registra inválidas), ciclo do comando, alertas, regra remota e agendamentos |
| `app/regras.py` | Lógica pura, testada isoladamente: transições do comando e condições das regras |
| `app/analitico.py` | Indicadores do painel: eventos por hora, tempo ativo (alarme, disponibilidade) e média móvel |
| `app/mqtt.py` | Cliente MQTT: assina os tópicos (QoS do contrato), reconecta e refaz as assinaturas, publica comandos |
| `app/auth.py` | Login com bcrypt, token JWT de 8 h, perfis `operador` e `administrador` |
| `app/notificacao.py` | Push no ntfy.sh |
| `app/main.py` | API REST e entrega da interface (`app/web/`) |
| `db/migrations/` | Esquema versionado, aplicado na subida e uma vez por arquivo (`schema_migrations`) |
| `tools/simulador.py` | Dispositivo simulado que segue o contrato (demonstração sem as placas e testes) |
| `tools/verificar_sistema.py` | Verificação ponta a ponta no compose (também roda no CI) |

## API
Todas as rotas exigem `Authorization: Bearer <token>`, exceto o login, a saúde e o binário de OTA.

| Método e rota | Perfil | O que faz |
| :--- | :--- | :--- |
| `POST /api/auth/login` | — | `{usuario, senha}` → `{token, nome, perfil}` |
| `GET /api/dispositivos` | todos | Estado atual, online, último contato e última leitura de cada grandeza |
| `GET /api/dispositivos/{id}/medicoes?grandeza=&de=&ate=&limite=` | todos | Histórico |
| `GET /api/alertas?ativos=true` | todos | Alertas (ativos ou todos) |
| `GET /api/comandos` · `GET /api/comandos/{commandId}` | todos | Comandos e suas fases |
| `POST /api/comandos` | administrador | `{deviceId, acao, params, expira_s}` → 202 com o comando em `enviado` |
| `GET /api/analitico/{id}?horas=24` | todos | Eventos por hora, tempo em alarme, disponibilidade, RSSI com média móvel |
| `GET /api/agendamentos` · `POST` · `DELETE /{id}` | leitura: todos; escrita: administrador | Armar/desarmar por horário (fuso de São Paulo) |
| `GET /api/configuracao` · `PUT` | leitura: todos; escrita: administrador | `limiteEscalonamento_s` (10 a 600) |
| `GET /api/mensagens-invalidas` | todos | Mensagens descartadas e o motivo |
| `GET /firmware/{arquivo}.bin` | público | Binário do OTA (pasta `firmware/ota/`); o dispositivo confere o SHA-256 |
| `GET /api/saude` | público | Usado pelo healthcheck do container |

Erro de contrato num comando volta como `422 {"erro": "E_...", "motivo": "..."}`, com os códigos
do catálogo do contrato, e nada é publicado.

## Ciclo do comando (R7)
`enviado` (gravado e publicado) → `recebido` (fase `recebido`, `aceito`) → `executado` (fase
`executado`, `ok`) → `confirmado` (o `estado` do dispositivo traz `ultimoComandoId` igual ao
`commandId`). Desvios:
- `rejeitado`: fase `recebido`, `rejeitado`, com `erro` e `motivo`;
- `falha`: fase `executado`, `falha`;
- `sem_resposta`: nada chegou em 10 s, ou o broker estava fora na hora de publicar. O backend
  **não** reenvia: um comando atrasado não pode ser executado quando a conexão volta.

Confirmação repetida (`duplicado: true`) ou fora de ordem nunca faz o estado regredir.

## Decisão remota (R8)
- **Escalonamento:** dispositivo armado, porta aberta há mais de `limiteEscalonamento_s` e alarme
  ainda soando (não silenciado) → alerta `abertura_armado`, push no celular e comando `disparar`
  (origem `regra`) para todos os dispositivos armados e online do mesmo local. Um alerta por
  episódio: ele se encerra quando a porta fecha ou o sistema é desarmado.
- **Offline armado:** dispositivo armado sem contato há mais de 60 s (Last Will ou 3 intervalos
  sem telemetria) → alerta `dispositivo_offline` e push.
- **Falha de sensor:** `estado.sensor` em `falha_*` → alerta `falha_sensor` e push.
- **Agendamentos:** no horário, o backend gera `armar`/`desarmar` (origem `agendamento`); se o
  dispositivo rejeitar, o motivo vai para o comando e sai um alerta `agendamento_nao_aplicado`.

O ciclo de regras roda a cada 2 s. Além das grandezas da telemetria, o backend grava a medição
derivada `alarme` (1 = disparado) a cada mudança do `estado`, base do indicador "tempo em alarme".

## Testes
```bash
pip install -r requirements-dev.txt
docker run -d --rm --name access-pg-teste -p 55432:5432 -e POSTGRES_USER=access \
  -e POSTGRES_PASSWORD=access -e POSTGRES_DB=access_test postgres:16-alpine
python -m pytest -v
```
- `tests/test_contrato.py`: um caso por regra de descarte da seção 6 do contrato, e os comandos.
- `tests/test_regras.py`: transições do comando, regras remotas, agendamento e indicadores.
- `tests/test_api.py`: integração com PostgreSQL real (autenticação e perfis, ingestão, ciclo de
  comando, sem resposta, broker fora, escalonamento, falha de sensor, offline, agendamento,
  analítico, OTA).

Banco de teste em outro endereço: `TEST_DATABASE_URL=postgresql://...`.
