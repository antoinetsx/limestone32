#include "departures.h"
#include "display.h"
#include "http_client.h"
#include "text_utils.h"
#include "time_utils.h"
#include "color_utils.h"
#include <strings.h>
#include <sys/time.h>
#include <WiFi.h>

static const char *lineCodeFromId(const char *lineId) {
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

static BadgeMode parseBadgeMode(const char *modeStr) {
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

static BadgeMode inferBadgeMode(const Stop &stop, const char *code, const char *badgeLabel) {
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

static void copyDestinationLabel(char *dest, size_t destLen, JsonObject departure) {
  const char *direction = departure["shortDestinationLabel"] | "";
  if (direction[0] == '\0') {
    direction = departure["destinationLabel"] | "";
  }
  if (direction[0] == '\0') {
    direction = departure["directionName"] | "Destination";
  }
  strncpy(dest, direction, destLen - 1);
  dest[destLen - 1] = '\0';
}

static bool branchMatches(const char *branchRef, const char *branchFilter) {
  if (branchFilter == nullptr || strlen(branchFilter) == 0) {
    return true;
  }
  return strcmp(branchRef, branchFilter) == 0;
}

static bool lineMatches(const char *lineRef, const char *lineId) {
  if (lineId == nullptr || strlen(lineId) == 0) {
    return true;
  }

  const char *code = lineCodeFromId(lineId);
  if (code[0] == '\0') {
    return true;
  }
  return strstr(lineRef, code) != nullptr;
}

static bool containsIgnoreCase(const char *haystack, const char *needle) {
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

static bool destinationMatches(JsonObject departure, const char *destinationFilter) {
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

static bool isCancelled(JsonObject departure) {
  JsonArray flags = departure["flags"].as<JsonArray>();
  for (JsonVariant flag : flags) {
    if (strcmp(flag.as<const char *>(), "SERVICE_IS_CANCELLED") == 0) {
      return true;
    }
  }
  return false;
}

bool buildDeparturesPath(const Stop &stop, char *path, size_t pathLen) {
  char encodedStopId[128];
  char lineJson[128];
  char encodedLines[256];

  snprintf(lineJson, sizeof(lineJson), "[\"%s\"]", stop.lineId);
  urlEncodeToBuffer(stop.stopId, encodedStopId, sizeof(encodedStopId));
  urlEncodeToBuffer(lineJson, encodedLines, sizeof(encodedLines));

  int n = snprintf(path, pathLen, "/departures/%s?linesIds=%s&getLineNotice=false",
                   encodedStopId, encodedLines);
  return n > 0 && (size_t)n < pathLen;
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
bool fetchAndDisplay(int stopIndex) {
  Stop &stop = stops[stopIndex];
  const uint32_t generation = fetchGeneration;
  gBoardVisible = false;
  drawLoadingScreen(stopIndex);
  if (isFetchStale(generation)) {
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    drawErrorScreen(stopIndex, "Wi-Fi disconnected", "");
    return !isFetchStale(generation);
  }

  const char *path = gDeparturePaths[stopIndex];
  Serial.print("Request: https://");
  Serial.print(LEON_API_HOST);
  Serial.println(path);

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
    err = fetchDeparturesJson(LEON_API_HOST, path, doc, gFilterDoc, httpCode, errorMsg,
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
    drawErrorScreen(stopIndex, "HTTPS error", detail);
    return !isFetchStale(generation);
  }

  if (httpCode != 200) {
    drawErrorScreen(stopIndex, "HTTP error", String(httpCode) + " - " + errorMsg);
    return !isFetchStale(generation);
  }

  if (err || doc.overflowed()) {
    String detail = err ? String(err.c_str()) : "overflow";
    drawErrorScreen(stopIndex, "JSON error", detail);
    Serial.println(err ? err.c_str() : "JsonDocument overflow");
    return !isFetchStale(generation);
  }

  if (isFetchStale(generation)) {
    return false;
  }

  const char *apiError = doc["error"] | doc["message"] | "";
  if (apiError[0] != '\0') {
    drawErrorScreen(stopIndex, "API error", apiError);
    return !isFetchStale(generation);
  }

  JsonArray departures = doc["departures"].as<JsonArray>();
  Serial.print("Parsed departures: ");
  Serial.println(departures.size());

  DepartureRow rows[MAX_DEPARTURES];
  memset(rows, 0, sizeof(rows));
  int shown = 0;
  int skippedBranch = 0;
  int skippedLine = 0;
  int skippedDestination = 0;
  int shownCancelled = 0;
  int skippedPast = 0;

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t nowUtc = time(nullptr);

  for (JsonObject departure : departures) {
    pollNavigationButtons();
    if (isFetchStale(generation)) {
      setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
      tzset();
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
      copyDestinationLabel(rows[shown].destination, sizeof(rows[shown].destination), departure);
      rows[shown].minutes = -1;
      rows[shown].departureEpoch = 0;
      rows[shown].cancelled = true;
      shownCancelled++;
      shown++;
      if (shown >= MAX_DEPARTURES) {
        break;
      }
      continue;
    }

    time_t depEpoch = 0;
    if (!parseIsoUtcEpoch(dateTime, depEpoch)) {
      skippedPast++;
      continue;
    }

    int minutes = 0;
    bool isPast = false;
    departureTimingFromEpoch(depEpoch, nowUtc, atStop, minutes, isPast);
    if (isPast) {
      skippedPast++;
      continue;
    }

    copyDestinationLabel(rows[shown].destination, sizeof(rows[shown].destination), departure);
    rows[shown].minutes = minutes;
    rows[shown].departureEpoch = depEpoch;
    rows[shown].cancelled = false;
    shown++;
    if (shown >= MAX_DEPARTURES) {
      break;
    }
  }

  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  Serial.print("After filter: shown=");
  Serial.print(shown);
  Serial.print(" skipped branch=");
  Serial.print(skippedBranch);
  Serial.print(" line=");
  Serial.print(skippedLine);
  Serial.print(" dest=");
  Serial.print(skippedDestination);
  Serial.print(" cancelled=");
  Serial.print(shownCancelled);
  Serial.print(" past=");
  Serial.println(skippedPast);

  if (isFetchStale(generation)) {
    return false;
  }

  const bool keepHeader = (gLastDrawnStopIndex == stopIndex && gShowingLoading);
  drawDepartureBoard(stopIndex, rows, shown, keepHeader);

  for (int i = 0; i < shown; i++) {
    gDisplayedRows[i] = rows[i];
  }
  gDisplayedCount = shown;
  gDisplayedStopIndex = stopIndex;
  gBoardVisible = true;

  return true;
}
