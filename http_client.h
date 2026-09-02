#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "app_state.h"

enum ReadHeadersResult : uint8_t {
  HEADERS_OK,
  HEADERS_ABORTED,
  HEADERS_TIMEOUT,
  HEADERS_ERROR,
};

DeserializationError fetchDeparturesJson(const char *host, const char *path, JsonDocument &doc,
                                         const JsonDocument &filterDoc, int &httpCode,
                                         String &errorMsg, int &responseBytes,
                                         uint32_t generation);
