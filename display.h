#pragma once

#include <TFT_eSPI.h>
#include <Arduino.h>
#include "app_state.h"
#include "config.h"

extern TFT_eSPI tft;

void setupDisplayColors();
String truncateToWidth(const String &text, int maxWidth, uint8_t textSize);
void drawLoadingScreen(int stopIndex);
void drawErrorScreen(int stopIndex, const char *title, const String &detail);
void drawDepartureBoard(int stopIndex, const DepartureRow *rows, int count, bool keepHeader);
void drawMinutesCell(int rowY, int minutes);
void updateMinuteCountdown();
