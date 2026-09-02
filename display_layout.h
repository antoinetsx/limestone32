#pragma once

// Screen geometry (240x135 landscape, rotation 1)
static const int SCREEN_W = 240;
static const int SCREEN_H = 135;
static const int HEADER_H = 38;
static const int BODY_Y = HEADER_H;
static const int BORDER_H = 5;
static const int CONTENT_Y = BODY_Y + BORDER_H;
static const int LEFT_W = 156;
static const int RIGHT_W = 84;
static const int RIGHT_X = LEFT_W;
static const int ROW_H = 45;
static const int ROW1_Y = 43;
static const int SEP_Y = 88;
static const int SEP_H = 2;
static const int ROW2_Y = 90;
static const int BADGE_X = 5;
static const int BADGE_Y = 5;
static const int BADGE_SIZE = 28;
static const int BADGE_BUS_W = 28;
static const int BADGE_BUS_H = 18;
static const int BADGE_GAP = 5;
static const int BADGE_RADIUS = 4;
static const int BADGE_CENTER_Y = BADGE_Y + BADGE_SIZE / 2;
static const int STATION_X = BADGE_X + BADGE_SIZE + BADGE_GAP;
static const int STATION_X_BUS = BADGE_X + BADGE_BUS_W + BADGE_GAP;
static const int STATION_TEXT_SIZE = 2;
static const int STATION_TEXT_H = 8 * STATION_TEXT_SIZE;
static const int STATION_Y = BADGE_CENTER_Y - STATION_TEXT_H / 2;
static const int STOP_INDICATOR_MARGIN = 6;
static const int STOP_DOTS_PER_ROW_MAX = 4;
static const int STOP_DOT_SPACING = 8;
static const int STOP_DOT_ROW_GAP = 8;
static const int STOP_DOT_ACTIVE_R = 4;
static const int STOP_DOT_INACTIVE_R = 2;
static const int STOP_INDICATOR_ZONE_W =
    (STOP_DOTS_PER_ROW_MAX - 1) * STOP_DOT_SPACING + STOP_DOT_ACTIVE_R * 2;
static const int MINUTES_NUM_SIZE = 3;
static const int MINUTES_SUFFIX_SIZE = 1;
static const int DEST_PAD_X = 5;
static const int DEST_MAX_W = 149;
static const int MAX_DEPARTURES = 2;
