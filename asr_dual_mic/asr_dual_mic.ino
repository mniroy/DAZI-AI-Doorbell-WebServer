#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha1.h>

// ==================== 麦克风类型选择 ====================
// 取消注释你使用的麦克风类型
//#define USE_PDM_MIC        // PDM 麦克风（如 ESP32-S3 板载麦克风）
 #define USE_INMP441     // INMP441 I2S MEMS 麦克风

// WiFi配置
const char* ssid = "xbotpark";
const char* password = "xbotpark@1122";

// API配置
const char* api_key = "07fcb4a5-b7b2-45d8-864a-8cc0292380df";
const char* cluster = "volcengine_input_en";
const char* ws_host = "openspeech.bytedance.com";
const int ws_port = 443;
const char* ws_path = "/api/v2/asr";

// ==================== 麦克风引脚配置 ====================
// PDM 麦克风引脚
#define PDM_CLK 42   // PDM Clock
#define PDM_DATA 41  // PDM Data

// INMP441 I2S 麦克风引脚
#define I2S_SCK 5   // Serial Clock (BCLK)
#define I2S_WS 4    // Word Select (LRCK)
#define I2S_SD 6     // Serial Data (DOUT)

// 设置麦克风类型名称
#ifdef USE_PDM_MIC
  #define MIC_TYPE "PDM"
#endif

#ifdef USE_INMP441
  #define MIC_TYPE "INMP441"
#endif

// 音频参数
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define CHANNELS 1
#define SAMPLES_PER_READ 800  // 每次读取800个样本 = 50ms数据
#define MAX_SECONDS 50
#define SEND_BATCH_SIZE 3200  // 批量发送缓冲区 (200ms音频 = 3200字节)
#define SILENCE_DURATION 1000  // 停顿检测时长（毫秒）

// I2S对象
I2SClass I2S;

// 消息类型
#define CLIENT_FULL_REQUEST 0b0001
#define CLIENT_AUDIO_ONLY_REQUEST 0b0010
#define SERVER_FULL_RESPONSE 0b1001
#define SERVER_ACK 0b1011
#define SERVER_ERROR_RESPONSE 0b1111

// 标志位
#define NO_SEQUENCE 0b0000
#define NEG_SEQUENCE 0b0010

WiFiClientSecure client;

bool ws_connected = false;
bool is_recording = false;
bool should_stop = false;
bool has_speech = false;
String last_result_text = "";
unsigned long recording_start_time = 0;
unsigned long last_speech_time = 0;  // 最后一次检测到语音的时间
int same_result_count = 0;

// 音频缓冲区
int16_t send_buffer[SEND_BATCH_SIZE / 2];  // 批量发送缓冲区
int send_buffer_pos = 0;  // 当前位置（样本数）

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Initialize random seed
  randomSeed(analogRead(0) + millis());

  // Clear startup marker
  Serial.println("\n\n========================================");
  Serial.println("     ESP32 ASR System Starting");
  Serial.println("========================================");
  Serial.print("Microphone Type: ");
  Serial.println(MIC_TYPE);
  Serial.print("Boot time: ");
  Serial.println(millis());
  Serial.println();

  // Connect WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting WiFi");
  for(int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    Serial.print(".");
    delay(500);
  }

  if(WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connection failed!");
    return;
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Free Heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  // Initialize microphone based on type
  #ifdef USE_PDM_MIC
    initPDMMicrophone();
  #endif

  #ifdef USE_INMP441
    initINMP441Microphone();
  #endif

  Serial.print(MIC_TYPE);
  Serial.println(" microphone initialized");

  // Wait for hardware to stabilize and clear buffer
  delay(500);
  for (int i = 0; i < 2000; i++) {
    I2S.read();
  }

  // Connect WebSocket
  connectWebSocket();
}

void initPDMMicrophone() {
  I2S.setPinsPdmRx(PDM_CLK, PDM_DATA);

  if (!I2S.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("PDM I2S initialization failed!");
    while (1);
  }
}

void initINMP441Microphone() {
  I2S.setPins(I2S_SCK, I2S_WS, -1, I2S_SD);

  if (!I2S.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("INMP441 I2S initialization failed!");
    while (1);
  }
}

void loop() {
  if (ws_connected) {
    // Check connection status
    if (!client.connected()) {
      Serial.println("Connection lost");
      ws_connected = false;
      is_recording = false;
      return;
    }

    // Process audio sending during recording
    if (is_recording && !should_stop) {
      // Check max duration
      if (millis() - recording_start_time > MAX_SECONDS * 1000) {
        // Send remaining buffer
        if (send_buffer_pos > 0) {
          sendAudioChunk((uint8_t*)send_buffer, send_buffer_pos * 2);
          send_buffer_pos = 0;
        }
        Serial.println("\nMax duration reached, stopping");
        stopRecording();
        return;
      }

      // Check silence - if speech detected and exceeded silence duration
      if (has_speech && last_speech_time > 0) {
        unsigned long silence = millis() - last_speech_time;
        if (silence >= SILENCE_DURATION) {
          // Send remaining buffer
          if (send_buffer_pos > 0) {
            sendAudioChunk((uint8_t*)send_buffer, send_buffer_pos * 2);
            send_buffer_pos = 0;
          }
          Serial.printf("\nSilence detected (%.1fs), stopping\n", silence / 1000.0);
          stopRecording();
          return;
        }
      }

      // Print progress dot every second
      static unsigned long last_dot = 0;
      if (millis() - last_dot > 1000) {
        Serial.print(".");
        last_dot = millis();
      }

      // Read audio samples and accumulate to send buffer
      for (int i = 0; i < SAMPLES_PER_READ; i++) {
        int sample = I2S.read();
        // Filter invalid data
        if (sample != 0 && sample != -1 && sample != 1) {
          send_buffer[send_buffer_pos++] = (int16_t)sample;

          // Buffer full, send batch
          if (send_buffer_pos >= SEND_BATCH_SIZE / 2) {
            sendAudioChunk((uint8_t*)send_buffer, send_buffer_pos * 2);
            send_buffer_pos = 0;
          }
        }
      }

      yield();
    }

    // Process received data - also during recording for silence detection
    if (client.available()) {
      // Fast processing during recording to avoid blocking audio capture
      if (is_recording) {
        // Process only one message to avoid blocking too long
        handleWebSocketData();
      } else {
        // Process all pending responses after recording
        while (client.available()) {
          handleWebSocketData();
          delay(10);
        }
      }
    }
  }
}

void connectWebSocket() {
  Serial.println("Connecting WebSocket...");

  client.setInsecure();

  if (!client.connect(ws_host, ws_port)) {
    Serial.println("SSL connection failed");
    return;
  }

  // Disable Nagle algorithm for immediate send
  client.setNoDelay(true);

  // Generate WebSocket Key and send handshake request
  String ws_key = generateWebSocketKey();
  String request = String("GET ") + ws_path + " HTTP/1.1\r\n";
  request += String("Host: ") + ws_host + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: " + ws_key + "\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += String("x-api-key: ") + api_key + "\r\n";
  request += "\r\n";

  client.print(request);

  // Read response
  unsigned long timeout = millis();
  while (client.connected() && !client.available()) {
    if (millis() - timeout > 5000) {
      Serial.println("Response timeout");
      client.stop();
      return;
    }
    delay(10);
  }

  String response = "";
  bool headers_complete = false;
  while (client.available() && !headers_complete) {
    String line = client.readStringUntil('\n');
    response += line + "\n";
    if (line == "\r" || line.length() == 0) {
      headers_complete = true;
    }
  }

  // Check response
  if (response.indexOf("101") >= 0 && response.indexOf("Switching Protocols") >= 0) {
    Serial.println("WebSocket connected");
    ws_connected = true;

    delay(100);
    sendFullRequest();

    // Wait for config confirmation
    timeout = millis();
    while (millis() - timeout < 2000) {
      if (client.available()) {
        handleWebSocketData();
        break;
      }
      delay(10);
    }

    startRecording();
    Serial.println("\n========================================");
    Serial.println("System ready, recording...");
    Serial.println("========================================");
  } else {
    Serial.println("WebSocket handshake failed");
    Serial.println(response);
    client.stop();
  }
}

String generateWebSocketKey() {
  uint8_t random_bytes[16];
  for (int i = 0; i < 16; i++) {
    random_bytes[i] = random(0, 256);
  }

  size_t output_len;
  unsigned char output[32];
  mbedtls_base64_encode(output, sizeof(output), &output_len, random_bytes, 16);

  return String((char*)output);
}

void handleWebSocketData() {
  // 读取WebSocket帧
  uint8_t header[2];
  if (client.readBytes(header, 2) != 2) {
    return;
  }

  bool fin = header[0] & 0x80;
  uint8_t opcode = header[0] & 0x0F;
  bool masked = header[1] & 0x80;
  uint64_t payload_len = header[1] & 0x7F;

  // 处理扩展长度
  if (payload_len == 126) {
    uint8_t len_bytes[2];
    client.readBytes(len_bytes, 2);
    payload_len = (len_bytes[0] << 8) | len_bytes[1];
  } else if (payload_len == 127) {
    uint8_t len_bytes[8];
    client.readBytes(len_bytes, 8);
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | len_bytes[i];
    }
  }

  // 读取mask key
  uint8_t mask_key[4] = {0};
  if (masked) {
    client.readBytes(mask_key, 4);
  }

  // 读取payload
  if (payload_len > 0 && payload_len < 100000) {
    uint8_t* payload = new uint8_t[payload_len];
    size_t bytes_read = client.readBytes(payload, payload_len);

    if (bytes_read == payload_len) {
      // 解mask
      if (masked) {
        for (size_t i = 0; i < payload_len; i++) {
          payload[i] ^= mask_key[i % 4];
        }
      }

      // 处理不同的opcode
      if (opcode == 0x01 || opcode == 0x02) {
        parseResponse(payload, payload_len);
      } else if (opcode == 0x08) {
        Serial.println("Server closed connection");
        ws_connected = false;
        client.stop();
      } else if (opcode == 0x09) {
        sendPong();
      }
    }

    delete[] payload;
  }
}

void sendWebSocketFrame(uint8_t* data, size_t len, uint8_t opcode) {
  if (!ws_connected || !client.connected()) return;

  // 构建WebSocket帧头
  uint8_t header[10];
  int header_len = 2;

  header[0] = 0x80 | opcode;
  header[1] = 0x80;

  // 长度
  if (len < 126) {
    header[1] |= len;
  } else if (len < 65536) {
    header[1] |= 126;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;
    header_len = 4;
  } else {
    header[1] |= 127;
    for (int i = 0; i < 8; i++) {
      header[2 + i] = (len >> (56 - i * 8)) & 0xFF;
    }
    header_len = 10;
  }

  // 生成mask key
  uint8_t mask_key[4];
  for (int i = 0; i < 4; i++) {
    mask_key[i] = random(0, 256);
  }
  memcpy(header + header_len, mask_key, 4);
  header_len += 4;

  // 发送帧头
  client.write(header, header_len);

  // Mask数据并发送
  for (size_t i = 0; i < len; i++) {
    data[i] ^= mask_key[i % 4];
  }
  client.write(data, len);
}

void sendFullRequest() {
  // Generate unique session ID using timestamp + random
  // This ensures each recording session is unique
  String reqid = String(millis()) + "_" + String(random(10000, 99999));

  // Use MAC address as stable user ID (device identity)
  String uid = String(ESP.getEfuseMac(), HEX);

  StaticJsonDocument<512> doc;
  doc["app"]["cluster"] = cluster;
  doc["user"]["uid"] = uid;
  doc["request"]["reqid"] = reqid;
  doc["request"]["nbest"] = 1;
  doc["request"]["workflow"] = "audio_in,resample,partition,vad,fe,decode,itn,nlu_punctuate";
  doc["request"]["result_type"] = "full";
  doc["request"]["sequence"] = 1;
  doc["audio"]["format"] = "raw";
  doc["audio"]["rate"] = SAMPLE_RATE;
  doc["audio"]["bits"] = BITS_PER_SAMPLE;
  doc["audio"]["channel"] = CHANNELS;
  doc["audio"]["codec"] = "raw";

  String json_str;
  serializeJson(doc, json_str);

  Serial.print("Request ID: ");
  Serial.println(reqid);
  Serial.println("Sending config:");
  Serial.println(json_str);

  // 协议头
  uint8_t header[4] = {0x11, (CLIENT_FULL_REQUEST << 4) | NO_SEQUENCE, 0x10, 0x00};
  uint32_t payload_len = json_str.length();
  uint8_t len_bytes[4];
  len_bytes[0] = (payload_len >> 24) & 0xFF;
  len_bytes[1] = (payload_len >> 16) & 0xFF;
  len_bytes[2] = (payload_len >> 8) & 0xFF;
  len_bytes[3] = payload_len & 0xFF;

  uint8_t* full_request = new uint8_t[8 + payload_len];
  memcpy(full_request, header, 4);
  memcpy(full_request + 4, len_bytes, 4);
  memcpy(full_request + 8, json_str.c_str(), payload_len);

  sendWebSocketFrame(full_request, 8 + payload_len, 0x02);
  delete[] full_request;
}

void sendAudioChunk(uint8_t* data, size_t len) {
  // 协议头
  uint8_t header[4] = {0x11, (CLIENT_AUDIO_ONLY_REQUEST << 4) | NO_SEQUENCE, 0x10, 0x00};
  uint8_t len_bytes[4];
  len_bytes[0] = (len >> 24) & 0xFF;
  len_bytes[1] = (len >> 16) & 0xFF;
  len_bytes[2] = (len >> 8) & 0xFF;
  len_bytes[3] = len & 0xFF;

  uint8_t* audio_request = new uint8_t[8 + len];
  memcpy(audio_request, header, 4);
  memcpy(audio_request + 4, len_bytes, 4);
  memcpy(audio_request + 8, data, len);

  sendWebSocketFrame(audio_request, 8 + len, 0x02);
  delete[] audio_request;
}

void sendEndMarker() {
  uint8_t header[4] = {0x11, (CLIENT_AUDIO_ONLY_REQUEST << 4) | NEG_SEQUENCE, 0x10, 0x00};
  uint8_t len_bytes[4] = {0x00, 0x00, 0x00, 0x00};
  uint8_t end_request[8];
  memcpy(end_request, header, 4);
  memcpy(end_request + 4, len_bytes, 4);

  sendWebSocketFrame(end_request, 8, 0x02);
  Serial.println("End marker sent");
}

void sendPong() {
  uint8_t pong_data[1] = {0};
  sendWebSocketFrame(pong_data, 0, 0x0A);
}

void startRecording() {
  is_recording = true;
  should_stop = false;
  has_speech = false;
  last_result_text = "";
  last_speech_time = 0;
  recording_start_time = millis();
  send_buffer_pos = 0;
  same_result_count = 0;
}

void stopRecording() {
  if (should_stop) {
    return;
  }

  Serial.println("\n========================================");
  Serial.println("Recording stopped");
  Serial.print("Final result: ");
  Serial.println(last_result_text);
  Serial.println("========================================\n");

  is_recording = false;
  should_stop = true;
  sendEndMarker();
}

void parseResponse(uint8_t* data, size_t len) {
  if (len < 4) return;

  uint8_t msg_type = data[1] >> 4;
  uint8_t header_size = data[0] & 0x0f;

  if (len < header_size * 4) return;

  uint8_t* payload = data + header_size * 4;
  size_t payload_len = len - header_size * 4;

  if (msg_type == SERVER_FULL_RESPONSE && payload_len > 4) {
    payload += 4;
    payload_len -= 4;
  } else if (msg_type == SERVER_ACK && payload_len >= 8) {
    payload += 8;
    payload_len -= 8;
  } else if (msg_type == SERVER_ERROR_RESPONSE && payload_len >= 8) {
    payload += 8;
    payload_len -= 8;
  }

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, payload, payload_len);

  if (error) {
    return;
  }

  if (doc.containsKey("code")) {
    int code = doc["code"];
    if (code != 1000 && code != 1013) {
      // Ignore 1000 (success) and 1013 (silence detection)
      Serial.print("\nError: ");
      serializeJson(doc, Serial);
      Serial.println();
    }
  }

  if (doc.containsKey("result")) {
    JsonVariant result = doc["result"];
    String current_text = "";

    if (result.is<JsonArray>() && result.size() > 0) {
      if (result[0].containsKey("text")) {
        current_text = result[0]["text"].as<String>();
      }
    }

    if (current_text.length() > 0 && current_text != " ") {
      if (!has_speech) {
        has_speech = true;
        Serial.println("\nSpeech detected...");
      }

      // Update last speech time
      last_speech_time = millis();

      if (current_text == last_result_text) {
        same_result_count++;
        if (same_result_count <= 3) {
          Serial.printf("Recognizing: %s\n", current_text.c_str());
        } else if (same_result_count == 4) {
          Serial.printf("Result stable: %s\n", current_text.c_str());
        }

        // Only trigger stop if still recording
        if (same_result_count >= 10 && is_recording && !should_stop) {
          Serial.println("\nResult stable, stopping recording");
          // Send remaining buffer
          if (send_buffer_pos > 0) {
            sendAudioChunk((uint8_t*)send_buffer, send_buffer_pos * 2);
            send_buffer_pos = 0;
          }
          stopRecording();
        }
      } else {
        same_result_count = 1;
        last_result_text = current_text;
        Serial.printf("Recognizing: %s\n", current_text.c_str());
      }
    }
  }
}
