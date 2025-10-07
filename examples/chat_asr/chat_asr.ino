#include <WiFi.h>
#include <ArduinoASRChat.h>
#include <ArduinoGPTChat.h>
#include "Audio.h"

// Define I2S pins for audio output (speaker)
#define I2S_DOUT 47
#define I2S_BCLK 48
#define I2S_LRC 45

// Define INMP441 microphone input pins
// INMP441 wiring:
// VDD -> 3.3V (DO NOT use 5V!)
// GND -> GND
// L/R -> GND (select left channel)
// WS  -> GPIO 4 (left/right clock)
// SCK -> GPIO 5 (serial clock)
// SD  -> GPIO 6 (serial data)
#define I2S_MIC_SERIAL_CLOCK 5    // SCK - serial clock
#define I2S_MIC_LEFT_RIGHT_CLOCK 4 // WS - left/right clock
#define I2S_MIC_SERIAL_DATA 6     // SD - serial data

// Define boot button pin (GPIO0 is the boot button on most ESP32 boards)
#define BOOT_BUTTON_PIN 0

// Sample rate for recording
#define SAMPLE_RATE 16000

// WiFi settings
const char* ssid     = "zh";
const char* password = "66666666";

// ByteDance ASR API configuration
const char* asr_api_key = "07fcb4a5-b7b2-45d8-864a-8cc0292380df";
const char* asr_cluster = "volcengine_input_en";

// OpenAI API configuration for LLM and TTS
const char* openai_apiKey = "sk-KkEHJ5tO1iiYIqr1jOmrH6FV2uagIICwzL0PDWarGIoHe3Zm";
const char* openai_apiBaseUrl = "https://api.chatanywhere.tech";

// System prompt configuration
const char* systemPrompt = "Please answer questions briefly, responses should not exceed 30 words. Avoid lengthy explanations, provide key information directly.";

// Global audio variable for TTS playback
Audio audio;

// Initialize ASR and GPT Chat instances
ArduinoASRChat asrChat(asr_api_key, asr_cluster);
ArduinoGPTChat gptChat(openai_apiKey, openai_apiBaseUrl);

// Continuous conversation mode state machine
enum ConversationState {
  STATE_IDLE,              // Waiting for button press to start
  STATE_LISTENING,         // ASR is recording and listening
  STATE_PROCESSING_LLM,    // Processing with ChatGPT
  STATE_PLAYING_TTS,       // TTS is playing
  STATE_WAIT_TTS_COMPLETE  // Waiting for TTS to complete
};

// State variables
ConversationState currentState = STATE_IDLE;
bool continuousMode = false;
bool buttonPressed = false;
bool wasButtonPressed = false;
unsigned long ttsStartTime = 0;
unsigned long ttsCheckTime = 0;

// TTS completion callback
void audio_eof_speech(const char* info) {
  Serial.println("\n[TTS] Playback completed");
  // This callback is called when TTS finishes playing
}

void setup() {
  // Initialize serial port
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n----- Voice Assistant System (ASR+LLM+TTS) Starting -----");

  // Initialize boot button
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  // Initialize random seed
  randomSeed(analogRead(0) + millis());

  // Connect to WiFi network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi...");

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    Serial.print('.');
    delay(1000);
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected successfully!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Free Heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");

    // Set I2S output pins for TTS playback
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    // Set volume
    audio.setVolume(100);

    // Set system prompt for GPT
    gptChat.setSystemPrompt(systemPrompt);

    // Initialize INMP441 microphone for ASR
    if (!asrChat.initINMP441Microphone(I2S_MIC_SERIAL_CLOCK, I2S_MIC_LEFT_RIGHT_CLOCK, I2S_MIC_SERIAL_DATA)) {
      Serial.println("Failed to initialize microphone!");
      return;
    }

    // Set audio parameters for ASR
    asrChat.setAudioParams(SAMPLE_RATE, 16, 1);
    asrChat.setSilenceDuration(1000);  // 1 second silence detection
    asrChat.setMaxRecordingSeconds(50);

    // Connect to ByteDance ASR WebSocket
    if (!asrChat.connectWebSocket()) {
      Serial.println("Failed to connect to ASR service!");
      return;
    }

    Serial.println("\n----- System Ready -----");
    Serial.println("Press BOOT button to start/stop continuous conversation mode");
    Serial.println("In continuous mode, ASR will automatically restart after TTS playback");
  } else {
    Serial.println("\nFailed to connect to WiFi. Please check network credentials and retry.");
  }
}

void startContinuousMode() {
  continuousMode = true;
  currentState = STATE_LISTENING;

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  Continuous Conversation Mode Started ║");
  Serial.println("║  Press BOOT again to stop             ║");
  Serial.println("╚════════════════════════════════════════╝");

  // Start ASR recording
  if (asrChat.startRecording()) {
    Serial.println("\n[ASR] Listening... Speak now");
  } else {
    Serial.println("\n[ERROR] Failed to start ASR");
    continuousMode = false;
    currentState = STATE_IDLE;
  }
}

void stopContinuousMode() {
  continuousMode = false;

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║  Continuous Conversation Mode Stopped ║");
  Serial.println("╚════════════════════════════════════════╝");

  // Stop any ongoing recording
  if (asrChat.isRecording()) {
    asrChat.stopRecording();
  }

  currentState = STATE_IDLE;
  Serial.println("\nPress BOOT button to start continuous conversation mode");
}

void handleASRResult() {
  String transcribedText = asrChat.getRecognizedText();
  asrChat.clearResult();

  if (transcribedText.length() > 0) {
    Serial.println("\n╔═══ ASR Recognition Result ═══╗");
    Serial.printf("║ %s\n", transcribedText.c_str());
    Serial.println("╚══════════════════════════════╝");

    currentState = STATE_PROCESSING_LLM;
    Serial.println("\n[LLM] Sending to ChatGPT...");

    // Send message to ChatGPT
    String response = gptChat.sendMessage(transcribedText);

    if (response != "" && response.length() > 0) {
      Serial.println("\n╔═══ ChatGPT Response ═══╗");
      Serial.printf("║ %s\n", response.c_str());
      Serial.println("╚════════════════════════╝");

      currentState = STATE_PLAYING_TTS;
      Serial.println("\n[TTS] Converting to speech and playing...");

      bool success = gptChat.textToSpeech(response);

      if (success) {
        currentState = STATE_WAIT_TTS_COMPLETE;
        ttsStartTime = millis();
        ttsCheckTime = millis();
      } else {
        Serial.println("[ERROR] Failed to play TTS audio");

        if (continuousMode) {
          // Restart ASR even if TTS fails
          delay(500);
          currentState = STATE_LISTENING;
          if (asrChat.startRecording()) {
            Serial.println("\n[ASR] Listening... Speak now");
          } else {
            stopContinuousMode();
          }
        } else {
          currentState = STATE_IDLE;
        }
      }
    } else {
      Serial.println("[ERROR] Failed to get ChatGPT response");

      if (continuousMode) {
        // Restart ASR even if LLM fails
        delay(500);
        currentState = STATE_LISTENING;
        if (asrChat.startRecording()) {
          Serial.println("\n[ASR] Listening... Speak now");
        } else {
          stopContinuousMode();
        }
      } else {
        currentState = STATE_IDLE;
      }
    }
  } else {
    Serial.println("[WARN] No text recognized");

    if (continuousMode) {
      // Restart ASR
      delay(500);
      currentState = STATE_LISTENING;
      if (asrChat.startRecording()) {
        Serial.println("\n[ASR] Listening... Speak now");
      } else {
        stopContinuousMode();
      }
    } else {
      currentState = STATE_IDLE;
    }
  }
}

void loop() {
  // Handle audio loop (TTS playback)
  audio.loop();

  // Handle ASR processing
  asrChat.loop();

  // Handle boot button (toggle continuous mode)
  buttonPressed = (digitalRead(BOOT_BUTTON_PIN) == LOW); // LOW when pressed

  if (buttonPressed && !wasButtonPressed) {
    wasButtonPressed = true;

    // Toggle continuous conversation mode
    if (!continuousMode && currentState == STATE_IDLE) {
      startContinuousMode();
    } else if (continuousMode) {
      stopContinuousMode();
    }
  } else if (!buttonPressed && wasButtonPressed) {
    wasButtonPressed = false;
  }

  // State machine for continuous conversation
  switch (currentState) {
    case STATE_IDLE:
      // Do nothing, waiting for button press
      break;

    case STATE_LISTENING:
      // Check if ASR has detected end of speech (VAD completed)
      if (asrChat.hasNewResult()) {
        handleASRResult();
      }
      break;

    case STATE_PROCESSING_LLM:
      // This state is handled in handleASRResult()
      break;

    case STATE_PLAYING_TTS:
      // This state is handled in handleASRResult()
      break;

    case STATE_WAIT_TTS_COMPLETE:
      // Check if TTS playback has completed
      // We check audio.isRunning() periodically
      if (millis() - ttsCheckTime > 100) {  // Check every 100ms
        ttsCheckTime = millis();

        if (!audio.isRunning()) {
          // TTS has completed
          Serial.println("[TTS] Playback finished");

          if (continuousMode) {
            // Restart ASR for next round
            delay(500);  // Small delay before restarting
            currentState = STATE_LISTENING;

            if (asrChat.startRecording()) {
              Serial.println("\n[ASR] Listening... Speak now");
            } else {
              Serial.println("[ERROR] Failed to restart ASR");
              stopContinuousMode();
            }
          } else {
            currentState = STATE_IDLE;
          }
        } else {
          // Still playing, check for timeout (optional)
          if (millis() - ttsStartTime > 60000) {  // 60 second timeout
            Serial.println("[WARN] TTS timeout, forcing restart");

            if (continuousMode) {
              currentState = STATE_LISTENING;
              if (asrChat.startRecording()) {
                Serial.println("\n[ASR] Listening... Speak now");
              } else {
                stopContinuousMode();
              }
            } else {
              currentState = STATE_IDLE;
            }
          }
        }
      }
      break;
  }

  // Very small delay - audio capture needs high priority
  if (currentState == STATE_LISTENING) {
    // During recording, minimize delay to ensure audio data is sent fast enough
    yield();
  } else {
    // In other states, can have slightly longer delay
    delay(10);
  }
}
