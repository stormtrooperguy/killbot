# Killbot

ESP32 controller for the Killbot robot. Runs as its own WiFi access point and
serves an admin web UI.

## Hardware

| Part | Pin | Notes |
|------|-----|-------|
| WS2812B strip, 45 LEDs (the eye) | GPIO 13 (D13) | Solid red at boot |

## Setup

1. Copy `src/secrets.h.example` to `src/secrets.h` and fill in the AP SSID and
   password (8-63 chars). `secrets.h` is git-ignored.
2. Build and flash:

   ```bash
   pio run -t upload
   ```

3. Join the configured AP and open http://192.168.4.1 (or http://killbot.local).

## Admin UI

- **Eye** toggle: turns the eye LEDs on/off. State is pushed live to all open
  pages via server-sent events (`/events`).

Actions are plain GETs to `/a/<action>` (e.g. `/a/eye` toggles the eye).
