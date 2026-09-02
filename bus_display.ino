// ============================================================
//  ESP32 bus/RER departure board for TTGO T-Display (240x135)
//  Data source: Leon API (ecrans-api.gwadz.fr)
// ============================================================
//
//  Libraries (Tools > Manage Libraries):
//    - TFT_eSPI
//    - ArduinoJson 7.x
//
//  Setup: copy .env.example to .env, then run:
//    python generate_config.py
//
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <strings.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "config.h"

#if __has_include("esp_wifi.h")
#include <esp_wifi.h>
#endif

#define BUTTON_NEXT     0
#define BUTTON_REFRESH  35  // input-only; T-Display has external pull-up
#define TFT_BACKLIGHT 4
#define LEON_API_HOST "ecrans-api.gwadz.fr"
#define FETCH_INTERVAL_MS 60000
#define TLS_TIMEOUT_SEC 30
#define CONNECT_ATTEMPT_MS 3000
#define BODY_IDLE_MS 2000
#define BODY_MAX_MS 60000
#define HTTP_READ_CHUNK 256
#define MAX_HTTP_REDIRECTS 3
// Leon/Cloudflare omits Content-Length; HTTP/1.0 needs manual stream read (~100 KB max hub).
// --- Layout (240x135 landscape, rotation 1) ---
static const int SCREEN_W = 240;
static const int SCREEN_H = 135;
static const int HEADER_H = 38;
static const int BODY_Y = 38;
static const int BORDER_H = 5;
static const int CONTENT_Y = BODY_Y + BORDER_H;
static const int LEFT_W = 156;
static const int RIGHT_W = 84;
static const int RIGHT_X = LEFT_W;
static const int ROW_H = 45;
static const int ROW1_Y = 43;
static const int SEP_Y = 88;
static const int SEP_H = 2;
static const int ROW2_Y = 90;
static const int BADGE_X = 5;
static const int BADGE_Y = 5;
static const int BADGE_SIZE = 28;
static const int BADGE_BUS_W = 28;
static const int BADGE_BUS_H = 18;
static const int BADGE_GAP = 5;
static const int BADGE_RADIUS = 4;
static const int BADGE_CENTER_Y = BADGE_Y + BADGE_SIZE / 2;
static const int STATION_X = BADGE_X + BADGE_SIZE + BADGE_GAP;
static const int STATION_X_BUS = BADGE_X + BADGE_BUS_W + BADGE_GAP;
static const int STATION_TEXT_SIZE = 2;
static const int STATION_TEXT_H = 8 * STATION_TEXT_SIZE;
static const int STATION_Y = BADGE_CENTER_Y - STATION_TEXT_H / 2;
static const int MINUTES_NUM_SIZE = 3;
static const int MINUTES_SUFFIX_SIZE = 1;
static const int MINUTES_NUM_H = 8 * MINUTES_NUM_SIZE;
static const int MINUTES_SUFFIX_H = 8 * MINUTES_SUFFIX_SIZE;
static const int DEST_PAD_X = 5;
static const int DEST_MAX_W = 149;
static const int MAX_DEPARTURES = 2;

TFT_eSPI tft = TFT_eSPI();

struct DepartureRow {
  String destination;
  int minutes;
};

enum BadgeMode : uint8_t {
  BADGE_BUS,
  BADGE_RER,
  BADGE_METRO,
  BADGE_TRAM,
};

struct LineTheme {
  uint16_t accent;
  const char *badgeLabel;
  BadgeMode mode;
  uint16_t textColor;
};

int currentStop = 0;
unsigned long lastFetch = 0;
bool needsRefresh = true;
volatile uint32_t fetchGeneration = 0;
WiFiClientSecure *gActiveFetchClient = nullptr;

uint16_t gColorDestText;
uint16_t gColorTimeYellow;
uint16_t gColorSepGray;

// --- Display helpers ---

uint16_t color565FromHex(uint32_t hex) {
  uint8_t r = (hex >> 16) & 0xFF;
  uint8_t g = (hex >> 8) & 0xFF;
  uint8_t b = hex & 0xFF;
  return tft.color565(r, g, b);
}

#define IDFM_TEXT_DARK  0x25303B
#define IDFM_TEXT_LIGHT 0xFFFFFF

float srgbChannelToLinear(uint8_t channel) {
  float c = channel / 255.0f;
  if (c <= 0.03928f) {
    return c / 12.92f;
  }
  return powf((c + 0.055f) / 1.055f, 2.4f);
}

float relativeLuminance(uint32_t hex) {
  uint8_t r = (hex >> 16) & 0xFF;
  uint8_t g = (hex >> 8) & 0xFF;
  uint8_t b = hex & 0xFF;
  float rLin = srgbChannelToLinear(r);
  float gLin = srgbChannelToLinear(g);
  float bLin = srgbChannelToLinear(b);
  return 0.2126f * rLin + 0.7152f * gLin + 0.0722f * bLin;
}

float contrastRatio(float L1, float L2) {
  float lighter = (L1 > L2) ? L1 : L2;
  float darker = (L1 > L2) ? L2 : L1;
  return (lighter + 0.05f) / (darker + 0.05f);
}

uint32_t bestTextColorForBackground(uint32_t bgHex) {
  float bgL = relativeLuminance(bgHex);
  float darkL = relativeLuminance(IDFM_TEXT_DARK);
  float lightL = relativeLuminance(IDFM_TEXT_LIGHT);
  float contrastDark = contrastRatio(bgL, darkL);
  float contrastLight = contrastRatio(bgL, lightL);
  return (contrastDark >= contrastLight) ? IDFM_TEXT_DARK : IDFM_TEXT_LIGHT;
}

void setupDisplayColors() {
  gColorDestText = color565FromHex(0x4D565F);
  gColorTimeYellow = color565FromHex(0xFEC107);
  gColorSepGray = color565FromHex(0xA9B1B9);
}

void drawLoadingScreen(const Stop &stop);

bool isFetchStale(uint32_t generation) {
  return generation != fetchGeneration;
}

void abortActiveFetchClient() {
  if (gActiveFetchClient != nullptr) {
    gActiveFetchClient->stop();
  }
}

void releaseActiveFetchClient(WiFiClientSecure &client) {
  if (gActiveFetchClient == &client) {
    gActiveFetchClient = nullptr;
  }
  client.stop();
}

// Poll NEXT/REFRESH during blocking fetch I/O so stop switches are instant.
void pollNavigationButtons() {
  static bool lastNextState = HIGH;
  bool nextState = digitalRead(BUTTON_NEXT);
  if (nextState == LOW && lastNextState == HIGH) {
    fetchGeneration++;
    abortActiveFetchClient();
    currentStop = (currentStop + 1) % NB_STOPS;
    needsRefresh = true;
    drawLoadingScreen(stops[currentStop]);
    unsigned long debounceStart = millis();
    while (millis() - debounceStart < 50) {
      delay(5);
    }
  }
  lastNextState = nextState;

  static bool lastRefreshState = HIGH;
  bool refreshState = digitalRead(BUTTON_REFRESH);
  if (refreshState == LOW && lastRefreshState == HIGH) {
    fetchGeneration++;
    abortActiveFetchClient();
    needsRefresh = true;
    drawLoadingScreen(stops[currentStop]);
    unsigned long debounceStart = millis();
    while (millis() - debounceStart < 50) {
      delay(5);
    }
  }
  lastRefreshState = refreshState;
}

void delayUnlessStale(uint32_t generation, unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    pollNavigationButtons();
    if (isFetchStale(generation)) {
      return;
    }
    delay(10);
  }
}

String urlEncode(const String &value) {
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

bool utf8Decode(const String &s, size_t &i, uint32_t &cp) {
  if (i >= s.length()) {
    return false;
  }

  uint8_t c = (uint8_t)s.charAt(i);
  if (c < 0x80) {
    cp = c;
    i++;
    return true;
  }

  if ((c & 0xE0) == 0xC0 && i + 1 < s.length()) {
    uint8_t c2 = (uint8_t)s.charAt(i + 1);
    if ((c2 & 0xC0) != 0x80) {
      cp = c;
      i++;
      return true;
    }
    cp = ((uint32_t)(c & 0x1F) << 6) | (c2 & 0x3F);
    i += 2;
    return true;
  }

  if ((c & 0xF0) == 0xE0 && i + 2 < s.length()) {
    uint8_t c2 = (uint8_t)s.charAt(i + 1);
    uint8_t c3 = (uint8_t)s.charAt(i + 2);
    if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
      cp = c;
      i++;
      return true;
    }
    cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
    i += 3;
    return true;
  }

  cp = c;
  i++;
  return true;
}

// GLCD fonts lack accents; transliterate UTF-8 to ASCII for the TFT.
String stripAccents(const String &input) {
  String out;
  out.reserve(input.length());

  size_t i = 0;
  while (i < input.length()) {
    uint32_t cp = 0;
    if (!utf8Decode(input, i, cp)) {
      break;
    }

    char single = 0;
    const char *multi = nullptr;

    switch (cp) {
      case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
        single = 'a'; break;
      case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
        single = 'A'; break;
      case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
        single = 'e'; break;
      case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        single = 'E'; break;
      case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
        single = 'i'; break;
      case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
        single = 'I'; break;
      case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6:
        single = 'o'; break;
      case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6:
        single = 'O'; break;
      case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
        single = 'u'; break;
      case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
        single = 'U'; break;
      case 0x00E7:
        single = 'c'; break;
      case 0x00C7:
        single = 'C'; break;
      case 0x00F1:
        single = 'n'; break;
      case 0x00D1:
        single = 'N'; break;
      case 0x00FD: case 0x00FF:
        single = 'y'; break;
      case 0x00DD:
        single = 'Y'; break;
      case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
        single = '-'; break;
      case 0x0153:
        multi = "oe"; break;
      case 0x0152:
        multi = "OE"; break;
      case 0x00E6:
        multi = "ae"; break;
      case 0x00C6:
        multi = "AE"; break;
      default:
        if (cp < 0x80) {
          out += (char)cp;
        }
        break;
    }

    if (single != 0) {
      out += single;
    } else if (multi != nullptr) {
      out += multi;
    }
  }

  return out;
}

bool syncNetworkTime() {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 20; i++) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println("Time synchronized (NTP)");
      return true;
    }
    delay(500);
  }
  Serial.println("Warning: time not synchronized");
  return false;
}

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

int parseHttpStatusLine(const char *line) {
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

bool readPollLine(Stream *stream, char *line, size_t lineLen, size_t &outLen, uint32_t generation,
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

bool connectTlsPoll(WiFiClientSecure &client, const char *host, uint32_t generation,
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

bool sendHttpGetRequest(WiFiClientSecure &client, const char *host, const String &path) {
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

enum ReadHeadersResult : uint8_t {
  HEADERS_OK,
  HEADERS_ABORTED,
  HEADERS_TIMEOUT,
  HEADERS_ERROR,
};

ReadHeadersResult readHttpResponseHeaders(Stream *stream, int &httpCode, int &contentLength,
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

String pathFromRedirectLocation(const char *location) {
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

bool isRedirectStatus(int httpCode) {
  return httpCode == 301 || httpCode == 302 || httpCode == 307 || httpCode == 308;
}

// Parse JSON straight from the TLS stream. HTTP/1.0 avoids chunked encoding.
// Skips buffering ~100 KB into a String (which OOM'd around 64 KB on RER hubs).
DeserializationError fetchDeparturesJson(const char *host, const String &path, JsonDocument &doc,
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

  String requestPath = path;
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

    sendHttpGetRequest(client, host, requestPath);

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
      requestPath = nextPath;
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

const char *lineCodeFromId(const char *lineId) {
  if (lineId == nullptr || lineId[0] == '\0') {
    return "";
  }

  size_t len = strlen(lineId);
  while (len > 0 && lineId[len - 1] == ':') {
    len--;
  }
  if (len == 0) {
    return "";
  }

  const char *end = lineId + len;
  const char *start = end - 1;
  while (start >= lineId && *start != ':') {
    start--;
  }
  return (start >= lineId) ? start + 1 : lineId;
}

BadgeMode parseBadgeMode(const char *modeStr) {
  if (modeStr == nullptr || modeStr[0] == '\0') {
    return BADGE_BUS;
  }
  if (strcasecmp(modeStr, "bus") == 0) {
    return BADGE_BUS;
  }
  if (strcasecmp(modeStr, "rer") == 0) {
    return BADGE_RER;
  }
  if (strcasecmp(modeStr, "metro") == 0) {
    return BADGE_METRO;
  }
  if (strcasecmp(modeStr, "tram") == 0) {
    return BADGE_TRAM;
  }
  return BADGE_BUS;
}

BadgeMode inferBadgeMode(const Stop &stop, const char *code, const char *badgeLabel) {
  if (stop.badgeMode != nullptr && stop.badgeMode[0] != '\0') {
    return parseBadgeMode(stop.badgeMode);
  }

  if (strstr(stop.label, "RER") != nullptr) {
    return BADGE_RER;
  }

  if (badgeLabel != nullptr && badgeLabel[0] != '\0') {
    if (badgeLabel[0] == 'T' && isdigit((unsigned char)badgeLabel[1])) {
      return BADGE_TRAM;
    }
    if (strlen(badgeLabel) == 1 && isalpha((unsigned char)badgeLabel[0])) {
      return BADGE_RER;
    }
    if (strlen(badgeLabel) <= 2 && isdigit((unsigned char)badgeLabel[0])) {
      return BADGE_METRO;
    }
  }

  if (code[0] == 'C' && code[1] == '0') {
    if (code[2] == '0') {
      return BADGE_METRO;
    }
    if (code[2] == '1') {
      return BADGE_RER;
    }
  }

  return BADGE_BUS;
}

// WCAG 2.x contrast: bus/RER/metro text on badgeColor; tram text on white center band.
uint16_t textColorForTheme(BadgeMode mode, uint32_t accentHex) {
  uint32_t bgHex = (mode == BADGE_TRAM) ? IDFM_TEXT_LIGHT : accentHex;
  return color565FromHex(bestTextColorForBackground(bgHex));
}

LineTheme themeForStop(const Stop &stop) {
  const char *code = lineCodeFromId(stop.lineId);
  uint32_t accentHex = 0x6E491E;
  const char *badgeLabel = code;

  if (strstr(stop.label, "RER") != nullptr) {
    accentHex = 0xC04191;
    badgeLabel = "E";
  }

  if (stop.badgeColor != nullptr && stop.badgeColor[0] != '\0') {
    accentHex = (uint32_t)strtoul(stop.badgeColor, nullptr, 16);
  }
  if (stop.badgeText != nullptr && stop.badgeText[0] != '\0') {
    badgeLabel = stop.badgeText;
  }

  BadgeMode mode = inferBadgeMode(stop, code, badgeLabel);
  uint16_t accent = color565FromHex(accentHex);
  uint16_t textColor = textColorForTheme(mode, accentHex);

  return {accent, badgeLabel, mode, textColor};
}

String truncateToWidth(const String &text, int maxWidth, uint8_t textSize) {
  tft.setTextSize(textSize);
  if ((int)tft.textWidth(text) <= maxWidth) {
    return text;
  }

  String truncated = text;
  while (truncated.length() > 0 && (int)tft.textWidth(truncated + "...") > maxWidth) {
    truncated.remove(truncated.length() - 1);
  }
  return truncated + "...";
}

int minutesUntilDeparture(const char *isoUtc) {
  if (isoUtc == nullptr || isoUtc[0] == '\0') {
    return -1;
  }

  char isoBuf[32];
  strncpy(isoBuf, isoUtc, sizeof(isoBuf) - 1);
  isoBuf[sizeof(isoBuf) - 1] = '\0';
  char *fraction = strchr(isoBuf, '.');
  if (fraction != nullptr) {
    *fraction = '\0';
  }

  struct tm departure = {};
  if (strptime(isoBuf, "%Y-%m-%dT%H:%M:%S", &departure) == nullptr) {
    return -1;
  }

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t departureUtc = mktime(&departure);

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  time_t now = time(nullptr);
  if (departureUtc == (time_t)-1 || now == (time_t)-1) {
    return -1;
  }

  long secondsUntil = difftime(departureUtc, now);
  if (secondsUntil < 0) {
    return 0;
  }
  return (int)((secondsUntil + 59) / 60);
}

void drawBadgeTextCentered(int x, int y, int w, int h, const char *label, uint8_t textSize,
                           uint16_t textColor, uint16_t bgColor) {
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(textSize);
  int textW = tft.textWidth(label);
  int textH = 8 * textSize;
  tft.setCursor(x + (w - textW) / 2, y + (h - textH) / 2);
  tft.print(label);
}

void drawBadgeBus(int x, int y, int w, int h, uint16_t accent, const char *label,
                  uint16_t textColor) {
  tft.fillRect(x, y, w, h, accent);
  drawBadgeTextCentered(x, y, w, h, label, 1, textColor, accent);
}

void drawBadgeRer(int x, int y, int w, int h, uint16_t accent, const char *label,
                  uint16_t textColor) {
  tft.fillRoundRect(x, y, w, h, BADGE_RADIUS, accent);
  drawBadgeTextCentered(x, y, w, h, label, 2, textColor, accent);
}

void drawBadgeMetro(int x, int y, int w, int h, uint16_t accent, const char *label,
                    uint16_t textColor) {
  int cx = x + w / 2;
  int cy = y + h / 2;
  tft.fillCircle(cx, cy, w / 2, accent);
  drawBadgeTextCentered(x, y, w, h, label, 2, textColor, accent);
}

void drawBadgeTram(int x, int y, int w, int h, uint16_t accent, const char *label,
                   uint16_t textColor) {
  const int bandH = 6;
  tft.fillRect(x, y, w, bandH, accent);
  tft.fillRect(x, y + h - bandH, w, bandH, accent);
  tft.fillRect(x, y + bandH, w, h - (2 * bandH), TFT_WHITE);
  drawBadgeTextCentered(x, y, w, h, label, 1, textColor, TFT_WHITE);
}

void drawLineBadge(const LineTheme &theme) {
  const char *label = theme.badgeLabel;
  switch (theme.mode) {
    case BADGE_RER:
      drawBadgeRer(BADGE_X, BADGE_Y, BADGE_SIZE, BADGE_SIZE, theme.accent, label,
                   theme.textColor);
      break;
    case BADGE_METRO:
      drawBadgeMetro(BADGE_X, BADGE_Y, BADGE_SIZE, BADGE_SIZE, theme.accent, label,
                     theme.textColor);
      break;
    case BADGE_TRAM:
      drawBadgeTram(BADGE_X, BADGE_Y, BADGE_SIZE, BADGE_SIZE, theme.accent, label,
                    theme.textColor);
      break;
    case BADGE_BUS:
    default: {
      int busY = BADGE_CENTER_Y - BADGE_BUS_H / 2;
      drawBadgeBus(BADGE_X, busY, BADGE_BUS_W, BADGE_BUS_H, theme.accent, label,
                   theme.textColor);
      break;
    }
  }
}

void drawHeader(const Stop &stop, const LineTheme &theme) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_WHITE);
  drawLineBadge(theme);

  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(STATION_TEXT_SIZE);
  int stationX = (theme.mode == BADGE_BUS) ? STATION_X_BUS : STATION_X;
  String station = truncateToWidth(stripAccents(String(stop.label)), SCREEN_W - stationX - 5,
                                   STATION_TEXT_SIZE);
  tft.setCursor(stationX, STATION_Y);
  tft.print(station);

  tft.fillRect(0, BODY_Y, SCREEN_W, BORDER_H, theme.accent);
}

void drawMinutesCell(int rowY, int minutes) {
  tft.fillRect(RIGHT_X, rowY, RIGHT_W, ROW_H, TFT_BLACK);

  if (minutes < 0) {
    return;
  }

  char numBuf[8];
  snprintf(numBuf, sizeof(numBuf), "%d", minutes);

  uint8_t numSize = MINUTES_NUM_SIZE;
  uint8_t minSize = MINUTES_SUFFIX_SIZE;

  tft.setTextColor(gColorTimeYellow, TFT_BLACK);
  tft.setTextSize(numSize);
  int numW = tft.textWidth(numBuf);
  tft.setTextSize(minSize);
  int minW = tft.textWidth("min");
  int totalW = numW + minW;

  if (totalW > RIGHT_W) {
    numSize = 3;
    minSize = 2;
    tft.setTextSize(numSize);
    numW = tft.textWidth(numBuf);
    tft.setTextSize(minSize);
    minW = tft.textWidth("min");
    totalW = numW + minW;
  }

  const int numH = 8 * numSize;
  const int minH = 8 * minSize;
  const int startX = RIGHT_X + (RIGHT_W - totalW) / 2;
  const int numY = rowY + (ROW_H - numH) / 2;
  const int minY = numY + numH - minH;

  tft.setTextSize(numSize);
  tft.setCursor(startX, numY);
  tft.print(numBuf);

  tft.setTextSize(minSize);
  tft.setCursor(startX + numW, minY);
  tft.print("min");
}

void drawDestinationCell(int rowY, const String &destination) {
  tft.fillRect(0, rowY, LEFT_W, ROW_H, TFT_WHITE);

  tft.setTextColor(gColorDestText, TFT_WHITE);
  tft.setTextSize(2);
  String dest = truncateToWidth(stripAccents(destination), DEST_MAX_W, 2);
  tft.setCursor(DEST_PAD_X, rowY + 14);
  tft.print(dest);
}

void drawRowSeparator() {
  tft.fillRect(0, SEP_Y, LEFT_W, SEP_H, TFT_WHITE);
  tft.fillRect(DEST_PAD_X, SEP_Y, LEFT_W - (DEST_PAD_X * 2), 1, gColorSepGray);
  tft.fillRect(RIGHT_X, SEP_Y, RIGHT_W, SEP_H, TFT_BLACK);
  tft.fillRect(RIGHT_X, SEP_Y, RIGHT_W, 1, TFT_WHITE);
}

void drawDepartureBoard(const Stop &stop, const DepartureRow *rows, int count) {
  LineTheme theme = themeForStop(stop);

  tft.fillScreen(TFT_WHITE);
  drawHeader(stop, theme);

  tft.fillRect(0, CONTENT_Y, LEFT_W, SCREEN_H - CONTENT_Y, TFT_WHITE);
  tft.fillRect(RIGHT_X, CONTENT_Y, RIGHT_W, SCREEN_H - CONTENT_Y, TFT_BLACK);

  if (count >= 1) {
    drawDestinationCell(ROW1_Y, rows[0].destination);
    drawMinutesCell(ROW1_Y, rows[0].minutes);
  } else {
    drawDestinationCell(ROW1_Y, "No departures");
    drawMinutesCell(ROW1_Y, -1);
  }

  drawRowSeparator();

  if (count >= 2) {
    drawDestinationCell(ROW2_Y, rows[1].destination);
    drawMinutesCell(ROW2_Y, rows[1].minutes);
  } else {
    drawDestinationCell(ROW2_Y, "");
    drawMinutesCell(ROW2_Y, -1);
  }
}

void drawLoadingScreen(const Stop &stop) {
  LineTheme theme = themeForStop(stop);
  tft.fillScreen(TFT_WHITE);
  drawHeader(stop, theme);
  tft.fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, TFT_WHITE);
  tft.setTextColor(gColorDestText, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 14);
  tft.print("Loading...");
}

void drawErrorScreen(const Stop &stop, const char *title, const String &detail) {
  LineTheme theme = themeForStop(stop);
  tft.fillScreen(TFT_WHITE);
  drawHeader(stop, theme);
  tft.fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, TFT_WHITE);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 8);
  tft.println(stripAccents(String(title)));
  tft.setTextSize(1);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 28);
  tft.println(stripAccents(detail));
}

// --- Departure filtering ---

bool branchMatches(const char *branchRef, const char *branchFilter) {
  if (branchFilter == nullptr || strlen(branchFilter) == 0) {
    return true;
  }
  return strcmp(branchRef, branchFilter) == 0;
}

bool lineMatches(const char *lineRef, const char *lineId) {
  if (lineId == nullptr || strlen(lineId) == 0) {
    return true;
  }

  const char *code = lineCodeFromId(lineId);
  if (code[0] == '\0') {
    return true;
  }
  return strstr(lineRef, code) != nullptr;
}

bool containsIgnoreCase(const char *haystack, const char *needle) {
  if (needle == nullptr || needle[0] == '\0') {
    return true;
  }
  if (haystack == nullptr) {
    return false;
  }

  size_t needleLen = strlen(needle);
  for (const char *cursor = haystack; *cursor != '\0'; cursor++) {
    size_t i = 0;
    while (i < needleLen &&
           tolower((unsigned char)cursor[i]) == tolower((unsigned char)needle[i])) {
      i++;
    }
    if (i == needleLen) {
      return true;
    }
  }
  return false;
}

bool destinationMatches(JsonObject departure, const char *destinationFilter) {
  if (destinationFilter == nullptr || destinationFilter[0] == '\0') {
    return true;
  }

  const char *labels[] = {
    departure["shortDestinationLabel"] | "",
    departure["destinationLabel"] | "",
    departure["directionName"] | "",
    departure["destinationStopPointLabel"] | "",
  };

  const char *cursor = destinationFilter;
  while (*cursor != '\0') {
    const char *segmentStart = cursor;
    while (*cursor != '\0' && *cursor != '|') {
      cursor++;
    }
    const char *segmentEnd = cursor;

    while (segmentStart < segmentEnd &&
           isspace((unsigned char)*segmentStart)) {
      segmentStart++;
    }
    while (segmentEnd > segmentStart &&
           isspace((unsigned char)*(segmentEnd - 1))) {
      segmentEnd--;
    }

    if (segmentEnd > segmentStart) {
      char term[64];
      size_t termLen = segmentEnd - segmentStart;
      if (termLen >= sizeof(term)) {
        termLen = sizeof(term) - 1;
      }
      memcpy(term, segmentStart, termLen);
      term[termLen] = '\0';

      for (const char *label : labels) {
        if (containsIgnoreCase(label, term)) {
          return true;
        }
      }
    }

    if (*cursor == '\0') {
      break;
    }
    cursor++;
  }

  return false;
}

bool isPastDeparture(const char *isoUtc, bool isAtStop) {
  if (isoUtc == nullptr || isoUtc[0] == '\0') {
    return false;
  }

  char isoBuf[32];
  strncpy(isoBuf, isoUtc, sizeof(isoBuf) - 1);
  isoBuf[sizeof(isoBuf) - 1] = '\0';
  char *fraction = strchr(isoBuf, '.');
  if (fraction != nullptr) {
    *fraction = '\0';
  }

  struct tm departure = {};
  if (strptime(isoBuf, "%Y-%m-%dT%H:%M:%S", &departure) == nullptr) {
    return false;
  }

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t departureUtc = mktime(&departure);

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  time_t now = time(nullptr);
  if (departureUtc == (time_t)-1 || now == (time_t)-1) {
    return false;
  }

  long secondsUntil = difftime(departureUtc, now);
  if (isAtStop) {
    return secondsUntil < -120;
  }
  return secondsUntil < -300;
}

bool isCancelled(JsonObject departure) {
  JsonArray flags = departure["flags"].as<JsonArray>();
  for (JsonVariant flag : flags) {
    if (strcmp(flag.as<const char *>(), "SERVICE_IS_CANCELLED") == 0) {
      return true;
    }
  }
  return false;
}

String buildDeparturesPath(const Stop &stop) {
  String lineJson = "[\"" + String(stop.lineId) + "\"]";
  String path = "/departures/" + urlEncode(String(stop.stopId));
  path += "?linesIds=" + urlEncode(lineJson);
  path += "&getLineNotice=false";
  return path;
}

void configureDeparturesFilter(JsonDocument &filter) {
  filter.clear();
  filter["error"] = true;
  filter["message"] = true;
  // Index 0 is the template for all departures[] elements (ArduinoJson v7).
  filter["departures"][0]["branchRef"] = true;
  filter["departures"][0]["lineRef"] = true;
  filter["departures"][0]["dateTime"] = true;
  filter["departures"][0]["isAtStop"] = true;
  filter["departures"][0]["shortDestinationLabel"] = true;
  filter["departures"][0]["destinationLabel"] = true;
  filter["departures"][0]["directionName"] = true;
  filter["departures"][0]["destinationStopPointLabel"] = true;
  filter["departures"][0]["flags"] = true;
}

// Returns true when the board was drawn; false when superseded by a stop switch.
bool fetchAndDisplay(Stop &stop) {
  const uint32_t generation = fetchGeneration;
  drawLoadingScreen(stop);
  if (isFetchStale(generation)) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawErrorScreen(stop, "Wi-Fi disconnected", "");
    return !isFetchStale(generation);
  }

  String path = buildDeparturesPath(stop);
  Serial.print("Request: https://");
  Serial.print(LEON_API_HOST);
  Serial.println(path);

  JsonDocument filterDoc;
  configureDeparturesFilter(filterDoc);

  JsonDocument doc;
  DeserializationError err = DeserializationError::EmptyInput;
  int httpCode = 0;
  int responseBytes = 0;
  String errorMsg;

  for (int attempt = 1; attempt <= 2; attempt++) {
    if (isFetchStale(generation)) {
      return false;
    }

    doc.clear();
    err = fetchDeparturesJson(LEON_API_HOST, path, doc, filterDoc, httpCode, errorMsg,
                              responseBytes, generation);

    if (err == DeserializationError::Ok && !doc.overflowed()) {
      break;
    }

    Serial.print("Attempt ");
    Serial.print(attempt);
    Serial.print(" - HTTP ");
    Serial.print(httpCode);
    Serial.print(" - JSON ");
    Serial.print(err.c_str());
    if (responseBytes > 0) {
      Serial.print(" (");
      Serial.print(responseBytes);
      Serial.print(" bytes)");
    }
    Serial.println();

    const bool retryable =
        (err == DeserializationError::NoMemory || err == DeserializationError::IncompleteInput);
    if (!retryable || attempt >= 2) {
      break;
    }

    delayUnlessStale(generation, 500);
  }

  if (isFetchStale(generation)) {
    return false;
  }

  if (httpCode <= 0) {
    String detail = errorMsg.length() > 0 ? errorMsg : "Connection failed";
    drawErrorScreen(stop, "HTTPS error", detail);
    return !isFetchStale(generation);
  }

  if (httpCode != 200) {
    drawErrorScreen(stop, "HTTP error", String(httpCode) + " - " + errorMsg);
    return !isFetchStale(generation);
  }

  if (err || doc.overflowed()) {
    String detail = err ? String(err.c_str()) : "overflow";
    drawErrorScreen(stop, "JSON error", detail);
    Serial.println(err ? err.c_str() : "JsonDocument overflow");
    return !isFetchStale(generation);
  }

  if (isFetchStale(generation)) {
    return false;
  }

  const char *apiError = doc["error"] | doc["message"] | "";
  if (apiError[0] != '\0') {
    drawErrorScreen(stop, "API error", apiError);
    return !isFetchStale(generation);
  }

  JsonArray departures = doc["departures"].as<JsonArray>();
  Serial.print("Parsed departures: ");
  Serial.println(departures.size());

  DepartureRow rows[MAX_DEPARTURES];
  int shown = 0;
  int skippedBranch = 0;
  int skippedLine = 0;
  int skippedDestination = 0;
  int skippedCancelled = 0;
  int skippedPast = 0;

  for (JsonObject departure : departures) {
    pollNavigationButtons();
    if (isFetchStale(generation)) {
      return false;
    }

    const char *branchRef = departure["branchRef"] | "";
    const char *lineRef = departure["lineRef"] | "";
    const char *dateTime = departure["dateTime"] | "";
    bool atStop = departure["isAtStop"] | false;
    if (!branchMatches(branchRef, stop.branchHash)) {
      skippedBranch++;
      continue;
    }
    if (!lineMatches(lineRef, stop.lineId)) {
      skippedLine++;
      continue;
    }
    if (!destinationMatches(departure, stop.destinationFilter)) {
      skippedDestination++;
      continue;
    }
    if (isCancelled(departure)) {
      skippedCancelled++;
      continue;
    }
    if (isPastDeparture(dateTime, atStop)) {
      skippedPast++;
      continue;
    }

    String direction = departure["shortDestinationLabel"] | "";
    if (direction.length() == 0) {
      direction = departure["destinationLabel"] | "";
    }
    if (direction.length() == 0) {
      direction = departure["directionName"] | "Destination";
    }

    rows[shown].destination = direction;
    rows[shown].minutes = minutesUntilDeparture(dateTime);
    shown++;
    if (shown >= MAX_DEPARTURES) {
      break;
    }
  }

  Serial.print("After filter: shown=");
  Serial.print(shown);
  Serial.print(" skipped branch=");
  Serial.print(skippedBranch);
  Serial.print(" line=");
  Serial.print(skippedLine);
  Serial.print(" dest=");
  Serial.print(skippedDestination);
  Serial.print(" cancelled=");
  Serial.print(skippedCancelled);
  Serial.print(" past=");
  Serial.println(skippedPast);

  if (isFetchStale(generation)) {
    return false;
  }

  drawDepartureBoard(stop, rows, shown);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  pinMode(BUTTON_NEXT, INPUT_PULLUP);
  pinMode(BUTTON_REFRESH, INPUT);

  tft.init();
  tft.setRotation(1);
  setupDisplayColors();
  tft.fillScreen(TFT_WHITE);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
#if __has_include("esp_wifi.h")
  esp_wifi_set_ps(WIFI_PS_NONE);
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connected");
  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());

  syncNetworkTime();
}

void loop() {
  pollNavigationButtons();

  if (needsRefresh || millis() - lastFetch > FETCH_INTERVAL_MS) {
    if (fetchAndDisplay(stops[currentStop])) {
      lastFetch = millis();
      needsRefresh = false;
    }
  }
}
