#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <time.h>
#include <ArduinoJson.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "secrets.h"
#include "config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- ESTADO DO SISTEMA ---
int contadorAberturas = 0;
bool ultimoEstado = false;
unsigned long tempoUltimaMudanca = 0;
unsigned long tempoUltimoAlarme = 0;
bool tomAlarme = false;

// --- LEITURA VALIDADA DO SENSOR (DEBOUNCE POR TEMPO) ---
// leituraBrutaAnterior/tempoUltimaLeituraBruta só existem para o filtro de
// lerSensorValidado() detectar quando o PINO mudou; estadoValidado é o valor
// já filtrado que o resto do firmware deve usar (é isso que digitalRead(REED_PIN)
// direto no loop() não fornecia antes desta versão).
bool leituraBrutaAnterior = false;
bool estadoValidado = false;
unsigned long tempoUltimaLeituraBruta = 0;

// sistemaArmado só é escrita no tratamento de comando MQTT (tratarComando).
// alarmeDisparado também só é escrita ali, com uma única exceção: o modo
// degradado (avaliarModoDegradado), quando o broker some por mais de
// TIMEOUT_MODO_DEGRADADO e o firmware volta a decidir sozinho. Nenhum outro
// trecho do firmware deve atribuir valor a essas duas variáveis.
bool sistemaArmado = false;
bool alarmeDisparado = false;
bool modoDegradado = false;

// --- CONTROLE MQTT (ESP-IDF) ---
esp_mqtt_client_handle_t clienteMqtt;
bool brokerConectado = false;
unsigned long ultimoContatoBroker = 0;
static unsigned long ultimaTelemetria = 0;
static unsigned long ultimaAtualizacaoOled = 0;

// --- CONTROLE DE WI-FI ---
unsigned long ultimaChecagemWifi = 0;
const unsigned long intervaloChecagemWifi = INTERVALO_CHECAGEM_WIFI;

// --- SINCRONIZAÇÃO DE HORA (SNTP PARA TLS) ---
void sincronizarHora() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[SNTP] Aguardando hora valida");
  time_t agora = time(nullptr);
  while (agora < 8 * 3600 * 2) {
    delay(200);
    Serial.print(".");
    agora = time(nullptr);
  }
  Serial.println("\n[SNTP] Hora sincronizada.");
}

// --- LEITURA VALIDADA DO SENSOR ---
// Só aceita uma mudança de estado do MC-38 como real depois que o pino ficar
// estável por DEBOUNCE_SENSOR_MS. Sem isso, bounce mecânico do reed switch no
// instante exato da abertura/fechamento (ou ruído elétrico) seria aceito na
// hora e propagado direto para contadorAberturas, publicarTelemetria,
// avaliarModoDegradado e atualizarDashboard, contando/alertando um evento que
// pode não ter acontecido de verdade. Debounce por tempo (não por contagem de
// amostras), no mesmo padrão não bloqueante do resto do firmware — nenhum
// delay(), só millis(), igual verificarReconexaoWiFi() e avaliarModoDegradado().
bool lerSensorValidado() {
  bool leituraBruta = digitalRead(REED_PIN);

  if (leituraBruta != leituraBrutaAnterior) {
    // O pino mudou agora — ainda não sabemos se é real ou só bounce.
    // Reinicia a contagem de estabilidade a partir deste instante.
    tempoUltimaLeituraBruta = millis();
    leituraBrutaAnterior = leituraBruta;
  }

  if (leituraBruta != estadoValidado &&
      (millis() - tempoUltimaLeituraBruta) >= DEBOUNCE_SENSOR_MS) {
    // Ficou estável tempo suficiente: agora sim é uma mudança real, não ruído.
    estadoValidado = leituraBruta;
  }

  return estadoValidado;
}

// --- TELEMETRIA MQTT ---
void publicarTelemetria(bool portaAberta) {
  if (!brokerConectado) return;

  StaticJsonDocument<200> doc;
  doc["porta"]      = portaAberta ? "aberta" : "fechada";
  doc["armado"]     = sistemaArmado;
  doc["disparado"]  = alarmeDisparado;
  doc["aberturas"]  = contadorAberturas;
  doc["rssi"]       = WiFi.RSSI();

  char buffer[200];
  size_t tamanho = serializeJson(doc, buffer);
  esp_mqtt_client_publish(clienteMqtt, TOPICO_SENSOR_PRESENCA, buffer, tamanho, 0, false);
  ultimoContatoBroker = millis();
  Serial.printf("[MQTT Telemetria] %s\n", buffer);
}

// --- COMANDOS REMOTOS E CONFIRMAÇÃO ---
static bool topicoRecebidoEh(esp_mqtt_event_handle_t event, const char* alvo) {
  size_t tamanhoAlvo = strlen(alvo);
  return event->topic_len == tamanhoAlvo && memcmp(event->topic, alvo, tamanhoAlvo) == 0;
}

// event->data chega sem terminador nulo: comparar sempre pelo data_len,
// nunca tratar como C-string.
static bool comandoRecebidoEh(esp_mqtt_event_handle_t event, const char* alvo) {
  size_t tamanhoAlvo = strlen(alvo);
  return event->data_len == tamanhoAlvo && memcmp(event->data, alvo, tamanhoAlvo) == 0;
}

void publicarConfirmacao(const char* comando, bool aplicado) {
  StaticJsonDocument<160> doc;
  doc["comando"]   = comando;
  doc["aplicado"]  = aplicado;
  doc["armado"]    = sistemaArmado;
  doc["disparado"] = alarmeDisparado;

  char buffer[160];
  size_t tamanho = serializeJson(doc, buffer);
  esp_mqtt_client_publish(clienteMqtt, TOPICO_STATUS_CONFIRMACAO, buffer, tamanho, 0, false);
  ultimoContatoBroker = millis();
  Serial.printf("[MQTT Confirmacao] %s\n", buffer);
}

void tratarComando(esp_mqtt_event_handle_t event) {
  const char* comando = NULL;
  bool aplicado = false;

  if (comandoRecebidoEh(event, "armar")) {
    comando = "armar";
    sistemaArmado = true;
    aplicado = true;
  } else if (comandoRecebidoEh(event, "desarmar")) {
    comando = "desarmar";
    sistemaArmado = false;
    alarmeDisparado = false;
    aplicado = true;
  } else if (comandoRecebidoEh(event, "disparar")) {
    comando = "disparar";
    aplicado = sistemaArmado; // recusa disparo remoto com sistema desarmado
    if (aplicado) alarmeDisparado = true;
  } else if (comandoRecebidoEh(event, "silenciar")) {
    comando = "silenciar";
    alarmeDisparado = false;
    aplicado = true;
  } else {
    Serial.println("[MQTT] Comando desconhecido recebido, ignorado.");
    return;
  }

  publicarConfirmacao(comando, aplicado);
}

// --- MODO DEGRADADO ---
// Sem laço de reconexão manual do broker (o cliente ESP-IDF já reconecta
// sozinho) — o que este trecho acompanha é a política de degradação: sem
// contato há mais de TIMEOUT_MODO_DEGRADADO, o firmware para de esperar
// comando remoto e volta a decidir o alarme pela leitura direta do sensor,
// como na v2. É a única exceção à regra de que só tratarComando escreve em
// alarmeDisparado.
void avaliarModoDegradado(bool estadoAtual) {
  bool semContatoBroker = (millis() - ultimoContatoBroker) > TIMEOUT_MODO_DEGRADADO;

  if (semContatoBroker != modoDegradado) {
    modoDegradado = semContatoBroker;
    if (modoDegradado) {
      Serial.println("\n[DEGRADADO] Sem contato com o broker ha mais de 30s - decidindo localmente.");
    } else {
      Serial.println("\n[DEGRADADO] Contato com o broker restabelecido - decisao volta a ser remota.");
    }
  }

  if (modoDegradado) {
    alarmeDisparado = estadoAtual;
  }
}

// --- HANDLER DE EVENTOS MQTT ---
static void mqttEventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t) eventData;

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      brokerConectado = true;
      ultimoContatoBroker = millis();
      Serial.println("[MQTT] Conectado ao broker.");
      esp_mqtt_client_publish(clienteMqtt, TOPICO_STATUS_PRESENCA_DISPOSITIVO, "online", 0, 0, true);
      esp_mqtt_client_subscribe(clienteMqtt, TOPICO_COMANDO_ALARME, 0);
      // estadoValidado (não digitalRead direto) — a reconexão MQTT pode cair
      // bem no meio de um bounce do sensor; publicar o valor já filtrado
      // evita telemetria inconsistente logo na primeira mensagem.
      publicarTelemetria(estadoValidado);
      break;

    case MQTT_EVENT_DISCONNECTED:
      brokerConectado = false;
      Serial.println("[MQTT] Desconectado do broker.");
      break;

    case MQTT_EVENT_DATA:
      if (topicoRecebidoEh(event, TOPICO_COMANDO_ALARME)) {
        tratarComando(event);
      }
      break;

    case MQTT_EVENT_ERROR:
      Serial.println("[MQTT] Erro de conexao (confira certificado e hora do sistema).");
      break;

    default:
      break;
  }
}

// --- INICIALIZAÇÃO MQTT SOBRE WSS ---
void iniciarMqtt() {
  String clientId = "access-" + WiFi.macAddress();

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = MQTT_URI;
  cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
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

// --- BUZZER ---
void somPortaFechou() {
  tone(BUZZER_POS, 1800, 40);
  delay(60);
  tone(BUZZER_POS, 2400, 60);
}

void processarAlarmePortaAberta() {
  if (millis() - tempoUltimoAlarme > 150) {
    tempoUltimoAlarme = millis();
    tomAlarme = !tomAlarme;
    tone(BUZZER_POS, tomAlarme ? 1200 : 800, 120);
  }
}

// --- INTERFACE OLED ---
void desenharIconePorta(int x, int y, bool aberta) {
  display.drawRect(x, y, 22, 36, SSD1306_WHITE);
  if (aberta) {
    display.drawLine(x, y, x + 12, y + 5, SSD1306_WHITE);
    display.drawLine(x + 12, y + 5, x + 12, y + 36, SSD1306_WHITE);
    display.drawLine(x, y + 36, x + 12, y + 36, SSD1306_WHITE);
    display.fillCircle(x + 9, y + 22, 1, SSD1306_WHITE);
  } else {
    display.fillRect(x + 2, y + 2, 18, 32, SSD1306_WHITE);
    display.fillCircle(x + 16, y + 18, 2, SSD1306_BLACK);
  }
}

void atualizarDashboard(bool aberta) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(2, 2);
  display.print("ACCESS");

  display.setCursor(54, 2);
  if (WiFi.status() != WL_CONNECTED) {
    display.print("[NO WIFI]");
  } else if (modoDegradado) {
    display.print("[LOCAL]");
  } else if (!brokerConectado) {
    display.print("[NO MQTT]");
  } else {
    display.print("[ONLINE]");
  }

  display.setTextColor(SSD1306_WHITE);
  desenharIconePorta(8, 18, aberta);

  display.setTextSize(2);
  display.setCursor(38, 18);
  display.println(aberta ? "ABERTA" : "FECHADA");

  display.drawLine(38, 36, 124, 36, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(38, 41);
  display.print("Aberturas: ");
  display.println(contadorAberturas);

  unsigned long segundos = (millis() - tempoUltimaMudanca) / 1000;
  display.setCursor(38, 52);
  display.print("Tempo: ");
  if (segundos >= 60) {
    display.print(segundos / 60);
    display.print("m ");
  }
  display.print(segundos % 60);
  display.print("s");

  display.display();
}

void conectarWiFi() {
  Serial.println("\n[Wi-Fi] Conectando...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 25) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Conectado!");
    Serial.print("[Wi-Fi] IP DHCP: ");
    Serial.println(WiFi.localIP());
  }
}

void verificarReconexaoWiFi() {
  if (millis() - ultimaChecagemWifi > intervaloChecagemWifi) {
    ultimaChecagemWifi = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n[Wi-Fi] Desconectado! Reconectando...");
      WiFi.reconnect();
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(REED_PIN, INPUT_PULLUP);

  pinMode(BUZZER_POS, OUTPUT);
  pinMode(BUZZER_GND, OUTPUT);
  digitalWrite(BUZZER_GND, LOW); // Terra virtual

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_PRIMARIO)) {
    display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_ALTERNATIVO);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 25);
  display.println("Iniciando...");
  display.display();

  conectarWiFi();
  sincronizarHora();
  iniciarMqtt();

  // Inicializa a leitura validada com o estado real do pino no boot, para o
  // primeiro ciclo do loop() não enxergar uma transição falsa.
  leituraBrutaAnterior = digitalRead(REED_PIN);
  estadoValidado = leituraBrutaAnterior;
  tempoUltimaLeituraBruta = millis();

  ultimoEstado = estadoValidado;
  tempoUltimaMudanca = millis();
}

void loop() {
  verificarReconexaoWiFi();

  bool estadoAtual = lerSensorValidado();
  avaliarModoDegradado(estadoAtual);

  // 1. Telemetria periódica
  if (millis() - ultimaTelemetria > INTERVALO_TELEMETRIA) {
    ultimaTelemetria = millis();
    publicarTelemetria(ultimoEstado);
  }

  // 2. Transições de estado do sensor
  if (estadoAtual != ultimoEstado) {
    if (estadoAtual == true) {
      contadorAberturas++;
      Serial.printf("\n[EVENTO] Porta Aberta! Total: %d\n", contadorAberturas);
      publicarTelemetria(true);
    } else {
      Serial.println("\n[EVENTO] Porta Fechada.");
      somPortaFechou();
      publicarTelemetria(false);
    }
    ultimoEstado = estadoAtual;
    tempoUltimaMudanca = millis();
  }

  // 3. Sirene: decisão vem da rede (alarmeDisparado), exceto em modo
  // degradado, onde avaliarModoDegradado() já a alimenta pelo sensor direto.
  if (alarmeDisparado) {
    processarAlarmePortaAberta();
  } else {
    noTone(BUZZER_POS);
  }

  // 4. Renderização gráfica com cadência própria, sem bloquear o loop
  if (millis() - ultimaAtualizacaoOled > INTERVALO_ATUALIZACAO_OLED) {
    ultimaAtualizacaoOled = millis();
    atualizarDashboard(estadoAtual);
  }
}