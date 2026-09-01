// ============================================================
//  Afficheur de prochains passages (bus / RER) sur TTGO T-Display
//  Source des donnees : API PRIM (Ile-de-France Mobilites)
// ============================================================
//
//  Bibliotheques necessaires (Tools > Manage Libraries) :
//    - TFT_eSPI (deja installee et configuree)
//    - ArduinoJson (version 7.x recommandee)
//    - GovoroxSSLClient (recherche "GovoroxSSLClient" dans le Library Manager)
//
//  IMPORTANT — PRIM exige TLS 1.3 (Cloudflare) :
//    - Installez le core ESP32 >= 3.0.0 (Boards Manager : "esp32 by Espressif Systems")
//    - Le core 2.0.x ne supporte que TLS 1.2 et ne peut pas joindre PRIM
//
// ============================================================

#include <WiFi.h>
#include <WiFiClient.h>
#include <SSLClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "config.h"   // generated from .env — run: python generate_config.py
#include "prim_ca.h"

#if __has_include("esp_wifi.h")
#include "esp_wifi.h"
#endif

#define BUTTON_NEXT   0   // bouton du bas sur la T-Display
#define TFT_BACKLIGHT 4
#define PRIM_HOST     "prim.iledefrance-mobilites.fr"

#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
#define BOARD_SUPPORTS_TLS13 1
#else
#define BOARD_SUPPORTS_TLS13 0
#endif

TFT_eSPI tft = TFT_eSPI();
WiFiClient wifiTransport;
SSLClient secureClient(&wifiTransport);

int currentStop = 0;
unsigned long lastFetch = 0;
bool needsRefresh = true;
bool networkReady = false;
bool tlsUseInsecure = false;

void printBoardInfo() {
  Serial.print("ESP32 core: ");
#if defined(ESP_ARDUINO_VERSION)
  Serial.print(ESP_ARDUINO_VERSION_MAJOR);
  Serial.print('.');
  Serial.print(ESP_ARDUINO_VERSION_MINOR);
  Serial.print('.');
  Serial.println(ESP_ARDUINO_VERSION_PATCH);
#else
  Serial.println("inconnu");
#endif

#if BOARD_SUPPORTS_TLS13
  Serial.println("TLS: core 3.x — TLS 1.3 disponible (requis pour PRIM)");
#else
  Serial.println("TLS: core 2.x — TLS 1.2 seulement");
  Serial.println("!! Mettez a jour le core ESP32 vers 3.x pour PRIM !!");
#endif
}

const char *describeTlsError(int errCode) {
  switch (errCode) {
    case -30592: return "alerte fatale du serveur (souvent version TLS incompatible)";
    case -29184: return "echec verification certificat";
    case -9984:  return "verification certificat X509 echouee";
    case -76:    return "connexion TCP refusee ou timeout";
    case -78:    return "connexion TCP perdue";
    default:     return nullptr;
  }
}

void printTlsError(const char *context) {
  char errBuf[160];
  int errCode = secureClient.lastError(errBuf, sizeof(errBuf));
  Serial.print(context);
  Serial.print(": ");
  Serial.print(errBuf);
  if (errCode != 0) {
    Serial.print(" (code ");
    Serial.print(errCode);
    const char *hint = describeTlsError(errCode);
    if (hint) {
      Serial.print(" — ");
      Serial.print(hint);
    }
    Serial.print(')');
  }
  Serial.println();
}

void initSecureClient() {
  secureClient.setTimeout(20000);
  secureClient.setHandshakeTimeout(20000);

  // PRIM utilise une chaine Google Trust Services (GTS Root R4).
  secureClient.setCACert(PRIM_ROOT_CA);
  tlsUseInsecure = false;
  Serial.println("TLS: verification avec GTS Root R4");
}

bool testHttpsHost(const char *host) {
  secureClient.stop();
  delay(100);

  Serial.print("Test HTTPS ");
  Serial.print(host);
  Serial.print(" ... ");

  if (secureClient.connect(host, 443)) {
    Serial.println("OK");
    secureClient.stop();
    delay(100);
    return true;
  }

  Serial.println("ECHEC");
  printTlsError("  detail");
  secureClient.stop();
  delay(100);
  return false;
}

bool testPrimWithSni() {
  IPAddress ip;
  if (!WiFi.hostByName(PRIM_HOST, ip)) {
    Serial.println("DNS: echec de resolution pour PRIM");
    return false;
  }

  Serial.print("DNS PRIM -> ");
  Serial.println(ip);

  secureClient.stop();
  delay(100);

  Serial.print("Connexion HTTPS ");
  Serial.print(PRIM_HOST);
  Serial.print(" ... ");

  if (secureClient.connect(PRIM_HOST, 443)) {
    Serial.println("OK");
    secureClient.stop();
    delay(100);
    return true;
  }

  Serial.println("ECHEC");
  printTlsError("  detail");

  // Retry once in insecure mode to separate cert vs protocol issues.
  Serial.println("  nouvel essai sans verification certificat...");
  secureClient.setInsecure();
  tlsUseInsecure = true;
  delay(100);

  if (secureClient.connect(PRIM_HOST, 443)) {
    Serial.println("  OK (insecure) — probleme de certificat, pas de version TLS");
    secureClient.stop();
    delay(100);
    return true;
  }

  secureClient.setCACert(PRIM_ROOT_CA);
  tlsUseInsecure = false;

  printTlsError("  detail insecure");
  secureClient.stop();
  delay(100);
  return false;
}

void runNetworkDiagnostics() {
  Serial.println("--- Diagnostic reseau ---");
  printBoardInfo();

  bool googleOk = testHttpsHost("www.google.com");
  bool httpbinOk = testHttpsHost("httpbin.org");
  bool primOk = testPrimWithSni();

  Serial.println("--- Resume ---");
  Serial.print("Google: ");
  Serial.println(googleOk ? "OK" : "ECHEC");
  Serial.print("httpbin: ");
  Serial.println(httpbinOk ? "OK" : "ECHEC");
  Serial.print("PRIM: ");
  Serial.println(primOk ? "OK" : "ECHEC");

  if (!googleOk && !httpbinOk) {
    Serial.println("=> Aucun HTTPS ne fonctionne: routeur ou pare-feu probable.");
    Serial.println("=> Teste avec un partage de connexion telephone.");
  } else if (googleOk && httpbinOk && !primOk) {
    Serial.println("=> HTTPS OK mais PRIM echoue.");
#if !BOARD_SUPPORTS_TLS13
    Serial.println("=> CAUSE PROBABLE: PRIM exige TLS 1.3, votre core ESP32 ne fait que TLS 1.2.");
    Serial.println("=> SOLUTION: Boards Manager -> esp32 by Espressif -> version 3.0.0 ou plus.");
    Serial.println("=> Installez aussi la lib GovoroxSSLClient (Library Manager).");
#else
    Serial.println("=> Verifiez la lib GovoroxSSLClient et la memoire libre.");
    Serial.println("=> PRIM utilise TLS 1.3 uniquement (Cloudflare).");
#endif
  }

  networkReady = primOk;
  Serial.println("-------------------------");
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

bool fetchHttpsGet(const char *host, const String &path, String &body, String &errorMsg) {
  secureClient.stop();
  delay(100);

  if (!secureClient.connect(host, 443)) {
    char errBuf[160];
    int errCode = secureClient.lastError(errBuf, sizeof(errBuf));
    errorMsg = String("Connexion: ") + errBuf;
    if (errCode != 0) {
      errorMsg += " (";
      errorMsg += errCode;
      errorMsg += ')';
    }
    secureClient.stop();
    return false;
  }

  secureClient.printf("GET %s HTTP/1.1\r\n", path.c_str());
  secureClient.printf("Host: %s\r\n", host);
  secureClient.printf("apiKey: %s\r\n", PRIM_API_KEY);
  secureClient.println("Accept: application/json");
  secureClient.println("User-Agent: bus-display-esp32/1.0");
  secureClient.println("Connection: close");
  secureClient.println();

  String statusLine = secureClient.readStringUntil('\n');
  if (statusLine.indexOf("200") < 0) {
    errorMsg = statusLine;
    secureClient.stop();
    return false;
  }

  while (secureClient.connected() || secureClient.available()) {
    String line = secureClient.readStringUntil('\n');
    if (line == "\r" || line.length() <= 1) {
      break;
    }
  }

  body = "";
  unsigned long start = millis();
  while ((secureClient.connected() || secureClient.available()) && millis() - start < 20000) {
    while (secureClient.available()) {
      body += (char)secureClient.read();
    }
    delay(1);
  }

  secureClient.stop();
  return body.length() > 0;
}

bool syncNetworkTime() {
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

  initSecureClient();
  syncNetworkTime();
  runNetworkDiagnostics();

  if (!networkReady) {
    tft.println("HTTPS bloque");
#if !BOARD_SUPPORTS_TLS13
    tft.setTextSize(1);
    tft.println("Core ESP32 3.x requis");
    tft.println("(TLS 1.3 pour PRIM)");
#endif
    Serial.println("Si Google echoue aussi: teste un partage de connexion.");
  } else {
    tft.println("Reseau OK");
  }
  delay(1500);
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

  Serial.print("Memoire libre avant requete: ");
  Serial.println(ESP.getFreeHeap());

  String path = "/marketplace/stop-monitoring?MonitoringRef=" + urlEncode(String(stop.monitoringRef));
  if (strlen(stop.apiLineRef) > 0) {
    path += "&LineRef=" + urlEncode(String(stop.apiLineRef));
  }
  Serial.print("Requete: ");
  Serial.println(path);

  String payload;
  String errorMsg;

  for (int attempt = 1; attempt <= 2; attempt++) {
    if (fetchHttpsGet(PRIM_HOST, path, payload, errorMsg)) {
      break;
    }

    Serial.print("Tentative ");
    Serial.print(attempt);
    Serial.print(" - ");
    Serial.println(errorMsg);

    if (attempt < 2) {
      delay(1000);
    }
  }

  if (payload.length() == 0) {
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(0, 0);
    tft.println(stop.label);
    tft.println("Erreur HTTPS");
    tft.setTextSize(1);
    tft.println(errorMsg);
    return;
  }

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
