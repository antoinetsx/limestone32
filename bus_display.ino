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
#include <HTTPClient.h>
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

#define BUTTON_NEXT   0
#define TFT_BACKLIGHT 4
#define LEON_API_HOST "ecrans-api.gwadz.fr"
#define FETCH_INTERVAL_MS 60000
#define TLS_TIMEOUT_SEC 15
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

bool isFetchStale(uint32_t generation) {
  return generation != fetchGeneration;
}

void delayUnlessStale(uint32_t generation, unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
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

// Parse departures JSON directly from the HTTPS stream (avoids getString() truncation on large hub responses).
DeserializationError fetchDeparturesJson(const char *host, const String &path, JsonDocument &doc,
                                         const JsonDocument &filterDoc, int &httpCode,
                                         String &errorMsg) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(TLS_TIMEOUT_SEC * 1000);
  client.setHandshakeTimeout(TLS_TIMEOUT_SEC);

  HTTPClient http;
  String url = String("https://") + host + path;
  if (!http.begin(client, url)) {
    errorMsg = "HTTP init failed";
    httpCode = 0;
    return DeserializationError::EmptyInput;
  }

  http.setTimeout(TLS_TIMEOUT_SEC * 1000);
  http.setReuse(false);
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "bus-display-esp32/2.0");

  httpCode = http.GET();
  if (httpCode <= 0) {
    errorMsg = http.errorToString(httpCode);
    http.end();
    return DeserializationError::EmptyInput;
  }

  if (httpCode != HTTP_CODE_OK) {
    errorMsg = "HTTP " + String(httpCode);
    http.end();
    return DeserializationError::EmptyInput;
  }

  WiFiClient *stream = http.getStreamPtr();
  if (stream == nullptr) {
    errorMsg = "No response stream";
    http.end();
    return DeserializationError::EmptyInput;
  }

  DeserializationError err =
      deserializeJson(doc, *stream, DeserializationOption::Filter(filterDoc));
  http.end();
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
  if (destinationFilter == nullptr || strlen(destinationFilter) == 0) {
    return true;
  }

  const char *labels[] = {
    departure["shortDestinationLabel"] | "",
    departure["destinationLabel"] | "",
    departure["directionName"] | "",
    departure["destinationStopPointLabel"] | "",
  };

  for (const char *label : labels) {
    if (containsIgnoreCase(label, destinationFilter)) {
      return true;
    }
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
  String errorMsg;

  for (int attempt = 1; attempt <= 2; attempt++) {
    if (isFetchStale(generation)) {
      return false;
    }

    doc.clear();
    err = fetchDeparturesJson(LEON_API_HOST, path, doc, filterDoc, httpCode, errorMsg);

    if (err == DeserializationError::Ok && !doc.overflowed()) {
      break;
    }

    Serial.print("Attempt ");
    Serial.print(attempt);
    Serial.print(" - HTTP ");
    Serial.print(httpCode);
    Serial.print(" - JSON ");
    Serial.println(err.c_str());

    const bool retryable = (err == DeserializationError::IncompleteInput ||
                            err == DeserializationError::NoMemory);
    if (!retryable || attempt >= 2) {
      break;
    }

    delayUnlessStale(generation, 1000);
  }

  if (isFetchStale(generation)) {
    return false;
  }

  if (httpCode <= 0) {
    String detail = errorMsg.length() > 0 ? errorMsg : "Connection failed";
    drawErrorScreen(stop, "HTTPS error", detail);
    return !isFetchStale(generation);
  }

  if (httpCode != HTTP_CODE_OK) {
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
  DepartureRow rows[MAX_DEPARTURES];
  int shown = 0;

  for (JsonObject departure : departures) {
    const char *branchRef = departure["branchRef"] | "";
    const char *lineRef = departure["lineRef"] | "";
    const char *dateTime = departure["dateTime"] | "";
    bool atStop = departure["isAtStop"] | false;
    if (!branchMatches(branchRef, stop.branchHash)) {
      continue;
    }
    if (!lineMatches(lineRef, stop.lineId)) {
      continue;
    }
    if (!destinationMatches(departure, stop.destinationFilter)) {
      continue;
    }
    if (isCancelled(departure)) {
      continue;
    }
    if (isPastDeparture(dateTime, atStop)) {
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
  static bool lastState = HIGH;
  bool state = digitalRead(BUTTON_NEXT);
  if (state == LOW && lastState == HIGH) {
    fetchGeneration++;
    currentStop = (currentStop + 1) % NB_STOPS;
    needsRefresh = true;
    delay(50);
  }
  lastState = state;

  if (needsRefresh || millis() - lastFetch > FETCH_INTERVAL_MS) {
    if (fetchAndDisplay(stops[currentStop])) {
      lastFetch = millis();
      needsRefresh = false;
    }
  }
}
