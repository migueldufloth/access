# Roteiro técnico — MQTT no firmware (Lucas)

Objetivo desta etapa: o teu firmware passa a **conectar no broker e publicar telemetria**
via MQTT sobre WSS. Comando remoto e disparo do alarme por rede (`comando/alarme`,
`status/confirmacao`) ficam para uma etapa seguinte — não implemente isso ainda, para não
haver dois pontos do código escrevendo no mesmo estado ao mesmo tempo.

## 0. O que já está pronto no repositório

Depois de dar `git pull`, você já tem:

- `firmware/Access/secrets.example.h` — modelo das credenciais, sem valores reais.
- `firmware/Access/cert_isrg_root_x1.h` — certificado raiz para validar o TLS do broker.
- `firmware/Access/config.h` — pinos, intervalos e os nomes dos tópicos, já como constantes
  (`TOPICO_SENSOR_PRESENCA`, `INTERVALO_TELEMETRIA` etc.). Não escreva o nome de um tópico à
  mão em nenhum lugar — use sempre a constante.
- `broker/` — a configuração do broker que já está rodando no servidor. Você não mexe nisso,
  é só para referência (endpoint, ACL).

## 1. Credenciais

Copie o exemplo e preencha com os valores reais (peça o usuário/senha do dispositivo
`access_esp32` para o Miguel — não peço aqui porque isso não pode ir para o repositório):

```bash
cp firmware/Access/secrets.example.h firmware/Access/secrets.h
```

Em `secrets.h`:

```cpp
#define WIFI_SSID   "sua-rede"
#define WIFI_PASS   "sua-senha"
#define MQTT_URI    "wss://accessgrupo5.duckdns.org/mqtt"
#define MQTT_USER   "access_esp32"
#define MQTT_PASS   "<pede pro Miguel>"
```

`secrets.h` está no `.gitignore`. Confirme antes de commitar (`git status` não deve listá-lo).

**Importante sobre a rede:** as portas 1883 e 9001 do broker não estão abertas para a
internet — só 443. Da faculdade, de casa, do hotspot, o único caminho é o `wss://` acima.

## 2. Bibliotecas

- Cliente MQTT: `#include "mqtt_client.h"` — já vem no core ESP32, **não instale
  `PubSubClient`**, ele não fala WebSocket e não vai conectar nesse broker.
- `ArduinoJson` (Benoît Blanchon) — instale pelo Gerenciador de Bibliotecas da Arduino IDE.

## 3. Sincronizar a hora antes do TLS

Armadilha real, não pule: o ESP32 liga achando que é 1970. A validação do certificado
recusa handshake com data inválida, e o erro que aparece não deixa isso óbvio. Sincronize
por SNTP **depois** do Wi-Fi conectar e **antes** de iniciar o cliente MQTT:

```cpp
void sincronizarHora() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[SNTP] Aguardando hora valida");
  time_t agora = time(nullptr);
  while (agora < 8 * 3600 * 2) { // ainda em 1970/1o de janeiro
    delay(200);
    Serial.print(".");
    agora = time(nullptr);
  }
  Serial.println("\n[SNTP] Hora sincronizada.");
}
```

Chame `sincronizarHora()` logo depois de `conectarWiFi()` no `setup()`, antes de iniciar o
MQTT.

## 4. Conectar ao broker (wss, TLS, Last Will)

```cpp
#include "mqtt_client.h"
#include "cert_isrg_root_x1.h"

esp_mqtt_client_handle_t clienteMqtt;

void iniciarMqtt() {
  String clientId = "access-" + WiFi.macAddress(); // precisa ser unico por dispositivo

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = MQTT_URI;
  cfg.broker.verification.certificate = ISRG_ROOT_X1_PEM;
  cfg.credentials.username = MQTT_USER;
  cfg.credentials.authentication.password = MQTT_PASS;
  cfg.credentials.client_id = clientId.c_str();
  cfg.session.last_will.topic = TOPICO_STATUS_PRESENCA_DISPOSITIVO;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.qos = 0;
  cfg.session.last_will.retain = true;

  clienteMqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(clienteMqtt, MQTT_EVENT_ANY, mqttEventHandler, NULL);
  esp_mqtt_client_start(clienteMqtt);
}
```

`clientId` duplicado entre dois dispositivos faz os dois ficarem se derrubando um ao outro,
com sintoma de reconexão infinita — se isso acontecer, o MAC de cada placa é diferente,
então confira se não copiou um client ID fixo em vez do `WiFi.macAddress()`.

## 5. Handler de eventos — só o necessário para esta etapa

```cpp
static void mqttEventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) eventData;

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      brokerConectado = true;
      ultimoContatoBroker = millis();
      Serial.println("[MQTT] Conectado ao broker.");
      esp_mqtt_client_publish(clienteMqtt, TOPICO_STATUS_PRESENCA_DISPOSITIVO, "online", 0, 0, true);
      break;

    case MQTT_EVENT_DISCONNECTED:
      brokerConectado = false;
      Serial.println("[MQTT] Desconectado do broker.");
      break;

    case MQTT_EVENT_ERROR:
      Serial.println("[MQTT] Erro de conexao (confira certificado e hora do sistema).");
      break;

    default:
      break;
  }
}
```

Não escreva um laço de reconexão manual — o cliente do ESP-IDF reconecta sozinho. Não
assine `access/grupo5/comando/alarme` ainda; isso é da próxima etapa (quem vai mexer nisso
é outra pessoa da equipe, para não conflitar com o teu trabalho).

## 6. Publicar telemetria

Payload conforme o contrato do projeto — os cinco campos, nem mais nem menos:

```cpp
#include <ArduinoJson.h>

void publicarTelemetria(bool portaAberta) {
  if (!brokerConectado) return;

  StaticJsonDocument<200> doc;
  doc["porta"] = portaAberta ? "aberta" : "fechada";
  doc["armado"] = sistemaArmado;
  doc["disparado"] = alarmeDisparado;
  doc["aberturas"] = contadorAberturas;
  doc["rssi"] = WiFi.RSSI();

  char buffer[200];
  size_t tamanho = serializeJson(doc, buffer);
  esp_mqtt_client_publish(clienteMqtt, TOPICO_SENSOR_PRESENCA, buffer, tamanho, 0, false);
  ultimoContatoBroker = millis();
}
```

Chame em dois pontos do `loop()`:

1. **No evento de mudança de estado da porta** (onde hoje chama `enviarNotificacao()` /
   `somPortaFechou()`).
2. **Periodicamente**, reaproveitando o intervalo que já existe:

```cpp
static unsigned long ultimaTelemetria = 0;
if (millis() - ultimaTelemetria > INTERVALO_TELEMETRIA) {
  ultimaTelemetria = millis();
  publicarTelemetria(estadoAtual);
}
```

Não precisa mexer em `sistemaArmado` nem `alarmeDisparado` — essas variáveis já existem em
`config.h`/`Access.ino` (Etapa 2), ainda em `false` por padrão. Só leia o valor delas aqui,
não escreva.

## 7. Como testar

- Da tua máquina, sem acesso ao servidor: confirme no Monitor Serial que aparece
  `[MQTT] Conectado ao broker.` e que a hora sincronizou antes disso.
- Peça para o Miguel rodar no servidor (ele tem acesso root e a senha do `access_web`):
  ```bash
  mosquitto_sub -h localhost -p 1883 -u access_web -P "<senha>" -t 'access/grupo5/sensor/#' -v
  ```
  Deve aparecer uma linha de telemetria a cada 5 s e uma a cada abertura/fechamento de
  porta.
- Abra a porta: confira que `porta` muda no payload. Sem alarme automático ainda — isso é
  intencional, não é bug; o disparo remoto só entra na etapa seguinte.
- Desligue o Wi-Fi e religue: confirme que reconecta sozinho (Wi-Fi e, em seguida, MQTT).

## 8. Checklist antes de subir

- [ ] `secrets.h` não aparece em `git status` nem em `git diff --stat`.
- [ ] Nenhum tópico escrito à mão — só as constantes de `config.h`.
- [ ] `alarmeDisparado` e `sistemaArmado` não são atribuídos em nenhum lugar do teu código
      (só lidos).
- [ ] Log serial mostra hora sincronizada antes de "Conectado ao broker".
- [ ] Commit separado, mensagem no padrão `feat: ...`, em português com acento.

Qualquer erro de TLS ("certificate has expired or is not yet valid" ou parecido) é quase
sempre a hora não sincronizada — antes de mexer em certificado, confira o passo 3.
