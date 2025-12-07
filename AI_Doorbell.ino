#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoASRChat.h>
#include <ArduinoGPTChat.h>
#include <PubSubClient.h>
#include "Audio.h"

// Included refactored files
#include "src/config.h"
#include "src/web_index.h"
#include "src/utils.h"

// ==========================================
// GLOBALS
// ==========================================

// Objects
Preferences preferences;
WebServer server(80);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

ArduinoASRChat *asrChat = NULL;
ArduinoGPTChat *gptChat = NULL;
Audio audio;

// Settings Variables
String wifi_ssid, wifi_pass;
String asr_key, asr_clust;
String openai_key, openai_url, sys_prompt;
String stream_url;
int asr_max_duration; 

// MQTT Variables
String mqtt_server, mqtt_port, mqtt_user, mqtt_pass;
String mqtt_topic_history; 
String mqtt_topic_sub;    

// Chat State
String lastUserText = "(Waiting for guest...)";
String lastAIText = "(Waiting for AI...)";
String sessionHistoryJSON = "[]"; // Accumulates full conversation

// Button Pulse State
unsigned long mqttButtonPressTime = 0;
bool mqttButtonActive = false;

// State Machine
enum ConversationState { STATE_IDLE, STATE_LISTENING, STATE_PROCESSING_LLM, STATE_PLAYING_TTS, STATE_WAIT_TTS_COMPLETE };
ConversationState currentState = STATE_IDLE;
bool continuousMode = false;
bool buttonPressed = false;
bool wasButtonPressed = false;
unsigned long ttsStartTime = 0;
unsigned long ttsCheckTime = 0;
bool isAPMode = false;
unsigned long lastMqttReconnectAttempt = 0;

// ==========================================
// FORWARD DECLARATIONS
// ==========================================
void startContinuousMode(bool isInitialTrigger = false); 
void stopContinuousMode();
void handleToggle();
boolean reconnectMQTT();

// ==========================================
// MQTT FUNCTIONS
// ==========================================

void mqttCallback(char* topic, uint8_t* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  sysLogLn("[MQTT] Rx on " + String(topic) + ": " + message);

  if (message.indexOf("trigger") >= 0 || message.indexOf("press") >= 0) {
    sysLogLn("[MQTT] Trigger command received!");
    if (!continuousMode) {
      startContinuousMode(true); 
    } else {
      stopContinuousMode();
    }
  }
}

void setupMQTT() {
  if (mqtt_server.length() > 0) {
    int port = mqtt_port.toInt();
    if (port == 0) port = 1883; 
    mqttClient.setServer(mqtt_server.c_str(), port);
    mqttClient.setCallback(mqttCallback);
    
    // Increase buffer size to handle large JSON payloads (Full History)
    mqttClient.setBufferSize(8192); 
    
    sysLogLn("MQTT Configured: " + mqtt_server + ":" + String(port) + " (Buffer: 8192)");
  }
}

boolean reconnectMQTT() {
  if (mqtt_server.length() == 0) return false;
   
  // Ensure WiFi is up
  if (WiFi.status() != WL_CONNECTED) {
    sysLogLn("[WiFi] Connection lost. Reconnecting WiFi...");
    WiFi.reconnect();
    int w = 0;
    while (WiFi.status() != WL_CONNECTED && w < 10) { delay(500); w++; }
  }

  // Try to connect
  if (mqttClient.connect("EscherDoorbell", mqtt_user.c_str(), mqtt_pass.c_str())) {
    sysLogLn("[MQTT] Connected");
    if (mqtt_topic_sub.length() > 0) {
      mqttClient.subscribe(mqtt_topic_sub.c_str());
    }
    return true;
  }
  return false;
}

bool publishToTopic(String topic, String message) {
  // 1. Check/Restore Connection
  if (!mqttClient.connected()) {
    sysLogLn("[MQTT] Connecting...");
    if (!reconnectMQTT()) {
       sysLogLn("[MQTT ERROR] Reconnect failed.");
       return false;
    }
  }
   
  // 2. Stabilize
  mqttClient.loop();
   
  // 3. Publish
  if (mqttClient.publish(topic.c_str(), message.c_str())) {
    return true;
  } else {
    sysLogLn("[MQTT ERROR] Publish failed. Len: " + String(message.length()));
    return false;
  }
}

// ==========================================
// SERVER HANDLERS
// ==========================================

void handleRoot() {
  String html = html_template; // From src/web_index.h
  html.replace("id=\"ssid\">", "id=\"ssid\" value=\"" + wifi_ssid + "\">");
  html.replace("id=\"pass\">", "id=\"pass\" value=\"" + wifi_pass + "\">"); 
  html.replace("id=\"asrkey\">", "id=\"asrkey\" value=\"" + asr_key + "\">");
  html.replace("id=\"asrclus\">", "id=\"asrclus\" value=\"" + asr_clust + "\">");
  html.replace("id=\"apikey\">", "id=\"apikey\" value=\"" + openai_key + "\">");
  html.replace("id=\"apiurl\">", "id=\"apiurl\" value=\"" + openai_url + "\">");
  html.replace("id=\"prompt\"></textarea>", "id=\"prompt\">" + sys_prompt + "</textarea>");
  
  html.replace("id=\"timeout\">", "id=\"timeout\" value=\"" + String(asr_max_duration) + "\">");

  html.replace("id=\"mqserver\">", "id=\"mqserver\" value=\"" + mqtt_server + "\">");
  html.replace("id=\"mqport\">", "id=\"mqport\" value=\"" + mqtt_port + "\">");
  html.replace("id=\"mquser\">", "id=\"mquser\" value=\"" + mqtt_user + "\">");
  html.replace("id=\"mqpass\">", "id=\"mqpass\" value=\"" + mqtt_pass + "\">");
  
  html.replace("id=\"mqpub_hist\">", "id=\"mqpub_hist\" value=\"" + mqtt_topic_history + "\">");
  html.replace("id=\"mqsub\">", "id=\"mqsub\" value=\"" + mqtt_topic_sub + "\">");

  html.replace("id=\"stream\">", "id=\"stream\" value=\"" + stream_url + "\">");
  html.replace("STREAM_URL_PLACEHOLDER", stream_url);

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid")) {
    preferences.putString("ssid", server.arg("ssid"));
    preferences.putString("pass", server.arg("pass"));
    preferences.putString("asrkey", server.arg("asrkey"));
    preferences.putString("asrclus", server.arg("asrclus"));
    preferences.putString("apikey", server.arg("apikey"));
    preferences.putString("apiurl", server.arg("apiurl"));
    preferences.putString("prompt", server.arg("prompt"));
    
    if (server.hasArg("timeout")) {
      preferences.putInt("timeout", server.arg("timeout").toInt());
    }

    preferences.putString("mqserver", server.arg("mqserver"));
    preferences.putString("mqport", server.arg("mqport"));
    preferences.putString("mquser", server.arg("mquser"));
    preferences.putString("mqpass", server.arg("mqpass"));
    
    preferences.putString("mqpub_h", server.arg("mqpub_hist"));
    preferences.putString("mqsub", server.arg("mqsub"));

    preferences.putString("stream", server.arg("stream"));
    
    preferences.end(); 
    
    // UPDATED: Return 200 OK and simple text for the AJAX handler
    server.send(200, "text/plain", "OK");
    
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleLogs() { server.send(200, "text/plain", getWebLogBuffer()); }
void handleClearLogs() { clearWebLogBuffer(); sysLogLn("--- Logs Cleared ---"); server.send(200, "text/plain", "Cleared"); }

// Sends full history + CURRENT STATE
void handleConversation() {
  String safeJSON = sessionHistoryJSON;
  if (safeJSON.endsWith(",")) safeJSON.remove(safeJSON.length() - 1);
  if (!safeJSON.endsWith("]")) safeJSON += "]";
  String response = "{\"history\":" + safeJSON + ",\"state\":" + String(currentState) + "}";
  server.send(200, "application/json", response);
}

void handleToggle() {
  if (continuousMode) {
    stopContinuousMode();
    server.send(200, "text/plain", "Stopped");
  } else {
    if (currentState == STATE_IDLE) {
      startContinuousMode(true); 
      server.send(200, "text/plain", "Started");
    } else {
      server.send(200, "text/plain", "Busy");
    }
  }
}

// ==========================================
// SETUP & LOGIC
// ==========================================

void setup() {
  Serial.begin(115200);
  delay(2000); 
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  setLedColor(0, 0, 0); 
  sysLogLn("\n\n--- Booting Escher Doorbell ---");

  // Load Settings
  preferences.begin("doorbell", false);
  
  wifi_ssid = preferences.getString("ssid", "EscherHome_IoT");
  wifi_pass = preferences.getString("pass", "1234567890");
  asr_key   = preferences.getString("asrkey", "07fcb4a5-b7b2-45d8-864a-8cc0292380df");
  asr_clust = preferences.getString("asrclus", "volcengine_input_id");
  openai_key= preferences.getString("apikey", "sk-KkEHJ5tO1iiYIqr1jOmrH6FV2uagIICwzL0PDWarGIoHe3Zm");
  openai_url= preferences.getString("apiurl", "https://api.chatanywhere.tech");
  sys_prompt= preferences.getString("prompt", DEFAULT_PROMPT);
  
  asr_max_duration = preferences.getInt("timeout", 50);

  mqtt_server = preferences.getString("mqserver", "");
  mqtt_port   = preferences.getString("mqport", "1883");
  mqtt_user   = preferences.getString("mquser", "");
  mqtt_pass   = preferences.getString("mqpass", "");
  
  mqtt_topic_history = preferences.getString("mqpub_h", "escher/doorbell/history");
  if (mqtt_topic_history.length() == 0) mqtt_topic_history = "escher/doorbell/history";

  mqtt_topic_sub    = preferences.getString("mqsub", "escher/doorbell/in");

  stream_url = preferences.getString("stream", DEFAULT_STREAM_URL);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  sysLog("Connecting to: " + wifi_ssid);
  
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 15) {
    delay(1000); sysLog("."); attempt++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    sysLogLn("\nWiFi Connected! IP: " + WiFi.localIP().toString());
    isAPMode = false;
  } else {
    sysLogLn("\nWiFi Connection Failed.");
    sysLogLn("Starting Access Point Mode...");
    isAPMode = true;
    setLedColor(20, 0, 0); 
  }

  // AP Mode
  if (isAPMode) {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Escher-Doorbell-Setup", "12345678");
    sysLogLn("AP Started. Connect to WiFi: 'Escher-Doorbell-Setup'");
    sysLogLn("Then go to: http://" + WiFi.softAPIP().toString());
  }

  // Web Server
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/clearlogs", HTTP_GET, handleClearLogs);
  server.on("/conversation", HTTP_GET, handleConversation);
  server.on("/toggle", HTTP_GET, handleToggle);
  server.begin();
  sysLogLn("Web Server running.");

  // Initialize AI
  if (!isAPMode) {
    setupMQTT();

    asrChat = new ArduinoASRChat(asr_key.c_str(), asr_clust.c_str());
    gptChat = new ArduinoGPTChat(openai_key.c_str(), openai_url.c_str());

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(100);

    gptChat->setSystemPrompt(sys_prompt.c_str());
    #if ENABLE_CONVERSATION_MEMORY
      gptChat->enableMemory(true);
    #else
      gptChat->enableMemory(false);
    #endif

    if (!asrChat->initINMP441Microphone(I2S_MIC_SERIAL_CLOCK, I2S_MIC_LEFT_RIGHT_CLOCK, I2S_MIC_SERIAL_DATA)) {
      sysLogLn("ERROR: Mic Init Failed!");
      setLedColor(20, 0, 0); 
    } else {
      sysLogLn("Mic Initialized: OK"); 
    }

    asrChat->setAudioParams(SAMPLE_RATE, 16, 1);
    asrChat->setSilenceDuration(1000);
    asrChat->setMaxRecordingSeconds(asr_max_duration);
    
    asrChat->setTimeoutNoSpeechCallback([]() {
      if (continuousMode) stopContinuousMode();
    });

    if (!asrChat->connectWebSocket()) {
      sysLogLn("ERROR: ASR WebSocket Failed! (Will retry on talk)");
      setLedColor(20, 0, 0); 
    } else {
      sysLogLn("ASR Ready.");
    }
    
    sysLogLn("AI Ready. Press BOOT button to talk.");
  }
}

// Logic
void startContinuousMode(bool isInitialTrigger) {
  if (isAPMode) return;
  continuousMode = true;
  
  if (isInitialTrigger) {
    sessionHistoryJSON = "["; 
    
    publishToTopic("escher/doorbell/button", "pressed");
    sysLogLn("[MQTT] Doorbell Event: Pressed");
    
    mqttButtonActive = true;
    mqttButtonPressTime = millis();
  }

  currentState = STATE_LISTENING;
  sysLogLn("\n--- START ---");
  sysLogLn("Listening...");
  publishToTopic("escher/doorbell/status", "Started Listening");
  setLedColor(0, 0, 50); // BLUE
  
  if (asrChat && asrChat->startRecording()) {
    // Success
  } else {
    sysLogLn("WebSocket dropped. Reconnecting...");
    setLedColor(50, 50, 0); 
    
    if (asrChat->connectWebSocket()) {
       sysLogLn("Reconnected. Retrying...");
       delay(200);
       if (asrChat->startRecording()) {
         setLedColor(0, 0, 50); 
         return; 
       }
    }
    
    sysLogLn("Failed. Check Mic (L/R->GND) and WiFi.");
    setLedColor(50, 0, 0); 
    continuousMode = false;
    currentState = STATE_IDLE;
  }
}

void stopContinuousMode() {
  continuousMode = false;
  sysLogLn("\n--- STOP ---");
  
  if (asrChat && asrChat->isRecording()) asrChat->stopRecording();
  
  sessionHistoryJSON += "{\"role\":\"system\",\"text\":\"--- Session Ended ---\"},";

  if (sessionHistoryJSON.endsWith(",")) {
    sessionHistoryJSON.remove(sessionHistoryJSON.length() - 1); 
  }
  sessionHistoryJSON += "]";
  
  sysLogLn("[MQTT] Final History Size: " + String(sessionHistoryJSON.length()) + " bytes");
  sysLogLn("[MQTT] Topic: " + mqtt_topic_history); 
  
  if (publishToTopic(mqtt_topic_history, sessionHistoryJSON)) {
    sysLogLn("[MQTT] History SENT successfully.");
  } else {
    sysLogLn("[MQTT WARN] First send failed. Forcing reconnect and retrying...");
    mqttClient.disconnect();
    delay(500); 
    
    if (reconnectMQTT()) {
        mqttClient.loop(); 
        delay(200);        
        
        if (publishToTopic(mqtt_topic_history, sessionHistoryJSON)) {
            sysLogLn("[MQTT] History SENT on retry.");
        } else {
            sysLogLn("[MQTT ERROR] Failed on retry.");
        }
    } else {
        sysLogLn("[MQTT ERROR] Could not reconnect for retry.");
    }
  }

  delay(250); 
  mqttClient.loop();
  publishToTopic("escher/doorbell/status", "Stopped Listening");

  currentState = STATE_IDLE;
  setLedColor(0, 0, 0); 
}

void handleASRResult() {
  if (!asrChat || !gptChat) return;

  String transcribedText = asrChat->getRecognizedText();
  asrChat->clearResult();

  if (transcribedText.length() > 0) {
    sysLogLn("\n[User]: " + transcribedText);
    lastUserText = transcribedText;
    
    sessionHistoryJSON += "{\"role\":\"guest\",\"text\":\"" + jsonEscape(transcribedText) + "\"},";

    mqttClient.loop(); 
    delay(10); 

    currentState = STATE_PROCESSING_LLM;
    sysLog("[AI]: Thinking...");
    setLedColor(0, 50, 0); 
    
    String response = gptChat->sendMessage(transcribedText);
    
    if (response.length() > 0) {
      sysLogLn(" " + response);
      lastAIText = response;

      sessionHistoryJSON += "{\"role\":\"ai\",\"text\":\"" + jsonEscape(response) + "\"},";

      if (!mqttClient.connected()) {
        reconnectMQTT();
      }
      mqttClient.loop(); 

      currentState = STATE_PLAYING_TTS;
      sysLog("[TTS]: Speaking...");
      
      if (gptChat->textToSpeech(response)) {
        currentState = STATE_WAIT_TTS_COMPLETE;
        ttsStartTime = millis();
        ttsCheckTime = millis();
      } else {
        sysLogLn("Error: TTS Failed");
        setLedColor(50, 0, 0); 
        if (continuousMode) { delay(500); startContinuousMode(false); } 
        else { currentState = STATE_IDLE; setLedColor(0,0,0); }
      }
    } else {
      sysLogLn("Error: LLM Failed");
      setLedColor(50, 0, 0); 
      if (continuousMode) { delay(500); startContinuousMode(false); }
      else { currentState = STATE_IDLE; setLedColor(0,0,0); }
    }
  } else {
    if (continuousMode) { delay(500); startContinuousMode(false); }
    else { currentState = STATE_IDLE; setLedColor(0,0,0); }
  }
}

void loop() {
  server.handleClient();
  if (isAPMode) return;

  audio.loop();
  
  if (mqttButtonActive && (millis() - mqttButtonPressTime > 1000)) {
    publishToTopic("escher/doorbell/button", "released");
    sysLogLn("[MQTT] Doorbell Event: Released (Auto-reset)");
    mqttButtonActive = false;
  }
  
  if (mqtt_server.length() > 0) {
    if (!mqttClient.connected()) {
      unsigned long now = millis();
      if (now - lastMqttReconnectAttempt > 5000) {
        lastMqttReconnectAttempt = now;
        if (reconnectMQTT()) {
          lastMqttReconnectAttempt = 0;
        }
      }
    } else {
      mqttClient.loop();
    }
  }

  if (asrChat) asrChat->loop();

  buttonPressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  if (buttonPressed && !wasButtonPressed) {
    wasButtonPressed = true;
    if (!continuousMode && currentState == STATE_IDLE) {
        startContinuousMode(true); 
    } else if (continuousMode) {
        stopContinuousMode();
    }
  } else if (!buttonPressed && wasButtonPressed) {
    wasButtonPressed = false;
  }

  switch (currentState) {
    case STATE_IDLE: break;
    case STATE_LISTENING:
      if (asrChat->hasNewResult()) handleASRResult();
      break;
    case STATE_PROCESSING_LLM: break;
    case STATE_PLAYING_TTS: break;
    case STATE_WAIT_TTS_COMPLETE:
      if (millis() - ttsCheckTime > 100) {
        ttsCheckTime = millis();
        if (!audio.isRunning()) {
          sysLogLn("[TTS]: Done.");
          if (continuousMode) {
            delay(500);
            startContinuousMode(false); 
          } else {
            currentState = STATE_IDLE;
            setLedColor(0, 0, 0); 
            stopContinuousMode(); 
          }
        }
      }
      break;
  }
  
  if (currentState == STATE_LISTENING) yield();
  else delay(10);
}
