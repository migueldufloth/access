# Firmware no contrato v1: o que falta nas placas

O servidor (broker, backend, banco e interface) já segue o [`CONTRATOS.md`](../CONTRATOS.md) e foi
verificado ponta a ponta com dispositivos simulados. Os firmwares v2 (`firmware-central-s3` e
`firmware-satelite-esp32`) ainda publicam no formato antigo (`sensor/porta` com texto `ABERTA`,
`status/alarme` com `1`, usuário `access_esp32`). O backend **descarta** esse formato, e a ACL nova
nem deixa publicá-lo. Este guia lista o que muda em cada placa.

**Referência executável:** [`backend/tools/simulador.py`](../backend/tools/simulador.py)
implementa exatamente o comportamento esperado de um dispositivo: mensagens, regra local,
comandos, rejeições, duplicados e Last Will. Na dúvida, o firmware faz o mesmo que ele.

**Como testar sem o servidor de produção:** `docker compose up -d` no notebook e o
`MQTT_URI` da placa apontando para `mqtt://<ip-do-notebook>:1883`. O painel em
`http://localhost:8000` mostra na hora se cada mensagem foi aceita. As recusadas aparecem em
`GET /api/mensagens-invalidas`, com o motivo.

## 1. Identidade e credenciais (R2, R13)

| Placa | `deviceId` | Usuário MQTT | Senha (`.env` do servidor) | Grandeza principal |
| :--- | :--- | :--- | :--- | :--- |
| Central da Porta (ESP32-S3) | `porta-01` | `porta-01` | `MQTT_SENHA_PORTA_01` | `abertura` (reed) |
| Satélite de Movimento (ESP32) | `janela-01` | `janela-01` | `MQTT_SENHA_JANELA_01` | `presenca` (PIR/ultrassônico) |

- `secrets.h`: `MQTT_USER` passa a ser o `deviceId` e `MQTT_PASS` a senha da linha acima.
  `DEVICE_ID` pode ficar em `config.h` (não é segredo).
- **O Satélite passa a ter MQTT próprio** (decisão da equipe para o R13). O ESP-NOW continua para
  o que ele faz bem: a sirene e o LED sincronizados em milissegundos, mesmo sem rede. Wi-Fi STA e
  ESP-NOW convivem no mesmo canal, e o Satélite já sincroniza o canal com o da Central.
- Prefixo de todos os tópicos: `access/grupo5/<deviceId>/`.

## 2. Mensagens que cada placa publica (R3)

Todas em JSON, com `v: 1`, `deviceId`, `ts` e `uptime_s`. O `ts` é ISO 8601 em UTC
(`2026-11-24T22:15:03Z`) depois do SNTP, ou `null` antes dele. O `uptime_s` é `millis() / 1000`.
Exemplos completos no `CONTRATOS.md`, seção 3.

| Tópico | QoS / retain | Quando | Observação |
| :--- | :--- | :--- | :--- |
| `telemetria` | 0 / não | A cada `intervaloTelemetria_s` (padrão 5 s) **e** a cada mudança do sensor | `medicoes`: `abertura` ou `presenca` (0/1, `bool`), `aberturas` (`contagem`), `rssi` (`dBm`) |
| `estado` | 1 / **sim** | No boot, ao reconectar e a cada mudança de qualquer campo | `fw`, `modo`, `armado`, `alarme`, `sensor`, `config`, `ultimoComandoId` |
| `confirmacao` | 1 / não | Para cada comando: fase `recebido` e, se aceito, fase `executado` | Ver seção 3 |
| `status` | 1 / **sim** | `online` ao conectar; Last Will `{"v":1,"deviceId":"porta-01","status":"offline"}` | O Last Will é configurado em `cfg.session.last_will` |

Valores aceitos (qualquer outro faz o backend descartar a mensagem inteira):
`modo` ∈ `automatico|manual|seguro`, `alarme` ∈ `inativo|entrada|disparado`,
`sensor` ∈ `ok|falha_aberto|falha_curto|calibrando`, `fw` no formato `MAJOR.MINOR.PATCH`
(sem sufixo), `rssi` de −120 a 0. O payload pode ter no máximo 512 bytes.

## 3. Comandos (R7)

Assinar `access/grupo5/<deviceId>/comando` com QoS 1 **dentro do evento de conexão**, para refazer
a assinatura a cada reconexão. Para cada mensagem recebida:

1. Ler o JSON. Sem JSON válido ou sem `commandId`, ignorar e registrar no Serial.
2. Se o `commandId` está entre os últimos 16 já tratados, republicar as confirmações guardadas com
   `duplicado: true` e **não executar de novo**.
3. Rejeitar (só a fase `recebido`, `resultado: "rejeitado"`, com `erro` e `motivo`) quando:
   - `v` diferente de 1 → `E_VERSAO_NAO_SUPORTADA`; `deviceId` de outra placa → `E_DEVICE_ID_INCORRETO`;
   - `acao` fora da lista → `E_ACAO_DESCONHECIDA`; parâmetro fora da faixa → `E_VALOR_FORA_FAIXA`;
   - `ts + expira_s` já passou (só com o relógio sincronizado) → `E_EXPIRADO`;
   - `armar` com a porta aberta → `E_PORTA_ABERTA`; `armar` com o sensor em falha → `E_SENSOR_FALHA`;
   - `disparar` desarmado → `E_DESARMADO`; `origem: "agendamento"` no modo `manual` → `E_MODO_MANUAL`.
4. Aceito: publicar `recebido`/`aceito`, executar, publicar `executado`/`ok` (ou `falha` com
   o erro), gravar `ultimoComandoId = commandId` e publicar o `estado`. É esse `estado` que faz o
   painel mostrar **confirmado**.

| `acao` | Efeito na placa |
| :--- | :--- |
| `armar` / `desarmar` | `armado = true/false`; desarmar também desliga a sirene. Gravar `armado` em NVS |
| `disparar` | `alarme = "disparado"`: sirene I2S e LED (e o Satélite via ESP-NOW) |
| `silenciar` | Desliga a sirene, continua armado |
| `configurar` | `tempoEntrada_s` (0–120), `intervaloTelemetria_s` (1–300), `timeoutManual_s` (60–7200): gravar em NVS e aplicar |
| `atualizar_firmware` | OTA (seção 5) |

## 4. Regra local, estado seguro e persistência (R2)

- **Regra local sem rede:** com `armado = true`, a porta abre → `alarme = "entrada"`. Passados
  `tempoEntrada_s`, a sirene toca (`"disparado"`). Isso substitui o `TOLERANCIA_SEM_GENTE_MS`
  fixo. Se a equipe quiser manter a lógica combinada (não disparar se houver movimento do Satélite
  durante a entrada), ela entra como condição extra do disparo, mas sempre só com o sistema
  armado. A notificação passa a ser feita pelo **backend** (alertas com push no ntfy), então o
  `enviarNotificacao()` HTTP da Central pode sair, ficando um único canal de notificação.
- **Configuração persistente:** `Preferences` (NVS) para `armado`, `tempoEntrada_s`,
  `intervaloTelemetria_s` e `timeoutManual_s`. Ler no `setup()`, gravar quando um comando mudar.
  Na demonstração: mudar o tempo de entrada pela interface, reiniciar a placa e mostrar que o
  valor ficou (aparece em `estado.config`).
- **Estado seguro:** reed com leitura impossível (fio rompido ou curto, conforme o
  [`hardware/`](../hardware/README.md)) → `modo = "seguro"`, `sensor = "falha_aberto"` ou
  `"falha_curto"`, publicar `estado`. Voltando ao normal, `modo = "automatico"` sem reiniciar.
  No Satélite, o PIR nos primeiros ~60 s reporta `sensor = "calibrando"`.
- **Sem `delay()` longo no `loop()`:** o `delay(DEBOUNCE_REED_MS)` e o `delay(30)` atuais são
  curtos. Se crescerem, trocar por `millis()`.
- **Logs no Serial** para cada mensagem publicada, comando recebido e transição de estado.

## 5. OTA (R17)

1. Compilar o novo firmware com o `fw` incrementado (por exemplo, `1.0.1`) e, na Arduino IDE,
   usar *Sketch → Export Compiled Binary*.
2. Copiar o `.bin` para `firmware/ota/` no servidor (servido em
   `https://accessgrupo5.duckdns.org/firmware/<arquivo>.bin`) e calcular `shasum -a 256 <arquivo>.bin`.
3. Enviar pela API o comando
   `{"acao": "atualizar_firmware", "params": {"url": "...", "sha256": "...", "versao": "1.0.1"}}`.
4. Na placa: `esp_https_ota` com `esp_crt_bundle_attach`, conferindo o SHA-256 do que foi baixado
   antes de trocar a partição. Hash diferente → `executado`/`falha` com `E_OTA_FALHA`. Ok →
   `executado`/`ok`, reiniciar, e o `estado` do boot já sai com o `fw` novo. É ali que a interface
   mostra a versão nova.
5. Esquema de partições com dois slots OTA (`Minimal SPIFFS` ou `Default 4MB with spiffs` no S3).

## 6. Hardware: pontos do R1 que reprovam

O edital lista "carga alimentada diretamente por GPIO" como **não atende**. Hoje a Central
alimenta a OLED (`OLED_VCC_PIN 4`, `OLED_GND_PIN 5`) e o terra do reed e do LED
(`REED_GND_PIN 16`, `LED_GND_PIN 1`) por GPIO. Antes da defesa:
- OLED no `3V3` e `GND` da placa (não em GPIO);
- reed e LED com o terra no `GND` real; o LED com resistor em série, no GPIO só o sinal;
- atualizar o mapa de pinos em [`hardware/mapa-de-pinos.md`](../hardware/mapa-de-pinos.md).

## 7. Checklist para dar como concluído

- [ ] As duas placas aparecem online no painel, cada uma com seu `deviceId`, sem nada novo em `mensagens-invalidas`
- [ ] Armar, desarmar, disparar e silenciar chegam a **confirmado**; `disparar` desarmado volta rejeitado com motivo
- [ ] Porta aberta com o sistema armado: entrada, depois sirene, também com o Wi-Fi desligado
- [ ] Tempo de entrada alterado pela interface sobrevive ao reinício
- [ ] Fio do reed solto: `modo seguro`, alerta e push no celular
- [ ] Um OTA real pela API (`POST /api/comandos`), terminando com o `fw` novo no painel
- [ ] Cenários 1 e 3 de [`docs/falhas/`](falhas/README.md) repetidos nas placas, com log Serial e foto/vídeo
