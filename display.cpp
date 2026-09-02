#include "display.h"
#include "display_layout.h"
#include "color_utils.h"
#include "text_utils.h"
#include "time_utils.h"

TFT_eSPI tft = TFT_eSPI();

void setupDisplayColors() {
  gColorDestText = color565FromHex(0x4D565F);
  gColorTimeYellow = color565FromHex(0xFEC107);
  gColorSepGray = color565FromHex(0xA9B1B9);
}

String truncateToWidth(const String &text, int maxWidth, uint8_t textSize) {
  tft.setTextSize(textSize);
  if ((int)tft.textWidth(text) <= maxWidth) {
    return text;
  }

  String truncated = text;
  while (truncated.length() > 0 && (int)tft.textWidth(truncated + "...") > maxWidth) {
    truncated.remove(truncated.length() - 1);
  }
  return truncated + "...";
}

static void drawBadgeTextCentered(int x, int y, int w, int h, const char *label, uint8_t textSize,
                                  uint16_t textColor, uint16_t bgColor) {
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(textSize);
  int textW = tft.textWidth(label);
  int textH = 8 * textSize;
  tft.setCursor(x + (w - textW) / 2, y + (h - textH) / 2);
  tft.print(label);
}

static void drawBadgeBus(int x, int y, int w, int h, uint16_t accent, const char *label,
                         uint16_t textColor) {
  tft.fillRect(x, y, w, h, accent);
  drawBadgeTextCentered(x, y, w, h, label, 1, textColor, accent);
}

static void drawBadgeRer(int x, int y, int w, int h, uint16_t accent, const char *label,
                         uint16_t textColor) {
  tft.fillRoundRect(x, y, w, h, BADGE_RADIUS, accent);
  drawBadgeTextCentered(x, y, w, h, label, 2, textColor, accent);
}

static void drawBadgeMetro(int x, int y, int w, int h, uint16_t accent, const char *label,
                           uint16_t textColor) {
  int cx = x + w / 2;
  int cy = y + h / 2;
  tft.fillCircle(cx, cy, w / 2, accent);
  drawBadgeTextCentered(x, y, w, h, label, 2, textColor, accent);
}

static void drawBadgeTram(int x, int y, int w, int h, uint16_t accent, const char *label,
                        uint16_t textColor) {
  const int bandH = 6;
  tft.fillRect(x, y, w, bandH, accent);
  tft.fillRect(x, y + h - bandH, w, bandH, accent);
  tft.fillRect(x, y + bandH, w, h - (2 * bandH), TFT_WHITE);
  drawBadgeTextCentered(x, y, w, h, label, 1, textColor, TFT_WHITE);
}

static void drawLineBadge(const LineTheme &theme) {
  const char *label = theme.badgeLabel;
  switch (theme.mode) {
    case BADGE_RER:
      drawBadgeRer(BADGE_X, BADGE_Y, BADGE_SIZE, BADGE_SIZE, theme.accent, label,
                   theme.textColor);
      break;
    case BADGE_METRO:
      drawBadgeMetro(BADGE_X, BADGE_Y, BADGE_SIZE, BADGE_SIZE, theme.accent, label,
                     theme.textColor);
      break;
    case BADGE_TRAM:
      drawBadgeTram(BADGE_X, BADGE_Y, BADGE_SIZE, BADGE_SIZE, theme.accent, label,
                    theme.textColor);
      break;
    case BADGE_BUS:
    default: {
      int busY = BADGE_CENTER_Y - BADGE_BUS_H / 2;
      drawBadgeBus(BADGE_X, busY, BADGE_BUS_W, BADGE_BUS_H, theme.accent, label,
                   theme.textColor);
      break;
    }
  }
}

static void drawHeader(const Stop &stop, const LineTheme &theme) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, TFT_WHITE);
  drawLineBadge(theme);

  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(STATION_TEXT_SIZE);
  int stationX = (theme.mode == BADGE_BUS) ? STATION_X_BUS : STATION_X;
  String station = truncateToWidth(stripAccents(String(stop.label)), SCREEN_W - stationX - 5,
                                   STATION_TEXT_SIZE);
  tft.setCursor(stationX, STATION_Y);
  tft.print(station);

  tft.fillRect(0, BODY_Y, SCREEN_W, BORDER_H, theme.accent);
}

void drawMinutesCell(int rowY, int minutes) {
  tft.fillRect(RIGHT_X, rowY, RIGHT_W, ROW_H, TFT_BLACK);

  if (minutes < 0) {
    return;
  }

  char numBuf[8];
  snprintf(numBuf, sizeof(numBuf), "%d", minutes);

  uint8_t numSize = MINUTES_NUM_SIZE;
  uint8_t minSize = MINUTES_SUFFIX_SIZE;

  tft.setTextColor(gColorTimeYellow, TFT_BLACK);
  tft.setTextSize(numSize);
  int numW = tft.textWidth(numBuf);
  tft.setTextSize(minSize);
  int minW = tft.textWidth("min");
  int totalW = numW + minW;

  if (totalW > RIGHT_W) {
    numSize = 3;
    minSize = 2;
    tft.setTextSize(numSize);
    numW = tft.textWidth(numBuf);
    tft.setTextSize(minSize);
    minW = tft.textWidth("min");
    totalW = numW + minW;
  }

  const int numH = 8 * numSize;
  const int minH = 8 * minSize;
  const int startX = RIGHT_X + (RIGHT_W - totalW) / 2;
  const int numY = rowY + (ROW_H - numH) / 2;
  const int minY = numY + numH - minH;

  tft.setTextSize(numSize);
  tft.setCursor(startX, numY);
  tft.print(numBuf);

  tft.setTextSize(minSize);
  tft.setCursor(startX + numW, minY);
  tft.print("min");
}

static void drawDestinationCell(int rowY, const char *destination) {
  tft.fillRect(0, rowY, LEFT_W, ROW_H, TFT_WHITE);

  tft.setTextColor(gColorDestText, TFT_WHITE);
  tft.setTextSize(2);
  String dest = truncateToWidth(stripAccents(String(destination)), DEST_MAX_W, 2);
  tft.setCursor(DEST_PAD_X, rowY + 14);
  tft.print(dest);
}

static void drawRowSeparator() {
  tft.fillRect(0, SEP_Y, LEFT_W, SEP_H, TFT_WHITE);
  tft.fillRect(DEST_PAD_X, SEP_Y, LEFT_W - (DEST_PAD_X * 2), 1, gColorSepGray);
  tft.fillRect(RIGHT_X, SEP_Y, RIGHT_W, SEP_H, TFT_BLACK);
  tft.fillRect(RIGHT_X, SEP_Y, RIGHT_W, 1, TFT_WHITE);
}

void drawDepartureBoard(int stopIndex, const DepartureRow *rows, int count, bool keepHeader) {
  const Stop &stop = stops[stopIndex];
  const LineTheme &theme = themes[stopIndex];

  if (!keepHeader) {
    tft.fillScreen(TFT_WHITE);
    drawHeader(stop, theme);
    gLastDrawnStopIndex = stopIndex;
  } else {
    tft.fillRect(0, CONTENT_Y, LEFT_W, SCREEN_H - CONTENT_Y, TFT_WHITE);
    tft.fillRect(RIGHT_X, CONTENT_Y, RIGHT_W, SCREEN_H - CONTENT_Y, TFT_BLACK);
  }

  if (count >= 1) {
    drawDestinationCell(ROW1_Y, rows[0].destination);
    drawMinutesCell(ROW1_Y, rows[0].minutes);
  } else {
    drawDestinationCell(ROW1_Y, "No departures");
    drawMinutesCell(ROW1_Y, -1);
  }

  drawRowSeparator();

  if (count >= 2) {
    drawDestinationCell(ROW2_Y, rows[1].destination);
    drawMinutesCell(ROW2_Y, rows[1].minutes);
  } else {
    drawDestinationCell(ROW2_Y, "");
    drawMinutesCell(ROW2_Y, -1);
  }

  gShowingLoading = false;
}

void drawLoadingScreen(int stopIndex) {
  const Stop &stop = stops[stopIndex];
  const LineTheme &theme = themes[stopIndex];

  if (gLastDrawnStopIndex != stopIndex) {
    tft.fillScreen(TFT_WHITE);
    drawHeader(stop, theme);
    gLastDrawnStopIndex = stopIndex;
  } else {
    tft.fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, TFT_WHITE);
  }

  tft.setTextColor(gColorDestText, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 14);
  tft.print("Loading...");
  gShowingLoading = true;
}

void drawErrorScreen(int stopIndex, const char *title, const String &detail) {
  const Stop &stop = stops[stopIndex];
  const LineTheme &theme = themes[stopIndex];

  if (gLastDrawnStopIndex != stopIndex) {
    tft.fillScreen(TFT_WHITE);
    drawHeader(stop, theme);
    gLastDrawnStopIndex = stopIndex;
  } else {
    tft.fillRect(0, CONTENT_Y, SCREEN_W, SCREEN_H - CONTENT_Y, TFT_WHITE);
  }

  tft.setTextColor(TFT_BLACK, TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 8);
  tft.println(stripAccents(String(title)));
  tft.setTextSize(1);
  tft.setCursor(DEST_PAD_X, ROW1_Y + 28);
  tft.println(stripAccents(detail));
  gShowingLoading = false;
}

void updateMinuteCountdown() {
  if (!gBoardVisible || gDisplayedCount <= 0 || gDisplayedStopIndex < 0) {
    return;
  }

  time_t nowEpoch = time(nullptr);
  if (nowEpoch == (time_t)-1) {
    return;
  }

  static const int rowYs[MAX_DEPARTURES] = {ROW1_Y, ROW2_Y};
  for (int i = 0; i < gDisplayedCount; i++) {
    if (gDisplayedRows[i].departureEpoch == 0) {
      continue;
    }
    int minutes = 0;
    bool isPast = false;
    departureTimingFromEpoch(gDisplayedRows[i].departureEpoch, nowEpoch,
                             false, minutes, isPast);
    if (minutes != gDisplayedRows[i].minutes) {
      gDisplayedRows[i].minutes = minutes;
      drawMinutesCell(rowYs[i], minutes);
    }
  }
}
