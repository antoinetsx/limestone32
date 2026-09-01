// ============================================================
//  Afficheur de prochains passages (bus / RER) sur TTGO T-Display
//  Source des donnees : API PRIM (Ile-de-France Mobilites)
// ============================================================
//
//  Bibliotheques necessaires (Tools > Manage Libraries) :
//    - TFT_eSPI (deja installee et configuree)
//    - ArduinoJson (version 7.x recommandee)
//
// ============================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "config.h"  // generated from .env — run: python generate_config.py

#define BUTTON_NEXT   0   // bouton du bas sur la T-Display
#define TFT_BACKLIGHT 4

TFT_eSPI tft = TFT_eSPI();

int currentStop = 0;
unsigned long lastFetch = 0;
bool needsRefresh = true;

void setup() {
  Serial.begin(115200);

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

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi connecte !");
}

void loop() {
  // Detection du bouton (passer a l'arret suivant)
  static bool lastState = HIGH;
  bool state = digitalRead(BUTTON_NEXT);
  if (state == LOW && lastState == HIGH) {
    currentStop = (currentStop + 1) % NB_STOPS;
    needsRefresh = true;
    delay(50); // anti-rebond simple
  }
  lastState = state;

  // Rafraichissement automatique toutes les 30 secondes,
  // ou immediatement si on vient de changer d'arret
  if (needsRefresh || millis() - lastFetch > 30000) {
    fetchAndDisplay(stops[currentStop]);
    lastFetch = millis();
    needsRefresh = false;
  }
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

  WiFiClientSecure client;
  client.setInsecure(); // simplifie la verification du certificat HTTPS

  Serial.print("Memoire libre avant requete: ");
  Serial.println(ESP.getFreeHeap());

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  String url = "https://prim.iledefrance-mobilites.fr/marketplace/stop-monitoring?MonitoringRef=" + String(stop.monitoringRef);
  http.begin(client, url);
  http.addHeader("apiKey", PRIM_API_KEY);

  int httpCode = http.GET();
  if (httpCode != 200) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(0, 0);
    tft.println(stop.label);
    tft.print("Erreur HTTP: ");
    tft.println(httpCode);
    Serial.print("Detail erreur: ");
    Serial.println(HTTPClient::errorToString(httpCode));
    http.end();
    client.stop();
    return;
  }

  String payload = http.getString();
  http.end();
  client.stop();

  JsonDocument doc; // ArduinoJson v7 : taille geree automatiquement
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(0, 0);
    tft.println(stop.label);
    tft.println("Erreur JSON");
    Serial.println(err.c_str());
    return;
  }

  JsonArray visits = doc["Siri"]["ServiceDelivery"]["StopMonitoringDelivery"][0]["MonitoredStopVisit"];

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0, 0);
  tft.println(stop.label);

  int shown = 0;
  for (JsonObject visit : visits) {
    JsonObject journey = visit["MonitoredVehicleJourney"];

    String lineRef = journey["LineRef"]["value"] | "";

    String direction = "";
    if (journey["DestinationName"].size() > 0) {
      direction = journey["DestinationName"][0]["value"].as<String>();
    } else if (journey["DirectionName"].size() > 0) {
      direction = journey["DirectionName"][0]["value"].as<String>();
    }

    bool lineOk = (strlen(stop.lineFilter) == 0) || (lineRef.indexOf(stop.lineFilter) >= 0);
    bool dirOk  = (strlen(stop.directionFilter) == 0) || (direction.indexOf(stop.directionFilter) >= 0);

    if (lineOk && dirOk) {
      String expected = journey["MonitoredCall"]["ExpectedArrivalTime"] | "";
      if (expected.length() == 0) {
        expected = journey["MonitoredCall"]["ExpectedDepartureTime"] | "";
      }
      String heure = (expected.length() >= 16) ? expected.substring(11, 16) : "??:??";

      tft.println(direction + " -> " + heure);
      shown++;
      if (shown >= 3) break; // affiche jusqu'a 3 prochains passages
    }
  }

  if (shown == 0) {
    tft.println("Aucun passage trouve");
  }
}
