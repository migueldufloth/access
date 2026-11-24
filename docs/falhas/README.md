# Cenários de falha (R10)

Plano original: CP12, Anexo B. Os quatro cenários obrigatórios foram provocados contra o sistema
real (Mosquitto, backend e PostgreSQL do `compose.yaml`) em 24/11/2026, com os dois dispositivos
simulados pelo [`backend/tools/simulador.py`](../../backend/tools/simulador.py), que segue o mesmo
contrato v1 do firmware. A execução é automática e repetível:

```bash
docker compose up -d --build
cd backend && python -m tools.verificar_sistema
```

O mesmo roteiro roda no CI a cada push, então a evidência se renova a cada mudança.

Evidências desta execução, em [`evidencias/`](evidencias/):
- [`verificacao-sistema.txt`](evidencias/verificacao-sistema.txt): saída do roteiro (17 de 17 itens passaram);
- [`consultas-banco.txt`](evidencias/consultas-banco.txt): tabelas `comandos`, `mensagens_invalidas`, `alertas` e `eventos_status` logo depois;
- [`log-backend.txt`](evidencias/log-backend.txt): log do backend no mesmo período (horários em UTC; nas consultas, horário de São Paulo);
- [`painel-comandos.png`](evidencias/painel-comandos.png): a interface mostrando confirmados, rejeitados com motivo e o `sem resposta`.

Falta repetir os cenários 1 e 3 **com as placas**, quando o firmware adaptado ao contrato v1 for
gravado (ver [`firmware-contrato-v1.md`](../firmware-contrato-v1.md)). O roteiro de cada um está
indicado abaixo em "Na placa".

---

## 1. Queda do Wi-Fi do dispositivo

| | |
| :--- | :--- |
| **Como foi provocado** | Processo do dispositivo `janela-01` encerrado sem desconexão limpa, o mesmo efeito de o ponto de acesso cair para o broker |
| **Esperado** | O broker publica o Last Will (`status` `offline`, retido); o backend marca o dispositivo offline e registra o evento; armado e sem contato por mais de 60 s, abre o alerta `dispositivo_offline` com push. Ao voltar, o dispositivo reconecta, reassina `comando` e publica `status online` e `estado` |
| **Observado** | `janela-01 ficou offline` no log do backend (04:12:11 UTC); linha `offline` em `eventos_status`; cartão do dispositivo cinza ("Offline") na interface. A reconexão e o retorno a online aparecem no cenário 2 (04:11:59) |
| **Evidência** | `verificacao-sistema.txt` (R10.1), `log-backend.txt`, `consultas-banco.txt` (`eventos_status`); teste `test_offline_armado_gera_alerta` |
| **Na placa** | Armar, desligar o roteador ou hotspot por 60 s e abrir a porta durante a queda. A sirene deve tocar pela regra local (entrada, depois disparado), sem rede. Registrar o log Serial e a OLED com `[NO WIFI]`. Ao religar, a placa volta sozinha e o painel mostra online de novo |

## 2. Broker indisponível

| | |
| :--- | :--- |
| **Como foi provocado** | `docker compose stop mosquitto`, comando `silenciar` pela interface com o broker fora, e `docker compose start mosquitto` |
| **Esperado** | O comando da queda vira `sem_resposta` e **não** é reexecutado quando o broker volta. Backend e dispositivos reconectam sozinhos, refazem as assinaturas e os comandos seguintes funcionam |
| **Observado** | Comando das 01:11:58 gravado como `sem_resposta` ("não publicado: broker MQTT indisponível"). O backend registrou `desconectado do broker` e, 3 s depois, `conectado ao broker; refazendo assinaturas`. Os dois dispositivos voltaram a online e o `silenciar` seguinte (01:12:11) chegou a `confirmado`. O comando da queda continuou `sem_resposta` |
| **Evidência** | `consultas-banco.txt` (`comandos`), `log-backend.txt` (04:11:56 a 04:11:59 UTC), `verificacao-sistema.txt` (R10.2); teste `test_sem_resposta_em_10s_e_broker_fora` |

## 3. Falha de sensor

| | |
| :--- | :--- |
| **Como foi provocado** | O dispositivo `porta-01` passou a reportar `sensor: falha_aberto` e `modo: seguro` (simulador: comando `falha`), depois `ok` |
| **Esperado** | Alerta `falha_sensor` com push; `armar` rejeitado com `E_SENSOR_FALHA`; ao normalizar, o alerta é encerrado sem reiniciar nada |
| **Observado** | Alerta `falha_sensor` aberto às 01:11:54 e encerrado quando o sensor voltou a `ok`. O `armar` das 01:11:56 foi `rejeitado` com `E_SENSOR_FALHA: sensor em falha` e o motivo apareceu na interface |
| **Evidência** | `consultas-banco.txt` (`alertas`, `comandos`), `log-backend.txt`, `verificacao-sistema.txt` (R10.3); teste `test_falha_de_sensor_abre_e_fecha_alerta` |
| **Na placa** | Soltar o fio do reed com o sistema armado. Em até 3 s, o modo vira `seguro` e o `estado` publica `sensor: falha_aberto`. O painel mostra o alerta e o celular recebe o push. Religar o fio deve voltar ao automático sem reiniciar |

## 4. Comando inválido ou duplicado

| | |
| :--- | :--- |
| **Como foi provocado** | (a) pela API: `configurar` com `tempoEntrada_s: 999` (e, nos testes, ação desconhecida e parâmetros a mais); (b) pelo MQTT, com o usuário do backend: o mesmo `commandId` publicado duas vezes; (c) `disparar` com o sistema desarmado; (d) um dispositivo publicando `rssi: 50` e tentando publicar no tópico de outro dispositivo |
| **Esperado** | (a) recusado com `E_VALOR_FORA_FAIXA`/`E_ACAO_DESCONHECIDA`, sem publicar nada. (b) A segunda entrega devolve as confirmações originais com `duplicado: true` e não reexecuta. (c) `rejeitado` com `E_DESARMADO`. (d) A mensagem inválida é descartada e registrada em `mensagens_invalidas`; a publicação no tópico alheio é barrada pela ACL |
| **Observado** | (a) `422 {"erro": "E_VALOR_FORA_FAIXA", ...}` e nenhuma publicação (teste de integração; `E_ACAO_DESCONHECIDA` em `test_comando_invalido`). (b) Confirmações `[recebido, executado]` com `duplicado=false` e, na reentrega, as mesmas com `duplicado=true`, sem nova execução. (c) `disparar` em `janela-01` às 01:11:29 rejeitado com `E_DESARMADO`. (d) `E_VALOR_FORA_FAIXA: rssi = 50 fora de -120..0` em `mensagens_invalidas`; nenhuma medição de presença gravada para `janela-01` |
| **Evidência** | `verificacao-sistema.txt` (R4, R7, R10.4, R16), `consultas-banco.txt` (`mensagens_invalidas`, `comandos`); testes `test_comando_fora_do_contrato_nao_e_publicado`, `test_mensagem_invalida_e_descartada_e_registrada` e a regra de descarte em `test_contrato.py` |
