# Killbot

ESP32 controller for the Killbot robot. Runs as its own WiFi access point and
serves an admin web UI.

## Hardware

| Part | Pin | Notes |
|------|-----|-------|
| WS2812B strip, 46 LEDs (the eye) | GPIO 13 (D13) | Solid red at boot |

Idle red brightness is set by `RED_LEVEL` in `src/main.cpp`; master brightness
stays at 255 so the animation's white flash runs at full output.

## Setup

1. Copy `src/secrets.h.example` to `src/secrets.h` and fill in the AP SSID and
   password (8-63 chars). `secrets.h` is git-ignored.
2. Build and flash:

   ```bash
   pio run -t upload
   ```

3. Join the configured AP and open http://192.168.4.1 (or http://killbot.local).

A captive-portal DNS server answers every lookup with the AP's own address, and
the OS connectivity-check URLs (`/generate_204`, `/hotspot-detect.html`,
`/ncsi.txt`, ...) are answered as "online". Without this, phones and tablets flag
the network as having no internet and may route traffic over cellular instead,
which makes the admin page stop responding.

## Admin UI

- **LASER** button: two white comets chase in from both ends of the eye
  over the red, meet in the middle, then the eye flashes white twice and
  returns to red. Re-presses during the animation are ignored.
- **Eye** toggle: turns the eye LEDs on/off. State is pushed live to all open
  pages via server-sent events (`/events`).

Actions are plain GETs to `/a/<action>` (`/a/eye` toggles the eye, `/a/laser` fires the laser animation).
