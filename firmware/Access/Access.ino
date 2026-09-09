#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"
#include "config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- CREDENCIAIS SEGURAS ---
const char* ssid      = WIFI_SSID;
const char* password  = WIFI_PASS;
const char* ntfyTopic = NTFY_TOPIC;

// --- VARIÁVEIS DE ESTADO ---
int contadorAberturas = 0;
bool ultimoEstado = false;
unsigned long tempoUltimaMudanca = 0;
unsigned long tempoUltimoAlarme = 0;
bool tomAlarme = false;

// --- CONTROLE DE RECONEXÃO WI-FI (NÃO-BLOQUEANTE) ---
unsigned long ultimaChecagemWifi = 0;
const unsigned long intervaloChecagemWifi = INTERVALO_CHECAGEM_WIFI;

// --- ESTADO DO ALARME (MQTT) ---
// sistemaArmado: alterada apenas por comando remoto (Etapa 5).
// alarmeDisparado: escrita exclusivamente pelo handler de eventos MQTT,
// com a única exceção do modo degradado (Etapa 7). Nenhum outro trecho do
// firmware deve atribuir valor a ela.
bool sistemaArmado = false;
bool alarmeDisparado = false;

// --- ESTADO DA CONEXÃO COM O BROKER MQTT ---
bool brokerConectado = false;
unsigned long ultimoContatoBroker = 0; // usado pelo modo degradado (Etapa 7)

void enviarNotificacao() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "https://ntfy.sh/" + String(ntfyTopic);
    
    http.begin(url);
    http.addHeader("Title", "Alerta de Seguranca");
    http.addHeader("Priority", "high");
    http.addHeader("Tags", "warning,door");
    
    String mensagem = "A porta foi aberta! Total de aberturas: " + String(contadorAberturas);
    http.POST(mensagem);
    http.end();
    Serial.println("[HTTP] Notificacao enviada com sucesso ao celular via ntfy.sh");
  } else {
    Serial.println("[HTTP] Alerta nao enviado: Wi-Fi desconectado.");
  }
}

void somPortaFechou() {
  tone(BUZZER_POS, 1800, 40);
  delay(60);
  tone(BUZZER_POS, 2400, 60);
}

void processarAlarmePortaAberta() {
  if (millis() - tempoUltimoAlarme > 150) {
    tempoUltimoAlarme = millis();
    tomAlarme = !tomAlarme;
    if (tomAlarme) {
      tone(BUZZER_POS, 1200, 120);
    } else {
      tone(BUZZER_POS, 800, 120);
    }
  }
}

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

  // Barra de status com indicativo de rede
  display.fillRect(0, 0, 128, 12, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  display.setTextSize(1);
  display.setCursor(4, 2);
  display.print("ACESSO");

  display.setCursor(62, 2);
  if (WiFi.status() == WL_CONNECTED) {
    display.print("[WI-FI ON]");
  } else {
    display.print("[RECONECT]");
  }

  display.setTextColor(SSD1306_WHITE);
  desenharIconePorta(8, 18, aberta);

  display.setTextSize(2);
  display.setCursor(38, 18);
  if (aberta) {
    display.println("ABERTA");
  } else {
    display.println("FECHADA");
  }

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
  Serial.println("\n[Wi-Fi] Iniciando conexao ao AP...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[Wi-Fi] Conectado com sucesso!");
    Serial.print("[Wi-Fi] Endereco IP (DHCP): ");
    Serial.println(WiFi.localIP());
    Serial.print("[Wi-Fi] Forca do sinal (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("\n[Wi-Fi] Falha ao conectar inicialmente. O sistema continuara em loop.");
  }
}

void verificarReconexaoWiFi() {
  if (millis() - ultimaChecagemWifi > intervaloChecagemWifi) {
    ultimaChecagemWifi = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n[Wi-Fi] Conexao perdida detectada!");
      Serial.println("[Wi-Fi] Tentando reconectar automaticamente...");
      WiFi.reconnect();
    } else {
      // Confirmação periódica de conectividade mantida
      Serial.print("[Wi-Fi OK | RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm]");
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(REED_PIN, INPUT_PULLUP);

  // Configuração do Buzzer com terra virtual (D18)
  pinMode(BUZZER_POS, OUTPUT);
  pinMode(BUZZER_GND, OUTPUT);
  digitalWrite(BUZZER_GND, LOW);

  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_PRIMARIO)) {
    display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_ALTERNATIVO);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 25);
  display.println("Iniciando Sistema...");
  display.display();

  conectarWiFi();

  ultimoEstado = digitalRead(REED_PIN);
  tempoUltimaMudanca = millis();
}

void loop() {
  // 1. Checagem e reconexão automática de rede (não bloqueante)
  verificarReconexaoWiFi();

  // 2. Leitura do sensor magnético MC-38
  bool estadoAtual = digitalRead(REED_PIN);

  if (estadoAtual != ultimoEstado) {
    if (estadoAtual == true) { 
      contadorAberturas++;
      Serial.printf("\n[EVENTO] Porta Aberta! Contagem: %d\n", contadorAberturas);
      enviarNotificacao();
    } else { 
      Serial.println("\n[EVENTO] Porta Fechada.");
      somPortaFechou();
    }
    ultimoEstado = estadoAtual;
    tempoUltimaMudanca = millis();
  }

  // 3. Alarme sonoro
  if (estadoAtual == true) {
    processarAlarmePortaAberta();
  } else {
    noTone(BUZZER_POS);
  }

  // 4. Renderização do display
  atualizarDashboard(estadoAtual);
  delay(30);
}