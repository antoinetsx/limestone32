#pragma once

#include <Arduino.h>

String urlEncode(const String &value);
size_t urlEncodeToBuffer(const char *value, char *out, size_t outLen);
bool utf8Decode(const String &s, size_t &i, uint32_t &cp);
String stripAccents(const String &input);
