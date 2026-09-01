// ============================================================
//  Afficheur de prochains passages (bus / RER) sur TTGO T-Display
//  Source des donnees : API PRIM (Ile-de-France Mobilites)
// ============================================================
//
//  Bibliotheques necessaires (Tools > Manage Libraries) :
//    - TFT_eSPI (deja installee et configuree)
//    - ArduinoJson (version 7.x recommandee)
//    - ESP32-EasyWolfSSL + wolfssl (core ESP32 >= 3.0, pour PRIM / TLS 1.3)
//
//  IMPORTANT — PRIM exige TLS 1.3 (Cloudflare) :
//    - Installez le core ESP32 >= 3.0.0 (Boards Manager : "esp32 by Espressif Systems")
//    - Le core 2.0.x ne supporte que TLS 1.2 et ne peut pas joindre PRIM
//    - Le mbedTLS precompile du core 3.x n'a PAS TLS 1.3 (sdkconfig) :
//      GovoroxSSLClient ne suffit pas — il reutilise ce mbedTLS.
//    - Solution : WolfSSLClient (ESP32-EasyWolfSSL) avec racines GTS R1 + R4
//    - wolfssl : ajoutez FP_MAX_BITS 8192 dans user_settings.h (voir README lib)
//
// ============================================================

#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include "config.h"   // generated from .env — run: python generate_config.py
#include "prim_ca.h"

#if defined(ESP_ARDUINO_VERSION) && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0))
#define BOARD_SUPPORTS_TLS13 1
#else
#define BOARD_SUPPORTS_TLS13 0
#endif

// mbedTLS du core ESP32 : TLS 1.3 compile ou non ?
#if defined(CONFIG_MBEDTLS_SSL_PROTO_TLS1_3) && CONFIG_MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_HAS_TLS13 1
#else
#define MBEDTLS_HAS_TLS13 0
#endif

#if BOARD_SUPPORTS_TLS13 && MBEDTLS_HAS_TLS13
// Futur core ESP32 avec TLS 1.3 dans mbedTLS : Govorox possible
#include <WiFiClientSecure.h>
#include <SSLClient.h>
#define PRIM_TLS_BACKEND 2
#elif BOARD_SUPPORTS_TLS13
// Core 3.x actuel : mbedTLS sans TLS 1.3 -> WolfSSL
#include <WolfSSLClient.h>
#define PRIM_TLS_BACKEND 1
#else
#include <WiFiClientSecure.h>
#define PRIM_TLS_BACKEND 0
#endif

#if __has_include("esp_wifi.h")
#include <esp_wifi.h>
#endif

#if __has_include("mbedtls/error.h")
#include <mbedtls/error.h>
#endif

#define BUTTON_NEXT   0   // bouton du bas sur la T-Display
#define TFT_BACKLIGHT 4
#define PRIM_HOST     "prim.iledefrance-mobilites.fr"
#define TLS_HANDSHAKE_TIMEOUT_SEC 20

TFT_eSPI tft = TFT_eSPI();

#if PRIM_TLS_BACKEND == 2
WiFiClient wifiTransport;
SSLClient primClient(&wifiTransport);
#elif PRIM_TLS_BACKEND == 1
WolfSSLClient primClient;
#endif

int currentStop = 0;
unsigned long lastFetch = 0;
bool needsRefresh = true;
bool networkReady = false;

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
#if MBEDTLS_HAS_TLS13
  Serial.println("mbedTLS core: TLS 1.3 compile");
#else
  Serial.println("mbedTLS core: TLS 1.2 seulement (TLS 1.3 absent du sdkconfig)");
#endif
#if PRIM_TLS_BACKEND == 2
  Serial.println("PRIM: GovoroxSSLClient (mbedTLS TLS 1.3)");
#elif PRIM_TLS_BACKEND == 1
  Serial.println("PRIM: WolfSSLClient (TLS 1.3)");
#endif
#else
  Serial.println("TLS: core 2.x — TLS 1.2 seulement");
  Serial.println("!! Mettez a jour le core ESP32 vers 3.x pour PRIM !!");
#endif
}

#if PRIM_TLS_BACKEND == 1
void configureDiagClient(WolfSSLClient &client) {
  client.setInsecure();
  client.setTimeout(TLS_HANDSHAKE_TIMEOUT_SEC * 1000);
}
#elif PRIM_TLS_BACKEND == 2
void configureNativeInsecure(WiFiClientSecure &client) {
  client.setInsecure();
  client.setTimeout(TLS_HANDSHAKE_TIMEOUT_SEC * 1000);
  client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SEC);
}
#else
void configureNativeInsecure(WiFiClientSecure &client) {
  client.setInsecure();
  client.setTimeout(TLS_HANDSHAKE_TIMEOUT_SEC * 1000);
  client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SEC);
}
#endif

#if BOARD_SUPPORTS_TLS13
void configurePrimClient(bool verifyCert) {
  primClient.setTimeout(TLS_HANDSHAKE_TIMEOUT_SEC * 1000);
#if PRIM_TLS_BACKEND == 2
  primClient.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_SEC);
#endif
  if (verifyCert) {
    primClient.setCACert(PRIM_ROOT_CA);
  } else {
    primClient.setInsecure();
  }
}

void stopPrimClient() {
  primClient.stop();
  delay(100);
}
#endif

const char *describeTlsError(int errCode) {
  switch (errCode) {
    case -0x7780: return "alerte fatale du serveur (souvent version TLS incompatible)";
    case -0x7200: return "echec verification certificat";
    case -0x2700: return "verification certificat X509 echouee";
    case -0x004C: return "connexion TCP refusee ou timeout";
    case -0x004E: return "connexion TCP perdue";
    case -0x0050: return "connexion reseau interrompue";
    case -2:        return "echec connexion TCP sous-jacente";
    case -1:        return "client TLS non initialise";
    default:        return nullptr;
  }
}

String formatTlsError(int errCode, const char *errBuf) {
  if (errCode == 0) {
    return "echec TLS handshake (Govorox retourne 0 — probablement TLS 1.3 indisponible dans mbedTLS core)";
  }

  String msg;
  if (errBuf != nullptr && errBuf[0] != '\0') {
    msg = errBuf;
  } else {
#if __has_include("mbedtls/error.h")
    char fallback[96];
    mbedtls_strerror(errCode, fallback, sizeof(fallback));
    msg = fallback;
#else
    msg = "erreur TLS";
#endif
  }

  msg += " (code ";
  msg += errCode;
  msg += ')';

  const char *hint = describeTlsError(errCode);
  if (hint != nullptr) {
    msg += " — ";
    msg += hint;
  }
  return msg;
}

#if PRIM_TLS_BACKEND != 1
String readNativeTlsError(WiFiClientSecure &client) {
  char errBuf[160];
  memset(errBuf, 0, sizeof(errBuf));
  int errCode = client.lastError(errBuf, sizeof(errBuf));
  return formatTlsError(errCode, errBuf);
}
#endif

#if BOARD_SUPPORTS_TLS13
String readPrimTlsError() {
#if PRIM_TLS_BACKEND == 2
  char errBuf[160];
  memset(errBuf, 0, sizeof(errBuf));
  int errCode = primClient.lastError(errBuf, sizeof(errBuf));
  return formatTlsError(errCode, errBuf);
#else
  return "echec handshake WolfSSL (verifiez NTP, certificat GTS, FP_MAX_BITS dans wolfssl)";
#endif
}
#endif

void printTlsError(const char *context, const String &errorMsg) {
  Serial.print(context);
  Serial.print(": ");
  Serial.println(errorMsg);
}

bool testHttpsHostNative(const char *host) {
#if PRIM_TLS_BACKEND == 1
  WolfSSLClient client;
  configureDiagClient(client);

  Serial.print("Test HTTPS ");
  Serial.print(host);
  Serial.print(" (WolfSSLClient) ... ");

  if (client.connect(host, 443)) {
    Serial.println("OK");
    client.stop();
    delay(100);
    return true;
  }

  Serial.println("ECHEC");
  client.stop();
  delay(100);
  return false;
#else
  WiFiClientSecure client;
  configureNativeInsecure(client);

  Serial.print("Test HTTPS ");
  Serial.print(host);
  Serial.print(" (WiFiClientSecure) ... ");

  if (client.connect(host, 443)) {
    Serial.println("OK");
    client.stop();
    delay(100);
    return true;
  }

  Serial.println("ECHEC");
  printTlsError("  detail", readNativeTlsError(client));
  client.stop();
  delay(100);
  return false;
#endif
}

#if BOARD_SUPPORTS_TLS13
bool testPrimHost(bool verifyCert) {
  configurePrimClient(verifyCert);
  stopPrimClient();

  Serial.print("Connexion HTTPS ");
  Serial.print(PRIM_HOST);
  if (verifyCert) {
    Serial.print(" (certificat GTS) ... ");
  } else {
    Serial.print(" (sans verification) ... ");
  }

  if (primClient.connect(PRIM_HOST, 443)) {
    Serial.println("OK");
#if PRIM_TLS_BACKEND == 1
    Serial.print("  protocole: ");
    Serial.println(primClient.getProtocolVersion());
#endif
    stopPrimClient();
    return true;
  }

  Serial.println("ECHEC");
  printTlsError("  detail", readPrimTlsError());
  stopPrimClient();
  return false;
}
#endif

bool testPrimWithSni() {
  IPAddress ip;
  if (!WiFi.hostByName(PRIM_HOST, ip)) {
    Serial.println("DNS: echec de resolution pour PRIM");
    return false;
  }

  Serial.print("DNS PRIM -> ");
  Serial.println(ip);

#if BOARD_SUPPORTS_TLS13
  if (testPrimHost(true)) {
    return true;
  }

  Serial.println("  nouvel essai sans verification certificat...");
  if (testPrimHost(false)) {
    Serial.println("  OK (insecure) — probleme de certificat, pas de version TLS");
    return true;
  }
  return false;
#else
  WiFiClientSecure client;
  configureNativeInsecure(client);
  Serial.print("Connexion HTTPS ");
  Serial.print(PRIM_HOST);
  Serial.print(" ... ");
  if (client.connect(PRIM_HOST, 443)) {
    Serial.println("OK");
    client.stop();
    return true;
  }
  Serial.println("ECHEC");
  printTlsError("  detail", readNativeTlsError(client));
  client.stop();
  return false;
#endif
}

void initSecureClient() {
#if BOARD_SUPPORTS_TLS13
  configurePrimClient(true);
#if PRIM_TLS_BACKEND == 2
  Serial.println("TLS PRIM: verification GTS Root R1 + R4 (GovoroxSSLClient)");
#elif PRIM_TLS_BACKEND == 1
  Serial.println("TLS PRIM: verification GTS Root R1 + R4 (WolfSSLClient)");
#endif
#else
  Serial.println("TLS: WiFiClientSecure (TLS 1.2 seulement)");
#endif
}

void runNetworkDiagnostics() {
  Serial.println("--- Diagnostic reseau ---");
  printBoardInfo();

  bool googleOk = testHttpsHostNative("www.google.com");
  bool httpbinOk = testHttpsHostNative("httpbin.org");
  bool primOk = testPrimWithSni();

  Serial.println("--- Resume ---");
  Serial.print("Google: ");
  Serial.println(googleOk ? "OK" : "ECHEC");
  Serial.print("httpbin: ");
  Serial.println(httpbinOk ? "OK" : "ECHEC");
  Serial.print("PRIM: ");
  Serial.println(primOk ? "OK" : "ECHEC");

  if (!googleOk && !httpbinOk) {
    Serial.println("=> Aucun HTTPS ne fonctionne: routeur, pare-feu, ou core ESP32.");
    Serial.println("=> Teste avec un partage de connexion telephone.");
  } else if (googleOk && httpbinOk && !primOk) {
    Serial.println("=> HTTPS general OK mais PRIM echoue.");
#if !BOARD_SUPPORTS_TLS13
    Serial.println("=> CAUSE PROBABLE: PRIM exige TLS 1.3, votre core ESP32 ne fait que TLS 1.2.");
    Serial.println("=> SOLUTION: Boards Manager -> esp32 by Espressif -> version 3.0.0 ou plus.");
    Serial.println("=> Installez ESP32-EasyWolfSSL + wolfssl (voir entete du sketch).");
#elif PRIM_TLS_BACKEND == 1
    Serial.println("=> Verifiez wolfssl (FP_MAX_BITS 8192), NTP/heure, et ESP32-EasyWolfSSL.");
    Serial.println("=> PRIM utilise TLS 1.3 uniquement (Cloudflare).");
#else
    Serial.println("=> Verifiez GovoroxSSLClient, NTP/heure systeme, et la memoire libre.");
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
#if BOARD_SUPPORTS_TLS13
  configurePrimClient(true);
  stopPrimClient();

  if (!primClient.connect(host, 443)) {
    errorMsg = "Connexion: " + readPrimTlsError();
    stopPrimClient();
    return false;
  }

  primClient.printf("GET %s HTTP/1.1\r\n", path.c_str());
  primClient.printf("Host: %s\r\n", host);
  primClient.printf("apiKey: %s\r\n", PRIM_API_KEY);
  primClient.println("Accept: application/json");
  primClient.println("User-Agent: bus-display-esp32/1.0");
  primClient.println("Connection: close");
  primClient.println();

  String statusLine = primClient.readStringUntil('\n');
  if (statusLine.indexOf("200") < 0) {
    errorMsg = statusLine;
    stopPrimClient();
    return false;
  }

  while (primClient.connected() || primClient.available()) {
    String line = primClient.readStringUntil('\n');
    if (line == "\r" || line.length() <= 1) {
      break;
    }
  }

  body = "";
  unsigned long start = millis();
  while ((primClient.connected() || primClient.available()) && millis() - start < 20000) {
    while (primClient.available()) {
      body += (char)primClient.read();
    }
    delay(1);
  }

  stopPrimClient();
  return body.length() > 0;
#else
  WiFiClientSecure client;
  configureNativeInsecure(client);

  if (!client.connect(host, 443)) {
    errorMsg = "Connexion: " + readNativeTlsError(client);
    client.stop();
    return false;
  }

  client.printf("GET %s HTTP/1.1\r\n", path.c_str());
  client.printf("Host: %s\r\n", host);
  client.printf("apiKey: %s\r\n", PRIM_API_KEY);
  client.println("Accept: application/json");
  client.println("User-Agent: bus-display-esp32/1.0");
  client.println("Connection: close");
  client.println();

  String statusLine = client.readStringUntil('\n');
  if (statusLine.indexOf("200") < 0) {
    errorMsg = statusLine;
    client.stop();
    return false;
  }

  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() <= 1) {
      break;
    }
  }

  body = "";
  unsigned long start = millis();
  while ((client.connected() || client.available()) && millis() - start < 20000) {
    while (client.available()) {
      body += (char)client.read();
    }
    delay(1);
  }

  client.stop();
  return body.length() > 0;
#endif
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

#if PRIM_TLS_BACKEND == 1
  if (!WolfSSLClient::initialize()) {
    Serial.println("ERREUR: initialisation WolfSSL echouee");
  }
#endif

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
