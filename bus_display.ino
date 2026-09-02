// ============================================================
//  Afficheur de prochains passages (bus / RER) sur TTGO T-Display
//  Source des donnees : API Leon (ecrans-api.gwadz.fr)
// ============================================================
//
//  Bibliotheques necessaires (Tools > Manage Libraries) :
//    - TFT_eSPI (deja installee et configuree)
//    - ArduinoJson (version 7.x recommandee)
//
//  Configuration : copiez .env.example vers .env, puis :
//    python generate_config.py
//
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
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

// Layout constants — 240x135 landscape (setRotation 1), matched to Figma frames 61:9 / 62:49
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
static const int BADGE_RADIUS = 4;
static const int BADGE_CENTER_Y = BADGE_Y + BADGE_SIZE / 2;
static const int STATION_X = 38;
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

struct LineTheme {
  uint16_t accent;
  const char *badgeLabel;
};

uint16_t color565FromHex(uint32_t hex) {
  uint8_t r = (hex >> 16) & 0xFF;
  uint8_t g = (hex >> 8) & 0xFF;
  uint8_t b = hex & 0xFF;
  return tft.color565(r, g, b);
}

uint16_t gColorDestText;
uint16_t gColorTimeYellow;
uint16_t gColorSepGray;

void setupDisplayColors() {
  gColorDestText = color565FromHex(0x4D565F);
  gColorTimeYellow = color565FromHex(0xFEC107);
  gColorSepGray = color565FromHex(0xA9B1B9);
}

int currentStop = 0;
unsigned long lastFetch = 0;
bool needsRefresh = true;

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

bool syncNetworkTime() {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 20; i++) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println("Heure synchronisee (NTP)");
      return true;
    }
    delay(500);
  }
  Serial.println("Attention: heure non synchronisee");
  return false;
}

bool fetchHttpsGet(const char *host, const String &path, String &body, int &httpCode, String &errorMsg) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(TLS_TIMEOUT_SEC * 1000);
  client.setHandshakeTimeout(TLS_TIMEOUT_SEC);

  HTTPClient http;
  String url = String("https://") + host + path;
  if (!http.begin(client, url)) {
    errorMsg = "HTTP init echoue";
    return false;
  }

  http.setTimeout(TLS_TIMEOUT_SEC * 1000);
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "bus-display-esp32/2.0");

  httpCode = http.GET();
  if (httpCode <= 0) {
    errorMsg = http.errorToString(httpCode);
    http.end();
    return false;
  }

  body = http.getString();
  http.end();
  return body.length() > 0;
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

LineTheme themeForStop(const Stop &stop) {
  const char *code = lineCodeFromId(stop.lineId);
  uint16_t accent;
  const char *badgeLabel;

  if (strcmp(code, "C01729") == 0) {
    accent = color565FromHex(0xC04191);
    badgeLabel = "E";
  } else if (strcmp(code, "C01221") == 0) {
    accent = color565FromHex(0x6E491E);
    badgeLabel = "206";
  } else if (strstr(stop.label, "RER") != nullptr) {
    accent = color565FromHex(0xC04191);
    badgeLabel = "E";
  } else {
    accent = color565FromHex(0x6E491E);
    badgeLabel = code;
  }

  if (stop.badgeColor != nullptr && stop.badgeColor[0] != '\0') {
    accent = color565FromHex((uint32_t)strtoul(stop.badgeColor, nullptr, 16));
  }
  if (stop.badgeText != nullptr && stop.badgeText[0] != '\0') {
    badgeLabel = stop.badgeText;
  }

  return {accent, badgeLabel};
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

void drawLineBadge(const LineTheme &theme) {
  tft.fillRoundRect(BADGE_X, BADGE_Y, BADGE_SIZE, BADGE_SIZE, BADGE_RADIUS, theme.accent);

  tft.setTextColor(TFT_WHITE, theme.accent);
  const char *label = theme.badgeLabel;
  size_t labelLen = strlen(label);

  if (labelLen <= 1) {
    tft.setTextSize(2);
    int textW = tft.textWidth(label);
    int textH = 16;
    tft.setCursor(BADGE_X + (BADGE_SIZE - textW) / 2, BADGE_Y + (BADGE_SIZE - textH) / 2);
    tft.print(label);
  } else {
    tft.setTextSize(1);
    int textW = tft.textWidth(label);
    int textH = 8;
    tft.setCursor(BADGE_X + (BADGE_SIZE - textW) / 2, BADGE_Y + (BADGE_SIZE - textH) / 2);
    tft.print(label);
  }
}

void drawHeader(const Stop &stop, const LineTheme &theme) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_WHITE);
  drawLineBadge(theme);

  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(STATION_TEXT_SIZE);
  String station = truncateToWidth(String(stop.label), SCREEN_W - STATION_X - 5, STATION_TEXT_SIZE);
  tft.setCursor(STATION_X, STATION_Y);
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
  String dest = truncateToWidth(destination, DEST_MAX_W, 2);
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
    drawDestinationCell(ROW1_Y, "Aucun passage");
    drawMinutesCell(ROW1_Y, -1);
  }

  drawRowSeparator();

  if (count >= 2) {
    drawDestinationCell(ROW2_Y, rows[1].destination);
    drawMinutesCell(ROW2_Y, rows[1].minutes);
  } else if (count == 0) {
    drawDestinationCell(ROW2_Y, "");
    drawMinutesCell(ROW2_Y, -1);
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
  tft.print("Chargement...");
}

void drawErrorScreen(const Stop &stop, const char *title, const String &detail) {
  LineTheme theme = themeForStop(stop);
  tft.fillScreen(TFT_WHITE);
  drawHeader(stop, theme);
  tft.fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, TFT_WHITE);
  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 8);
  tft.println(title);
  tft.setTextSize(1);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 28);
  tft.println(detail);
}

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

void fetchAndDisplay(Stop &stop) {
  drawLoadingScreen(stop);

  if (WiFi.status() != WL_CONNECTED) {
    drawErrorScreen(stop, "Wi-Fi deconnecte", "");
    return;
  }

  String path = buildDeparturesPath(stop);
  Serial.print("Requete: https://");
  Serial.print(LEON_API_HOST);
  Serial.println(path);

  String payload;
  String errorMsg;
  int httpCode = 0;
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (fetchHttpsGet(LEON_API_HOST, path, payload, httpCode, errorMsg)) {
      break;
    }

    Serial.print("Tentative ");
    Serial.print(attempt);
    Serial.print(" - HTTP ");
    Serial.print(httpCode);
    Serial.print(" - ");
    Serial.println(errorMsg);

    if (attempt < 2) {
      delay(1000);
    }
  }

  if (payload.length() == 0) {
    String detail = "HTTP " + String(httpCode);
    if (errorMsg.length() > 0) {
      detail += " - " + errorMsg;
    }
    drawErrorScreen(stop, "Erreur HTTPS", detail);
    return;
  }

  if (httpCode != 200) {
    drawErrorScreen(stop, "Erreur HTTP", String(httpCode) + " - " + errorMsg);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    drawErrorScreen(stop, "Erreur JSON", String(err.c_str()));
    Serial.println(err.c_str());
    Serial.println(payload.substring(0, 120));
    return;
  }

  const char *apiError = doc["error"] | doc["message"] | "";
  if (apiError[0] != '\0') {
    drawErrorScreen(stop, "Erreur API", apiError);
    return;
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

  drawDepartureBoard(stop, rows, shown);
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
  Serial.println("\nWi-Fi connecte !");
  Serial.print("IP locale: ");
  Serial.println(WiFi.localIP());

  syncNetworkTime();
}

void loop() {
  static bool lastState = HIGH;
  bool state = digitalRead(BUTTON_NEXT);
  if (state == LOW && lastState == HIGH) {
    currentStop = (currentStop + 1) % NB_STOPS;
    needsRefresh = true;
    delay(50);
  }
  lastState = state;

  if (needsRefresh || millis() - lastFetch > FETCH_INTERVAL_MS) {
    fetchAndDisplay(stops[currentStop]);
    lastFetch = millis();
    needsRefresh = false;
  }
}
