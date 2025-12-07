#include "utils.h"
#include "config.h"

// Internal buffer for web logging
static String webLogBuffer = "";

void sysLog(String msg) {
  // Disabled Serial.print to prevent hanging if USB is disconnected on S3
  // if (Serial) Serial.print(msg); 
   
  webLogBuffer += msg; 
  if (webLogBuffer.length() > 4000) {
    webLogBuffer = webLogBuffer.substring(webLogBuffer.length() - 2000);
  }
}

void sysLogLn(String msg) {
  sysLog(msg + "\n");
}

String getWebLogBuffer() {
    return webLogBuffer;
}

void clearWebLogBuffer() {
    webLogBuffer = "";
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
