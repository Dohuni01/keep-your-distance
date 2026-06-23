# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

UWB (Ultra-Wideband) safety distance detection system using Makerfabs ESP32 UWB DW3000 modules. The system measures the real-time distance between a fixed Anchor and a mobile Tag, and triggers a relay when the tag enters a danger zone (≤ 3.00 m).

## Build & Flash

This project uses the Arduino IDE. There is no build script — compile and upload via the IDE or `arduino-cli`.

```bash
# Using arduino-cli (install separately)
arduino-cli compile --fqbn esp32:esp32:esp32 Anchor/ancher_v1/ancher_v1.ino
arduino-cli upload  --fqbn esp32:esp32:esp32 -p <PORT> Anchor/ancher_v1/ancher_v1.ino

arduino-cli compile --fqbn esp32:esp32:esp32 Tag/tag_v1/tag_v1.ino
arduino-cli upload  --fqbn esp32:esp32:esp32 -p <PORT> Tag/tag_v1/tag_v1.ino
```

Serial monitor baud rate: **115200**

Required libraries: `dw3000` (Makerfabs DW3000 Arduino library), `SPI` (bundled with ESP32 core).

## Architecture

### Roles

| Sketch | Role | Key output |
|---|---|---|
| `Anchor/ancher_v1/ancher_v1.ino` | Fixed device — initiates ranging, calculates distance, drives relay | GPIO 26 (relay) |
| `Tag/tag_v1/tag_v1.ino` | Mobile device worn by worker — passive responder only | Serial "Responded" |

### Two-Way Ranging (TWR) Protocol

1. **Anchor** sends a poll frame (`WAVE` + `0xE0`) every 500 ms.
2. **Tag** receives the poll, waits 800 µs, then sends a response frame (`VEWA` + `0xE1`) embedding its own RX and TX timestamps.
3. **Anchor** extracts the four timestamps (poll TX, response RX, poll RX @ tag, response TX @ tag) and computes:
   ```
   ToF = ((RTD_init − RTD_resp × (1 − clockOffsetRatio)) / 2) × DWT_TIME_UNITS
   distance = ToF × SPEED_OF_LIGHT
   ```

Both devices use identical DW3000 radio config: **channel 5**, preamble 128, PAC8, **6.8 Mbps**, STS off.

### Relay Safety Logic (Anchor only)

| Condition | Action |
|---|---|
| distance ≤ 3.00 m | Relay ON immediately |
| distance ≥ 3.20 m × 3 consecutive readings | Relay OFF |
| No valid range × 20 consecutive polls | Relay OFF (fail-safe) |

`outsideCount` must **not** be reset on a no-range tick — doing so would prevent the relay from ever turning off while range is intermittent (see comment in `noRangeTick()`).

### Pin Assignment (both boards)

| Pin | Function |
|---|---|
| GPIO 4 | SPI SS (chip select) |
| GPIO 27 | DW3000 RST |
| GPIO 34 | DW3000 IRQ |
| GPIO 26 | Relay output (Anchor only) |

SPI clock: 16 MHz, MSBFIRST, SPI_MODE0. Antenna delays are both set to **16385** (tune per physical calibration if distance accuracy drifts).
