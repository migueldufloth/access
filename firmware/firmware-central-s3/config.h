#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// Placa 1: Central da Porta (ESP32-S3) — Contrato MQTT v1 (N3) + ESP-NOW
// =============================================================================

// --- IDENTIDADE DO DISPOSITIVO (CONTRATOS.md v1) ---
#define DEVICE_ID                  "porta-01"
#define FW_VERSION                 "1.0.0"     // Estritamente MAJOR.MINOR.PATCH
#define PREFIXO_MQTT               "access/grupo5/" DEVICE_ID "/"

// --- DISPLAY OLED SSD1306 (128x64 I2C) ---
// R1: VCC ligado no 3V3 real e GND no GND real da placa (nunca em GPIO!)
#define SCREEN_WIDTH               128
#define SCREEN_HEIGHT              64
#define OLED_RESET                 -1
#define I2C_SDA                    14
#define I2C_SCL                    12
#define OLED_ADDR_PRIMARIO         0x3C
#define OLED_ADDR_ALTERNATIVO      0x3D

// --- SENSOR MAGNETICO DE PORTA (MC-38) ---
// R1: terra do reed ligado no GND real da placa (nunca em GPIO!)
// REED_SUPERVISIONADO = 0: MC-38 digital direto no GPIO 15 (INPUT_PULLUP)
// REED_SUPERVISIONADO = 1: MC-38 analogico supervisionado (R1=10k, Rs=2.2k, Reol=4.7k) no GPIO 6 (ADC1_CH5)
#define REED_SUPERVISIONADO        0
#define REED_PIN                   15          // Entrada digital (HIGH = porta aberta)
#define REED_ADC_PIN               6           // ADC1_CH5 no ESP32-S3 (funciona com Wi-Fi ativo)
#define REED_MV_CURTO_MAX          150         // < 150 mV  -> falha_curto
#define REED_MV_FECHADA_MAX        1000        // 150..1000 mV -> porta fechada
#define REED_MV_ABERTA_MAX         2300        // 1000..2300 mV -> porta aberta; > 2300 mV -> falha_aberto
#define TEMPO_FALHA_SENSOR_MS      3000        // Tempo em leitura impossivel para entrar em modo seguro

// --- LED DE ALERTA LOCAL ---
// R1: GPIO 2 -> resistor 220 Ohm -> anodo (+) do LED -> catodo (-) no GND real
#define LED_POS_PIN                2

// --- AMPLIFICADOR I2S MAX98357 + CAIXA DE SOM ---
// Usa exclusivamente o driver nativo driver/i2s.h (sem ESP8266Audio)
#define I2S_SPK_PORT               I2S_NUM_0
#define SPK_BCLK                   47
#define SPK_LRC                    48
#define SPK_DIN                    21
#define SAMPLE_RATE                44100
#define VOLUME_BIPE_PORTA          14000.0f    // Bipes audiveis de abrir/fechar porta (0..20000)
#define VOLUME_SIRENE              20000.0f    // Volume maximo da sirene para toda a sala ouvir (0..20000)

// --- WI-FI (8.5 dBm: ja validado nesse ESP32-S3 sem queda de tensao) ---
#define WIFI_TX_POWER              WIFI_POWER_8_5dBm
#define INTERVALO_CHECAGEM_WIFI    15000       // Reconexao automatica sem cortar tentativa em andamento (R10)
#define INTERVALO_SNTP_TIMEOUT_MS  15000

// --- ESP-NOW (sincronizacao imediata de sirene/LED e movimento sem depender de rede) ---
#define ESPNOW_MAGIC               0xACCE5502UL
#define ESPNOW_TIPO_MOVIMENTO      1           // Satelite -> Central
#define ESPNOW_TIPO_ALARME         2           // Central  -> Satelite (alarme + heartbeat)
#define CANAL_ESPNOW_PADRAO        1
#define INTERVALO_ESPNOW_HB_MS     1000
#define TIMEOUT_SATELITE_MS        10000

// --- LOGICA LOCAL E VALORES PADRAO NVS (CONTRATOS.md) ---
#define DEBOUNCE_REED_MS           50
#define DEFAULT_TEMPO_ENTRADA_S    8           // Faixa valida: 0..120 (8s padrao; alteravel pelo painel e salvo em NVS)
#define DEFAULT_INTERVALO_TEL_S    5           // Faixa valida: 1..300
#define DEFAULT_TIMEOUT_MANUAL_S   1800        // Faixa valida: 60..7200
#define USAR_MOVIMENTO_SEGURAR     1           // 1 = movimento do Satelite segura a entrada/silencia quando armado

#endif
