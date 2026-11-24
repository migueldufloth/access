#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// Placa 2: Satelite de Movimento (ESP32 Dev Module) — Contrato MQTT v1 (N3) + ESP-NOW
// =============================================================================

// --- IDENTIDADE DO DISPOSITIVO (CONTRATOS.md v1) ---
#define DEVICE_ID                  "janela-01"
#define FW_VERSION                 "1.0.0"     // Estritamente MAJOR.MINOR.PATCH
#define PREFIXO_MQTT               "access/grupo5/" DEVICE_ID "/"

// --- PINOS ---
// R1: LED externo deve usar resistor 220 Ohm em serie para GND real; PIR alimentado no VIN (5V)
#define LED_ALARME_PIN             2           // LED de alerta (onboard ou externo com resistor 220 Ohm)
#define PIR_PIN                    4           // Sensor PIR HC-SR501 (OUT -> GPIO 4, VCC -> VIN 5V)
#define ULTRA_TRIG_PIN             18          // Sensor Ultrassonico HC-SR04 (TRIG)
#define ULTRA_ECHO_PIN             19          // Sensor Ultrassonico HC-SR04 (ECHO)

// --- HABILITACAO DE SENSORES (coloque 0 no sensor que NAO estiver ligado) ---
#define USAR_PIR                   1
#define USAR_ULTRASSONICO          1

// --- PARAMETROS DE DETECCAO, CALIBRACAO E FILTRO ANTI-RUIDO ---
#define TEMPO_CALIBRACAO_PIR_MS    60000       // Primeiros ~60s reporta sensor="calibrando" (CONTRATOS.md)
#define DISTANCIA_MIN_CM           3           // Ignora ruido < 3 cm
#define DISTANCIA_DETECCAO_CM      50          // Limiar do ultrassonico (cm)
#define INTERVALO_LEITURA_ULTRA    70          // >= 60ms entre pulsos (datasheet HC-SR04)
#define LEITURAS_ULTRA_CONFIRMA    2           // Exige 2 leituras seguidas < 50 cm
#define DEBOUNCE_PIR_MS            40          // Debounce curto no PIR
#define TEMPO_PAROU_GENTE_MS       5000        // Tempo sem movimento para voltar a presenca=0
#define INTERVALO_PISCA_LED_MS     120         // Cadencia de pisca do LED em alarme

// --- WI-FI E ESP-NOW (convivem no mesmo canal do roteador) ---
// Potencia moderada (8.5 dBm) alcanca o roteador reduzindo interferencia de RF no PIR
#define WIFI_TX_POWER_SATELITE     WIFI_POWER_8_5dBm
#define INTERVALO_CHECAGEM_WIFI    15000
#define INTERVALO_SNTP_TIMEOUT_MS  15000

#define ESPNOW_MAGIC               0xACCE5502UL
#define ESPNOW_TIPO_MOVIMENTO      1           // Satelite -> Central
#define ESPNOW_TIPO_ALARME         2           // Central  -> Satelite (alarme + heartbeat)
#define CANAL_ESPNOW_PADRAO        1
#define INTERVALO_ENVIO_MOV_MS     500
#define INTERVALO_HEARTBEAT_MS     3000
#define TIMEOUT_CENTRAL_MS         10000

// --- VALORES PADRAO NVS (CONTRATOS.md) ---
#define DEFAULT_TEMPO_ENTRADA_S    20          // Faixa valida: 0..120
#define DEFAULT_INTERVALO_TEL_S    5           // Faixa valida: 1..300
#define DEFAULT_TIMEOUT_MANUAL_S   1800        // Faixa valida: 60..7200

#endif
