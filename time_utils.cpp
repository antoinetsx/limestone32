#include "time_utils.h"
#include <sys/time.h>

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

// Parse ISO8601 UTC datetime to epoch. Caller must have TZ=UTC active.
bool parseIsoUtcEpoch(const char *isoUtc, time_t &outEpoch) {
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

  outEpoch = mktime(&departure);
  return outEpoch != (time_t)-1;
}

void departureTimingFromEpoch(time_t depEpoch, time_t nowEpoch, bool isAtStop, int &minutes,
                              bool &isPast) {
  long secondsUntil = difftime(depEpoch, nowEpoch);
  if (isAtStop) {
    isPast = secondsUntil < -120;
  } else {
    isPast = secondsUntil < -300;
  }
  if (secondsUntil < 0) {
    minutes = 0;
  } else {
    minutes = (int)((secondsUntil + 59) / 60);
  }
}
