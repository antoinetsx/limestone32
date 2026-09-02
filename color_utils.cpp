#include "color_utils.h"
#include "app_state.h"
#include <math.h>

uint16_t color565FromHex(uint32_t hex) {
  uint8_t r = (hex >> 16) & 0xFF;
  uint8_t g = (hex >> 8) & 0xFF;
  uint8_t b = hex & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

float srgbChannelToLinear(uint8_t channel) {
  float c = channel / 255.0f;
  if (c <= 0.03928f) {
    return c / 12.92f;
  }
  return powf((c + 0.055f) / 1.055f, 2.4f);
}

float relativeLuminance(uint32_t hex) {
  uint8_t r = (hex >> 16) & 0xFF;
  uint8_t g = (hex >> 8) & 0xFF;
  uint8_t b = hex & 0xFF;
  float rLin = srgbChannelToLinear(r);
  float gLin = srgbChannelToLinear(g);
  float bLin = srgbChannelToLinear(b);
  return 0.2126f * rLin + 0.7152f * gLin + 0.0722f * bLin;
}

float contrastRatio(float L1, float L2) {
  float lighter = (L1 > L2) ? L1 : L2;
  float darker = (L1 > L2) ? L2 : L1;
  return (lighter + 0.05f) / (darker + 0.05f);
}

uint32_t bestTextColorForBackground(uint32_t bgHex) {
  float bgL = relativeLuminance(bgHex);
  float darkL = relativeLuminance(IDFM_TEXT_DARK);
  float lightL = relativeLuminance(IDFM_TEXT_LIGHT);
  float contrastDark = contrastRatio(bgL, darkL);
  float contrastLight = contrastRatio(bgL, lightL);
  return (contrastDark >= contrastLight) ? IDFM_TEXT_DARK : IDFM_TEXT_LIGHT;
}

// WCAG 2.x contrast: bus/RER/metro text on badgeColor; tram text on white center band.
uint16_t textColorForTheme(BadgeMode mode, uint32_t accentHex) {
  uint32_t bgHex = (mode == BADGE_TRAM) ? IDFM_TEXT_LIGHT : accentHex;
  return color565FromHex(bestTextColorForBackground(bgHex));
}
