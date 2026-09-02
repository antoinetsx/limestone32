#include "http_client.h"
#include <strings.h>

// Wrap TLS stream: count bytes and block-read until idle (TLS may outlive http.connected()).
class IdleCountingStream : public Stream {
 public:
  IdleCountingStream(Stream *source, unsigned long maxMs, uint32_t generation)
      : _source(source), _count(0), _deadline(millis() + maxMs), _generation(generation) {}

  size_t byteCount() const { return _count; }

  int available() override { return _source != nullptr ? _source->available() : 0; }

  int read() override {
    uint8_t c = 0;
    return readBytes(reinterpret_cast<char *>(&c), 1) == 1 ? c : -1;
  }

  int peek() override { return _source != nullptr ? _source->peek() : -1; }

  void flush() override {
    if (_source != nullptr) {
      _source->flush();
    }
  }

  size_t readBytes(char *buffer, size_t length) override {
    if (_source == nullptr || length == 0) {
      return 0;
    }

    size_t total = 0;
    unsigned long lastByteMs = millis();

    while (total < length && millis() < _deadline) {
      pollNavigationButtons();
      if (isFetchStale(_generation)) {
        break;
      }

      const int avail = _source->available();
      if (avail > 0) {
        size_t chunk = length - total;
        if (chunk > (size_t)avail) {
          chunk = (size_t)avail;
        }
        if (chunk > HTTP_READ_CHUNK) {
          chunk = HTTP_READ_CHUNK;
        }
        const size_t n = _source->readBytes(buffer + total, chunk);
        if (n > 0) {
          total += n;
          _count += n;
          lastByteMs = millis();
          continue;
        }
      }
      if (total > 0 && millis() - lastByteMs >= BODY_IDLE_MS) {
        break;
      }
      delay(1);
    }

    return total;
  }

  size_t write(uint8_t) override { return 0; }

 private:
  Stream *_source;
  size_t _count;
  unsigned long _deadline;
  uint32_t _generation;
};

// Drain unread TLS bytes until idle (no data for BODY_IDLE_MS) or BODY_MAX_MS elapsed.
static size_t drainStreamIdle(Stream *stream, size_t alreadyRead, char *stopReason,
                              size_t stopReasonLen, uint32_t generation) {
  if (stream == nullptr) {
    strncpy(stopReason, "no_stream", stopReasonLen);
    return alreadyRead;
  }

  uint8_t buf[512];
  size_t total = alreadyRead;
  unsigned long lastByteMs = millis();
  const unsigned long deadline = millis() + BODY_MAX_MS;

  while (millis() < deadline) {
    pollNavigationButtons();
    if (isFetchStale(generation)) {
      strncpy(stopReason, "aborted", stopReasonLen);
      return total;
    }

    const int avail = stream->available();
    if (avail > 0) {
      const int n = stream->readBytes(buf, sizeof(buf));
      if (n > 0) {
        total += (size_t)n;
        lastByteMs = millis();
        continue;
      }
    }
    if (millis() - lastByteMs >= BODY_IDLE_MS) {
      strncpy(stopReason, "idle", stopReasonLen);
      return total;
    }
    delay(1);
  }

  strncpy(stopReason, "timeout", stopReasonLen);
  return total;
}

static int parseHttpStatusLine(const char *line) {
  if (line == nullptr || strncmp(line, "HTTP/", 5) != 0) {
    return 0;
  }
  const char *cursor = strchr(line, ' ');
  if (cursor == nullptr) {
    return 0;
  }
  while (*cursor == ' ') {
    cursor++;
  }
  return atoi(cursor);
}

static bool readPollLine(Stream *stream, char *line, size_t lineLen, size_t &outLen, uint32_t generation,
                         unsigned long deadline, bool &aborted) {
  outLen = 0;
  aborted = false;
  if (lineLen == 0) {
    return false;
  }

  while (millis() < deadline) {
    pollNavigationButtons();
    if (isFetchStale(generation)) {
      aborted = true;
      return false;
    }

    while (stream->available() > 0) {
      int c = stream->read();
      if (c < 0) {
        break;
      }
      if (c == '\n') {
        if (outLen > 0 && line[outLen - 1] == '\r') {
          outLen--;
        }
        line[outLen] = '\0';
        return true;
      }
      if (outLen + 1 < lineLen) {
        line[outLen++] = (char)c;
      }
    }
    delay(1);
  }

  return false;
}

static bool connectTlsPoll(WiFiClientSecure &client, const char *host, uint32_t generation,
                           String &errorMsg) {
  const unsigned long connectDeadline = millis() + TLS_TIMEOUT_SEC * 1000UL;
  while (millis() < connectDeadline) {
    pollNavigationButtons();
    if (isFetchStale(generation)) {
      errorMsg = "Aborted";
      return false;
    }
    if (client.connect(host, 443, CONNECT_ATTEMPT_MS)) {
      return true;
    }
    delay(10);
  }

  errorMsg = "TLS connect failed";
  return false;
}

static bool sendHttpGetRequest(WiFiClientSecure &client, const char *host, const char *path) {
  client.print("GET ");
  client.print(path);
  client.print(" HTTP/1.0\r\n");
  client.print("Host: ");
  client.print(host);
  client.print("\r\n");
  client.print("Accept: application/json\r\n");
  client.print("Accept-Encoding: identity\r\n");
  client.print("User-Agent: bus-display-esp32/2.0\r\n");
  client.print("Connection: close\r\n");
  client.print("\r\n");
  return true;
}

static ReadHeadersResult readHttpResponseHeaders(Stream *stream, int &httpCode, int &contentLength,
                                                 char *location, size_t locationLen, uint32_t generation) {
  httpCode = 0;
  contentLength = -1;
  if (location != nullptr && locationLen > 0) {
    location[0] = '\0';
  }

  const unsigned long deadline = millis() + TLS_TIMEOUT_SEC * 1000UL;
  char line[320];
  size_t lineLen = 0;
  bool gotStatus = false;

  while (true) {
    bool aborted = false;
    if (!readPollLine(stream, line, sizeof(line), lineLen, generation, deadline, aborted)) {
      if (aborted || isFetchStale(generation)) {
        return HEADERS_ABORTED;
      }
      return HEADERS_TIMEOUT;
    }

    if (!gotStatus) {
      httpCode = parseHttpStatusLine(line);
      gotStatus = true;
      if (httpCode == 0) {
        return HEADERS_ERROR;
      }
      continue;
    }

    if (lineLen == 0) {
      return HEADERS_OK;
    }

    if (strncasecmp(line, "Content-Length:", 15) == 0) {
      contentLength = atoi(line + 15);
    } else if (location != nullptr && locationLen > 0 && strncasecmp(line, "Location:", 9) == 0) {
      const char *value = line + 9;
      while (*value == ' ') {
        value++;
      }
      strncpy(location, value, locationLen - 1);
      location[locationLen - 1] = '\0';
    }
  }
}

static String pathFromRedirectLocation(const char *location) {
  if (location == nullptr || location[0] == '\0') {
    return String();
  }
  if (location[0] == '/') {
    return String(location);
  }
  if (strncmp(location, "https://", 8) == 0) {
    const char *pathStart = strchr(location + 8, '/');
    if (pathStart != nullptr) {
      return String(pathStart);
    }
  }
  return String();
}

static bool isRedirectStatus(int httpCode) {
  return httpCode == 301 || httpCode == 302 || httpCode == 307 || httpCode == 308;
}

// Parse JSON straight from the TLS stream. HTTP/1.0 avoids chunked encoding.
// Skips buffering ~100 KB into a String (which OOM'd around 64 KB on RER hubs).
DeserializationError fetchDeparturesJson(const char *host, const char *path, JsonDocument &doc,
                                         const JsonDocument &filterDoc, int &httpCode,
                                         String &errorMsg, int &responseBytes,
                                         uint32_t generation) {
  responseBytes = 0;
  if (isFetchStale(generation)) {
    errorMsg = "Aborted";
    httpCode = 0;
    return DeserializationError::EmptyInput;
  }

  WiFiClientSecure client;
  gActiveFetchClient = &client;
  client.setInsecure();
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  client.setConnectionTimeout(CONNECT_ATTEMPT_MS);
#else
  client.setTimeout(1);
#endif
  client.setHandshakeTimeout(15);

  if (!connectTlsPoll(client, host, generation, errorMsg)) {
    httpCode = 0;
    releaseActiveFetchClient(client);
    return DeserializationError::EmptyInput;
  }

  char requestPathBuf[DEPARTURE_PATH_MAX];
  strncpy(requestPathBuf, path, sizeof(requestPathBuf) - 1);
  requestPathBuf[sizeof(requestPathBuf) - 1] = '\0';
  int contentLength = -1;
  char location[256];

  for (int redirect = 0; redirect <= MAX_HTTP_REDIRECTS; redirect++) {
    if (isFetchStale(generation)) {
      errorMsg = "Aborted";
      httpCode = 0;
      releaseActiveFetchClient(client);
      return DeserializationError::EmptyInput;
    }

    if (!client.connected() && !connectTlsPoll(client, host, generation, errorMsg)) {
      httpCode = 0;
      releaseActiveFetchClient(client);
      return DeserializationError::EmptyInput;
    }

    sendHttpGetRequest(client, host, requestPathBuf);

    ReadHeadersResult headerResult =
        readHttpResponseHeaders(&client, httpCode, contentLength, location, sizeof(location),
                                generation);
    if (headerResult == HEADERS_ABORTED || isFetchStale(generation)) {
      errorMsg = "Aborted";
      httpCode = 0;
      releaseActiveFetchClient(client);
      return DeserializationError::EmptyInput;
    }
    if (headerResult != HEADERS_OK) {
      errorMsg = (headerResult == HEADERS_TIMEOUT) ? "Header timeout" : "Bad HTTP headers";
      httpCode = 0;
      releaseActiveFetchClient(client);
      return DeserializationError::EmptyInput;
    }

    if (isRedirectStatus(httpCode)) {
      String nextPath = pathFromRedirectLocation(location);
      if (nextPath.length() == 0 || redirect >= MAX_HTTP_REDIRECTS) {
        errorMsg = "HTTP redirect failed";
        releaseActiveFetchClient(client);
        return DeserializationError::EmptyInput;
      }
      if (nextPath.length() >= sizeof(requestPathBuf)) {
        errorMsg = "Redirect path too long";
        releaseActiveFetchClient(client);
        return DeserializationError::EmptyInput;
      }
      strncpy(requestPathBuf, nextPath.c_str(), sizeof(requestPathBuf) - 1);
      requestPathBuf[sizeof(requestPathBuf) - 1] = '\0';
      releaseActiveFetchClient(client);
      gActiveFetchClient = &client;
      if (!connectTlsPoll(client, host, generation, errorMsg)) {
        httpCode = 0;
        releaseActiveFetchClient(client);
        return DeserializationError::EmptyInput;
      }
      continue;
    }
    break;
  }

  if (httpCode != 200) {
    errorMsg = "HTTP " + String(httpCode);
    releaseActiveFetchClient(client);
    return DeserializationError::EmptyInput;
  }

  IdleCountingStream bodyStream(&client, BODY_MAX_MS, generation);
  const DeserializationError err =
      deserializeJson(doc, bodyStream, DeserializationOption::Filter(filterDoc));

  char stopReason[12] = "complete";
  size_t totalBytes = bodyStream.byteCount();
  if (isFetchStale(generation)) {
    strncpy(stopReason, "aborted", sizeof(stopReason));
    errorMsg = "Aborted";
    responseBytes = (int)totalBytes;
    releaseActiveFetchClient(client);
    return DeserializationError::EmptyInput;
  }

  if (err == DeserializationError::Ok) {
    strncpy(stopReason, "complete", sizeof(stopReason));
  } else if (err == DeserializationError::IncompleteInput) {
    totalBytes = drainStreamIdle(&client, totalBytes, stopReason, sizeof(stopReason), generation);
  } else if (client.available() > 0) {
    totalBytes = drainStreamIdle(&client, totalBytes, stopReason, sizeof(stopReason), generation);
  } else {
    strncpy(stopReason, "parse_error", sizeof(stopReason));
  }

  responseBytes = (int)totalBytes;
  releaseActiveFetchClient(client);

  if (isFetchStale(generation)) {
    errorMsg = "Aborted";
    return DeserializationError::EmptyInput;
  }

  Serial.print("Fetch: read ");
  Serial.print(responseBytes);
  Serial.print(" bytes (Content-Length: ");
  if (contentLength > 0) {
    Serial.print(contentLength);
  } else {
    Serial.print("unknown");
  }
  Serial.print(", stopped: ");
  Serial.print(stopReason);
  Serial.println(")");

  if (totalBytes == 0) {
    errorMsg = "Empty response";
    return DeserializationError::EmptyInput;
  }

  return err;
}
