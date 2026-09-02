#include "text_utils.h"

String urlEncode(const String &value) {
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

size_t urlEncodeToBuffer(const char *value, char *out, size_t outLen) {
  size_t pos = 0;
  if (value == nullptr || outLen == 0) {
    if (outLen > 0) {
      out[0] = '\0';
    }
    return 0;
  }

  for (size_t i = 0; value[i] != '\0'; i++) {
    unsigned char c = (unsigned char)value[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      if (pos + 1 >= outLen) {
        break;
      }
      out[pos++] = (char)c;
    } else {
      if (pos + 3 >= outLen) {
        break;
      }
      snprintf(out + pos, outLen - pos, "%%%02X", c);
      pos += 3;
    }
  }
  out[pos] = '\0';
  return pos;
}

bool utf8Decode(const String &s, size_t &i, uint32_t &cp) {
  if (i >= s.length()) {
    return false;
  }

  uint8_t c = (uint8_t)s.charAt(i);
  if (c < 0x80) {
    cp = c;
    i++;
    return true;
  }

  if ((c & 0xE0) == 0xC0 && i + 1 < s.length()) {
    uint8_t c2 = (uint8_t)s.charAt(i + 1);
    if ((c2 & 0xC0) != 0x80) {
      cp = c;
      i++;
      return true;
    }
    cp = ((uint32_t)(c & 0x1F) << 6) | (c2 & 0x3F);
    i += 2;
    return true;
  }

  if ((c & 0xF0) == 0xE0 && i + 2 < s.length()) {
    uint8_t c2 = (uint8_t)s.charAt(i + 1);
    uint8_t c3 = (uint8_t)s.charAt(i + 2);
    if ((c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) {
      cp = c;
      i++;
      return true;
    }
    cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
    i += 3;
    return true;
  }

  cp = c;
  i++;
  return true;
}

// GLCD fonts lack accents; transliterate UTF-8 to ASCII for the TFT.
String stripAccents(const String &input) {
  String out;
  out.reserve(input.length());

  size_t i = 0;
  while (i < input.length()) {
    uint32_t cp = 0;
    if (!utf8Decode(input, i, cp)) {
      break;
    }

    char single = 0;
    const char *multi = nullptr;

    switch (cp) {
      case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
        single = 'a'; break;
      case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
        single = 'A'; break;
      case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
        single = 'e'; break;
      case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
        single = 'E'; break;
      case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
        single = 'i'; break;
      case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
        single = 'I'; break;
      case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6:
        single = 'o'; break;
      case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6:
        single = 'O'; break;
      case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
        single = 'u'; break;
      case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
        single = 'U'; break;
      case 0x00E7:
        single = 'c'; break;
      case 0x00C7:
        single = 'C'; break;
      case 0x00F1:
        single = 'n'; break;
      case 0x00D1:
        single = 'N'; break;
      case 0x00FD: case 0x00FF:
        single = 'y'; break;
      case 0x00DD:
        single = 'Y'; break;
      case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
        single = '-'; break;
      case 0x0153:
        multi = "oe"; break;
      case 0x0152:
        multi = "OE"; break;
      case 0x00E6:
        multi = "ae"; break;
      case 0x00C6:
        multi = "AE"; break;
      default:
        if (cp < 0x80) {
          out += (char)cp;
        }
        break;
    }

    if (single != 0) {
      out += single;
    } else if (multi != nullptr) {
      out += multi;
    }
  }

  return out;
}
