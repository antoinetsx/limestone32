#pragma once

#include <Arduino.h>
#include "app_state.h"

#define IDFM_TEXT_DARK  0x25303B
#define IDFM_TEXT_LIGHT 0xFFFFFF

uint16_t color565FromHex(uint32_t hex);
float srgbChannelToLinear(uint8_t channel);
float relativeLuminance(uint32_t hex);
float contrastRatio(float L1, float L2);
uint32_t bestTextColorForBackground(uint32_t bgHex);
uint16_t textColorForTheme(BadgeMode mode, uint32_t accentHex);
