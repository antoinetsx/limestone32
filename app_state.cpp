#include "app_state.h"

int currentStop = 0;
unsigned long lastFetch = 0;
unsigned long lastStopShownAt = 0;
bool needsRefresh = true;
volatile uint32_t fetchGeneration = 0;
WiFiClientSecure *gActiveFetchClient = nullptr;

LineTheme themes[NB_STOPS];
char gDeparturePaths[NB_STOPS][DEPARTURE_PATH_MAX];
JsonDocument gFilterDoc;

DepartureRow gDisplayedRows[MAX_DEPARTURES];
int gDisplayedCount = 0;
int gDisplayedStopIndex = -1;
bool gBoardVisible = false;
bool gPendingLoadingDraw = false;
int gLastDrawnStopIndex = -1;
bool gShowingLoading = false;

uint16_t gColorDestText;
uint16_t gColorTimeYellow;
uint16_t gColorSepGray;

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

void rotateToNextStop() {
  fetchGeneration++;
  abortActiveFetchClient();
  currentStop = (currentStop + 1) % NB_STOPS;
  needsRefresh = true;
  gBoardVisible = false;
  gPendingLoadingDraw = true;
  lastStopShownAt = millis();
}

// Poll NEXT/REFRESH during blocking fetch I/O so stop switches are instant.
void pollNavigationButtons() {
  static bool lastNextState = HIGH;
  bool nextState = digitalRead(BUTTON_NEXT);
  if (nextState == LOW && lastNextState == HIGH) {
    rotateToNextStop();
    unsigned long debounceStart = millis();
    while (millis() - debounceStart < BUTTON_DEBOUNCE_MS) {
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
    gBoardVisible = false;
    gPendingLoadingDraw = true;
    unsigned long debounceStart = millis();
    while (millis() - debounceStart < BUTTON_DEBOUNCE_MS) {
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
