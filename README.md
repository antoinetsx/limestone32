# Limestone32

ESP32 departure board for **TTGO T-Display** (240×135). Shows upcoming buses, RER, metro, and tram times from the [Leon API](https://ecrans-api.gwadz.fr).

## Hardware

- LilyGO TTGO T-Display (ESP32, ST7789 240×135)

## Setup

1. **Clone** this repo and open the `limestone32` folder in the Arduino IDE (the folder name must match `limestone32.ino`).

2. **Arduino libraries** (Tools → Manage Libraries):
   - [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) — configure for your T-Display in `User_Setup` / board profile as usual
   - [ArduinoJson](https://arduinojson.org/) 7.x

3. **WiFi and stops**
   - Copy `.env.example` to `.env`
   - Edit `.env` with your WiFi credentials and stop lines (see comments in `.env.example`)

4. **Generate firmware config**
   ```bash
   python generate_config.py
   ```
   This writes `config.h` and `config.cpp` (gitignored) from `.env`.

5. **Flash** `limestone32.ino` to the board.

## Project layout

| File | Role |
|------|------|
| `limestone32.ino` | Main sketch |
| `generate_config.py` | Builds `config.h` / `config.cpp` from `.env` |
| `departures.cpp` | Leon API fetch and parsing |
| `display.cpp` | TFT UI and badges |
| `http_client.cpp` | HTTPS client |

## License

See repository for license terms.
