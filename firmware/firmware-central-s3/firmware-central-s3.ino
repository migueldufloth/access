#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "secrets.h"
#include "config.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// =============================================================================
// PROTOCOLO ESP-NOW (Placa 1 <-> Placa 2, sem depender de internet/broker)
// =============================================================================
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  tipo;         // 1 = MOVIMENTO (Sat->Cen), 2 = ALARME/HB (Cen->Sat)
  uint8_t  valor;        // tipo 1: 1=DETECTADO, 0=LIMPO | tipo 2: 1=ALARME, 0=OFF
  uint8_t  fonte;        // tipo 1: 0=nenhum, 1=PIR, 2=ULTRA, 3=AMBOS | tipo 2: portaAberta
  uint8_t  distanciaCm;  // tipo 1: distancia em cm (255=sem leitura) | tipo 2: canal Wi-Fi
} PacoteAccessEspNow;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool espNowAtivo = false;
bool modoOfflineEspNow = false;

// --- CONTROLE MQTT (ESP-IDF, suporta wss:// TLS e mqtt:// LAN) ---
esp_mqtt_client_handle_t clienteMqtt = nullptr;
bool mqttIniciado = false;
bool horaSincronizada = false;
volatile bool brokerConectado = false;

// --- VARIAVEIS DE CONTROLE E LOGICA COMBINADA ---
int contadorAberturas = 0;
bool portaAberta = false;
volatile bool temGentePassando = false;
volatile uint8_t fonteMovimento = 0;       // 0=nenhum, 1=PIR, 2=ULTRA, 3=AMBOS
volatile uint8_t distanciaMovimentoCm = 255;
volatile bool movimentoMudouPendente = false;
volatile unsigned long ultimoPacoteSateliteMs = 0;
bool sateliteOnlineAnterior = false;

bool alarmeDisparado = false;
bool jaNotificouAlarme = false;

unsigned long tempoUltimaMudanca = 0;
volatile unsigned long ultimoMomentoComGente = 0;
unsigned long tempoUltimoAlarme = 0;
unsigned long ultimaChecagemWifi = 0;
unsigned long ultimaTelemetria = 0;
unsigned long ultimoHeartbeatEspNow = 0;
bool tomAlarme = false;

// =============================================================================
// AUDIO I2S NATIVO (MAX98357 — sem bibliotecas MP3 pesadas para estabilidade OLED)
// =============================================================================
void setupSpeaker() {
  i2s_config_t spk_cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true
  };

  i2s_pin_config_t spk_pins = {
    .bck_io_num = SPK_BCLK,
    .ws_io_num = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_SPK_PORT, &spk_cfg, 0, NULL);
  i2s_set_pin(I2S_SPK_PORT, &spk_pins);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
}

void tocarCaixaSom(int freq, int duracaoMs, float amplitude) {
  int totalFrames = (SAMPLE_RATE * duracaoMs) / 1000;
  int16_t buffer[256];
  size_t bytesEscritos = 0;
  int framesEnviados = 0;
  float fase = 0.0;
  float inc = (2.0 * M_PI * freq) / SAMPLE_RATE;

  while (framesEnviados < totalFrames) {
    int lote = min(128, totalFrames - framesEnviados);
    for (int i = 0; i < lote; i++) {
      int16_t amostra = 0;
      if (freq > 0) {
        float env = 1.0 - ((float)(framesEnviados + i) / totalFrames);
        amostra = (int16_t)(sin(fase) * amplitude * env);
        fase += inc;
        if (fase >= 2.0 * M_PI) fase -= 2.0 * M_PI;
      }
      buffer[i * 2]     = amostra;
      buffer[i * 2 + 1] = amostra;
    }
    i2s_write(I2S_SPK_PORT, buffer, lote * 2 * sizeof(int16_t), &bytesEscritos, portMAX_DELAY);
    framesEnviados += lote;
  }
  int16_t silencio[256] = {0};
  i2s_write(I2S_SPK_PORT, silencio, sizeof(silencio), &bytesEscritos, portMAX_DELAY);
}

void somPortaAbriuSuave() {
  tocarCaixaSom(587, 60, VOLUME_BIPE_PORTA);
  delay(20);
  tocarCaixaSom(880, 80, VOLUME_BIPE_PORTA);
}

void somPortaFechou() {
  tocarCaixaSom(1800, 50, VOLUME_BIPE_PORTA);
  delay(30);
  tocarCaixaSom(2400, 80, VOLUME_BIPE_PORTA);
}

void processarAlarmeCaixaSom() {
  if (millis() - tempoUltimoAlarme > 160) {
    tempoUltimoAlarme = millis();
    tomAlarme = !tomAlarme;
    digitalWrite(LED_POS_PIN, tomAlarme ? HIGH : LOW);
    if (tomAlarme) {
      tocarCaixaSom(1200, 110, VOLUME_SIRENE);
    } else {
      tocarCaixaSom(800, 110, VOLUME_SIRENE);
    }
  }
}

// =============================================================================
// ESP-NOW (comunicacao local imediata com o Satelite)
// =============================================================================
void aplicarEstadoMovimento(bool detectado) {
  bool anterior = temGentePassando;
  if (detectado) {
    temGentePassando = true;
    ultimoMomentoComGente = millis();
  } else {
    if (anterior) {
      temGentePassando = false;
      ultimoMomentoComGente = millis() - TOLERANCIA_SEM_GENTE_MS;
    } else {
      temGentePassando = false;
    }
  }
  if (anterior != temGentePassando) {
    movimentoMudouPendente = true;
  }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != (int)sizeof(PacoteAccessEspNow)) return;

  PacoteAccessEspNow pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != ESPNOW_MAGIC) return;

  if (pkt.tipo == ESPNOW_TIPO_MOVIMENTO) {
    ultimoPacoteSateliteMs = millis();
    fonteMovimento = pkt.fonte;
    distanciaMovimentoCm = pkt.distanciaCm;
    aplicarEstadoMovimento(pkt.valor == 1);
  }
}

void enviarEstadoEspNow() {
  if (!espNowAtivo) return;

  uint8_t canalAtual = CANAL_ESPNOW_PADRAO;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&canalAtual, &sec);

  PacoteAccessEspNow pkt = {};
  pkt.magic       = ESPNOW_MAGIC;
  pkt.tipo        = ESPNOW_TIPO_ALARME;
  pkt.valor       = alarmeDisparado ? 1 : 0;
  pkt.fonte       = portaAberta ? 1 : 0;
  pkt.distanciaCm = canalAtual;

  esp_now_send(BROADCAST_MAC, (const uint8_t*)&pkt, sizeof(pkt));
}

bool iniciarEspNow() {
  if (espNowAtivo) return true;

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Falha ao inicializar!");
    return false;
  }

  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BROADCAST_MAC, 6);
  peerInfo.channel = 0; // Usa o canal atual do Wi-Fi STA
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("[ESP-NOW] Falha ao registrar peer broadcast!");
      return false;
    }
  }

  espNowAtivo = true;
  Serial.println("[ESP-NOW] Pronto para comunicacao direta com o Satelite.");
  return true;
}

bool sateliteConectado() {
  return ultimoPacoteSateliteMs > 0 && (millis() - ultimoPacoteSateliteMs) <= TIMEOUT_SATELITE_MS;
}

// =============================================================================
// NOTIFICACAO PUSH HTTP (ntfy.sh)
// =============================================================================
void enviarNotificacao(const String& mensagem) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://ntfy.sh/" + String(NTFY_TOPIC);

    http.setTimeout(4000);
    http.begin(url);
    http.addHeader("Title", "Alerta Porta Esquecida");
    http.addHeader("Priority", "high");
    http.addHeader("Tags", "warning,door");

    int codigo = http.POST(mensagem);
    Serial.printf("[NTFY] POST %s -> HTTP %d\n", url.c_str(), codigo);
    http.end();
  }
}

// =============================================================================
// REDE, SNTP E BROKER MOSQUITTO (WSS TLS / LAN MQTT)
// =============================================================================
static bool uriUsaTls(const char* uri) {
  return strncmp(uri, "wss://", 6) == 0 || strncmp(uri, "mqtts://", 8) == 0;
}

static bool topicoRecebidoEh(esp_mqtt_event_handle_t event, const char* alvo) {
  size_t tamanhoAlvo = strlen(alvo);
  return event->topic_len == (int)tamanhoAlvo && memcmp(event->topic, alvo, tamanhoAlvo) == 0;
}

static bool payloadEh(esp_mqtt_event_handle_t event, const char* alvo) {
  size_t tamanhoAlvo = strlen(alvo);
  return event->data_len == (int)tamanhoAlvo && memcmp(event->data, alvo, tamanhoAlvo) == 0;
}

void publicarMqtt(const char* topico, const char* payload, int qos, bool retain) {
  if (!brokerConectado || clienteMqtt == nullptr) return;
  esp_mqtt_client_publish(clienteMqtt, topico, payload, strlen(payload), qos, retain ? 1 : 0);
}

void publicarTelemetria() {
  if (!brokerConectado || clienteMqtt == nullptr) return;

  char buffer[240];
  snprintf(
    buffer,
    sizeof(buffer),
    "{\"porta\":\"%s\",\"armado\":true,\"movimento\":%s,\"disparado\":%s,\"aberturas\":%d,\"rssi\":%d}",
    portaAberta ? "aberta" : "fechada",
    temGentePassando ? "true" : "false",
    alarmeDisparado ? "true" : "false",
    contadorAberturas,
    WiFi.RSSI()
  );

  esp_mqtt_client_publish(clienteMqtt, TOPICO_TELEMETRIA, buffer, strlen(buffer), 0, 0);
}

bool sincronizarHora() {
  if (WiFi.status() != WL_CONNECTED) return false;

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[SNTP] Aguardando hora valida");

  unsigned long inicio = millis();
  time_t agora = time(nullptr);
  while (agora < 8 * 3600 * 2 && (millis() - inicio) < INTERVALO_SNTP_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
    agora = time(nullptr);
  }

  horaSincronizada = (agora >= 8 * 3600 * 2);
  if (horaSincronizada) {
    Serial.println("\n[SNTP] Hora sincronizada.");
  } else {
    Serial.println("\n[SNTP] Timeout ao sincronizar hora.");
  }
  return horaSincronizada;
}

static void mqttEventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      brokerConectado = true;
      Serial.println("[MQTT] Conectado ao broker Mosquitto.");
      esp_mqtt_client_publish(clienteMqtt, TOPICO_STATUS_PRESENCA, "online", 6, 1, 1);
      esp_mqtt_client_publish(clienteMqtt, TOPICO_STATUS_CENTRAL, "online", 6, 1, 1);
      esp_mqtt_client_publish(clienteMqtt, TOPICO_STATUS_SATELITE, sateliteConectado() ? "online" : "offline", 0, 1, 1);
      esp_mqtt_client_publish(clienteMqtt, TOPICO_PORTA, portaAberta ? "ABERTA" : "FECHADA", 0, 1, 1);
      esp_mqtt_client_publish(clienteMqtt, TOPICO_MOVIMENTO, temGentePassando ? "DETECTADO" : "LIMPO", 0, 1, 1);
      esp_mqtt_client_publish(clienteMqtt, TOPICO_ALARME, alarmeDisparado ? "1" : "0", 1, 1, 1);
      publicarTelemetria();
      break;

    case MQTT_EVENT_DISCONNECTED:
      brokerConectado = false;
      Serial.println("[MQTT] Desconectado do broker.");
      break;

    case MQTT_EVENT_DATA:
      break;

    case MQTT_EVENT_ERROR:
      Serial.println("[MQTT] Erro de conexao (ESP-NOW continua ativo normalmente).");
      break;

    default:
      break;
  }
}

void iniciarMqtt() {
  if (mqttIniciado) return;

  static String clientId;
  clientId = "access-central-s3-" + WiFi.macAddress();

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = MQTT_URI;
  if (uriUsaTls(MQTT_URI)) {
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  if (strlen(MQTT_USER) > 0) {
    cfg.credentials.username = MQTT_USER;
    cfg.credentials.authentication.password = MQTT_PASS;
  }
  cfg.credentials.client_id = clientId.c_str();
  cfg.session.last_will.topic = TOPICO_STATUS_PRESENCA;
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.msg_len = 7;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = true;

  clienteMqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(clienteMqtt, MQTT_EVENT_ANY, mqttEventHandler, NULL);
  esp_mqtt_client_start(clienteMqtt);
  mqttIniciado = true;
}

static const char* nomeStatusWifi(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "REDE_NAO_ENCONTRADA (confira 2.4GHz / SSID)";
    case WL_SCAN_COMPLETED:  return "SCAN_OK";
    case WL_CONNECTED:       return "CONECTADO";
    case WL_CONNECT_FAILED:  return "FALHA_AUTENTICACAO (confira senha)";
    case WL_CONNECTION_LOST: return "CONEXAO_PERDIDA";
    case WL_DISCONNECTED:    return "DESCONECTADO";
    default:                 return "OUTRO";
  }
}

void verificarReconexaoWiFi() {
  // Em modo offline (Wi-Fi falhou no boot), mantem o radio travado no canal fixo
  // para nao derrubar o ESP-NOW com varreduras continuas de Wi-Fi.
  if (modoOfflineEspNow) return;

  if (millis() - ultimaChecagemWifi < INTERVALO_CHECAGEM_WIFI) return;
  ultimaChecagemWifi = millis();

  wl_status_t st = WiFi.status();
  if (st != WL_CONNECTED) {
    Serial.printf("[Wi-Fi] Status=%d (%s). Reconectando...\n",
                  (int)st, nomeStatusWifi(st));
    WiFi.setTxPower(WIFI_TX_POWER);
    WiFi.reconnect();
    return;
  }

  // Se o Wi-Fi conectou depois do boot, sincroniza SNTP (quando TLS) e inicia o MQTT
  if (!mqttIniciado) {
    Serial.printf("[Wi-Fi] Conectado! IP: %s | Canal: %d | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
    if (!uriUsaTls(MQTT_URI) || horaSincronizada || sincronizarHora()) {
      iniciarMqtt();
    }
  }
}

// =============================================================================
// INTERFACE OLED SSD1306
// =============================================================================
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

void atualizarDashboard() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(3, 2);
  display.print("ACCESS");

  bool satOk = sateliteConectado();
  display.setCursor(46, 2);
  if (brokerConectado && satOk) {
    display.print("[MQTT+NOW]");
  } else if (brokerConectado) {
    display.print("[MQTT ON]");
  } else if (satOk) {
    display.print("[ESP-NOW OK]");
  } else if (WiFi.status() == WL_CONNECTED) {
    display.print("[WIFI OK]");
  } else {
    display.print("[SEM REDE]");
  }

  display.setTextColor(SSD1306_WHITE);
  desenharIconePorta(8, 18, portaAberta);

  display.setTextSize(2);
  display.setCursor(38, 18);
  if (!portaAberta) {
    display.println("FECHADA");
  } else if (alarmeDisparado) {
    display.println("ALARME!");
  } else {
    display.println("ABERTA");
  }

  display.drawLine(38, 36, 124, 36, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(38, 41);
  if (!portaAberta) {
    display.print("Aberturas: ");
    display.println(contadorAberturas);
  } else if (temGentePassando) {
    if (fonteMovimento == 1) {
      display.println("Mov: PIR");
    } else if (fonteMovimento == 2) {
      display.print("Mov: ");
      display.print(distanciaMovimentoCm);
      display.println("cm");
    } else if (fonteMovimento == 3) {
      display.print("Mov:PIR+");
      display.print(distanciaMovimentoCm);
      display.println("cm");
    } else {
      display.println("Movimento: SIM");
    }
  } else if (alarmeDisparado) {
    display.println("SEM MOVIMENTO!");
  } else {
    display.println("Aguardando...");
  }

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

// =============================================================================
// SETUP E LOOP PRINCIPAL
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Alimentacao virtual do OLED com capacidade maxima de corrente nos pinos
  pinMode(OLED_GND_PIN, OUTPUT);
  digitalWrite(OLED_GND_PIN, LOW);
  gpio_set_drive_capability((gpio_num_t)OLED_GND_PIN, GPIO_DRIVE_CAP_3);

  pinMode(OLED_VCC_PIN, OUTPUT);
  digitalWrite(OLED_VCC_PIN, HIGH);
  gpio_set_drive_capability((gpio_num_t)OLED_VCC_PIN, GPIO_DRIVE_CAP_3);

  // Sensor magnetico MC-38
  pinMode(REED_GND_PIN, OUTPUT);
  digitalWrite(REED_GND_PIN, LOW);
  pinMode(REED_PIN, INPUT_PULLUP);

  // LED de alerta local
  pinMode(LED_GND_PIN, OUTPUT);
  digitalWrite(LED_GND_PIN, LOW);
  pinMode(LED_POS_PIN, OUTPUT);
  digitalWrite(LED_POS_PIN, LOW);

  delay(250);

  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_PRIMARIO)) {
    display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_ALTERNATIVO);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(12, 20);
  display.println("Conectando Wi-Fi:");
  display.setCursor(12, 34);
  display.println(WIFI_SSID);
  display.display();

  setupSpeaker();
  somPortaFechou();

  // Potencia de TX controlada evita queda de tensao na OLED durante o boot do Wi-Fi
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Mantem radio acordado para receber 100% dos pacotes ESP-NOW
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_TX_POWER);

  // Inicia ESP-NOW ja no boot (funciona mesmo sem Wi-Fi/internet)
  iniciarEspNow();

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 25) {
    delay(400);
    Serial.print(".");
    tentativas++;
  }
  Serial.println();

  portaAberta = (digitalRead(REED_PIN) == HIGH);
  tempoUltimaMudanca = millis();
  ultimoMomentoComGente = millis();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[Wi-Fi] Conectado! IP: %s | Canal: %d | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());

    if (uriUsaTls(MQTT_URI)) {
      display.clearDisplay();
      display.setCursor(8, 26);
      display.println("Sincronizando hora...");
      display.display();
      if (sincronizarHora()) {
        iniciarMqtt();
      }
    } else {
      iniciarMqtt();
    }
  } else {
    wl_status_t st = WiFi.status();
    modoOfflineEspNow = true;
    Serial.printf("[Wi-Fi] Sem conexao no boot. Status=%d (%s)\n",
                  (int)st, nomeStatusWifi(st));
    Serial.printf("[ESP-NOW] Travando canal %d para comunicacao local estavel com o Satelite.\n",
                  CANAL_ESPNOW_PADRAO);
    WiFi.disconnect(false);
    esp_wifi_set_channel(CANAL_ESPNOW_PADRAO, WIFI_SECOND_CHAN_NONE);
  }

  enviarEstadoEspNow();
}

void loop() {
  verificarReconexaoWiFi();

  // 0. Replica mudancas de movimento/status do Satelite para o Serial e Mosquitto
  if (movimentoMudouPendente) {
    movimentoMudouPendente = false;
    Serial.printf("[ESP-NOW] Movimento: %s\n", temGentePassando ? "DETECTADO" : "LIMPO");
    publicarMqtt(TOPICO_MOVIMENTO, temGentePassando ? "DETECTADO" : "LIMPO", 1, true);
    publicarTelemetria();
  }

  bool satOnlineAgora = sateliteConectado();
  if (satOnlineAgora != sateliteOnlineAnterior) {
    sateliteOnlineAnterior = satOnlineAgora;
    Serial.printf("[ESP-NOW] Satelite %s\n", satOnlineAgora ? "CONECTADO" : "OFFLINE");
    if (!satOnlineAgora && temGentePassando) {
      // Nao deixa "Movimento: SIM" travado se perder contato com o Satelite
      temGentePassando = false;
      fonteMovimento = 0;
      distanciaMovimentoCm = 255;
    }
    publicarMqtt(TOPICO_STATUS_SATELITE, satOnlineAgora ? "online" : "offline", 1, true);
  }

  // 1. Leitura do sensor magnetico com debounce
  bool estadoAtual = (digitalRead(REED_PIN) == HIGH);

  if (estadoAtual != portaAberta) {
    delay(DEBOUNCE_REED_MS);
    estadoAtual = (digitalRead(REED_PIN) == HIGH);

    if (estadoAtual != portaAberta) {
      portaAberta = estadoAtual;
      tempoUltimaMudanca = millis();

      if (portaAberta) {
        contadorAberturas++;
        ultimoMomentoComGente = millis();
        jaNotificouAlarme = false;
        somPortaAbriuSuave();
        enviarEstadoEspNow();
        publicarMqtt(TOPICO_PORTA, "ABERTA", 1, true);
        publicarTelemetria();
      } else {
        alarmeDisparado = false;
        jaNotificouAlarme = false;
        digitalWrite(LED_POS_PIN, LOW);
        enviarEstadoEspNow();
        publicarMqtt(TOPICO_PORTA, "FECHADA", 1, true);
        publicarMqtt(TOPICO_ALARME, "0", 1, true);
        publicarTelemetria();
        somPortaFechou();
      }
    }
  }

  // 2. Regra de negocio combinada (Porta + Movimento do Satelite)
  if (portaAberta) {
    if (temGentePassando) {
      ultimoMomentoComGente = millis();
      if (alarmeDisparado) {
        alarmeDisparado = false;
        digitalWrite(LED_POS_PIN, LOW);
        enviarEstadoEspNow();
        publicarMqtt(TOPICO_ALARME, "0", 1, true);
        publicarTelemetria();
      }
    } else {
      if (millis() - ultimoMomentoComGente >= TOLERANCIA_SEM_GENTE_MS) {
        if (!alarmeDisparado) {
          alarmeDisparado = true;
          enviarEstadoEspNow();
          publicarMqtt(TOPICO_ALARME, "1", 1, true);
          publicarTelemetria();

          if (!jaNotificouAlarme) {
            jaNotificouAlarme = true;
            enviarNotificacao(
              "ALERTA: Porta deixada aberta e sem movimento! (Abertura #" + String(contadorAberturas) + ")"
            );
          }
        }
      }
    }
  } else {
    if (alarmeDisparado) {
      alarmeDisparado = false;
      enviarEstadoEspNow();
    }
  }

  // 3. Heartbeat ESP-NOW periodico (mantem o Satelite sincronizado no canal e estado de alarme)
  if (millis() - ultimoHeartbeatEspNow >= INTERVALO_ESPNOW_HB_MS) {
    ultimoHeartbeatEspNow = millis();
    enviarEstadoEspNow();
  }

  // 4. Sirene I2S + LED local sincronizado
  if (alarmeDisparado) {
    processarAlarmeCaixaSom();
  } else {
    digitalWrite(LED_POS_PIN, LOW);
  }

  // 5. Telemetria periodica para o broker Mosquitto / dashboard
  if (millis() - ultimaTelemetria >= INTERVALO_TELEMETRIA) {
    ultimaTelemetria = millis();
    publicarTelemetria();
  }

  // 6. Atualiza OLED
  atualizarDashboard();
  delay(30);
}
