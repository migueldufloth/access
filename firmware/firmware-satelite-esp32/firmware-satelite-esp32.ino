#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <string.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

#include "secrets.h"
#include "config.h"

Preferences prefs;

// =============================================================================
// PROTOCOLO ESP-NOW (Placa 2 <-> Placa 1, LED sincronizado mesmo sem rede)
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
EstadoSensor sensorAtual = USAR_PIR ? SENSOR_CALIBRANDO : SENSOR_OK;
bool         sistemaArmado = false;

uint16_t cfgTempoEntradaS       = DEFAULT_TEMPO_ENTRADA_S;
uint16_t cfgIntervaloTelemetria = DEFAULT_INTERVALO_TEL_S;
uint16_t cfgTimeoutManualS      = DEFAULT_TIMEOUT_MANUAL_S;

char ultimoComandoId[40] = "";

// Estado operacional local e sensores
bool temGenteAqui = false;
uint8_t fonteAtual = 0;
uint32_t contadorDeteccoes = 0;
bool falhaForcadaSerial = false;
bool disparoManualUsuario = false;

volatile bool alarmeRemotoEspNow = false;
volatile bool centralCancelouAlarmePendente = false;
volatile unsigned long ultimoContatoCentralMs = 0;

unsigned long bootMs = 0;
unsigned long entradaInicioMs = 0;
unsigned long ultimoMovimentoMs = 0;
unsigned long ultimoEnvioEspNowMs = 0;
unsigned long ultimaChecagemWifiMs = 0;
unsigned long ultimoBeginWifiMs = 0;
unsigned long ultimaTelemetriaMs = 0;
unsigned long ultimoPiscaMs = 0;
bool estadoLed = false;

// Filtros anti-ruido
unsigned long inicioPirHighMs = 0;
unsigned long ultimaLeituraUltraMs = 0;
int contagemUltraValida = 0;
long ultimaDistanciaCm = 999;

// =============================================================================
// HELPERS DE CONTRATO E NVS
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

void carregarNvs() {
  prefs.begin("access", false);
  sistemaArmado          = prefs.getBool("armado", false);
  cfgTempoEntradaS       = prefs.getUShort("tEntrada", DEFAULT_TEMPO_ENTRADA_S);
  cfgIntervaloTelemetria = prefs.getUShort("intTel", DEFAULT_INTERVALO_TEL_S);
  cfgTimeoutManualS      = prefs.getUShort("tManual", DEFAULT_TIMEOUT_MANUAL_S);

  if (cfgTempoEntradaS > 120) cfgTempoEntradaS = DEFAULT_TEMPO_ENTRADA_S;
  if (cfgIntervaloTelemetria < 1 || cfgIntervaloTelemetria > 300) cfgIntervaloTelemetria = DEFAULT_INTERVALO_TEL_S;
  if (cfgTimeoutManualS < 60 || cfgTimeoutManualS > 7200) cfgTimeoutManualS = DEFAULT_TIMEOUT_MANUAL_S;

  Serial.printf("[NVS] Carregado: armado=%d, tempoEntrada_s=%u, intervaloTelemetria_s=%u, timeoutManual_s=%u\n",
                sistemaArmado ? 1 : 0, cfgTempoEntradaS, cfgIntervaloTelemetria, cfgTimeoutManualS);
}

void salvarNvs() {
  prefs.putBool("armado", sistemaArmado);
  prefs.putUShort("tEntrada", cfgTempoEntradaS);
  prefs.putUShort("intTel", cfgIntervaloTelemetria);
  prefs.putUShort("tManual", cfgTimeoutManualS);
  Serial.printf("[NVS] Salvo: armado=%d, tempoEntrada_s=%u, intervaloTelemetria_s=%u, timeoutManual_s=%u\n",
                sistemaArmado ? 1 : 0, cfgTempoEntradaS, cfgIntervaloTelemetria, cfgTimeoutManualS);
}

// =============================================================================
// ESP-NOW (comunicacao local imediata com a Central porta-01)
// =============================================================================
void enviarMovimentoEspNow() {
  if (!espNowAtivo) return;
  // Nao interrompe o handshake Wi-Fi nos primeiros 10s apos WiFi.begin()
  if (WiFi.status() != WL_CONNECTED && (millis() - ultimoBeginWifiMs) < 10000) return;

  PacoteAccessEspNow pkt = {};
  pkt.magic       = ESPNOW_MAGIC;
  pkt.tipo        = ESPNOW_TIPO_MOVIMENTO;
  pkt.valor       = temGenteAqui ? 1 : 0;
  pkt.fonte       = temGenteAqui ? fonteAtual : 0;
  pkt.distanciaCm = (ultimaDistanciaCm >= 0 && ultimaDistanciaCm <= 254) ? (uint8_t)ultimaDistanciaCm : 255;

  esp_now_send(BROADCAST_MAC, (const uint8_t*)&pkt, sizeof(pkt));
  ultimoEnvioEspNowMs = millis();
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

  if (pkt.tipo == ESPNOW_TIPO_ALARME) {
    ultimoContatoCentralMs = millis();
    bool novoRemoto = (pkt.valor == 1);
    if (alarmeRemotoEspNow && !novoRemoto) {
      // Central silenciou/cancelou o alarme (movimento ou porta fechada) -> sincroniza o Satelite tambem
      centralCancelouAlarmePendente = true;
    }
    alarmeRemotoEspNow = novoRemoto;
  }
}

void iniciarEspNow() {
  if (espNowAtivo) return;
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Falha ao inicializar!");
    return;
  }

  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BROADCAST_MAC, 6);
  peerInfo.channel = 0; // Usa o canal atual do Wi-Fi STA (mesmo roteador da Central)
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
    esp_now_add_peer(&peerInfo);
  }
  espNowAtivo = true;
  Serial.println("[ESP-NOW] Satelite pronto para sincronizacao com a Central.");
}

// =============================================================================
// CONTRATO MQTT v1 — PUBLICACAO DE MENSAGENS
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
  uint32_t contVal = min(contadorDeteccoes, (uint32_t)1000000);

  JsonArray med = doc["medicoes"].to<JsonArray>();
  JsonObject m1 = med.add<JsonObject>();
  m1["grandeza"] = "presenca";
  m1["valor"]    = temGenteAqui ? 1 : 0; // inteiro 0/1, nunca bool
  m1["unidade"]  = "bool";

  JsonObject m2 = med.add<JsonObject>();
  m2["grandeza"] = "aberturas";
  m2["valor"]    = contVal;
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
// VALIDACAO, CACHE DE DUPLICADOS, COMANDOS E OTA (CONTRATOS.md secao 4 & R17)
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
  outEpoch = mktime(&t);
  return outEpoch > 0;
}

static bool jsonEhInteiroEstrito(JsonVariantConst v) {
  return !v.isNull() && !v.is<bool>() && (v.is<int>() || v.is<long>() || v.is<uint32_t>());
}

bool fazerOta(const char* url, const char* shaEsperadoHex, char* motivoErro, size_t tamMotivo) {
  Serial.printf("[OTA] Iniciando download: %s\n", url);
  digitalWrite(LED_ALARME_PIN, HIGH);

  NetworkClientSecure cli;
  cli.useBuiltinCACertBundle();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(cli, url)) {
    snprintf(motivoErro, tamMotivo, "falha ao abrir conexao HTTPS");
    digitalWrite(LED_ALARME_PIN, LOW);
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    snprintf(motivoErro, tamMotivo, "HTTP %d ao baixar firmware", httpCode);
    http.end();
    digitalWrite(LED_ALARME_PIN, LOW);
    return false;
  }

  int total = http.getSize();
  if (total <= 0) {
    snprintf(motivoErro, tamMotivo, "tamanho do binario invalido");
    http.end();
    digitalWrite(LED_ALARME_PIN, LOW);
    return false;
  }

  if (!Update.begin((size_t)total)) {
    snprintf(motivoErro, tamMotivo, "espaco insuficiente na particao OTA");
    http.end();
    digitalWrite(LED_ALARME_PIN, LOW);
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
          digitalWrite(LED_ALARME_PIN, LOW);
          return false;
        }
        lidos += n;
      }
    } else {
      if (!http.connected() || (millis() - ultimoDadoMs) > 15000) break;
      delay(5);
    }
  }

  uint8_t hash[32];
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  http.end();

  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", hash[i]);
  hex[64] = '\0';

  Serial.printf("[OTA] Baixados %d/%d bytes | SHA256=%s\n", lidos, total, hex);

  if (lidos != total) {
    snprintf(motivoErro, tamMotivo, "download incompleto");
    Update.abort();
    digitalWrite(LED_ALARME_PIN, LOW);
    return false;
  }

  if (strcmp(hex, shaEsperadoHex) != 0) {
    snprintf(motivoErro, tamMotivo, "hash sha256 divergente");
    Update.abort();
    digitalWrite(LED_ALARME_PIN, LOW);
    return false;
  }

  if (!Update.end(true)) {
    snprintf(motivoErro, tamMotivo, "falha ao finalizar particao OTA");
    digitalWrite(LED_ALARME_PIN, LOW);
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
  if (strlen(commandId) == 0 || strlen(commandId) >= 40) return;

  int idxHist = buscarHistoricoCmd(commandId);
  if (idxHist >= 0) {
    Serial.printf("[CMD] Duplicado (%s): reenviando confirmacoes.\n", commandId);
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

  if (relogioValido() && doc["ts"].is<const char*>()) {
    time_t emitido = 0;
    if (parseIsoUtc(doc["ts"].as<const char*>(), emitido)) {
      if (time(nullptr) > emitido + expiraS) {
        rejeitar("E_EXPIRADO", "comando expirado");
        return;
      }
    }
  }

  const char* origem = doc["origem"].is<const char*>() ? doc["origem"].as<const char*>() : "";
  if (strcmp(origem, "agendamento") == 0 && modoAtual == MODO_MANUAL) {
    rejeitar("E_MODO_MANUAL", "agendamento ignorado no modo manual");
    return;
  }
  if (strcmp(acao, "armar") == 0 && sensorAtual != SENSOR_OK) {
    rejeitar("E_SENSOR_FALHA", sensorAtual == SENSOR_CALIBRANDO ? "sensor PIR calibrando" : "sensor em falha");
    return;
  }
  if (strcmp(acao, "disparar") == 0 && !sistemaArmado) {
    rejeitar("E_DESARMADO", "disparar exige o sistema armado");
    return;
  }

  // Aceito
  strlcpy(confs[0].fase, "recebido", sizeof(confs[0].fase));
  strlcpy(confs[0].resultado, "aceito", sizeof(confs[0].resultado));
  publicarConfirmacaoRegistro(commandId, confs[0], false);

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
    salvarNvs();
  } else if (strcmp(acao, "desarmar") == 0) {
    sistemaArmado = false;
    alarmeAtual = ALARME_INATIVO;
    disparoManualUsuario = false;
    salvarNvs();
  } else if (strcmp(acao, "disparar") == 0) {
    bool veioDeRegra = (strcmp(origem, "regra") == 0);
    if (veioDeRegra && temGenteAqui) {
      // Se ja ha gente passando na frente do sensor, nao trava em disparado pela regra automatica
      alarmeAtual = ALARME_INATIVO;
      disparoManualUsuario = false;
    } else {
      alarmeAtual = ALARME_DISPARADO;
      disparoManualUsuario = !veioDeRegra;
    }
  } else if (strcmp(acao, "silenciar") == 0) {
    alarmeAtual = ALARME_INATIVO;
    disparoManualUsuario = false;
  } else if (strcmp(acao, "configurar") == 0) {
    if (params["tempoEntrada_s"].is<int>())        cfgTempoEntradaS       = params["tempoEntrada_s"].as<uint16_t>();
    if (params["intervaloTelemetria_s"].is<int>()) cfgIntervaloTelemetria = params["intervaloTelemetria_s"].as<uint16_t>();
    if (params["timeoutManual_s"].is<int>())       cfgTimeoutManualS      = params["timeoutManual_s"].as<uint16_t>();
    salvarNvs();
  }

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
      Serial.println("[MQTT] Satelite conectado ao broker Mosquitto (N3).");
      esp_mqtt_client_subscribe(clienteMqtt, TOPICO_COMANDO, 1);
      precisaPublicarConexao = true;
      break;

    case MQTT_EVENT_DISCONNECTED:
      brokerConectado = false;
      Serial.println("[MQTT] Satelite desconectado do broker.");
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

void conectarWifiComScan() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER_SATELITE);

  int n = WiFi.scanNetworks();
  int32_t melhorCanal = 0;
  int32_t melhorRssi = -999;
  uint8_t melhorBssid[6] = {0};
  int redeEscolhida = 0;

  for (int i = 0; i < n; i++) {
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
    Serial.printf("[Wi-Fi] Conectando em \"%s\" (ch %ld, %ld dBm)...\n",
                  WIFI_SSID, (long)melhorCanal, (long)melhorRssi);
    WiFi.begin(WIFI_SSID, WIFI_PASS, melhorCanal, melhorBssid);
#ifdef WIFI_SSID_2
  } else if (redeEscolhida == 2) {
    Serial.printf("[Wi-Fi] Conectando na rede 2 \"%s\" (ch %ld, %ld dBm)...\n",
                  WIFI_SSID_2, (long)melhorCanal, (long)melhorRssi);
    WiFi.begin(WIFI_SSID_2, WIFI_PASS_2, melhorCanal, melhorBssid);
#endif
  } else {
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
    delay(50);
    conectarWifiComScan();
    return;
  }

  if (!mqttIniciado) {
    Serial.printf("[Wi-Fi] Conectado! IP: %s | Canal: %d | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
    if (!espNowAtivo) iniciarEspNow();
    if (!uriUsaTls(MQTT_URI) || relogioValido() || sincronizarHora()) {
      iniciarMqtt();
    }
  }
}

// =============================================================================
// SENSORES (PIR HC-SR501 COM CALIBRACAO ~60s + ULTRASSONICO HC-SR04)
// =============================================================================
long lerUltrassonicoCm() {
  digitalWrite(ULTRA_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRA_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRA_TRIG_PIN, LOW);
  long duracao = pulseIn(ULTRA_ECHO_PIN, HIGH, 4000);
  if (duracao == 0) return 999;
  return (long)(duracao * 0.034 / 2);
}

bool lerPirFiltrado() {
#if USAR_PIR
  if (sensorAtual == SENSOR_CALIBRANDO) return false;
  if (digitalRead(PIR_PIN) == HIGH) {
    if (inicioPirHighMs == 0) inicioPirHighMs = millis();
    return (millis() - inicioPirHighMs) >= DEBOUNCE_PIR_MS;
  }
  inicioPirHighMs = 0;
#endif
  return false;
}

bool lerUltrassonicoFiltrado() {
#if USAR_ULTRASSONICO
  if (millis() - ultimaLeituraUltraMs >= INTERVALO_LEITURA_ULTRA) {
    ultimaLeituraUltraMs = millis();
    ultimaDistanciaCm = lerUltrassonicoCm();

    if (ultimaDistanciaCm >= DISTANCIA_MIN_CM && ultimaDistanciaCm < DISTANCIA_DETECCAO_CM) {
      if (contagemUltraValida < LEITURAS_ULTRA_CONFIRMA) contagemUltraValida++;
    } else {
      contagemUltraValida = 0;
    }
  }
  return contagemUltraValida >= LEITURAS_ULTRA_CONFIRMA;
#else
  ultimaDistanciaCm = 999;
  return false;
#endif
}

void verificarComandosSerial() {
  if (!Serial.available()) return;
  String linha = Serial.readStringUntil('\n');
  linha.trim();
  if (linha == "falha") {
    falhaForcadaSerial = true;
    sensorAtual = SENSOR_FALHA_ABERTO;
    modoAtual = MODO_SEGURO;
    publicarEstado();
  } else if (linha == "ok") {
    falhaForcadaSerial = false;
    sensorAtual = SENSOR_OK;
    modoAtual = MODO_AUTOMATICO;
    publicarEstado();
  }
}

// =============================================================================
// SETUP E LOOP PRINCIPAL
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.printf("\n=== Access N3 Satelite (%s | fw %s) ===\n", DEVICE_ID, FW_VERSION);

  pinMode(LED_ALARME_PIN, OUTPUT);
  digitalWrite(LED_ALARME_PIN, LOW);
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(ULTRA_TRIG_PIN, OUTPUT);
  digitalWrite(ULTRA_TRIG_PIN, LOW);
  pinMode(ULTRA_ECHO_PIN, INPUT_PULLDOWN);

  bootMs = millis();
  carregarNvs();

  // Wi-Fi STA com fallback automatico (rede de casa ou hotspot do celular)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  conectarWifiComScan();

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 25) {
    digitalWrite(LED_ALARME_PIN, !digitalRead(LED_ALARME_PIN));
    delay(400);
    Serial.print(".");
    tentativas++;
  }
  Serial.println();
  digitalWrite(LED_ALARME_PIN, LOW);

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[Wi-Fi] Conectado! IP: %s | Canal: %d | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
    iniciarEspNow();
    if (!uriUsaTls(MQTT_URI) || sincronizarHora()) {
      iniciarMqtt();
    }
  } else {
    Serial.printf("[Wi-Fi] Sem conexao no boot (status=%d). Operando em ESP-NOW canal %d.\n",
                  (int)WiFi.status(), CANAL_ESPNOW_PADRAO);
    esp_wifi_set_channel(CANAL_ESPNOW_PADRAO, WIFI_SECOND_CHAN_NONE);
    iniciarEspNow();
  }

  enviarMovimentoEspNow();
}

void loop() {
  verificarReconexaoWiFi();
  verificarComandosSerial();

  if (precisaPublicarConexao && brokerConectado) {
    precisaPublicarConexao = false;
    publicarStatusOnline();
    publicarEstado();
    publicarTelemetria();
  }

  drenarFilaComandos();

  // 1. Fim da calibracao do PIR (~60 s apos boot)
  if (!falhaForcadaSerial && sensorAtual == SENSOR_CALIBRANDO &&
      (millis() - bootMs) >= TEMPO_CALIBRACAO_PIR_MS) {
    sensorAtual = SENSOR_OK;
    Serial.println("[PIR] Calibracao concluida -> sensor=ok");
    publicarEstado();
  }

  // 2. Leitura filtrada dos sensores de presenca
  bool pirAtivo   = lerPirFiltrado();
  bool ultraAtivo = lerUltrassonicoFiltrado();
  bool detectouAgora = (sensorAtual == SENSOR_OK) && (pirAtivo || ultraAtivo);

  if (detectouAgora) {
    ultimoMovimentoMs = millis();
    fonteAtual = (pirAtivo ? 1 : 0) | (ultraAtivo ? 2 : 0);
    if (!temGenteAqui) {
      temGenteAqui = true;
      contadorDeteccoes++;
      Serial.printf("[PRESENCA] DETECTADA (PIR=%d | Ultra=%d, %ldcm | total=%lu)\n",
                    pirAtivo ? 1 : 0, ultraAtivo ? 1 : 0, ultimaDistanciaCm, (unsigned long)contadorDeteccoes);
      // Regra combinada Access: o Satelite reporta presenca para a Central segurar o alarme
      // e publica telemetria no painel, mas NAO dispara sirene sozinho com a porta fechada!
      enviarMovimentoEspNow();
      publicarTelemetria();
    } else if (millis() - ultimoEnvioEspNowMs >= INTERVALO_ENVIO_MOV_MS) {
      enviarMovimentoEspNow();
    }
  } else {
    if (temGenteAqui && (millis() - ultimoMovimentoMs >= TEMPO_PAROU_GENTE_MS)) {
      temGenteAqui = false;
      fonteAtual = 0;
      Serial.println("[PRESENCA] LIMPA");
      enviarMovimentoEspNow();
      publicarTelemetria();
    } else if (millis() - ultimoEnvioEspNowMs >= INTERVALO_HEARTBEAT_MS) {
      enviarMovimentoEspNow();
    }
  }

  // 3. Sincroniza cancelamento com o sensor de presenca e com a Central:
  //    se foi disparo da Regra Automatica (nao clique manual) e detectou alguem OU a Central ja parou,
  //    encerra o alarme do Satelite imediatamente (para de piscar e limpa o painel).
  bool centralOnlineAgora = ultimoContatoCentralMs > 0 &&
                            (millis() - ultimoContatoCentralMs) <= TIMEOUT_CENTRAL_MS;
  if (centralCancelouAlarmePendente) {
    centralCancelouAlarmePendente = false;
    if (alarmeAtual != ALARME_INATIVO && !disparoManualUsuario) {
      alarmeAtual = ALARME_INATIVO;
      Serial.println("[ESP-NOW] Central cancelou alarme -> Satelite voltou para inativo.");
      publicarEstado();
    }
  }
  if (alarmeAtual == ALARME_DISPARADO && !disparoManualUsuario) {
    if (temGenteAqui || (centralOnlineAgora && !alarmeRemotoEspNow)) {
      alarmeAtual = ALARME_INATIVO;
      Serial.println("[REGRA] Presenca detectada / Central inativa -> alarme do Satelite encerrado.");
      publicarEstado();
    }
  }
  if (!sistemaArmado && alarmeAtual != ALARME_INATIVO) {
    alarmeAtual = ALARME_INATIVO;
    disparoManualUsuario = false;
    publicarEstado();
  }

  // 4. LED sincronizado: pisca se o proprio alarme estiver disparado OU se a Central mandar alerta via ESP-NOW
  bool ledDevePiscar = (alarmeAtual == ALARME_DISPARADO) || (centralOnlineAgora && alarmeRemotoEspNow);

  if (ledDevePiscar) {
    if (millis() - ultimoPiscaMs > INTERVALO_PISCA_LED_MS) {
      ultimoPiscaMs = millis();
      estadoLed = !estadoLed;
      digitalWrite(LED_ALARME_PIN, estadoLed ? HIGH : LOW);
    }
  } else {
    digitalWrite(LED_ALARME_PIN, LOW);
  }

  // 5. Telemetria periodica (intervaloTelemetria_s vindo da NVS/config)
  unsigned long intervaloTelMs = (unsigned long)cfgIntervaloTelemetria * 1000UL;
  if (millis() - ultimaTelemetriaMs >= intervaloTelMs) {
    ultimaTelemetriaMs = millis();
    publicarTelemetria();
  }

  delay(20);
}
