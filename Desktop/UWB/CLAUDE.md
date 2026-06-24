# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

UWB (Ultra-Wideband) safety distance detection system using Makerfabs ESP32 UWB DW3000 modules. The system measures the real-time distance between a fixed Anchor and a mobile Tag, and triggers a relay when the tag enters a danger zone (≤ 3.00 m).

## Build & Flash

This project uses the Arduino IDE. There is no build script — compile and upload via the IDE or `arduino-cli`.

```powershell
# Bundled arduino-cli (no standalone install needed)
$cli = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$lib = "$env:USERPROFILE\Documents\Arduino\libraries"
& $cli compile --fqbn esp32:esp32:esp32 --libraries $lib Anchor/ancher_v1
& $cli compile --fqbn esp32:esp32:esp32 --libraries $lib Tag/tag_v1
```

Serial monitor baud rate: **115200**

Required libraries: `dw3000` (Makerfabs DW3000 Arduino library), `SPI` (bundled), `PubSubClient` (Nick O'Leary — Anchor only).

## Architecture

### Roles

| Sketch | Role | Key output |
|---|---|---|
| `Anchor/ancher_v1/ancher_v1.ino` | Fixed device — initiates ranging, calculates distance, drives relay | GPIO 26 (relay) |
| `Tag/tag_v1/tag_v1.ino` | Mobile device worn by worker — passive responder only | Serial "Responded" |

### Two-Way Ranging (TWR) Protocol

1. **Anchor** polls one tag at a time, round-robin (`RNG_DELAY_MS` = 150 ms per tag).
2. **Tag** receives the poll, waits 1500 µs (`POLL_RX_TO_RESP_TX_DLY_UUS`), then sends a response frame embedding its own RX and TX timestamps.
3. **Anchor** extracts the four timestamps and computes:
   ```
   ToF = ((RTD_init − RTD_resp × (1 − clockOffsetRatio)) / 2) × DWT_TIME_UNITS
   distance = ToF × SPEED_OF_LIGHT
   ```

Both devices use identical DW3000 radio config: **channel 5**, preamble 128, PAC8, **6.8 Mbps**, STS off.

### Frame Layout (both devices must match exactly)

```
poll frame    : 12 bytes
response frame: 20 bytes

Index  Field
[0]    0x41  (frame ctrl lo)
[1]    0x88  (frame ctrl hi)
[2]    SEQ   (sequence number, zeroed before compare)
[3]    0xCA  (PAN ID lo)
[4]    0xDE  (PAN ID hi)
[5]    ANCHOR_ID
[6]    TAG_ID
[7]    'U'   (FRAME_MARKER0 — sync marker)
[8]    'W'   (FRAME_MARKER1 — sync marker)
[9]    0xE0  (MSG_TYPE_POLL) or 0xE1 (MSG_TYPE_RESP)
[10-13] poll_rx_ts   (response only)
[14-17] resp_tx_ts   (response only)
[10-11] FCS placeholder (poll only)
[18-19] FCS placeholder (response only)
```

> ⚠️ Do NOT change frame indices without updating BOTH sketches simultaneously.
> The 'U'/'W' sync markers at [7][8] were the key fix that resolved "no range" on hardware.

### DW3000 Initialization Sequence

Both devices call this before any TX/RX:
```cpp
dwt_softreset();          // required for reliable re-init
delay(2);
dwt_checkidlerc();        // wait for IDLE
dwt_initialise(DWT_DW_INIT);
dwt_configure(&config);
dwt_configuretxrf(&txconfig_options);
dwt_setrxantennadelay(RX_ANT_DLY);
dwt_settxantennadelay(TX_ANT_DLY);
clearUwbStatus();         // clear all status bits before first RX/TX
```

SPI speed: **7 MHz** (not 16 MHz — 7 MHz matches Makerfabs examples and is more stable).

### Relay Safety Logic (Anchor only)

| Condition | Action |
|---|---|
| enterDist (median-3) ≤ 3.00 m | Relay ON immediately |
| exitDist (median-5) ≥ 3.20 m × 3 consecutive readings | Relay OFF |
| No valid range × 20 consecutive polls | Relay OFF (fail-safe) |

Asymmetric filter (C1 fix): enter uses median(3) for fast response, exit uses median(5) for debounce.

`outsideCount` must **not** be reset on a no-range tick.

### Relay Safety Logic — Phase B (adaptive polling)

Tags with 10 consecutive no-range (absent) are moved to 1-in-5 slow polling to shorten the active-tag cycle. Tags that are `inDanger` are **never** slow-polled (safety invariant).

## Known Issues & Current Priorities

Critical findings from 2026-06-23 architecture review:

| ID | Issue | Status |
|---|---|---|
| C1 | 5-sample moving average ≈ 3 s detection lag | ✅ Fixed — asymmetric median(3)/median(5) |
| C2 | Tag RX timeout missing → reboots every 10 s when anchor absent | ✅ Fixed — `dwt_setrxtimeout(60000)` + re-arm |
| C3 | No anchor-to-anchor RF coordination | N/A — 1 anchor hardware confirmed |
| C4 | Timestamps are `millis()` (uptime), not epoch | ⬜ Phase C |
| C5 | Uncalibrated antenna delay (±10–30 cm) | 🟡 SOP documented in code comments, physical cal needed |

Authority chain (do not violate): **anchor decides & timestamps → broker → backend persists → dashboard displays.**

## ESP32 Core 3.x Compatibility Notes

- **WDT API**: `esp_task_wdt_init(timeout, panic)` is gone. Use `esp_task_wdt_config_t` + `esp_task_wdt_reconfigure()`. Arduino core 3.x pre-initializes TWDT so call `reconfigure` first, fall back to `init` only if `ESP_ERR_INVALID_STATE`.
- **`volatile` structs**: C++ default copy-assign does not accept `volatile` struct. Shared data is protected by `portMUX` critical sections (which provide the necessary memory barrier), so `volatile` is not needed on `TagReport[]` / `g_reportTimestamp[]`.

### Pin Assignment (both boards)

| Pin | Function |
|---|---|
| GPIO 4 | SPI SS (chip select) |
| GPIO 27 | DW3000 RST |
| GPIO 34 | DW3000 IRQ |
| GPIO 26 | Relay output (Anchor only) |

SPI: 7 MHz, MSBFIRST, SPI_MODE0. Antenna delays both set to **16385** (tune per physical calibration).
