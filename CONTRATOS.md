# Contrato MQTT — Access

**Versão do contrato: 1** (congelado em 27/10/2026, CP12 completo). Qualquer mudança depois desta data gera nova versão (`v: 2`), registrada na [tabela de versões](#histórico-de-versões) no fim deste arquivo.

Este contrato vale para o firmware da N3, para o backend (que valida cada mensagem contra ele) e para os testes automatizados. O firmware atual (v3, `access/grupo5/sensor/presenca` e afins) usa o contrato informal **v0**, descrito no [README](README.md#contrato-de-tópicos); ele deixa de valer quando o firmware da N3 for gravado.

## 1. Convenções

| Item | Regra |
| :--- | :--- |
| Prefixo | `access/grupo5/<deviceId>/...` |
| `deviceId` | Texto em minúsculas, números e hífen, de 3 a 24 caracteres (`^[a-z0-9-]{3,24}$`). Dispositivos da N3: `porta-01` e `janela-01` |
| Formato | JSON UTF-8, objeto único, até 512 bytes. Exceção: o payload do Last Will e do `status` também é JSON |
| Campos obrigatórios em **toda** mensagem | `v` (inteiro, versão do contrato, hoje `1`), `deviceId` (igual ao do tópico), `ts` e `uptime_s` |
| `ts` | Texto ISO 8601 em UTC (`2026-11-10T22:15:03Z`) ou `null` enquanto o relógio do dispositivo não estiver sincronizado por SNTP. Mensagem com `ts: null` é aceita e o backend carimba a hora de recebimento |
| `uptime_s` | Inteiro, segundos desde o boot. Permite ordenar mensagens quando `ts` é `null` |
| Unidades | Sempre explícitas por medição (`unidade`), nunca implícitas |
| Mensagem fora do contrato | O backend a descarta e a registra em `mensagens_invalidas` (tópico, payload, motivo). O firmware a ignora e, se for um comando, responde com erro (seção 5) |

## 2. Árvore de tópicos

```text
access/grupo5/<deviceId>/
├── telemetria     dispositivo → backend   medições periódicas e por evento
├── estado         dispositivo → backend   estado atual (retido)
├── comando        backend → dispositivo   ordem a executar
├── confirmacao    dispositivo → backend   resultado do comando, por fase
└── status         dispositivo → backend   online/offline (Last Will, retido)
```

| Tópico | Publica | Assina | QoS | Retain | Justificativa |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `telemetria` | Dispositivo | Backend | 0 | Não | É periódica (padrão 5 s): se uma se perder, a próxima chega logo. Reter faria um assinante novo receber uma medição velha como se fosse atual |
| `estado` | Dispositivo | Backend | 1 | **Sim** | O backend precisa saber o estado atual ao reiniciar ou reconectar, sem esperar a próxima mudança. QoS 1 garante que a mudança de estado chegue |
| `comando` | Backend | Dispositivo | 1 | **Não** | Não pode ser perdido (QoS 1), mas nunca deve ser reexecutado por reconexão: sem retain, sessão limpa e prazo de validade (`expira_s`) |
| `confirmacao` | Dispositivo | Backend | 1 | Não | Resultado de um comando específico (`commandId`); cada fase precisa chegar, mas é evento, não estado |
| `status` | Dispositivo e broker (Last Will) | Backend | 1 | **Sim** | Online/offline precisa estar disponível para quem conectar depois; o Last Will cobre a queda sem aviso |

### Exemplo de ACL por dispositivo

Usuário MQTT do dispositivo = `deviceId`. Usuário do backend = `access_backend`. A interface web **não** fala com o broker.

```text
user access_backend
topic read  access/grupo5/+/telemetria
topic read  access/grupo5/+/estado
topic read  access/grupo5/+/confirmacao
topic read  access/grupo5/+/status
topic write access/grupo5/+/comando

pattern write access/grupo5/%u/telemetria
pattern write access/grupo5/%u/estado
pattern write access/grupo5/%u/confirmacao
pattern write access/grupo5/%u/status
pattern read  access/grupo5/%u/comando
```

## 3. Payloads de dispositivo para backend

### 3.1 `telemetria`

```json
{
  "v": 1,
  "deviceId": "porta-01",
  "ts": "2026-11-10T22:15:03Z",
  "uptime_s": 4312,
  "medicoes": [
    { "grandeza": "abertura",  "valor": 1,   "unidade": "bool" },
    { "grandeza": "aberturas", "valor": 12,  "unidade": "contagem" },
    { "grandeza": "rssi",      "valor": -52, "unidade": "dBm" }
  ]
}
```

| `grandeza` | Dispositivos | Valores aceitos | `unidade` |
| :--- | :--- | :--- | :--- |
| `abertura` | Todos (reed) | `0` fechada, `1` aberta | `bool` |
| `presenca` | `janela-01` (PIR) | `0` sem presença, `1` presença | `bool` |
| `aberturas` | Todos | Inteiro de 0 a 1 000 000 (contador desde o boot) | `contagem` |
| `rssi` | Todos | Inteiro de −120 a 0 | `dBm` |

Publicada a cada `intervaloTelemetria_s` (padrão 5 s) e **por evento**, a cada mudança de `abertura` ou `presenca`. Medição cujo `valor` ou `unidade` não conste na tabela invalida a mensagem inteira.

### 3.2 `estado`

```json
{
  "v": 1,
  "deviceId": "porta-01",
  "ts": "2026-11-10T22:15:03Z",
  "uptime_s": 4312,
  "fw": "1.0.0",
  "modo": "automatico",
  "armado": true,
  "alarme": "inativo",
  "sensor": "ok",
  "config": { "tempoEntrada_s": 20, "intervaloTelemetria_s": 5, "timeoutManual_s": 1800 },
  "ultimoComandoId": "7c9e6679-7425-40de-944b-e07fc1f90ae7"
}
```

| Campo | Valores |
| :--- | :--- |
| `fw` | Versão do firmware (`MAJOR.MINOR.PATCH`) |
| `modo` | `automatico`, `manual`, `seguro` |
| `armado` | `true` ou `false` |
| `alarme` | `inativo`, `entrada` (contagem regressiva), `disparado` |
| `sensor` | `ok`, `falha_aberto` (fio rompido), `falha_curto`, `calibrando` (PIR em aquecimento) |
| `config` | Parâmetros persistentes (NVS); faixas na seção 4 |
| `ultimoComandoId` | `commandId` do último comando executado, ou `null`. É por ele que o backend marca o comando como **confirmado** |

Publicada no boot, a cada mudança de qualquer campo e ao reconectar.

### 3.3 `confirmacao`

Cada comando gera **duas** mensagens, uma por fase. O estado "confirmado" não vem daqui: é o backend que o registra quando vê `ultimoComandoId` no `estado`.

```json
{
  "v": 1,
  "deviceId": "porta-01",
  "ts": "2026-11-10T22:15:04Z",
  "uptime_s": 4313,
  "commandId": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "fase": "recebido",
  "resultado": "rejeitado",
  "erro": "E_DESARMADO",
  "motivo": "disparar exige o sistema armado",
  "duplicado": false
}
```

| Campo | Valores |
| :--- | :--- |
| `fase` | `recebido` (dispositivo aceitou ou rejeitou) e `executado` (atuador agiu) |
| `resultado` | Em `recebido`: `aceito` ou `rejeitado`. Em `executado`: `ok` ou `falha` |
| `erro` | Código do [catálogo de erros](#5-catálogo-de-códigos-de-erro); `null` quando não há erro |
| `motivo` | Texto curto para a interface; `null` quando não há erro |
| `duplicado` | `true` se o `commandId` já tinha sido tratado (ver seção 4); a confirmação original é repetida, sem reexecutar |

Comando rejeitado só tem a fase `recebido`.

### 3.4 `status` (online/offline e Last Will)

Publicado como `online` ao conectar (retido). O Last Will, registrado na conexão, publica `offline` (retido) se o dispositivo cair sem aviso. Como o Last Will é definido antes do dispositivo saber a hora, ele não traz `ts` nem `uptime_s`.

```json
{ "v": 1, "deviceId": "porta-01", "status": "online", "ts": "2026-11-10T22:15:00Z", "uptime_s": 1, "fw": "1.0.0" }
```

```json
{ "v": 1, "deviceId": "porta-01", "status": "offline" }
```

## 4. Comandos (backend → dispositivo)

Tópico `access/grupo5/<deviceId>/comando`. Só o backend publica nele.

```json
{
  "v": 1,
  "deviceId": "porta-01",
  "ts": "2026-11-10T22:15:03Z",
  "uptime_s": null,
  "commandId": "7c9e6679-7425-40de-944b-e07fc1f90ae7",
  "origem": "usuario",
  "acao": "armar",
  "params": {},
  "expira_s": 30
}
```

| Campo | Regra |
| :--- | :--- |
| `commandId` | UUID v4 gerado pelo backend; identifica o comando em todas as fases |
| `origem` | `usuario` (painel), `agendamento` ou `regra` (decisão remota). Gravada na tabela `comandos` |
| `acao` | Uma das ações da tabela abaixo |
| `params` | Objeto com os parâmetros da ação (vazio quando não há) |
| `expira_s` | Inteiro de 5 a 120 (padrão 30). O dispositivo descarta o comando se `ts` + `expira_s` já passou (`E_EXPIRADO`); com `ts: null` no dispositivo, só vale a regra de duplicado |
| `uptime_s` | Sempre `null` no comando (o backend não tem o uptime do dispositivo) |

| `acao` | `params` e faixas | Efeito | Rejeitada quando |
| :--- | :--- | :--- | :--- |
| `armar` | — | `armado = true` | Porta aberta (`E_PORTA_ABERTA`) ou sensor com falha (`E_SENSOR_FALHA`) |
| `desarmar` | — | `armado = false`, sirene desligada | — |
| `disparar` | — | Aciona a sirene (`alarme = disparado`) | Sistema desarmado (`E_DESARMADO`) |
| `silenciar` | — | Desliga a sirene; continua armado | — |
| `configurar` | Um ou mais de: `tempoEntrada_s` 0 a 120 · `intervaloTelemetria_s` 1 a 300 · `timeoutManual_s` 60 a 7200 | Grava em NVS (sobrevive ao reinício) e aplica | Valor fora da faixa (`E_VALOR_FORA_FAIXA`) |
| `atualizar_firmware` | `url` (HTTPS, até 200 caracteres) · `sha256` (64 hexadecimais) · `versao` (`MAJOR.MINOR.PATCH`) | Baixa, confere o hash e reinicia na nova versão (OTA) | Hash diferente (`E_OTA_FALHA`) |

Regras gerais de tratamento no dispositivo:

- Comando com `origem: "agendamento"` é recusado com `E_MODO_MANUAL` enquanto o modo for `manual`.
- **Duplicado:** o dispositivo guarda os últimos 16 `commandId`. Se receber um repetido (reentrega QoS 1 ou reenvio), **não executa de novo**: repete a confirmação original com `duplicado: true`.
- Estados de um comando, do ponto de vista do sistema: `enviado` (backend gravou e publicou) → `recebido` (fase `recebido`) → `executado` (fase `executado`) → `confirmado` (`estado.ultimoComandoId` = `commandId`). Sem nenhuma resposta em 10 s o backend marca `sem_resposta`.

## 5. Catálogo de códigos de erro

| Código | Quando acontece | Onde aparece |
| :--- | :--- | :--- |
| `E_JSON_INVALIDO` | Payload que não é JSON ou passa de 512 bytes | `confirmacao` (comando); `mensagens_invalidas` (backend) |
| `E_VERSAO_NAO_SUPORTADA` | `v` diferente de `1` | idem |
| `E_CAMPO_AUSENTE` | Falta campo obrigatório ou tipo errado | idem |
| `E_DEVICE_ID_INCORRETO` | `deviceId` do payload diferente do tópico | idem |
| `E_ACAO_DESCONHECIDA` | `acao` fora da lista | `confirmacao` |
| `E_VALOR_FORA_FAIXA` | Parâmetro fora das faixas da seção 4 | `confirmacao` |
| `E_DESARMADO` | `disparar` com o sistema desarmado | `confirmacao` |
| `E_PORTA_ABERTA` | `armar` com a porta aberta | `confirmacao` |
| `E_SENSOR_FALHA` | `armar` com sensor em falha | `confirmacao` |
| `E_MODO_MANUAL` | Comando de agendamento durante o modo manual | `confirmacao` |
| `E_EXPIRADO` | `ts` + `expira_s` já passou | `confirmacao` |
| `E_OTA_FALHA` | Download, hash ou gravação do firmware falhou | `confirmacao` (fase `executado`, `falha`) |
| `E_INTERNO` | Falha inesperada no dispositivo | `confirmacao` |

Duplicado **não** é erro: vem como `duplicado: true`.

## 6. Validação no backend

O backend descarta e registra qualquer mensagem de dispositivo que:

1. não seja JSON válido de até 512 bytes;
2. tenha `v` diferente de `1`;
3. tenha `deviceId` diferente do segmento do tópico;
4. falte `ts` (a chave, mesmo com valor `null`) ou `uptime_s`;
5. traga medição com `grandeza`, `valor` ou `unidade` fora da tabela da seção 3.1;
6. traga valor fora da faixa (por exemplo, `rssi` de 50).

Os esquemas de validação (pydantic) e os testes automatizados que cobrem estes casos ficam no backend, com um caso de teste por regra.

## Histórico de versões

| Versão | Data | Mudança |
| :--- | :--- | :--- |
| v0 | até 20/10/2026 | Contrato informal do firmware da N1: tópicos fixos `access/grupo5/sensor/presenca`, `comando/alarme`, `status/confirmacao` e `status/presenca_dispositivo`, sem versão, `deviceId` nem `commandId` |
| **v1** | 27/10/2026 | Primeira versão formal: tópicos por dispositivo, `commandId`, confirmação por fase, catálogo de erros, QoS e retain justificados. **Congelada** |
