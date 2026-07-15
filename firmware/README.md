# Edge-Fusion Retrofit — Firmware

Upgrade target for the original *IoT Intruder Detection Robot* (Arduino Uno R3).
See the case study document for full rationale; this file covers what's
needed to actually build/flash/test what's here.

## Repository layout

```
firmware/
  include/fusion_logic.h    # pure decision logic, no hardware deps
  src/fusion_logic.cpp      # implementation
  src/main.cpp              # ESP32 Arduino-framework sketch (hardware I/O + wiring)
tests/
  test_fusion_logic.cpp     # desktop-compilable unit tests for fusion_logic
```

`fusion_logic.*` is intentionally hardware-free so it is the one part of this
retrofit that is **actually proven to run correctly** in this take-home
window — compiled and executed on a desktop, see `tests/`. `main.cpp` is the
integration layer; it has been written and reviewed but not flashed to a
physical board (the retrofit BOM below was not in hand during the 3-5 day
window). That's a deliberate, disclosed limitation, not an oversight — see
"What still needs embedded judgment" in the case study document.

## Bill of materials (retrofit additions over the original build)

| Part | Role | Why it's here |
|---|---|---|
| ESP32 DevKit | MCU | Dual-core, WiFi+BLE onboard, more headroom than Uno R3 for the fusion logic + TLS stack |
| PCA9685 16-ch PWM driver | Actuation | Moves servo/buzzer current off the MCU's own 5V rail — this is the direct fix for "unable to run more than 1 servo smoothly" |
| 2x HC-SR04 (added, 3 total) | Sensing | Fixed-angle fan replaces the single sensor + servo sweep |
| PIR motion sensor | Sensing | Fast pre-trigger; sonar+sweep alone was too slow for fast-moving objects |
| Dedicated 5V/3A buck converter | Power | Feeds the PCA9685 rail only — isolates actuation current from MCU logic |
| 2S LiPo + TP4056 charge module | Power | Removes the USB-tether-to-laptop dependency |
| DHT11, sound sensor | Sensing | Carried over from the original build |
| *(removed)* GY-NEO6M GPS | — | Dropped: needs outdoor sky view for minutes to get a fix, near-zero value for a static indoor node. Judgment call, documented in the case study. |

## Build

This listing targets PlatformIO (ESP32 board, Arduino framework). Expected
`platformio.ini` dependencies: `adafruit/Adafruit PWM Servo Driver Library`,
`adafruit/DHT sensor library`, `knolleary/PubSubClient`. Not included here
since no physical board was available to validate a full build in this
window — see the "honest scope" note at the top of `main.cpp`.

## Run the tests (this part *is* runnable right now)

```bash
g++ -std=c++17 -Iinclude tests/test_fusion_logic.cpp src/fusion_logic.cpp -o test_fusion_logic
./test_fusion_logic
```
