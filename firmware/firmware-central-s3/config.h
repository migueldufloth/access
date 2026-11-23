#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// Placa 1: Central da Porta (ESP32-S3) — Hardware, ESP-NOW e MQTT (v2 Hibrido)
// =============================================================================

// --- DISPLAY OLED SSD1306 (128x64 I2C) ---
// Alimentacao via GPIOs com drive maximo (GPIO_DRIVE_CAP_3) para estabilidade
#define SCREEN_WIDTH           128
#define SCREEN_HEIGHT          64
#define OLED_RESET             -1
#define OLED_VCC_PIN           4
#define OLED_GND_PIN           5
#define I2C_SDA                14
#define I2C_SCL                12
#define OLED_ADDR_PRIMARIO     0x3C
#define OLED_ADDR_ALTERNATIVO  0x3D

// --- SENSOR MAGNETICO DE PORTA (MC-38) ---
#define REED_PIN               15   // INPUT_PULLUP (HIGH = porta aberta)
#define REED_GND_PIN           16   // GND virtual (LOW)

// --- LED DE ALERTA LOCAL ---
#define LED_POS_PIN            2    // Positivo (+)
#define LED_GND_PIN            1    // GND virtual (LOW)

// --- AMPLIFICADOR I2S MAX98357 + CAIXA DE SOM ---
// Usa exclusivamente o driver nativo driver/i2s.h (sem ESP8266Audio)
#define I2S_SPK_PORT           I2S_NUM_0
#define SPK_BCLK               47
#define SPK_LRC                48
#define SPK_DIN                21
#define SAMPLE_RATE            44100

// Volume do audio I2S (escala de 0 a 20000; antes estava em 20000)
#define VOLUME_BIPE_PORTA      4500.0f  // Bipes suaves de abrir/fechar porta
#define VOLUME_SIRENE          6500.0f  // Volume da sirene de alarme

// --- WI-FI (potencia controlada para nao derrubar tensao da OLED) ---
// 8.5 dBm equilibra estabilidade da OLED com alcance para roteador residencial
#define WIFI_TX_POWER              WIFI_POWER_8_5dBm

// --- ESP-NOW (comunicacao direta entre Placa 1 e Placa 2) ---
#define ESPNOW_MAGIC               0xACCE5502UL
#define ESPNOW_TIPO_MOVIMENTO      1  // Satelite -> Central
#define ESPNOW_TIPO_ALARME         2  // Central  -> Satelite (alarme + heartbeat)
#define CANAL_ESPNOW_PADRAO        1  // Canal usado se a Central estiver sem Wi-Fi
#define INTERVALO_ESPNOW_HB_MS     1000
#define TIMEOUT_SATELITE_MS        10000

// --- TEMPORIZACOES (ms) ---
#define TOLERANCIA_SEM_GENTE_MS    8000   // Tolerancia inicial sem movimento com porta aberta
#define DEBOUNCE_REED_MS           50     // Debounce do sensor magnetico MC-38
#define INTERVALO_CHECAGEM_WIFI    30000  // Tenta reconectar Wi-Fi sem interromper o ESP-NOW
#define INTERVALO_TELEMETRIA       5000   // Publicacao periodica de telemetria JSON
#define INTERVALO_SNTP_TIMEOUT_MS  15000  // Timeout maximo para sincronizacao SNTP no boot

// --- TOPICO NTFY (fallback caso secrets.h nao defina NTFY_TOPIC) ---
#ifndef NTFY_TOPIC
#define NTFY_TOPIC                 "AccessPortaTeste"
#endif

// --- TOPICOS MQTT (namespace access/grupo5/... do broker deste repositorio) ---
#define TOPICO_PORTA               "access/grupo5/sensor/porta"
#define TOPICO_MOVIMENTO           "access/grupo5/sensor/movimento"
#define TOPICO_TELEMETRIA          "access/grupo5/sensor/presenca"
#define TOPICO_ALARME              "access/grupo5/status/alarme"
#define TOPICO_STATUS_CENTRAL      "access/grupo5/status/central"
#define TOPICO_STATUS_SATELITE     "access/grupo5/status/satelite"
#define TOPICO_STATUS_PRESENCA     "access/grupo5/status/presenca_dispositivo"

#endif
