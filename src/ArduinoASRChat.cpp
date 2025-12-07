#include "ArduinoASRChat.h"

ArduinoASRChat::ArduinoASRChat(const char* apiKey, const char* modelId) {
  _apiKey = apiKey;
  _modelId = modelId;

  // Allocate send buffer
  _sendBuffer = new int16_t[_sendBatchSize / 2];
}

void ArduinoASRChat::setApiConfig(const char* apiKey, const char* modelId) {
  if (apiKey != nullptr) {
    _apiKey = apiKey;
  }
  if (modelId != nullptr) {
    _modelId = modelId;
  }
}

void ArduinoASRChat::setMicrophoneType(MicrophoneType micType) {
  _micType = micType;
}

void ArduinoASRChat::setAudioParams(int sampleRate, int bitsPerSample, int channels) {
  _sampleRate = sampleRate;
  _bitsPerSample = bitsPerSample;
  _channels = channels;
}

void ArduinoASRChat::setSilenceDuration(unsigned long duration) {
  _silenceDuration = duration;
}

void ArduinoASRChat::setMaxRecordingSeconds(int seconds) {
  _maxSeconds = seconds;
}

bool ArduinoASRChat::initPDMMicrophone(int pdmClkPin, int pdmDataPin) {
  _micType = MIC_TYPE_PDM;
  _I2S.setPinsPdmRx(pdmClkPin, pdmDataPin);

  if (!_I2S.begin(I2S_MODE_PDM_RX, _sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("PDM I2S initialization failed!");
    return false;
  }

  Serial.println("PDM microphone initialized");

  // Wait for hardware to stabilize and clear buffer
  delay(500);
  for (int i = 0; i < 2000; i++) {
    _I2S.read();
  }

  return true;
}

bool ArduinoASRChat::initINMP441Microphone(int i2sSckPin, int i2sWsPin, int i2sSdPin) {
  _micType = MIC_TYPE_INMP441;
  _I2S.setPins(i2sSckPin, i2sWsPin, -1, i2sSdPin);

  if (!_I2S.begin(I2S_MODE_STD, _sampleRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT)) {
    Serial.println("INMP441 I2S initialization failed!");
    return false;
  }

  Serial.println("INMP441 microphone initialized");

  // Wait for hardware to stabilize and clear buffer
  delay(500);
  for (int i = 0; i < 2000; i++) {
    _I2S.read();
  }

  return true;
}

String ArduinoASRChat::generateWebSocketKey() {
  uint8_t random_bytes[16];
  for (int i = 0; i < 16; i++) {
    random_bytes[i] = random(0, 256);
  }

  size_t output_len;
  unsigned char output[32];
  mbedtls_base64_encode(output, sizeof(output), &output_len, random_bytes, 16);

  return String((char*)output);
}

bool ArduinoASRChat::connectWebSocket() {
  Serial.println("Connecting to ElevenLabs WebSocket...");

  _client.setInsecure();

  if (!_client.connect(_wsHost, _wsPort)) {
    Serial.println("SSL connection failed");
    return false;
  }

  // Disable Nagle algorithm for immediate send
  _client.setNoDelay(true);

  // Construct Path with Model ID (defaulting to scribe_v2 if not set)
  String modelParam = (_modelId != nullptr && strlen(_modelId) > 0) ? _modelId : "scribe_v2";
  String fullPath = String(_wsPath) + "?model_id=" + modelParam;

  // Generate WebSocket Key and send handshake request
  String ws_key = generateWebSocketKey();
  String request = String("GET ") + fullPath + " HTTP/1.1\r\n";
  request += String("Host: ") + _wsHost + "\r\n";
  request += "Upgrade: websocket\r\n";
  request += "Connection: Upgrade\r\n";
  request += "Sec-WebSocket-Key: " + ws_key + "\r\n";
  request += "Sec-WebSocket-Version: 13\r\n";
  request += String("xi-api-key: ") + _apiKey + "\r\n"; // ElevenLabs Auth Header
  request += "\r\n";

  _client.print(request);

  // Read response
  unsigned long timeout = millis();
  while (_client.connected() && !_client.available()) {
    if (millis() - timeout > 5000) {
      Serial.println("Response timeout");
      _client.stop();
      return false;
    }
    delay(10);
  }

  String response = "";
  bool headers_complete = false;
  while (_client.available() && !headers_complete) {
    String line = _client.readStringUntil('\n');
    response += line + "\n";
    if (line == "\r" || line.length() == 0) {
      headers_complete = true;
    }
  }

  // Check response
  if (response.indexOf("101") >= 0 && response.indexOf("Switching Protocols") >= 0) {
    Serial.println("WebSocket connected to ElevenLabs");
    _wsConnected = true;
    return true;
  } else {
    Serial.println("WebSocket handshake failed");
    Serial.println(response);
    _client.stop();
    return false;
  }
}

void ArduinoASRChat::disconnectWebSocket() {
  if (_wsConnected) {
    _client.stop();
    _wsConnected = false;
    Serial.println("WebSocket disconnected");
  }
}

bool ArduinoASRChat::isWebSocketConnected() {
  return _wsConnected && _client.connected();
}

bool ArduinoASRChat::startRecording() {
  if (!_wsConnected) {
    Serial.println("WebSocket not connected!");
    if (!connectWebSocket()) {
        return false;
    }
  }

  if (_isRecording) {
    Serial.println("Already recording!");
    return false;
  }

  Serial.println("\n========================================");
  Serial.println("ElevenLabs Recording started...");
  Serial.println("========================================");

  _isRecording = true;
  _shouldStop = false;
  _hasSpeech = false;
  _hasNewResult = false;
  _lastResultText = "";
  _recognizedText = "";
  _lastSpeechTime = 0;
  _recordingStartTime = millis();
  _sendBufferPos = 0;
  _sameResultCount = 0;
  _lastDotTime = millis();

  // Send initial configuration frame
  sendStartConfig();
  
  return true;
}

void ArduinoASRChat::stopRecording() {
  if (!_isRecording) {
    return;
  }

  // Send remaining buffer
  if (_sendBufferPos > 0) {
    sendAudioChunk((uint8_t*)_sendBuffer, _sendBufferPos * 2);
    _sendBufferPos = 0;
  }

  Serial.println("\n========================================");
  Serial.println("Recording stopped");
  Serial.print("Final result: ");
  Serial.println(_lastResultText);
  Serial.println("========================================\n");

  _isRecording = false;
  _shouldStop = true;
  _recognizedText = _lastResultText;
  _hasNewResult = true;

  // Send End of Stream JSON
  sendEndMarker();

  // Trigger callback if set
  if (_resultCallback != nullptr && _recognizedText.length() > 0) {
    _resultCallback(_recognizedText);
  }
  
  // Clean disconnect after session
  disconnectWebSocket();
}

bool ArduinoASRChat::isRecording() {
  return _isRecording;
}

void ArduinoASRChat::loop() {
  if (!_wsConnected) {
    return;
  }

  // Check connection status
  if (!_client.connected()) {
    Serial.println("Connection lost");
    _wsConnected = false;
    _isRecording = false;
    return;
  }

  // Process audio sending during recording
  if (_isRecording && !_shouldStop) {
    processAudioSending();
    checkRecordingTimeout();
    checkSilence();
  }

  // Process received data
  if (_client.available()) {
     handleWebSocketData();
  }
}

void ArduinoASRChat::processAudioSending() {
  // Print progress dot every second
  if (millis() - _lastDotTime > 1000) {
    Serial.print(".");
    _lastDotTime = millis();
  }

  // Read audio samples in a tight loop to keep up with I2S data rate
  for (int i = 0; i < _samplesPerRead; i++) {
    if (!_I2S.available()) {
      break;  // No more data available
    }

    int sample = _I2S.read();

    // Filter invalid data
    if (sample != 0 && sample != -1 && sample != 1) {
      _sendBuffer[_sendBufferPos++] = (int16_t)sample;

      // Buffer full, send batch immediately
      if (_sendBufferPos >= _sendBatchSize / 2) {
        sendAudioChunk((uint8_t*)_sendBuffer, _sendBufferPos * 2);
        _sendBufferPos = 0;
      }
    }
  }

  yield();
}

void ArduinoASRChat::checkRecordingTimeout() {
  // Check max duration
  if (millis() - _recordingStartTime > _maxSeconds * 1000) {
    Serial.println("\nMax duration reached");

    // If no speech detected and callback is set, trigger timeout callback
    if (!_hasSpeech && _timeoutNoSpeechCallback != nullptr) {
      Serial.println("No speech detected during recording, exiting continuous mode");
      stopRecording();
      _timeoutNoSpeechCallback();
    } else {
      Serial.println("Stopping recording");
      stopRecording();
    }
  }
}

void ArduinoASRChat::checkSilence() {
  // Check silence - if speech detected and exceeded silence duration
  if (_hasSpeech && _lastSpeechTime > 0) {
    unsigned long silence = millis() - _lastSpeechTime;
    if (silence >= _silenceDuration) {
      Serial.printf("\nSilence detected (%.1fs), stopping\n", silence / 1000.0);
      stopRecording();
    }
  }
}

String ArduinoASRChat::getRecognizedText() {
  return _recognizedText;
}

bool ArduinoASRChat::hasNewResult() {
  return _hasNewResult;
}

void ArduinoASRChat::clearResult() {
  _hasNewResult = false;
}

void ArduinoASRChat::setResultCallback(ResultCallback callback) {
  _resultCallback = callback;
}

void ArduinoASRChat::setTimeoutNoSpeechCallback(TimeoutNoSpeechCallback callback) {
  _timeoutNoSpeechCallback = callback;
}

void ArduinoASRChat::sendStartConfig() {
    StaticJsonDocument<256> doc;
    doc["type"] = "start";
    
    String json_str;
    serializeJson(doc, json_str);
    // Send as Text Frame (0x01)
    sendWebSocketFrame((uint8_t*)json_str.c_str(), json_str.length(), 0x01);
}

void ArduinoASRChat::sendAudioChunk(uint8_t* data, size_t len) {
  // 1. Encode Audio to Base64
  size_t output_len;
  size_t base64_len = ((len + 2) / 3) * 4 + 1; // Approximation
  unsigned char* base64_output = new unsigned char[base64_len];
  
  mbedtls_base64_encode(base64_output, base64_len, &output_len, data, len);
  base64_output[output_len] = 0; // Null terminate

  // 2. Wrap in JSON
  // Standard ElevenLabs pattern: {"audio_event": "audio_chunk", "audio_base_64": "..."}
  // Using manual string building to save JSON overhead in critical loop
  String json_payload = "{\"audio_event\":\"audio_chunk\",\"audio_base_64\":\"";
  json_payload += (char*)base64_output;
  json_payload += "\"}";

  // 3. Send as Text Frame
  sendWebSocketFrame((uint8_t*)json_payload.c_str(), json_payload.length(), 0x01); // 0x01 = Text Frame

  delete[] base64_output;
}

void ArduinoASRChat::sendEndMarker() {
  // Send End of Stream JSON (empty chunk)
  String eos = "{\"audio_event\":\"audio_chunk\",\"audio_base_64\":\"\"}"; 
  sendWebSocketFrame((uint8_t*)eos.c_str(), eos.length(), 0x01);
  Serial.println("End marker sent");
}

void ArduinoASRChat::sendPong() {
  uint8_t pong_data[1] = {0};
  sendWebSocketFrame(pong_data, 0, 0x0A);
}

void ArduinoASRChat::sendWebSocketFrame(uint8_t* data, size_t len, uint8_t opcode) {
  if (!_wsConnected || !_client.connected()) return;

  // Build WebSocket frame header
  uint8_t header[10];
  int header_len = 2;

  header[0] = 0x80 | opcode; // FIN bit set
  header[1] = 0x80; // Mask bit set

  // Length
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

  // Generate mask key
  uint8_t mask_key[4];
  for (int i = 0; i < 4; i++) {
    mask_key[i] = random(0, 256);
  }
  memcpy(header + header_len, mask_key, 4);
  header_len += 4;

  // Send frame header
  _client.write(header, header_len);

  // Mask data and send
  for (size_t i = 0; i < len; i++) {
    data[i] ^= mask_key[i % 4];
  }
  _client.write(data, len);
}

void ArduinoASRChat::handleWebSocketData() {
  // Read WebSocket frame
  uint8_t header[2];
  if (_client.readBytes(header, 2) != 2) {
    return;
  }

  bool fin = header[0] & 0x80;
  uint8_t opcode = header[0] & 0x0F;
  bool masked = header[1] & 0x80;
  uint64_t payload_len = header[1] & 0x7F;

  // Handle extended length
  if (payload_len == 126) {
    uint8_t len_bytes[2];
    _client.readBytes(len_bytes, 2);
    payload_len = (len_bytes[0] << 8) | len_bytes[1];
  } else if (payload_len == 127) {
    uint8_t len_bytes[8];
    _client.readBytes(len_bytes, 8);
    payload_len = 0;
    for (int i = 0; i < 8; i++) {
      payload_len = (payload_len << 8) | len_bytes[i];
    }
  }

  // Read mask key
  uint8_t mask_key[4] = {0};
  if (masked) {
    _client.readBytes(mask_key, 4);
  }

  // Read payload
  if (payload_len > 0 && payload_len < 100000) {
    uint8_t* payload = new uint8_t[payload_len + 1];
    size_t bytes_read = _client.readBytes(payload, payload_len);

    if (bytes_read == payload_len) {
      payload[payload_len] = 0; // Null terminate
      
      // Unmask if needed
      if (masked) {
        for (size_t i = 0; i < payload_len; i++) {
          payload[i] ^= mask_key[i % 4];
        }
      }

      // Handle Text Frames (JSON)
      if (opcode == 0x01) { 
        parseResponse(payload, payload_len);
      } else if (opcode == 0x08) {
        Serial.println("Server closed connection");
        _wsConnected = false;
        _client.stop();
      } else if (opcode == 0x09) {
        sendPong();
      }
    }

    delete[] payload;
  }
}

void ArduinoASRChat::parseResponse(uint8_t* data, size_t len) {
  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, (char*)data);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  // Handle ElevenLabs Response Structure
  if (doc.containsKey("text")) {
    const char* text = doc["text"];
    String current_text = String(text);
    // ElevenLabs sends "is_final" boolean
    bool is_final = doc["is_final"] | false; 

    if (current_text.length() > 0 && current_text != " ") {
      if (!_hasSpeech) {
        _hasSpeech = true;
        Serial.println("\nSpeech detected...");
      }

      // Update last speech time
      _lastSpeechTime = millis();
      _lastResultText = current_text;
      
      Serial.printf("Recognizing: %s\n", current_text.c_str());

      // If ElevenLabs says it's final, we trust it
      if (is_final) {
          Serial.printf("Final Phrase: %s\n", current_text.c_str());
          _recognizedText = current_text; 
          _hasNewResult = true;
          
          if (_isRecording && !_shouldStop) {
             Serial.println("Received final result, stopping.");
             stopRecording();
          }
      }
    }
  }
}
