#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// Placa 2: Satelite de Movimento (ESP32 Dev Module) — Hardware e ESP-NOW (v2)
// =============================================================================

// --- PINOS ---
#define LED_ALARME_PIN          2    // LED de alerta remoto (onboard/externo)
#define PIR_PIN                 4    // Sensor PIR HC-SR501 (INPUT_PULLDOWN)
#define ULTRA_TRIG_PIN          18   // Sensor Ultrassonico HC-SR04 (TRIG)
#define ULTRA_ECHO_PIN          19   // Sensor Ultrassonico HC-SR04 (ECHO)

// --- HABILITACAO DE SENSORES (coloque 0 no sensor que NAO estiver ligado) ---
#define USAR_PIR                  1    // 1 = usa PIR HC-SR501 no GPIO 4
#define USAR_ULTRASSONICO         1    // 1 = usa Ultrassonico HC-SR04 nos GPIOs 18/19

// --- PARAMETROS DE DETECCAO, FILTRO ANTI-RUIDO E TEMPORIZACAO ---
#define DISTANCIA_MIN_CM          3      // Ignora leituras falsas/ruido abaixo de 3 cm
#define DISTANCIA_DETECCAO_CM     50     // Limiar do ultrassonico para detectar passagem (cm)
#define INTERVALO_LEITURA_ULTRA   70     // Datasheet HC-SR04 recomenda >= 60ms entre pulsos
#define LEITURAS_ULTRA_CONFIRMA   2      // Exige 2 leituras seguidas < 50cm (evita fantasma)
#define DEBOUNCE_PIR_MS           40     // Debounce curto no PIR (responde rapido, filtra picos de RF)
#define TEMPO_PAROU_GENTE_MS      5000   // Tempo sem movimento para enviar "LIMPO" (ms)
#define INTERVALO_PISCA_LED_MS    120    // Cadencia de pisca do LED durante alarme (ms)

// --- ESP-NOW (comunicacao direta com a Central ESP32-S3) ---
// Potencia baixa (5 dBm) reduz ~30x a interferencia de radio RF no sensor PIR HC-SR501
#define WIFI_TX_POWER_SATELITE    WIFI_POWER_5dBm
#define ESPNOW_MAGIC              0xACCE5502UL
#define ESPNOW_TIPO_MOVIMENTO     1  // Satelite -> Central
#define ESPNOW_TIPO_ALARME        2  // Central  -> Satelite (alarme + heartbeat)
#define CANAL_ESPNOW_PADRAO       1
#define INTERVALO_ENVIO_MOV_MS    500    // Reenvia estado enquanto houver movimento (ms)
#define INTERVALO_HEARTBEAT_MS    3000   // Heartbeat espacado reduz pulsos de RF perto do PIR (ms)
#define TIMEOUT_CENTRAL_MS        6000   // Sem resposta da Central -> busca canal 1..13
#define INTERVALO_TROCA_CANAL_MS  1200   // Tempo parado em cada canal durante a busca

#endif
