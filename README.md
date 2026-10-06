# Killbot

ESP32 controller for the Killbot robot. Runs as its own WiFi access point and
serves an admin web UI.

## Hardware

| Part | Pin | Notes |
|------|-----|-------|
| WS2812B strip, 46 LEDs (the eye) | GPIO 13 (D13) | Solid red at boot |

Idle red brightness is set by `RED_LEVEL` in `src/main.cpp`; master brightness
stays at 255 so the animation's white flash runs at full output.

## WiFi channel

`WIFI_CHANNEL` in `src/main.cpp` sets the AP's 2.4GHz channel (default 11).
Use only **1, 6 or 11** — they are the only non-overlapping channels, and a
partially overlapping channel is worse than sharing a busy one. Channels 12-14
are restricted in some regions and may stop clients associating.

To judge a venue on site, set `CHANNEL_SCAN_ON_BOOT` to 1, flash, and read the
serial log: it lists how many APs are on each channel and the strongest signal
on each, so you can pick the quietest of 1/6/11. Set it back to 0 afterwards —
it adds about 2s to boot.

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
