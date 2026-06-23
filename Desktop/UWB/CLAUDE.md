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

1. **Anchor** polls one tag at a time, round-robin (`RNG_DELAY_MS` = 150 ms per tag; full cycle = 150 ms × N tags = 600 ms for 4 tags).
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

> ⚠️ The ENTER decision currently uses the 5-sample moving average, which adds ~3 s of latency (review finding C1). The intended fix (Phase A) is asymmetric filtering: enter on raw/median(3), exit on median(5). See ROADMAP.md.

## Known Issues & Current Priorities

A 2026-06-23 architecture review re-sequenced the roadmap so that **safety correctness comes before monitoring features** (GPS/backend). Before changing firmware, read `PROJECT_STATUS.md` (critical findings table) and `ROADMAP.md` (Phase A–G). `CLAUDE_CONTEXT.md` holds the non-negotiable principles.

Critical, fix-first items:

| ID | Issue | Where | Phase |
|---|---|---|---|
| C1 | 5-sample moving average ≈ 3 s detection lag (machine stops late) | `ancher_v1.ino` `applyFilter` | A |
| C2 | Tag busy-waits with no RX timeout → reboots every 10 s when anchor absent | `tag_v1.ino` loop (no `dwt_setrxtimeout`) | A |
| C3 | No anchor-to-anchor RF coordination → collisions with 5 anchors | protocol (single channel 5) | B |
| C4 | Timestamps are `millis()` (uptime), not epoch | all MQTT payloads | C |
| C5 | Uncalibrated antenna delay (±10–30 cm) on a 3.00 m threshold | `TX/RX_ANT_DLY` | A |

Authority chain (do not violate): **anchor decides & timestamps → broker → backend persists → dashboard displays.** The dashboard must never re-derive danger events from distance.

### Pin Assignment (both boards)

| Pin | Function |
|---|---|
| GPIO 4 | SPI SS (chip select) |
| GPIO 27 | DW3000 RST |
| GPIO 34 | DW3000 IRQ |
| GPIO 26 | Relay output (Anchor only) |

SPI clock: 16 MHz, MSBFIRST, SPI_MODE0. Antenna delays are both set to **16385** (tune per physical calibration if distance accuracy drifts).
