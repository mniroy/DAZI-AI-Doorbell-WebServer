#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoASRChat.h>
#include <ArduinoGPTChat.h>
#include <PubSubClient.h>
#include "Audio.h"

// ==========================================
// CONFIGURATION & GLOBALS
// ==========================================

// Enable conversation memory
#define ENABLE_CONVERSATION_MEMORY 1

// Hardware Pin Definitions
#define I2S_DOUT 47
#define I2S_BCLK 46
#define I2S_LRC 45

#define I2S_MIC_SERIAL_CLOCK 5      // SCK
#define I2S_MIC_LEFT_RIGHT_CLOCK 4 // WS
#define I2S_MIC_SERIAL_DATA 6      // SD

#define BOOT_BUTTON_PIN 0
#define SAMPLE_RATE 16000

// RGB LED (NeoPixel) - GPIO 48 is standard for ESP32-S3 DevKit
#define RGB_LED_PIN 48 

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
String asr_key, asr_model; // Renamed from asr_clust for clarity, though keeping storage key same if needed
String openai_key, openai_url, sys_prompt;
String stream_url;
int asr_max_duration; 

// MQTT Variables
String mqtt_server, mqtt_port, mqtt_user, mqtt_pass;
String mqtt_topic_history; 
String mqtt_topic_sub;    

// Web Logging Buffer & Chat State
String webLogBuffer = "";
String lastUserText = "(Waiting for guest...)";
String lastAIText = "(Waiting for AI...)";
String sessionHistoryJSON = "[]"; 

// Button Pulse State
unsigned long mqttButtonPressTime = 0;
bool mqttButtonActive = false;

// Default System Prompt
const char* default_prompt = 
"Kamu adalah asisten doorbell pintar di Rumah Escher, rumah keluarga Indonesia. "
"Perkenalkan diri: 'Halo, saya AI Rumah Escher, asisten rumah ini. Saya bantu jawab bel ya.' "
"Fokusmu: ajak ngobrol orang yang menekan bel, cari tahu mereka siapa dan apa keperluannya. "
"Selalu klasifikasikan pengunjung sebagai: kurir paket, kurir galon aqua, satpam, keluarga, atau tamu lain. "
"Gunakan Bahasa Indonesia santai tapi sopan. Jawaban pendek: 1–2 kalimat, maksimal sekitar 25 kata.";

// Default Stream URL
const char* default_stream = "http://192.168.1.34:1984/stream.html?src=Doorbell";

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
// HELPERS
// ==========================================

void sysLog(String msg) {
  webLogBuffer += msg; 
  if (webLogBuffer.length() > 4000) {
    webLogBuffer = webLogBuffer.substring(webLogBuffer.length() - 2000);
  }
}

void sysLogLn(String msg) {
  sysLog(msg + "\n");
}

String jsonEscape(String s) {
  s.replace("\"", "\\\"");
  s.replace("\n", " ");
  s.replace("\r", "");
  return s;
}

void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  #ifdef RGB_BUILTIN
    neopixelWrite(RGB_BUILTIN, r, g, b);
  #else
    neopixelWrite(RGB_LED_PIN, r, g, b);
  #endif
}

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
    mqttClient.setBufferSize(8192); 
    sysLogLn("MQTT Configured: " + mqtt_server + ":" + String(port));
  }
}

boolean reconnectMQTT() {
  if (mqtt_server.length() == 0) return false;
   
  if (WiFi.status() != WL_CONNECTED) {
    sysLogLn("[WiFi] Connection lost. Reconnecting WiFi...");
    WiFi.reconnect();
    int w = 0;
    while (WiFi.status() != WL_CONNECTED && w < 10) { delay(500); w++; }
  }

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
  if (!mqttClient.connected()) {
    sysLogLn("[MQTT] Connecting...");
    if (!reconnectMQTT()) {
       sysLogLn("[MQTT ERROR] Reconnect failed.");
       return false;
    }
  }
  mqttClient.loop();
  if (mqttClient.publish(topic.c_str(), message.c_str())) {
    return true;
  } else {
    sysLogLn("[MQTT ERROR] Publish failed. Len: " + String(message.length()));
    return false;
  }
}

// ==========================================
// HTML PAGE TEMPLATE
// ==========================================
const char* html_template = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Escher Doorbell AI</title>
<style>
  :root {
    --bg-body: #f2f2f2; --bg-card: #ffffff; --text-main: #333333; --text-label: #555555;
    --input-bg: #ffffff; --input-border: #ddd; --heading-color: #007bff; --divider-color: #eee;
    --chat-guest-bg: #007bff; --chat-guest-text: #ffffff;
    --chat-ai-bg: #f1f0f0; --chat-ai-text: #333333;
    --chat-sys-bg: #e9ecef; --chat-sys-text: #6c757d;
  }
  body.dark-mode {
    --bg-body: #121212; --bg-card: #1e1e1e; --text-main: #e0e0e0; --text-label: #bbbbbb;
    --input-bg: #2d2d2d; --input-border: #444; --heading-color: #4dabf7; --divider-color: #333;
    --chat-guest-bg: #0d6efd; --chat-guest-text: #ffffff;
    --chat-ai-bg: #333333; --chat-ai-text: #e0e0e0;
    --chat-sys-bg: #343a40; --chat-sys-text: #adb5bd;
  }
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: var(--bg-body); color: var(--text-main); padding: 20px; margin: 0; transition: background 0.3s, color 0.3s; }
  .container { display: flex; flex-wrap: wrap; gap: 20px; max-width: 1000px; margin: 0 auto; }
  .header-bar { width: 100%; display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px; }
  h1.main-title { margin: 0; color: var(--heading-color); font-size: 24px; }
  .theme-toggle-btn { background: var(--bg-card); border: 1px solid var(--input-border); color: var(--text-main); padding: 8px 15px; border-radius: 20px; cursor: pointer; font-size: 14px; font-weight: bold; display: flex; align-items: center; gap: 5px; }
  .card { background: var(--bg-card); flex: 1; min-width: 300px; padding: 20px; border-radius: 10px; box-shadow: 0 4px 6px rgba(0,0,0,0.1); display: flex; flex-direction: column; transition: background 0.3s; }
  h2 { color: var(--heading-color); margin-top: 0; margin-bottom: 20px; border-bottom: 2px solid var(--divider-color); padding-bottom: 10px; }
  label { font-weight: bold; display: block; margin-top: 15px; font-size: 13px; color: var(--text-label); }
  input[type=text], input[type=password], input[type=number], textarea { width: 100%; padding: 10px; margin-top: 5px; border: 1px solid var(--input-border); background: var(--input-bg); color: var(--text-main); border-radius: 5px; box-sizing: border-box; font-size: 14px; }
  input:focus, textarea:focus { border-color: var(--heading-color); outline: none; }
  textarea.config { height: 150px; font-family: 'Courier New', monospace; font-size: 12px; line-height: 1.4; }
  button.save-btn { width: 100%; background-color: var(--heading-color); color: white; padding: 12px; border: none; border-radius: 5px; cursor: pointer; margin-top: 20px; font-size: 16px; font-weight: bold; }
   
  .chat-container { height: 350px; overflow-y: auto; background: var(--bg-card); border: 1px solid var(--input-border); border-radius: 8px; padding: 15px; display: flex; flex-direction: column; gap: 12px; margin-bottom: 15px; }
  .msg { display: flex; width: 100%; }
  .msg.guest { justify-content: flex-end; }
  .msg.ai { justify-content: flex-start; }
  .msg.system { justify-content: center; margin: 5px 0; }
  
  .bubble { max-width: 75%; padding: 10px 15px; border-radius: 18px; font-size: 15px; line-height: 1.4; position: relative; word-wrap: break-word; }
  .msg.guest .bubble { background-color: var(--chat-guest-bg); color: var(--chat-guest-text); border-bottom-right-radius: 4px; }
  .msg.ai .bubble { background-color: var(--chat-ai-bg); color: var(--chat-ai-text); border-bottom-left-radius: 4px; }
  .msg.system .bubble { background-color: var(--chat-sys-bg); color: var(--chat-sys-text); border-radius: 20px; font-size: 12px; padding: 5px 15px; font-weight: bold; max-width: 90%; }
  .chat-label-small { font-size: 10px; margin-bottom: 2px; opacity: 0.6; padding: 0 5px; }
  .msg.guest .chat-label-small { text-align: right; }
  
  /* Animations */
  .listening-wave { display: inline-block; position: relative; width: 40px; height: 15px; display: flex; align-items: center; justify-content: space-between; }
  .listening-wave div { background-color: #fff; width: 6px; height: 100%; animation: wave 1.2s infinite ease-in-out; }
  .listening-wave div:nth-child(1) { animation-delay: -1.2s; }
  .listening-wave div:nth-child(2) { animation-delay: -1.1s; }
  .listening-wave div:nth-child(3) { animation-delay: -1.0s; }
  @keyframes wave { 0%, 40%, 100% { transform: scaleY(0.4); } 20% { transform: scaleY(1.0); } }

  .typing-dots { display: inline-block; width: 40px; }
  .typing-dots div { display: inline-block; width: 6px; height: 6px; border-radius: 50%; background-color: var(--text-main); animation: bounce 1.4s infinite ease-in-out both; margin: 0 2px; }
  .typing-dots div:nth-child(1) { animation-delay: -0.32s; }
  .typing-dots div:nth-child(2) { animation-delay: -0.16s; }
  @keyframes bounce { 0%, 80%, 100% { transform: scale(0); } 40% { transform: scale(1.0); } }

  .speaking-bars { display: flex; align-items: center; height: 15px; }
  .speaking-bars div { background-color: var(--text-main); width: 4px; height: 100%; margin: 0 2px; animation: speak 0.8s infinite ease-in-out; }
  .speaking-bars div:nth-child(1) { animation-delay: 0.1s; }
  .speaking-bars div:nth-child(2) { animation-delay: 0.2s; }
  .speaking-bars div:nth-child(3) { animation-delay: 0.3s; }
  @keyframes speak { 0%, 100% { height: 4px; } 50% { height: 15px; } }

  #terminal { background-color: #1e1e1e; color: #00ff00; font-family: 'Courier New', monospace; padding: 15px; height: 150px; overflow-y: auto; border-radius: 5px; font-size: 11px; white-space: pre-wrap; line-height: 1.3; border: 1px solid var(--input-border); }
  .clear-btn { background: #6c757d; color: white; border: none; padding: 6px 15px; border-radius: 4px; cursor: pointer; font-size: 12px; margin-top: 10px; }
  .toggle-btn { background: #28a745; color: white; border: none; padding: 6px 15px; border-radius: 20px; cursor: pointer; font-size: 13px; font-weight: bold; }
  .status-badge { display:inline-block; padding: 6px 12px; border-radius: 20px; background: #e9ecef; color: #495057; font-weight: bold; font-size: 12px; border: 1px solid #ced4da; }
  .status-badge.live { background: #d4edda; color: #155724; border-color: #c3e6cb; }
  .status-badge.live::before { content: "● "; color: #28a745; }
  .stream-box { width: 100%; height: 0; padding-bottom: 56.25%; position: relative; background: #000; border-radius: 8px; margin-bottom: 15px; overflow: hidden; }
  .stream-box iframe { position: absolute; top:0; left: 0; width: 100%; height: 100%; border: none; }
</style>
<script>
  function toggleTheme() {
    document.body.classList.toggle('dark-mode');
    const isDark = document.body.classList.contains('dark-mode');
    document.getElementById('theme-icon').innerText = isDark ? '☀️' : '🌙';
    document.getElementById('theme-text').innerText = isDark ? 'Light Mode' : 'Dark Mode';
  }

  let lastChatHTML = "";
  let lastState = -1;

  setInterval(function() {
    fetch('/logs').then(r => r.text()).then(data => {
      var term = document.getElementById("terminal");
      if(term.innerHTML !== data) { term.innerHTML = data; term.scrollTop = term.scrollHeight; }
    });
    
    fetch('/conversation').then(r => r.json()).then(data => {
      const chatBox = document.getElementById("chat-container");
      let html = "";
      let history = data.history || [];
      let state = data.state || 0; 
      
      if (history.length === 0 && state === 0) {
        html = '<div style="text-align:center; color:gray; font-size:13px; padding-top:20px;">No conversation yet.<br>Press "Toggle Talk" to start.</div>';
      } else {
          history.forEach(msg => {
            if(msg.role === 'guest') {
                 html += `<div class="msg guest"><div><div class="chat-label-small">Guest</div><div class="bubble">${msg.text}</div></div></div>`;
            } else if(msg.role === 'ai') {
                 html += `<div class="msg ai"><div><div class="chat-label-small">AI</div><div class="bubble">${msg.text}</div></div></div>`;
            } else if(msg.role === 'system') {
                 html += `<div class="msg system"><div class="bubble">${msg.text}</div></div>`;
            }
          });
      }
      
      if (state === 1) html += `<div class="msg guest"><div><div class="chat-label-small">Listening...</div><div class="bubble"><div class="listening-wave"><div></div><div></div><div></div></div></div></div></div>`;
      else if (state === 2) html += `<div class="msg ai"><div><div class="chat-label-small">Thinking...</div><div class="bubble"><div class="typing-dots"><div></div><div></div><div></div></div></div></div></div>`;
      else if (state === 3 || state === 4) html += `<div class="msg ai"><div><div class="chat-label-small">Speaking...</div><div class="bubble"><div class="speaking-bars"><div></div><div></div><div></div></div></div></div></div>`;
      
      if(lastChatHTML !== html || lastState !== state) {
          chatBox.innerHTML = html;
          chatBox.scrollTop = chatBox.scrollHeight;
          lastChatHTML = html;
          lastState = state;
      }
    });
  }, 1000);
</script>
</head>
<body>
<div class="container">
  <div class="header-bar">
    <h1 class="main-title">Escher Doorbell Dashboard</h1>
    <button class="theme-toggle-btn" onclick="toggleTheme()">
      <span id="theme-icon">🌙</span> <span id="theme-text">Dark Mode</span>
    </button>
  </div>

  <div class="card">
    <h2>Configuration</h2>
    <form action="/save" method="POST">
      <label>WiFi SSID</label><input type="text" name="ssid" id="ssid">
      <label>WiFi Password</label><input type="password" name="pass" id="pass">
      
      <div style="margin: 20px 0; border-top: 1px solid var(--divider-color);"></div>
      <h3>AI Settings</h3>
      <label>ElevenLabs API Key</label><input type="text" name="asrkey" id="asrkey">
      <label>ElevenLabs Model ID</label><input type="text" name="asrclus" id="asrclus" placeholder="scribe_v2">
      <label>OpenAI Key</label><input type="password" name="apikey" id="apikey">
      <label>Base URL</label><input type="text" name="apiurl" id="apiurl">
      <label>System Prompt</label><textarea class="config" name="prompt" id="prompt"></textarea>
      
      <label>No Speech Timeout (sec)</label><input type="number" name="timeout" id="timeout">

      <div style="margin: 20px 0; border-top: 1px solid var(--divider-color);"></div>
      <h3>IoT Settings</h3>
      <label>Broker IP/URL</label><input type="text" name="mqserver" id="mqserver">
      <label>Port (Default 1883)</label><input type="text" name="mqport" id="mqport">
      <label>Username (Optional)</label><input type="text" name="mquser" id="mquser">
      <label>Password (Optional)</label><input type="password" name="mqpass" id="mqpass">
      
      <label>History Topic (JSON)</label><input type="text" name="mqpub_hist" id="mqpub_hist">
      <label>Subscribe Topic</label><input type="text" name="mqsub" id="mqsub">
      
      <label style="color:#28a745;">Camera Stream URL</label>
      <input type="text" name="stream" id="stream">
      
      <button class="save-btn" type="submit">Save & Restart Device</button>
    </form>
  </div>

  <div class="card">
    <h2>Live Interaction</h2>
    <div class="stream-box"><iframe src="STREAM_URL_PLACEHOLDER" allowfullscreen></iframe></div>

    <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:15px;">
      <div class="status-badge live">System Active</div>
      <button class="toggle-btn" onclick="fetch('/toggle')">🎤 Toggle Talk</button>
    </div>

    <div id="chat-container" class="chat-container"></div>

    <hr style="margin: 20px 0; border: 0; border-top: 1px solid var(--divider-color);">
    <label style="margin-top:0;">System Log</label>
    <div id="terminal">Loading logs...</div>
    <button class="clear-btn" onclick="fetch('/clearlogs')">Clear Logs</button>
  </div>
</div>
</body></html>
)rawliteral";

void handleRoot() {
  String html = html_template;
  html.replace("id=\"ssid\">", "id=\"ssid\" value=\"" + wifi_ssid + "\">");
  html.replace("id=\"pass\">", "id=\"pass\" value=\"" + wifi_pass + "\">"); 
  html.replace("id=\"asrkey\">", "id=\"asrkey\" value=\"" + asr_key + "\">");
  html.replace("id=\"asrclus\">", "id=\"asrclus\" value=\"" + asr_model + "\">");
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
    
    if (server.hasArg("timeout")) preferences.putInt("timeout", server.arg("timeout").toInt());

    preferences.putString("mqserver", server.arg("mqserver"));
    preferences.putString("mqport", server.arg("mqport"));
    preferences.putString("mquser", server.arg("mquser"));
    preferences.putString("mqpass", server.arg("mqpass"));
    
    preferences.putString("mqpub_h", server.arg("mqpub_hist"));
    preferences.putString("mqsub", server.arg("mqsub"));
    preferences.putString("stream", server.arg("stream"));
    
    preferences.end(); 
    
    server.send(200, "text/html", "<body style='background:#121212;color:white;text-align:center;padding:50px;font-family:sans-serif;'><h1>Saved!</h1><p>Restarting...</p><a href='/'>Go Back</a></body>");
    delay(1000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleLogs() { server.send(200, "text/plain", webLogBuffer); }
void handleClearLogs() { webLogBuffer = ""; sysLogLn("--- Logs Cleared ---"); server.send(200, "text/plain", "Cleared"); }

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

  preferences.begin("doorbell", false);
  
  wifi_ssid = preferences.getString("ssid", "EscherHome_IoT");
  wifi_pass = preferences.getString("pass", "1234567890");
  asr_key   = preferences.getString("asrkey", "YOUR_ELEVENLABS_KEY");
  asr_model = preferences.getString("asrclus", "scribe_v2");
  openai_key= preferences.getString("apikey", "sk-KkEHJ5tO1iiYIqr1jOmrH6FV2uagIICwzL0PDWarGIoHe3Zm");
  openai_url= preferences.getString("apiurl", "https://api.chatanywhere.tech");
  sys_prompt= preferences.getString("prompt", default_prompt);
  asr_max_duration = preferences.getInt("timeout", 50);

  mqtt_server = preferences.getString("mqserver", "");
  mqtt_port   = preferences.getString("mqport", "1883");
  mqtt_user   = preferences.getString("mquser", "");
  mqtt_pass   = preferences.getString("mqpass", "");
  
  mqtt_topic_history = preferences.getString("mqpub_h", "escher/doorbell/history");
  if (mqtt_topic_history.length() == 0) mqtt_topic_history = "escher/doorbell/history";

  mqtt_topic_sub    = preferences.getString("mqsub", "escher/doorbell/in");
  stream_url = preferences.getString("stream", default_stream);

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

  if (isAPMode) {
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Escher-Doorbell-Setup", "12345678");
    sysLogLn("AP Started. Connect to WiFi: 'Escher-Doorbell-Setup'");
    sysLogLn("Then go to: http://" + WiFi.softAPIP().toString());
  }

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/clearlogs", HTTP_GET, handleClearLogs);
  server.on("/conversation", HTTP_GET, handleConversation);
  server.on("/toggle", HTTP_GET, handleToggle);
  server.begin();
  sysLogLn("Web Server running.");

  if (!isAPMode) {
    setupMQTT();

    // Init ElevenLabs ASR with Model ID
    asrChat = new ArduinoASRChat(asr_key.c_str(), asr_model.c_str());
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
  setLedColor(0, 0, 50); 
  
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
