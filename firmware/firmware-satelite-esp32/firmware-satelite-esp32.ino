#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "secrets.h"
#include "config.h"

// =============================================================================
// PROTOCOLO ESP-NOW (Placa 2 <-> Placa 1, comunicacao direta instantanea)
// =============================================================================
typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint8_t  tipo;         // 1 = MOVIMENTO (Sat->Cen), 2 = ALARME/HB (Cen->Sat)
  uint8_t  valor;        // tipo 1: 1=DETECTADO, 0=LIMPO | tipo 2: 1=ALARME, 0=OFF
  uint8_t  fonte;        // tipo 1: 0=nenhum, 1=PIR, 2=ULTRA, 3=AMBOS | tipo 2: portaAberta
  uint8_t  distanciaCm;  // tipo 1: distancia em cm (255=sem leitura) | tipo 2: canal Wi-Fi
} PacoteAccessEspNow;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- ESTADO DO SATELITE ---
bool temGenteAqui = false;
uint8_t fonteAtual = 0; // 0=nenhum, 1=PIR, 2=ULTRA, 3=AMBOS
volatile bool alarmeGeralAtivo = false;
volatile unsigned long ultimoContatoCentralMs = 0;
volatile bool centralReconectouLog = false;

uint8_t canalAtual = CANAL_ESPNOW_PADRAO;
unsigned long ultimoMovimentoMs = 0;
unsigned long ultimoEnvioMovimentoMs = 0;
unsigned long ultimaTrocaCanalMs = 0;
unsigned long ultimoPiscaMs = 0;
bool estadoLed = false;

// Filtros anti-ruido dos sensores
unsigned long inicioPirHighMs = 0;
unsigned long ultimaLeituraUltraMs = 0;
unsigned long ultimoLogDiagMs = 0;
int contagemUltraValida = 0;
long ultimaDistanciaCm = 999;

// =============================================================================
// ESP-NOW E SINCRONIZACAO AUTOMATICA DE CANAL WI-FI
// =============================================================================
void definirCanalWifi(uint8_t canal) {
  if (canal < 1 || canal > 13) canal = CANAL_ESPNOW_PADRAO;
  canalAtual = canal;
  esp_wifi_set_channel(canalAtual, WIFI_SECOND_CHAN_NONE);
}

void enviarMovimentoEspNow() {
  PacoteAccessEspNow pkt = {};
  pkt.magic       = ESPNOW_MAGIC;
  pkt.tipo        = ESPNOW_TIPO_MOVIMENTO;
  pkt.valor       = temGenteAqui ? 1 : 0;
  pkt.fonte       = temGenteAqui ? fonteAtual : 0;
  pkt.distanciaCm = (ultimaDistanciaCm >= 0 && ultimaDistanciaCm <= 254) ? (uint8_t)ultimaDistanciaCm : 255;

  esp_now_send(BROADCAST_MAC, (const uint8_t*)&pkt, sizeof(pkt));
  ultimoEnvioMovimentoMs = millis();
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
    bool estavaDesconectado = (ultimoContatoCentralMs == 0) ||
                              ((millis() - ultimoContatoCentralMs) > TIMEOUT_CENTRAL_MS);
    ultimoContatoCentralMs = millis();
    alarmeGeralAtivo = (pkt.valor == 1);
    if (estavaDesconectado) {
      centralReconectouLog = true;
    }
  }
}

uint8_t buscarCanalDoRoteador(const char* ssidAlvo) {
  if (ssidAlvo == nullptr || strlen(ssidAlvo) == 0) return CANAL_ESPNOW_PADRAO;

  Serial.printf("[ESP-NOW] Escaneando canal da rede \"%s\"...\n", ssidAlvo);
  int n = WiFi.scanNetworks();
  uint8_t canalEncontrado = 0;

  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssidAlvo) {
      canalEncontrado = (uint8_t)WiFi.channel(i);
      Serial.printf("[ESP-NOW] Rede \"%s\" encontrada no canal %d!\n", ssidAlvo, canalEncontrado);
      break;
    }
  }
  WiFi.scanDelete();

  if (canalEncontrado == 0) {
    Serial.printf("[ESP-NOW] Rede \"%s\" nao encontrada; iniciando no canal %d.\n",
                  ssidAlvo, CANAL_ESPNOW_PADRAO);
    return CANAL_ESPNOW_PADRAO;
  }
  return canalEncontrado;
}

void iniciarEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Falha ao inicializar! Reiniciando...");
    delay(1000);
    ESP.restart();
  }

  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, BROADCAST_MAC, 6);
  peerInfo.channel = 0; // Usa o canal configurado via esp_wifi_set_channel
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;

  if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
    esp_now_add_peer(&peerInfo);
  }
  Serial.println("[ESP-NOW] Satelite pronto!");
}

// Se ficar sem receber heartbeat da Central, varre canais 1..13 automaticamente
// ate encontrar a Central (funciona mesmo trocando de roteador ou sem Wi-Fi).
void manterSincronismoCanal() {
  bool centralConectada = (ultimoContatoCentralMs > 0) &&
                          ((millis() - ultimoContatoCentralMs) <= TIMEOUT_CENTRAL_MS);

  if (centralReconectouLog) {
    centralReconectouLog = false;
    Serial.printf("[ESP-NOW] Central encontrada e sincronizada no canal %d!\n", canalAtual);
    enviarMovimentoEspNow();
  }

  if (!centralConectada) {
    alarmeGeralAtivo = false;
    if (millis() - ultimaTrocaCanalMs >= INTERVALO_TROCA_CANAL_MS) {
      ultimaTrocaCanalMs = millis();
      uint8_t proximoCanal = (canalAtual % 13) + 1;
      definirCanalWifi(proximoCanal);
      Serial.printf("[ESP-NOW] Procurando Central no canal %d...\n", canalAtual);
      enviarMovimentoEspNow();
    }
  }
}

// =============================================================================
// SENSORES (PIR HC-SR501 + ULTRASSONICO HC-SR04 COM FILTRO ANTI-RUIDO)
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
  if (digitalRead(PIR_PIN) == HIGH) {
    if (inicioPirHighMs == 0) {
      inicioPirHighMs = millis();
    }
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
      if (contagemUltraValida < LEITURAS_ULTRA_CONFIRMA) {
        contagemUltraValida++;
      }
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

// =============================================================================
// SETUP E LOOP PRINCIPAL
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(LED_ALARME_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT_PULLDOWN);
  pinMode(ULTRA_TRIG_PIN, OUTPUT);
  digitalWrite(ULTRA_TRIG_PIN, LOW);
  // INPUT_PULLDOWN evita disparos "fantasma" se o HC-SR04 nao estiver conectado
  pinMode(ULTRA_ECHO_PIN, INPUT_PULLDOWN);

  // Piscada rapida indicando boot do Satelite
  digitalWrite(LED_ALARME_PIN, HIGH);
  delay(150);
  digitalWrite(LED_ALARME_PIN, LOW);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_TX_POWER_SATELITE);

  // Descobre automaticamente o canal do roteador da Central (sem precisar conectar)
  uint8_t canalInicial = buscarCanalDoRoteador(WIFI_SSID);
  definirCanalWifi(canalInicial);

  iniciarEspNow();
  enviarMovimentoEspNow();
  ultimaTrocaCanalMs = millis();
}

void loop() {
  manterSincronismoCanal();

  // 1. Leitura filtrada dos sensores de movimento (PIR e/ou Ultrassonico)
  bool pirAtivo   = lerPirFiltrado();
  bool ultraAtivo = lerUltrassonicoFiltrado();
  bool detectouAgora = pirAtivo || ultraAtivo;

  // Log de diagnostico a cada 1s no Monitor Serial para enxergar o sensor ao vivo
  if (millis() - ultimoLogDiagMs >= 1000) {
    ultimoLogDiagMs = millis();
    bool centralOk = (ultimoContatoCentralMs > 0) &&
                     ((millis() - ultimoContatoCentralMs) <= TIMEOUT_CENTRAL_MS);
    Serial.printf("[DIAG] PIR_RAW=%d (filt=%d) | Ultra=%ldcm (filt=%d) | Mov=%s | Central=%s (ch %d)\n",
                  digitalRead(PIR_PIN), pirAtivo ? 1 : 0,
                  ultimaDistanciaCm, ultraAtivo ? 1 : 0,
                  temGenteAqui ? "DETECTADO" : "LIMPO",
                  centralOk ? "OK" : "BUSCANDO", canalAtual);
  }

  if (detectouAgora) {
    ultimoMovimentoMs = millis();
    fonteAtual = (pirAtivo ? 1 : 0) | (ultraAtivo ? 2 : 0);
    if (!temGenteAqui) {
      temGenteAqui = true;
      Serial.printf("[MOVIMENTO] DETECTADO (PIR=%d | Ultra=%d, %ldcm)\n",
                    pirAtivo ? 1 : 0, ultraAtivo ? 1 : 0, ultimaDistanciaCm);
      enviarMovimentoEspNow();
    } else if (millis() - ultimoEnvioMovimentoMs >= INTERVALO_ENVIO_MOV_MS) {
      enviarMovimentoEspNow();
    }
  } else {
    if (temGenteAqui && (millis() - ultimoMovimentoMs >= TEMPO_PAROU_GENTE_MS)) {
      temGenteAqui = false;
      fonteAtual = 0;
      Serial.println("[MOVIMENTO] LIMPO");
      enviarMovimentoEspNow();
    } else if (millis() - ultimoEnvioMovimentoMs >= INTERVALO_HEARTBEAT_MS) {
      enviarMovimentoEspNow();
    }
  }

  // 2. LED de alerta remoto sincronizado com o alarme enviado pela Central
  if (alarmeGeralAtivo) {
    if (millis() - ultimoPiscaMs > INTERVALO_PISCA_LED_MS) {
      ultimoPiscaMs = millis();
      estadoLed = !estadoLed;
      digitalWrite(LED_ALARME_PIN, estadoLed ? HIGH : LOW);
    }
  } else {
    digitalWrite(LED_ALARME_PIN, LOW);
  }

  delay(30);
}
