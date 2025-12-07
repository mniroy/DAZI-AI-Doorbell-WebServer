#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==========================================
// PINS & HARDWARE CONFIG
// ==========================================

// Enable conversation memory
#define ENABLE_CONVERSATION_MEMORY 1

// I2S Speaker (MAX98357A)
#define I2S_DOUT 47
#define I2S_BCLK 46
#define I2S_LRC 45

// I2S Microphone (INMP441)
#define I2S_MIC_SERIAL_CLOCK 5      // SCK
#define I2S_MIC_LEFT_RIGHT_CLOCK 4  // WS
#define I2S_MIC_SERIAL_DATA 6       // SD

// Buttons & LEDs
#define BOOT_BUTTON_PIN 0
// RGB LED (NeoPixel) - GPIO 48 is standard for ESP32-S3 DevKit
#define RGB_LED_PIN 48 

// Audio Settings
#define SAMPLE_RATE 16000

// ==========================================
// DEFAULTS
// ==========================================

const char* const DEFAULT_PROMPT = 
"Kamu adalah asisten doorbell pintar di Rumah Escher, rumah keluarga Indonesia. "
"Perkenalkan diri: 'Halo, saya AI Rumah Escher, asisten rumah ini. Saya bantu jawab bel ya.' "
"Fokusmu: ajak ngobrol orang yang menekan bel, cari tahu mereka siapa dan apa keperluannya. "
"Selalu klasifikasikan pengunjung sebagai: kurir paket, kurir galon aqua, satpam, keluarga, atau tamu lain. "
"Gunakan Bahasa Indonesia santai tapi sopan. Jawaban pendek: 1–2 kalimat, maksimal sekitar 25 kata.";

const char* const DEFAULT_STREAM_URL = "http://192.168.1.34:1984/stream.html?src=Doorbell";

#endif
