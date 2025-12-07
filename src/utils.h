#ifndef UTILS_H
#define UTILS_H

#include <Arduino.h>

void sysLog(String msg);
void sysLogLn(String msg);
String jsonEscape(String s);
void setLedColor(uint8_t r, uint8_t g, uint8_t b);

// Access to the internal buffer for the web server
String getWebLogBuffer();
void clearWebLogBuffer();

#endif
