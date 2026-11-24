#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <driver/i2s.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "secrets.h"
#include "config.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Preferences prefs;

// =============================================================================
// PROTOCOLO ESP-NOW (Placa 1 <-> Placa 2, sirene/LED sincronizados mesmo offline)
// =============================================================================
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  tipo;         // 1 = MOVIMENTO (Sat->Cen), 2 = ALARME/HB (Cen->Sat)
  uint8_t  valor;        // tipo 1: 1=DETECTADO, 0=LIMPO | tipo 2: 1=LED/ALARME, 0=OFF
  uint8_t  fonte;        // tipo 1: 0=nenhum, 1=PIR, 2=ULTRA, 3=AMBOS | tipo 2: portaAberta
  uint8_t  distanciaCm;  // tipo 1: distancia em cm (255=sem leitura) | tipo 2: canal Wi-Fi
} PacoteAccessEspNow;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool espNowAtivo = false;

// =============================================================================
// CONTROLE MQTT (ESP-IDF, wss:// TLS + mqtt:// LAN) E FILA DE COMANDOS
// =============================================================================
static const char* LWT_PAYLOAD = "{\"v\":1,\"deviceId\":\"" DEVICE_ID "\",\"status\":\"offline\"}";
static const char* TOPICO_COMANDO = PREFIXO_MQTT "comando";

esp_mqtt_client_handle_t clienteMqtt = nullptr;
bool mqttIniciado = false;
bool horaSincronizada = false;
volatile bool brokerConectado = false;
volatile bool precisaPublicarConexao = false;

#define FILA_CMD_TAM 4
#define CMD_BUF_MAX  513
struct SlotComando {
  char payload[CMD_BUF_MAX];
  size_t len;
  bool ocupado;
};
SlotComando filaComandos[FILA_CMD_TAM];
volatile uint8_t filaHead = 0;
volatile uint8_t filaTail = 0;
portMUX_TYPE muxFila = portMUX_INITIALIZER_UNLOCKED;

// =============================================================================
// CACHE DE DUPLICADOS (ultimos 16 commandId — CONTRATOS.md secao 4)
// =============================================================================
struct RegistroConfirmacao {
  char fase[12];
  char resultado[12];
  char erro[28];
  char motivo[64];
};
struct EntradaHistoricoCmd {
  char commandId[40];
  uint8_t qtd;
  RegistroConfirmacao confs[2];
};
#define HIST_CMD_MAX 16
EntradaHistoricoCmd historicoCmds[HIST_CMD_MAX];
uint8_t histCount = 0;
uint8_t histNext = 0;

// =============================================================================
// ESTADO DO DISPOSITIVO (CONTRATOS.md v1) + NVS
// =============================================================================
enum ModoSistema   : uint8_t { MODO_AUTOMATICO = 0, MODO_MANUAL = 1, MODO_SEGURO = 2 };
enum EstadoAlarme  : uint8_t { ALARME_INATIVO = 0, ALARME_ENTRADA = 1, ALARME_DISPARADO = 2 };
enum EstadoSensor  : uint8_t { SENSOR_OK = 0, SENSOR_FALHA_ABERTO = 1, SENSOR_FALHA_CURTO = 2, SENSOR_CALIBRANDO = 3 };

ModoSistema  modoAtual   = MODO_AUTOMATICO;
EstadoAlarme alarmeAtual = ALARME_INATIVO;
EstadoSensor sensorAtual = SENSOR_OK;
bool         sistemaArmado = false;

uint16_t cfgTempoEntradaS       = DEFAULT_TEMPO_ENTRADA_S;
uint16_t cfgIntervaloTelemetria = DEFAULT_INTERVALO_TEL_S;
uint16_t cfgTimeoutManualS      = DEFAULT_TIMEOUT_MANUAL_S;

char ultimoComandoId[40] = "";

// Controle operacional local
bool portaAberta = false;
bool leituraReedAnterior = false;
unsigned long ultimoDebounceReedMs = 0;
uint32_t contadorAberturas = 0;

bool ledAlertaAtivo = false;
bool silenciadoManualmente = false;
bool disparoManualForcado = false;
bool movEstavaAtivoNoManual = false;
bool otaEmAndamento = false;

volatile bool temGentePassando = false;
volatile uint8_t fonteMovimento = 0;
volatile uint8_t distanciaMovimentoCm = 255;
volatile unsigned long ultimoPacoteSateliteMs = 0;
volatile bool movimentoMudouPendente = false;

unsigned long tempoUltimaMudancaMs = 0;
unsigned long entradaInicioMs = 0;
unsigned long inicioFalhaSensorMs = 0;
EstadoSensor  falhaCandidata = SENSOR_OK;
bool          falhaForcadaSerial = false;

unsigned long tempoUltimoAlarmeMs = 0;
unsigned long ultimaChecagemWifiMs = 0;
unsigned long ultimoBeginWifiMs = 0;
unsigned long ultimaTelemetriaMs = 0;
unsigned long ultimoHeartbeatEspNowMs = 0;
unsigned long ultimaAtualizacaoOledMs = 0;
bool tomAlarme = false;

// =============================================================================
// HELPERS DE STRING DE CONTRATO
// =============================================================================
const char* textoModo(ModoSistema m) {
  switch (m) {
    case MODO_MANUAL: return "manual";
    case MODO_SEGURO: return "seguro";
    default:          return "automatico";
  }
}

const char* textoAlarme(EstadoAlarme a) {
  switch (a) {
    case ALARME_ENTRADA:   return "entrada";
    case ALARME_DISPARADO: return "disparado";
    default:               return "inativo";
  }
}

const char* textoSensor(EstadoSensor s) {
  switch (s) {
    case SENSOR_FALHA_ABERTO: return "falha_aberto";
    case SENSOR_FALHA_CURTO:  return "falha_curto";
    case SENSOR_CALIBRANDO:   return "calibrando";
    default:                  return "ok";
  }
}

// =============================================================================
// PERSISTENCIA NVS (Preferences)
// =============================================================================
void carregarNvs() {
  prefs.begin("access", false);
  sistemaArmado          = prefs.getBool("armado", false);
  cfgTempoEntradaS       = prefs.getUShort("tEnt2", DEFAULT_TEMPO_ENTRADA_S);
  cfgIntervaloTelemetria = prefs.getUShort("intTel", DEFAULT_INTERVALO_TEL_S);
  cfgTimeoutManualS      = prefs.getUShort("tManual", DEFAULT_TIMEOUT_MANUAL_S);

  // Garante faixas validas mesmo em flash nova/corrompida
  if (cfgTempoEntradaS > 120) cfgTempoEntradaS = DEFAULT_TEMPO_ENTRADA_S;
  if (cfgIntervaloTelemetria < 1 || cfgIntervaloTelemetria > 300) cfgIntervaloTelemetria = DEFAULT_INTERVALO_TEL_S;
  if (cfgTimeoutManualS < 60 || cfgTimeoutManualS > 7200) cfgTimeoutManualS = DEFAULT_TIMEOUT_MANUAL_S;

  Serial.printf("[NVS] Carregado: armado=%d, tempoEntrada_s=%u, intervaloTelemetria_s=%u, timeoutManual_s=%u\n",
                sistemaArmado ? 1 : 0, cfgTempoEntradaS, cfgIntervaloTelemetria, cfgTimeoutManualS);
}

void salvarNvs() {
  prefs.putBool("armado", sistemaArmado);
  prefs.putUShort("tEnt2", cfgTempoEntradaS);
  prefs.putUShort("intTel", cfgIntervaloTelemetria);
  prefs.putUShort("tManual", cfgTimeoutManualS);
  Serial.printf("[NVS] Salvo: armado=%d, tempoEntrada_s=%u, intervaloTelemetria_s=%u, timeoutManual_s=%u\n",
                sistemaArmado ? 1 : 0, cfgTempoEntradaS, cfgIntervaloTelemetria, cfgTimeoutManualS);
}

// =============================================================================
// AUDIO I2S NATIVO (MAX98357 — driver/i2s.h)
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

void processarAlertaVisualESonoro() {
  bool sireneAtiva = (alarmeAtual == ALARME_DISPARADO);
  if (!ledAlertaAtivo && !sireneAtiva) {
    digitalWrite(LED_POS_PIN, LOW);
    return;
  }

  if (millis() - tempoUltimoAlarmeMs > 160) {
    tempoUltimoAlarmeMs = millis();
    tomAlarme = !tomAlarme;
    digitalWrite(LED_POS_PIN, (ledAlertaAtivo && tomAlarme) ? HIGH : LOW);
    if (sireneAtiva) {
      tocarCaixaSom(tomAlarme ? 1200 : 800, 110, VOLUME_SIRENE);
    }
  }
}

// =============================================================================
// ESP-NOW (sincronizacao local imediata com o Satelite janela-01)
// =============================================================================
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
    bool novoMov = (pkt.valor == 1);
    fonteMovimento = pkt.fonte;
    distanciaMovimentoCm = pkt.distanciaCm;
    if (novoMov != temGentePassando) {
      temGentePassando = novoMov;
      movimentoMudouPendente = true;
    }
  }
}

void enviarEstadoEspNow() {
  if (!espNowAtivo) return;
  // Nao interrompe o handshake Wi-Fi nos primeiros 10s apos WiFi.begin()
  if (WiFi.status() != WL_CONNECTED && (millis() - ultimoBeginWifiMs) < 10000) return;

  uint8_t canalAtual = CANAL_ESPNOW_PADRAO;
  wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
  esp_wifi_get_channel(&canalAtual, &sec);

  PacoteAccessEspNow pkt = {};
  pkt.magic       = ESPNOW_MAGIC;
  pkt.tipo        = ESPNOW_TIPO_ALARME;
  pkt.valor       = (ledAlertaAtivo || alarmeAtual == ALARME_DISPARADO) ? 1 : 0;
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
  peerInfo.channel = 0;
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
    esp_now_add_peer(&peerInfo);
  }
  espNowAtivo = true;
  Serial.println("[ESP-NOW] Pronto para sincronizacao local com o Satelite.");
  return true;
}

bool sateliteConectado() {
  return ultimoPacoteSateliteMs > 0 && (millis() - ultimoPacoteSateliteMs) <= TIMEOUT_SATELITE_MS;
}

// =============================================================================
// CONTRATO MQTT v1 — MONTAGEM E PUBLICACAO DE MENSAGENS
// =============================================================================
static bool uriUsaTls(const char* uri) {
  return strncmp(uri, "wss://", 6) == 0 || strncmp(uri, "mqtts://", 8) == 0;
}

bool relogioValido() {
  return time(nullptr) > 1700000000;
}

void preencherBase(JsonDocument& doc) {
  doc["v"] = 1;
  doc["deviceId"] = DEVICE_ID;
  time_t agora = time(nullptr);
  if (agora > 1700000000) {
    struct tm tmUtc;
    gmtime_r(&agora, &tmUtc);
    char ts[25];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmUtc);
    doc["ts"] = ts;
  } else {
    doc["ts"] = nullptr;
  }
  doc["uptime_s"] = (uint32_t)(millis() / 1000);
}

void publicarJson(const char* sufixo, JsonDocument& doc, int qos, bool retain) {
  if (!brokerConectado || clienteMqtt == nullptr) return;
  char topico[80];
  char buf[512];
  snprintf(topico, sizeof(topico), PREFIXO_MQTT "%s", sufixo);
  size_t n = serializeJson(doc, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf)) {
    Serial.printf("[MQTT] ERRO: payload excedeu 512 bytes em %s\n", topico);
    return;
  }
  esp_mqtt_client_publish(clienteMqtt, topico, buf, n, qos, retain ? 1 : 0);
  Serial.printf("[MQTT] -> %s %s\n", topico, buf);
}

void publicarStatusOnline() {
  JsonDocument doc;
  preencherBase(doc);
  doc["status"] = "online";
  doc["fw"] = FW_VERSION;
  publicarJson("status", doc, 1, true);
}

void publicarEstado() {
  JsonDocument doc;
  preencherBase(doc);
  doc["fw"] = FW_VERSION;
  doc["modo"] = textoModo(modoAtual);
  doc["armado"] = sistemaArmado;
  doc["alarme"] = textoAlarme(alarmeAtual);
  doc["sensor"] = textoSensor(sensorAtual);

  JsonObject cfg = doc["config"].to<JsonObject>();
  cfg["tempoEntrada_s"] = cfgTempoEntradaS;
  cfg["intervaloTelemetria_s"] = cfgIntervaloTelemetria;
  cfg["timeoutManual_s"] = cfgTimeoutManualS;

  if (ultimoComandoId[0] != '\0') {
    doc["ultimoComandoId"] = ultimoComandoId;
  } else {
    doc["ultimoComandoId"] = nullptr;
  }

  publicarJson("estado", doc, 1, true);
}

void publicarTelemetria() {
  if (!brokerConectado) return;
  JsonDocument doc;
  preencherBase(doc);

  int rssi = constrain((int)WiFi.RSSI(), -120, 0);
  uint32_t aberturasVal = min(contadorAberturas, (uint32_t)1000000);

  JsonArray med = doc["medicoes"].to<JsonArray>();
  JsonObject m1 = med.add<JsonObject>();
  m1["grandeza"] = "abertura";
  m1["valor"]    = portaAberta ? 1 : 0; // inteiro 0/1, nunca bool
  m1["unidade"]  = "bool";

  JsonObject m2 = med.add<JsonObject>();
  m2["grandeza"] = "aberturas";
  m2["valor"]    = aberturasVal;
  m2["unidade"]  = "contagem";

  JsonObject m3 = med.add<JsonObject>();
  m3["grandeza"] = "rssi";
  m3["valor"]    = rssi;
  m3["unidade"]  = "dBm";

  publicarJson("telemetria", doc, 0, false);
}

void publicarConfirmacaoRegistro(const char* commandId, const RegistroConfirmacao& r, bool duplicado) {
  JsonDocument doc;
  preencherBase(doc);
  doc["commandId"] = commandId;
  doc["fase"]      = r.fase;
  doc["resultado"] = r.resultado;
  if (r.erro[0] != '\0')   doc["erro"]   = r.erro;
  else                     doc["erro"]   = nullptr;
  if (r.motivo[0] != '\0') doc["motivo"] = r.motivo;
  else                     doc["motivo"] = nullptr;
  doc["duplicado"] = duplicado;
  publicarJson("confirmacao", doc, 1, false);
}

// =============================================================================
// CACHE DE DUPLICADOS + VALIDACAO E EXECUCAO DE COMANDOS (CONTRATOS.md secao 4)
// =============================================================================
int buscarHistoricoCmd(const char* commandId) {
  for (uint8_t i = 0; i < histCount; i++) {
    if (strcmp(historicoCmds[i].commandId, commandId) == 0) return (int)i;
  }
  return -1;
}

void salvarHistoricoCmd(const char* commandId, const RegistroConfirmacao* confs, uint8_t qtd) {
  EntradaHistoricoCmd& e = historicoCmds[histNext];
  strlcpy(e.commandId, commandId, sizeof(e.commandId));
  e.qtd = qtd;
  for (uint8_t i = 0; i < qtd && i < 2; i++) {
    e.confs[i] = confs[i];
  }
  histNext = (histNext + 1) % HIST_CMD_MAX;
  if (histCount < HIST_CMD_MAX) histCount++;
}

bool validarSemver(const char* s) {
  if (!s) return false;
  int a, b, c;
  char extra;
  return sscanf(s, "%d.%d.%d%c", &a, &b, &c, &extra) == 3 && a >= 0 && b >= 0 && c >= 0;
}

bool validarSha256Hex(const char* s) {
  if (!s || strlen(s) != 64) return false;
  for (int i = 0; i < 64; i++) {
    char ch = s[i];
    bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    if (!ok) return false;
  }
  return true;
}

bool parseIsoUtc(const char* tsStr, time_t& outEpoch) {
  if (!tsStr || strlen(tsStr) != 20) return false;
  int Y, M, D, h, m, s;
  if (sscanf(tsStr, "%4d-%2d-%2dT%2d:%2d:%2dZ", &Y, &M, &D, &h, &m, &s) != 6) return false;
  struct tm t = {};
  t.tm_year = Y - 1900;
  t.tm_mon  = M - 1;
  t.tm_mday = D;
  t.tm_hour = h;
  t.tm_min  = m;
  t.tm_sec  = s;
  outEpoch = mktime(&t); // TZ=UTC0 definido por configTime(0, 0, ...)
  return outEpoch > 0;
}

static bool jsonEhInteiroEstrito(JsonVariantConst v) {
  return !v.isNull() && !v.is<bool>() && (v.is<int>() || v.is<long>() || v.is<uint32_t>());
}

// =============================================================================
// OTA HTTPS COM VALIDACAO DE SHA-256 EM STREAMING (R17)
// =============================================================================
bool fazerOta(const char* url, const char* shaEsperadoHex, char* motivoErro, size_t tamMotivo) {
  Serial.printf("[OTA] Iniciando download: %s\n", url);
  otaEmAndamento = true;
  digitalWrite(LED_POS_PIN, LOW);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 20);
  display.println("Atualizando OTA...");
  display.setCursor(10, 36);
  display.println("Nao desligue!");
  display.display();

  NetworkClientSecure cli;
  cli.useBuiltinCACertBundle();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(cli, url)) {
    snprintf(motivoErro, tamMotivo, "falha ao abrir conexao HTTPS");
    otaEmAndamento = false;
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    snprintf(motivoErro, tamMotivo, "HTTP %d ao baixar firmware", httpCode);
    http.end();
    otaEmAndamento = false;
    return false;
  }

  int total = http.getSize();
  if (total <= 0) {
    snprintf(motivoErro, tamMotivo, "tamanho do binario invalido");
    http.end();
    otaEmAndamento = false;
    return false;
  }

  if (!Update.begin((size_t)total)) {
    snprintf(motivoErro, tamMotivo, "espaco insuficiente na particao OTA");
    http.end();
    otaEmAndamento = false;
    return false;
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  NetworkClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int lidos = 0;
  unsigned long ultimoDadoMs = millis();

  while (lidos < total) {
    size_t disp = stream->available();
    if (disp > 0) {
      int pedir = min((int)sizeof(buf), min((int)disp, total - lidos));
      int n = stream->readBytes(buf, pedir);
      if (n > 0) {
        ultimoDadoMs = millis();
        mbedtls_sha256_update(&ctx, buf, n);
        if (Update.write(buf, n) != (size_t)n) {
          snprintf(motivoErro, tamMotivo, "erro de gravacao na flash");
          Update.abort();
          mbedtls_sha256_free(&ctx);
          http.end();
          otaEmAndamento = false;
          return false;
        }
        lidos += n;
      }
    } else {
      if (!http.connected() || (millis() - ultimoDadoMs) > 15000) {
        break;
      }
      delay(5);
    }
  }

  uint8_t hash[32];
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  http.end();

  char hex[65];
  for (int i = 0; i < 32; i++) {
    sprintf(hex + 2 * i, "%02x", hash[i]);
  }
  hex[64] = '\0';

  Serial.printf("[OTA] Baixados %d/%d bytes | SHA256=%s\n", lidos, total, hex);

  if (lidos != total) {
    snprintf(motivoErro, tamMotivo, "download incompleto");
    Update.abort();
    otaEmAndamento = false;
    return false;
  }

  if (strcmp(hex, shaEsperadoHex) != 0) {
    snprintf(motivoErro, tamMotivo, "hash sha256 divergente");
    Update.abort();
    otaEmAndamento = false;
    return false;
  }

  if (!Update.end(true)) {
    snprintf(motivoErro, tamMotivo, "falha ao finalizar particao OTA");
    otaEmAndamento = false;
    return false;
  }

  Serial.println("[OTA] Firmware validado e gravado com sucesso!");
  return true;
}

void tratarComandoJson(const char* payload, size_t len) {
  Serial.printf("[MQTT] <- comando %.*s\n", (int)len, payload);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err || !doc.is<JsonObject>() || !doc["commandId"].is<const char*>()) {
    Serial.println("[CMD] JSON invalido ou sem commandId: ignorado.");
    return;
  }

  const char* commandId = doc["commandId"].as<const char*>();
  if (strlen(commandId) == 0 || strlen(commandId) >= 40) {
    Serial.println("[CMD] commandId invalido: ignorado.");
    return;
  }

  // 1. Checagem de duplicado (ultimos 16)
  int idxHist = buscarHistoricoCmd(commandId);
  if (idxHist >= 0) {
    Serial.printf("[CMD] Duplicado (%s): reenviando confirmacoes originais.\n", commandId);
    const EntradaHistoricoCmd& e = historicoCmds[idxHist];
    for (uint8_t i = 0; i < e.qtd; i++) {
      publicarConfirmacaoRegistro(commandId, e.confs[i], true);
    }
    return;
  }

  RegistroConfirmacao confs[2] = {};
  auto rejeitar = [&](const char* erroCod, const char* motivoTxt) {
    strlcpy(confs[0].fase, "recebido", sizeof(confs[0].fase));
    strlcpy(confs[0].resultado, "rejeitado", sizeof(confs[0].resultado));
    strlcpy(confs[0].erro, erroCod, sizeof(confs[0].erro));
    strlcpy(confs[0].motivo, motivoTxt, sizeof(confs[0].motivo));
    publicarConfirmacaoRegistro(commandId, confs[0], false);
    salvarHistoricoCmd(commandId, confs, 1);
  };

  // 2. Validacoes de contrato
  if (!jsonEhInteiroEstrito(doc["v"]) || doc["v"].as<int>() != 1) {
    rejeitar("E_VERSAO_NAO_SUPORTADA", "versao nao suportada");
    return;
  }
  if (!doc["deviceId"].is<const char*>() || strcmp(doc["deviceId"].as<const char*>(), DEVICE_ID) != 0) {
    rejeitar("E_DEVICE_ID_INCORRETO", "deviceId de outro dispositivo");
    return;
  }
  if (!doc["acao"].is<const char*>()) {
    rejeitar("E_ACAO_DESCONHECIDA", "acao ausente");
    return;
  }
  const char* acao = doc["acao"].as<const char*>();
  bool acaoValida = strcmp(acao, "armar") == 0 ||
                    strcmp(acao, "desarmar") == 0 ||
                    strcmp(acao, "disparar") == 0 ||
                    strcmp(acao, "silenciar") == 0 ||
                    strcmp(acao, "configurar") == 0 ||
                    strcmp(acao, "atualizar_firmware") == 0;
  if (!acaoValida) {
    rejeitar("E_ACAO_DESCONHECIDA", "acao desconhecida");
    return;
  }
  if (!doc["params"].is<JsonObject>()) {
    rejeitar("E_CAMPO_AUSENTE", "params deve ser objeto");
    return;
  }
  if (!jsonEhInteiroEstrito(doc["expira_s"])) {
    rejeitar("E_CAMPO_AUSENTE", "expira_s obrigatorio");
    return;
  }
  int expiraS = doc["expira_s"].as<int>();
  if (expiraS < 5 || expiraS > 120) {
    rejeitar("E_VALOR_FORA_FAIXA", "expira_s fora de 5..120");
    return;
  }

  // Validacao de parametros por acao
  JsonObject params = doc["params"].as<JsonObject>();
  if (strcmp(acao, "configurar") == 0) {
    if (params.size() == 0) {
      rejeitar("E_CAMPO_AUSENTE", "configurar exige ao menos um parametro");
      return;
    }
    for (JsonPair kv : params) {
      const char* k = kv.key().c_str();
      if (!jsonEhInteiroEstrito(kv.value())) {
        rejeitar("E_CAMPO_AUSENTE", "parametro deve ser inteiro");
        return;
      }
      int val = kv.value().as<int>();
      if (strcmp(k, "tempoEntrada_s") == 0) {
        if (val < 0 || val > 120) { rejeitar("E_VALOR_FORA_FAIXA", "tempoEntrada_s fora de 0..120"); return; }
      } else if (strcmp(k, "intervaloTelemetria_s") == 0) {
        if (val < 1 || val > 300) { rejeitar("E_VALOR_FORA_FAIXA", "intervaloTelemetria_s fora de 1..300"); return; }
      } else if (strcmp(k, "timeoutManual_s") == 0) {
        if (val < 60 || val > 7200) { rejeitar("E_VALOR_FORA_FAIXA", "timeoutManual_s fora de 60..7200"); return; }
      } else {
        rejeitar("E_VALOR_FORA_FAIXA", "parametro desconhecido");
        return;
      }
    }
  } else if (strcmp(acao, "atualizar_firmware") == 0) {
    if (params.size() != 3 || !params["url"].is<const char*>() ||
        !params["sha256"].is<const char*>() || !params["versao"].is<const char*>()) {
      rejeitar("E_CAMPO_AUSENTE", "exige url, sha256 e versao");
      return;
    }
    const char* url = params["url"].as<const char*>();
    const char* sha = params["sha256"].as<const char*>();
    const char* ver = params["versao"].as<const char*>();
    if (strncmp(url, "https://", 8) != 0 || strlen(url) > 200) {
      rejeitar("E_VALOR_FORA_FAIXA", "url deve ser HTTPS com ate 200 caracteres");
      return;
    }
    if (!validarSha256Hex(sha)) {
      rejeitar("E_VALOR_FORA_FAIXA", "sha256 deve ter 64 hexadecimais");
      return;
    }
    if (!validarSemver(ver)) {
      rejeitar("E_VALOR_FORA_FAIXA", "versao fora de MAJOR.MINOR.PATCH");
      return;
    }
  } else {
    if (params.size() != 0) {
      rejeitar("E_VALOR_FORA_FAIXA", "acao nao aceita parametros");
      return;
    }
  }

  // Validacao de expiracao (apenas com relogio sincronizado)
  if (relogioValido() && doc["ts"].is<const char*>()) {
    time_t emitido = 0;
    if (parseIsoUtc(doc["ts"].as<const char*>(), emitido)) {
      if (time(nullptr) > emitido + expiraS) {
        rejeitar("E_EXPIRADO", "comando expirado");
        return;
      }
    }
  }

  // Regras de negocio / rejeicoes de estado
  const char* origem = doc["origem"].is<const char*>() ? doc["origem"].as<const char*>() : "";
  if (strcmp(origem, "agendamento") == 0 && modoAtual == MODO_MANUAL) {
    rejeitar("E_MODO_MANUAL", "agendamento ignorado no modo manual");
    return;
  }
  if (strcmp(acao, "armar") == 0 && portaAberta) {
    rejeitar("E_PORTA_ABERTA", "feche a porta antes de armar");
    return;
  }
  if (strcmp(acao, "armar") == 0 && sensorAtual != SENSOR_OK) {
    rejeitar("E_SENSOR_FALHA", "sensor em falha");
    return;
  }
  if (strcmp(acao, "disparar") == 0 && !sistemaArmado) {
    rejeitar("E_DESARMADO", "disparar exige o sistema armado");
    return;
  }

  // 3. Aceito: publica fase "recebido" / "aceito"
  strlcpy(confs[0].fase, "recebido", sizeof(confs[0].fase));
  strlcpy(confs[0].resultado, "aceito", sizeof(confs[0].resultado));
  publicarConfirmacaoRegistro(commandId, confs[0], false);

  // 4. Executa a acao
  if (strcmp(acao, "atualizar_firmware") == 0) {
    char motivoOta[64] = "";
    bool okOta = fazerOta(params["url"].as<const char*>(), params["sha256"].as<const char*>(), motivoOta, sizeof(motivoOta));
    strlcpy(confs[1].fase, "executado", sizeof(confs[1].fase));
    if (!okOta) {
      strlcpy(confs[1].resultado, "falha", sizeof(confs[1].resultado));
      strlcpy(confs[1].erro, "E_OTA_FALHA", sizeof(confs[1].erro));
      strlcpy(confs[1].motivo, motivoOta[0] ? motivoOta : "falha na atualizacao OTA", sizeof(confs[1].motivo));
      publicarConfirmacaoRegistro(commandId, confs[1], false);
      salvarHistoricoCmd(commandId, confs, 2);
      return;
    }

    strlcpy(confs[1].resultado, "ok", sizeof(confs[1].resultado));
    publicarConfirmacaoRegistro(commandId, confs[1], false);
    strlcpy(ultimoComandoId, commandId, sizeof(ultimoComandoId));
    publicarEstado();
    delay(600);
    ESP.restart();
    return;
  }

  if (strcmp(acao, "armar") == 0) {
    sistemaArmado = true;
    silenciadoManualmente = false;
    disparoManualForcado = false;
    salvarNvs();
  } else if (strcmp(acao, "desarmar") == 0) {
    sistemaArmado = false;
    alarmeAtual = ALARME_INATIVO;
    ledAlertaAtivo = false;
    silenciadoManualmente = false;
    disparoManualForcado = false;
    digitalWrite(LED_POS_PIN, LOW);
    enviarEstadoEspNow();
    salvarNvs();
  } else if (strcmp(acao, "disparar") == 0) {
    bool veioDeRegra = (strcmp(origem, "regra") == 0);
    // Se veio da Regra Automatica enquanto ja tem movimento na frente do sensor,
    // mantem pausado em "entrada" em vez de atropelar o sensor de presenca
    if (veioDeRegra && USAR_MOVIMENTO_SEGURAR && portaAberta && temGentePassando) {
      alarmeAtual = ALARME_ENTRADA;
      entradaInicioMs = millis();
      ledAlertaAtivo = false;
      disparoManualForcado = false;
      digitalWrite(LED_POS_PIN, LOW);
    } else {
      alarmeAtual = ALARME_DISPARADO;
      ledAlertaAtivo = true;
      silenciadoManualmente = false;
      disparoManualForcado = !veioDeRegra; // so trava como manual se clicado pelo usuario
      movEstavaAtivoNoManual = temGentePassando;
    }
    enviarEstadoEspNow();
  } else if (strcmp(acao, "silenciar") == 0) {
    alarmeAtual = ALARME_INATIVO;
    disparoManualForcado = false;
    if (portaAberta) {
      // Silencia a sirene, mantendo os LEDs piscando ate fechar a porta
      silenciadoManualmente = true;
      ledAlertaAtivo = true;
    } else {
      silenciadoManualmente = false;
      ledAlertaAtivo = false;
      digitalWrite(LED_POS_PIN, LOW);
    }
    enviarEstadoEspNow();
  } else if (strcmp(acao, "configurar") == 0) {
    if (params["tempoEntrada_s"].is<int>())        cfgTempoEntradaS       = params["tempoEntrada_s"].as<uint16_t>();
    if (params["intervaloTelemetria_s"].is<int>()) cfgIntervaloTelemetria = params["intervaloTelemetria_s"].as<uint16_t>();
    if (params["timeoutManual_s"].is<int>())       cfgTimeoutManualS      = params["timeoutManual_s"].as<uint16_t>();
    salvarNvs();
  }

  // 5. Publica fase "executado" / "ok", atualiza ultimoComandoId e publica "estado"
  strlcpy(confs[1].fase, "executado", sizeof(confs[1].fase));
  strlcpy(confs[1].resultado, "ok", sizeof(confs[1].resultado));
  publicarConfirmacaoRegistro(commandId, confs[1], false);
  salvarHistoricoCmd(commandId, confs, 2);

  strlcpy(ultimoComandoId, commandId, sizeof(ultimoComandoId));
  publicarEstado();
}

void drenarFilaComandos() {
  while (true) {
    char copia[CMD_BUF_MAX];
    size_t tam = 0;

    portENTER_CRITICAL(&muxFila);
    if (filaTail != filaHead && filaComandos[filaTail].ocupado) {
      tam = filaComandos[filaTail].len;
      memcpy(copia, filaComandos[filaTail].payload, tam);
      copia[tam] = '\0';
      filaComandos[filaTail].ocupado = false;
      filaTail = (filaTail + 1) % FILA_CMD_TAM;
    }
    portEXIT_CRITICAL(&muxFila);

    if (tam == 0) break;
    tratarComandoJson(copia, tam);
  }
}

// =============================================================================
// REDE, SNTP E HANDLER MQTT
// =============================================================================
bool sincronizarHora() {
  if (WiFi.status() != WL_CONNECTED) return false;
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[SNTP] Aguardando hora valida");

  unsigned long inicio = millis();
  while (!relogioValido() && (millis() - inicio) < INTERVALO_SNTP_TIMEOUT_MS) {
    delay(200);
    Serial.print(".");
  }
  horaSincronizada = relogioValido();
  Serial.println(horaSincronizada ? "\n[SNTP] Hora sincronizada." : "\n[SNTP] Timeout SNTP.");
  return horaSincronizada;
}

static void mqttEventHandler(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)eventData;

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      brokerConectado = true;
      Serial.println("[MQTT] Conectado ao broker Mosquitto (N3).");
      esp_mqtt_client_subscribe(clienteMqtt, TOPICO_COMANDO, 1);
      precisaPublicarConexao = true;
      break;

    case MQTT_EVENT_DISCONNECTED:
      brokerConectado = false;
      Serial.println("[MQTT] Desconectado do broker.");
      break;

    case MQTT_EVENT_DATA:
      if (event->topic_len == (int)strlen(TOPICO_COMANDO) &&
          memcmp(event->topic, TOPICO_COMANDO, event->topic_len) == 0) {
        if (event->data_len > 0 && event->data_len < CMD_BUF_MAX) {
          portENTER_CRITICAL(&muxFila);
          uint8_t nextHead = (filaHead + 1) % FILA_CMD_TAM;
          if (!filaComandos[filaHead].ocupado) {
            memcpy(filaComandos[filaHead].payload, event->data, event->data_len);
            filaComandos[filaHead].payload[event->data_len] = '\0';
            filaComandos[filaHead].len = event->data_len;
            filaComandos[filaHead].ocupado = true;
            filaHead = nextHead;
          }
          portEXIT_CRITICAL(&muxFila);
        }
      }
      break;

    case MQTT_EVENT_ERROR:
      Serial.println("[MQTT] Erro de conexao (confira credenciais, TLS e SNTP).");
      break;

    default:
      break;
  }
}

void iniciarMqtt() {
  if (mqttIniciado) return;

  static String clientId;
  clientId = String(DEVICE_ID) + "-" + WiFi.macAddress();

  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = MQTT_URI;
  if (uriUsaTls(MQTT_URI)) {
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.credentials.username = MQTT_USER;
  cfg.credentials.authentication.password = MQTT_PASS;
  cfg.credentials.client_id = clientId.c_str();
  cfg.session.keepalive = 15;
  cfg.session.last_will.topic   = PREFIXO_MQTT "status";
  cfg.session.last_will.msg     = LWT_PAYLOAD;
  cfg.session.last_will.msg_len = strlen(LWT_PAYLOAD);
  cfg.session.last_will.qos     = 1;
  cfg.session.last_will.retain  = true;

  clienteMqtt = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(clienteMqtt, MQTT_EVENT_ANY, mqttEventHandler, NULL);
  esp_mqtt_client_start(clienteMqtt);
  mqttIniciado = true;
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("\n[Wi-Fi EVT] Associado ao AP (aguardando IP DHCP...)");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("\n[Wi-Fi EVT] IP obtido: %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("\n[Wi-Fi EVT] Desconectado! reason=%d\n", info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

void aplicarConfigIpParaRede(int numRede) {
#ifdef USE_STATIC_IP_REDE_1
  if (numRede == 1) {
    WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GATEWAY), IPAddress(STATIC_SUBNET),
                IPAddress(STATIC_DNS1), IPAddress(STATIC_DNS2));
    Serial.println("[Wi-Fi] Usando IP estatico para rede 1.");
    return;
  }
#endif
#ifdef USE_STATIC_IP_REDE_2
  if (numRede == 2) {
    WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GATEWAY), IPAddress(STATIC_SUBNET),
                IPAddress(STATIC_DNS1), IPAddress(STATIC_DNS2));
    Serial.println("[Wi-Fi] Usando IP estatico para rede 2.");
    return;
  }
#endif
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
  Serial.println("[Wi-Fi] Usando DHCP automatico.");
}

void conectarWifiComScan() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);

  Serial.println("[Wi-Fi] Escaneando redes 2.4GHz...");
  int n = WiFi.scanNetworks();
  Serial.printf("[Wi-Fi] Encontradas %d redes:\n", n);

  int32_t melhorCanal = 0;
  int32_t melhorRssi = -999;
  uint8_t melhorBssid[6] = {0};
  int redeEscolhida = 0; // 1 = WIFI_SSID, 2 = WIFI_SSID_2

  for (int i = 0; i < n; i++) {
    if (i < 8) {
      Serial.printf("  - \"%s\" (ch %d, %d dBm)\n",
                    WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
    }
    if (WiFi.SSID(i) == WIFI_SSID && (redeEscolhida != 1 || WiFi.RSSI(i) > melhorRssi)) {
      redeEscolhida = 1;
      melhorRssi = WiFi.RSSI(i);
      melhorCanal = WiFi.channel(i);
      uint8_t* b = WiFi.BSSID(i);
      if (b != nullptr) memcpy(melhorBssid, b, 6);
    }
#ifdef WIFI_SSID_2
    else if (redeEscolhida != 1 && WiFi.SSID(i) == WIFI_SSID_2 && WiFi.RSSI(i) > melhorRssi) {
      redeEscolhida = 2;
      melhorRssi = WiFi.RSSI(i);
      melhorCanal = WiFi.channel(i);
      uint8_t* b = WiFi.BSSID(i);
      if (b != nullptr) memcpy(melhorBssid, b, 6);
    }
#endif
  }
  WiFi.scanDelete();

  ultimoBeginWifiMs = millis();
  if (redeEscolhida == 1) {
    aplicarConfigIpParaRede(1);
    Serial.printf("[Wi-Fi] Conectando em \"%s\" (ch %ld, %ld dBm)...\n",
                  WIFI_SSID, (long)melhorCanal, (long)melhorRssi);
    WiFi.begin(WIFI_SSID, WIFI_PASS, melhorCanal, melhorBssid);
#ifdef WIFI_SSID_2
  } else if (redeEscolhida == 2) {
    aplicarConfigIpParaRede(2);
    Serial.printf("[Wi-Fi] Conectando na rede 2 \"%s\" (ch %ld, %ld dBm)...\n",
                  WIFI_SSID_2, (long)melhorCanal, (long)melhorRssi);
    WiFi.begin(WIFI_SSID_2, WIFI_PASS_2, melhorCanal, melhorBssid);
#endif
  } else {
    aplicarConfigIpParaRede(1);
    Serial.printf("[Wi-Fi] Nenhuma rede conhecida no scan; tentando \"%s\"...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void verificarReconexaoWiFi() {
  if (millis() - ultimaChecagemWifiMs < INTERVALO_CHECAGEM_WIFI) return;
  ultimaChecagemWifiMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[Wi-Fi] Sem conexao (status=%d). Buscando redes conhecidas...\n",
                  (int)WiFi.status());
    WiFi.disconnect(false);
    delay(100);
    conectarWifiComScan();
    return;
  }

  if (!mqttIniciado) {
    Serial.printf("[Wi-Fi] Conectado! IP: %s | Canal: %d | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
    WiFi.setSleep(false);
    if (!espNowAtivo) iniciarEspNow();
    if (!uriUsaTls(MQTT_URI) || relogioValido() || sincronizarHora()) {
      iniciarMqtt();
    }
  }
}

// =============================================================================
// LEITURA DO REED (DIGITAL OU SUPERVISIONADO ADC1) + ESTADO SEGURO (R2/R10)
// =============================================================================
void atualizarEstadoSensorSeguro(EstadoSensor novoSensor) {
  if (novoSensor == sensorAtual) return;
  sensorAtual = novoSensor;
  if (sensorAtual != SENSOR_OK) {
    modoAtual = MODO_SEGURO;
    Serial.printf("[SEGURO] Falha no sensor (%s) -> modo=seguro\n", textoSensor(sensorAtual));
  } else {
    modoAtual = MODO_AUTOMATICO;
    Serial.println("[SEGURO] Sensor normalizado -> modo=automatico");
  }
  publicarEstado();
}

void lerSensorPorta() {
  if (falhaForcadaSerial) return;

#if REED_SUPERVISIONADO
  int mv = analogReadMilliVolts(REED_ADC_PIN);
  EstadoSensor leituraSensor = SENSOR_OK;
  bool leituraAberta = portaAberta;

  if (mv < REED_MV_CURTO_MAX) {
    leituraSensor = SENSOR_FALHA_CURTO;
  } else if (mv > REED_MV_ABERTA_MAX) {
    leituraSensor = SENSOR_FALHA_ABERTO;
  } else {
    leituraSensor = SENSOR_OK;
    leituraAberta = (mv > REED_MV_FECHADA_MAX);
  }

  if (leituraSensor != SENSOR_OK) {
    if (falhaCandidata != leituraSensor) {
      falhaCandidata = leituraSensor;
      inicioFalhaSensorMs = millis();
    } else if (millis() - inicioFalhaSensorMs >= TEMPO_FALHA_SENSOR_MS) {
      atualizarEstadoSensorSeguro(leituraSensor);
    }
    return;
  }

  // Leitura voltou para faixa valida
  falhaCandidata = SENSOR_OK;
  if (sensorAtual != SENSOR_OK) {
    atualizarEstadoSensorSeguro(SENSOR_OK);
  }
#else
  bool leituraAberta = (digitalRead(REED_PIN) == HIGH);
#endif

  // Debounce nao bloqueante por millis()
  if (leituraAberta != leituraReedAnterior) {
    leituraReedAnterior = leituraAberta;
    ultimoDebounceReedMs = millis();
  }

  if (leituraAberta != portaAberta && (millis() - ultimoDebounceReedMs) >= DEBOUNCE_REED_MS) {
    portaAberta = leituraAberta;
    tempoUltimaMudancaMs = millis();

    if (portaAberta) {
      contadorAberturas++;
      silenciadoManualmente = false;
      disparoManualForcado = false;
      Serial.printf("[PORTA] ABERTA (total=%lu, armado=%d)\n",
                    (unsigned long)contadorAberturas, sistemaArmado ? 1 : 0);
      somPortaAbriuSuave();

      if (sistemaArmado && alarmeAtual == ALARME_INATIVO && sensorAtual == SENSOR_OK) {
        alarmeAtual = ALARME_ENTRADA;
        entradaInicioMs = millis();
        Serial.printf("[REGRA] Alarme -> ENTRADA (%us)\n", cfgTempoEntradaS);
      }

      enviarEstadoEspNow();
      publicarTelemetria();
      publicarEstado();
    } else {
      bool mudouEstado = (alarmeAtual != ALARME_INATIVO) || ledAlertaAtivo;
      alarmeAtual = ALARME_INATIVO;
      ledAlertaAtivo = false;
      silenciadoManualmente = false;
      disparoManualForcado = false;
      digitalWrite(LED_POS_PIN, LOW);
      Serial.println("[PORTA] FECHADA");
      somPortaFechou();

      enviarEstadoEspNow();
      publicarTelemetria();
      if (mudouEstado) publicarEstado();
    }
  }
}

// Comandos pelo Serial para testes de bancada (falha, curto, ok)
void verificarComandosSerial() {
  if (!Serial.available()) return;
  String linha = Serial.readStringUntil('\n');
  linha.trim();
  if (linha == "falha") {
    falhaForcadaSerial = true;
    atualizarEstadoSensorSeguro(SENSOR_FALHA_ABERTO);
  } else if (linha == "curto") {
    falhaForcadaSerial = true;
    atualizarEstadoSensorSeguro(SENSOR_FALHA_CURTO);
  } else if (linha == "ok") {
    falhaForcadaSerial = false;
    atualizarEstadoSensorSeguro(SENSOR_OK);
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
  if (otaEmAndamento) return;
  if (millis() - ultimaAtualizacaoOledMs < 200) return;
  ultimaAtualizacaoOledMs = millis();

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
    display.print("[LOCAL+NOW]");
  } else if (WiFi.status() == WL_CONNECTED) {
    display.print("[WIFI OK]");
  } else {
    display.print("[LOCAL]");
  }

  display.setTextColor(SSD1306_WHITE);
  desenharIconePorta(8, 18, portaAberta);

  display.setTextSize(2);
  display.setCursor(38, 18);
  if (modoAtual == MODO_SEGURO) {
    display.println("SEGURO!");
  } else if (alarmeAtual == ALARME_DISPARADO) {
    display.println("ALARME!");
  } else if (alarmeAtual == ALARME_ENTRADA) {
    display.println("ENTRADA");
  } else if (portaAberta) {
    display.println("ABERTA");
  } else {
    display.println("FECHADA");
  }

  display.drawLine(38, 36, 124, 36, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(38, 41);
  if (modoAtual == MODO_SEGURO) {
    display.println("FALHA SENSOR!");
  } else if (!sistemaArmado) {
    display.println("DESARMADO");
  } else if (silenciadoManualmente) {
    display.println("SILENCIADO");
  } else if (alarmeAtual == ALARME_ENTRADA) {
    unsigned long decorrido = (millis() - entradaInicioMs) / 1000;
    long falta = (long)cfgTempoEntradaS - (long)decorrido;
    if (falta < 0) falta = 0;
    if (USAR_MOVIMENTO_SEGURAR && temGentePassando) {
      display.println("Mov: segurando");
    } else {
      display.print("Dispara: ");
      display.print(falta);
      display.println("s");
    }
  } else if (!portaAberta) {
    display.print("Armado #");
    display.println(contadorAberturas);
  } else if (temGentePassando) {
    display.println("Movimento: SIM");
  } else {
    display.println("SEM MOVIMENTO!");
  }

  unsigned long segundos = (millis() - tempoUltimaMudancaMs) / 1000;
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
  delay(250);
  Serial.printf("\n=== Access N3 Central (%s | fw %s) ===\n", DEVICE_ID, FW_VERSION);

  // R1: LED com resistor em serie para GND real; reed para GND real
  pinMode(LED_POS_PIN, OUTPUT);
  // Teste rapido no boot (3 piscadas) para confirmar visualmente que o LED na protoboard esta OK
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_POS_PIN, HIGH);
    delay(120);
    digitalWrite(LED_POS_PIN, LOW);
    delay(100);
  }

#if REED_SUPERVISIONADO
  analogReadResolution(12);
  analogSetPinAttenuation(REED_ADC_PIN, ADC_11db);
#else
  pinMode(REED_PIN, INPUT_PULLUP);
#endif

  // R1: OLED alimentada no 3V3 e GND reais da placa
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_PRIMARIO)) {
    display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_ALTERNATIVO);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(8, 16);
  display.println("ACCESS N3 " DEVICE_ID);
  display.setCursor(8, 32);
  display.println("Conectando Wi-Fi...");
  display.display();

  carregarNvs();
  setupSpeaker();
  somPortaFechou();

  // Estado inicial do reed antes de iniciar rede
#if REED_SUPERVISIONADO
  int mvInit = analogReadMilliVolts(REED_ADC_PIN);
  portaAberta = (mvInit > REED_MV_FECHADA_MAX && mvInit <= REED_MV_ABERTA_MAX);
#else
  portaAberta = (digitalRead(REED_PIN) == HIGH);
#endif
  leituraReedAnterior = portaAberta;
  ultimoDebounceReedMs = millis();
  tempoUltimaMudancaMs = millis();

  // Wi-Fi STA com scan previo + eventos detalhados de diagnostico
  WiFi.persistent(false);
  WiFi.onEvent(onWifiEvent);
  WiFi.setAutoReconnect(true);
  conectarWifiComScan();

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 35) {
    delay(300);
    Serial.print(".");
    tentativas++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[Wi-Fi] Conectado! IP: %s | Canal: %d | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
    WiFi.setSleep(false);
    iniciarEspNow();
    if (!uriUsaTls(MQTT_URI) || sincronizarHora()) {
      iniciarMqtt();
    }
  } else {
    Serial.printf("[Wi-Fi] Sem conexao no boot (status=%d). Operando em regra local.\n",
                  (int)WiFi.status());
    esp_wifi_set_channel(CANAL_ESPNOW_PADRAO, WIFI_SECOND_CHAN_NONE);
    iniciarEspNow();
  }

  enviarEstadoEspNow();
}

void loop() {
  verificarReconexaoWiFi();
  verificarComandosSerial();

  // Publicacoes de conexao/reconexao feitas fora da callback do ESP-IDF
  if (precisaPublicarConexao && brokerConectado) {
    precisaPublicarConexao = false;
    publicarStatusOnline();
    publicarEstado();
    publicarTelemetria();
  }

  drenarFilaComandos();

  if (movimentoMudouPendente) {
    movimentoMudouPendente = false;
    Serial.printf("[ESP-NOW] Movimento Satelite: %s\n", temGentePassando ? "DETECTADO" : "LIMPO");
  }
  if (!sateliteConectado() && temGentePassando) {
    temGentePassando = false;
    fonteMovimento = 0;
    distanciaMovimentoCm = 255;
  }

  // 1. Sensor de porta (debounce + supervisao de falha)
  lerSensorPorta();

  // 2. Regra local (funciona mesmo sem Wi-Fi / broker — R2 & R10)
  if (!sistemaArmado) {
    if (alarmeAtual != ALARME_INATIVO || ledAlertaAtivo) {
      alarmeAtual = ALARME_INATIVO;
      ledAlertaAtivo = false;
      disparoManualForcado = false;
      silenciadoManualmente = false;
      digitalWrite(LED_POS_PIN, LOW);
      enviarEstadoEspNow();
      publicarEstado();
    }
  } else if (disparoManualForcado) {
    if (!temGentePassando) movEstavaAtivoNoManual = false;
    if (USAR_MOVIMENTO_SEGURAR && portaAberta && temGentePassando && !movEstavaAtivoNoManual) {
      disparoManualForcado = false;
      alarmeAtual = ALARME_INATIVO;
      ledAlertaAtivo = false;
      digitalWrite(LED_POS_PIN, LOW);
      enviarEstadoEspNow();
      publicarEstado();
    } else {
      alarmeAtual = ALARME_DISPARADO;
      ledAlertaAtivo = true;
    }
  } else if (alarmeAtual == ALARME_ENTRADA) {
    if (USAR_MOVIMENTO_SEGURAR && temGentePassando) {
      // Gente passando segura a contagem de entrada
      entradaInicioMs = millis();
    } else if ((millis() - entradaInicioMs) >= (unsigned long)cfgTempoEntradaS * 1000UL) {
      alarmeAtual = ALARME_DISPARADO;
      ledAlertaAtivo = true;
      Serial.println("[REGRA] Tempo de entrada esgotado -> ALARME DISPARADO!");
      enviarEstadoEspNow();
      publicarEstado();
    }
  } else if (alarmeAtual == ALARME_DISPARADO) {
    if (USAR_MOVIMENTO_SEGURAR && portaAberta && temGentePassando) {
      alarmeAtual = ALARME_ENTRADA;
      entradaInicioMs = millis();
      ledAlertaAtivo = false;
      digitalWrite(LED_POS_PIN, LOW);
      Serial.println("[REGRA] Movimento detectado -> sirene pausada (volta para entrada).");
      enviarEstadoEspNow();
      publicarEstado();
    }
  } else if (silenciadoManualmente && portaAberta) {
    ledAlertaAtivo = true;
  } else if (portaAberta && alarmeAtual == ALARME_INATIVO && sensorAtual == SENSOR_OK) {
    // Porta continuou aberta e armada apos silenciar por movimento -> reinicia contagem de entrada
    alarmeAtual = ALARME_ENTRADA;
    entradaInicioMs = millis();
    publicarEstado();
  }

  // 3. Heartbeat ESP-NOW para o Satelite
  if (millis() - ultimoHeartbeatEspNowMs >= INTERVALO_ESPNOW_HB_MS) {
    ultimoHeartbeatEspNowMs = millis();
    enviarEstadoEspNow();
  }

  // 4. Sirene I2S + LED local sincronizado
  processarAlertaVisualESonoro();

  // 5. Telemetria periodica (intervaloTelemetria_s vindo da NVS/config)
  unsigned long intervaloTelMs = (unsigned long)cfgIntervaloTelemetria * 1000UL;
  if (millis() - ultimaTelemetriaMs >= intervaloTelMs) {
    ultimaTelemetriaMs = millis();
    publicarTelemetria();
  }

  // 6. Display OLED
  atualizarDashboard();
  delay(20);
}
