#pragma once

// --- PINOS ---
#define REED_PIN    4   // Sensor magnético MC-38
#define BUZZER_POS  25  // Buzzer Positivo (+)
#define BUZZER_GND  18  // Buzzer Negativo (-) -> GND virtual
#define OLED_SDA    21
#define OLED_SCL    22
#define OLED_ADDR_PRIMARIO     0x3C
#define OLED_ADDR_ALTERNATIVO  0x3D

// --- INTERVALOS (ms) ---
#define INTERVALO_CHECAGEM_WIFI      5000   // reavaliação da conexão Wi-Fi
#define INTERVALO_TELEMETRIA         5000   // publicação periódica de telemetria
#define INTERVALO_ATUALIZACAO_OLED    200   // cadência de redesenho do display
#define TIMEOUT_MODO_DEGRADADO      30000   // sem contato com o broker -> decide localmente

// --- TÓPICOS MQTT (access/grupo5/...) ---
#define TOPICO_SENSOR_PRESENCA              "access/grupo5/sensor/presenca"
#define TOPICO_COMANDO_ALARME               "access/grupo5/comando/alarme"
#define TOPICO_STATUS_CONFIRMACAO           "access/grupo5/status/confirmacao"
#define TOPICO_STATUS_PRESENCA_DISPOSITIVO  "access/grupo5/status/presenca_dispositivo"
