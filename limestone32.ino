// ============================================================
//  Limestone32 — ESP32 bus/RER departure board for TTGO T-Display (240x135)
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
#include <TFT_eSPI.h>
#include "config.h"
#include "app_state.h"
#include "display.h"
#include "departures.h"
#include "time_utils.h"

#if __has_include("esp_wifi.h")
#include <esp_wifi.h>
#endif

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

  for (int i = 0; i < NB_STOPS; i++) {
    themes[i] = themeForStop(stops[i]);
    if (!buildDeparturesPath(stops[i], gDeparturePaths[i], DEPARTURE_PATH_MAX)) {
      Serial.print("Warning: departure path too long for stop ");
      Serial.println(i);
    }
  }
  configureDeparturesFilter(gFilterDoc);
}

void loop() {
  pollNavigationButtons();

  if (gPendingLoadingDraw) {
    gPendingLoadingDraw = false;
    drawLoadingScreen(currentStop);
  }

  updateMinuteCountdown();

  if (needsRefresh || millis() - lastFetch > FETCH_INTERVAL_MS) {
    if (fetchAndDisplay(currentStop)) {
      lastFetch = millis();
      needsRefresh = false;
    }
  }
}
