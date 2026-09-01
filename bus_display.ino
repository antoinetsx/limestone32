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

TFT_eSPI tft = TFT_eSPI();

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

void showFetchError(const char *label, const char *title, const String &detail) {
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.setTextSize(2);
  tft.println(label);
  tft.println(title);
  tft.setTextSize(1);
  tft.println(detail);
}

String formatLocalTime(const char *isoUtc) {
  struct tm departure = {};
  if (strptime(isoUtc, "%Y-%m-%dT%H:%M:%S", &departure) == nullptr) {
    return "??:??";
  }

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t utc = mktime(&departure);

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  struct tm localTime;
  localtime_r(&utc, &localTime);

  char buf[6];
  strftime(buf, sizeof(buf), "%H:%M", &localTime);
  return String(buf);
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
  return strstr(lineRef, lineId) != nullptr;
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
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.setTextSize(2);
  tft.println(stop.label);
  tft.println("Chargement...");

  if (WiFi.status() != WL_CONNECTED) {
    tft.println("Wi-Fi deconnecte");
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
    showFetchError(stop.label, "Erreur HTTPS", detail);
    return;
  }

  if (httpCode != 200) {
    showFetchError(stop.label, "Erreur HTTP", String(httpCode) + " - " + errorMsg);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    showFetchError(stop.label, "Erreur JSON", String(err.c_str()));
    Serial.println(err.c_str());
    Serial.println(payload.substring(0, 120));
    return;
  }

  const char *apiError = doc["error"] | doc["message"] | "";
  if (apiError[0] != '\0') {
    showFetchError(stop.label, "Erreur API", apiError);
    return;
  }

  JsonArray departures = doc["departures"].as<JsonArray>();
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.setTextSize(2);
  tft.println(stop.label);

  int shown = 0;
  for (JsonObject departure : departures) {
    const char *branchRef = departure["branchRef"] | "";
    const char *lineRef = departure["lineRef"] | "";
    if (!branchMatches(branchRef, stop.branchHash)) {
      continue;
    }
    if (!lineMatches(lineRef, stop.lineId)) {
      continue;
    }
    if (isCancelled(departure)) {
      continue;
    }

    String direction = departure["shortDestinationLabel"] | "";
    if (direction.length() == 0) {
      direction = departure["destinationLabel"] | "";
    }
    if (direction.length() == 0) {
      direction = departure["directionName"] | "Destination";
    }

    const char *dateTime = departure["dateTime"] | "";
    String heure = formatLocalTime(dateTime);

    tft.println(direction + " -> " + heure);
    shown++;
    if (shown >= 3) {
      break;
    }
  }

  if (shown == 0) {
    tft.println("Aucun passage trouve");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  pinMode(BUTTON_NEXT, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(0, 0);
  tft.println("Connexion Wi-Fi...");

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

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println("Reseau OK");
  delay(800);
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
