#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "config.h"
#include "display_layout.h"

#define BUTTON_NEXT     0
#define BUTTON_REFRESH  35
#define TFT_BACKLIGHT   4
#define LEON_API_HOST   "ecrans-api.gwadz.fr"
#define FETCH_INTERVAL_MS 30000
#define STOP_ROTATE_INTERVAL_MS 120000
#define TLS_TIMEOUT_SEC   30
#define CONNECT_ATTEMPT_MS 3000
#define BUTTON_DEBOUNCE_MS 50
#define BODY_IDLE_MS       800
#define BODY_MAX_MS        60000
#define HTTP_READ_CHUNK    512
#define MAX_HTTP_REDIRECTS 3
#define DEPARTURE_PATH_MAX 256
#define DEPARTURE_DEST_MAX 64

struct DepartureRow {
  char destination[DEPARTURE_DEST_MAX];
  int minutes;
  time_t departureEpoch;
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

extern int currentStop;
extern unsigned long lastFetch;
extern unsigned long lastStopShownAt;
extern bool needsRefresh;
extern volatile uint32_t fetchGeneration;
extern WiFiClientSecure *gActiveFetchClient;

extern LineTheme themes[NB_STOPS];
extern char gDeparturePaths[NB_STOPS][DEPARTURE_PATH_MAX];
extern JsonDocument gFilterDoc;

extern DepartureRow gDisplayedRows[MAX_DEPARTURES];
extern int gDisplayedCount;
extern int gDisplayedStopIndex;
extern bool gBoardVisible;
extern bool gPendingLoadingDraw;
extern int gLastDrawnStopIndex;
extern bool gShowingLoading;

extern uint16_t gColorDestText;
extern uint16_t gColorTimeYellow;
extern uint16_t gColorSepGray;

bool isFetchStale(uint32_t generation);
void abortActiveFetchClient();
void releaseActiveFetchClient(WiFiClientSecure &client);
void pollNavigationButtons();
void rotateToNextStop();
void delayUnlessStale(uint32_t generation, unsigned long ms);
