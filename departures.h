#pragma once

#include <ArduinoJson.h>
#include "app_state.h"
#include "config.h"

LineTheme themeForStop(const Stop &stop);
bool buildDeparturesPath(const Stop &stop, char *path, size_t pathLen);
void configureDeparturesFilter(JsonDocument &filter);
bool fetchAndDisplay(int stopIndex);
