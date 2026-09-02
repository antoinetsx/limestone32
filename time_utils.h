#pragma once

#include <Arduino.h>
#include <time.h>

bool syncNetworkTime();
bool parseIsoUtcEpoch(const char *isoUtc, time_t &outEpoch);
void departureTimingFromEpoch(time_t depEpoch, time_t nowEpoch, bool isAtStop, int &minutes,
                              bool &isPast);
