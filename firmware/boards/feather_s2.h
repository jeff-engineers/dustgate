// =============================================================================
// boards/feather_s2.h — Adafruit ESP32-S2 Feather (#5000) pin map
//
// The original DustGate target. Kept as a working, unadvertised variant after
// the primary target moved to the ESP32-DevKitC (see boards/devkitc_wroom32.h
// and the target-board decision doc). Selected by -DBOARD_FEATHER_S2.
//
// All GPIO are 3.3V logic. Serial1 is on the Feather's RX/TX header pins.
// =============================================================================
#pragma once

// Build-target identity, reported in the NodeLink WELCOME frame and matching
// the topology schema's controllers[].board values (docs/topology-schema.md).
#define BOARD_NAME "feather_s2"

// -- TMC2209 control pins --
#define PIN_TMC_STEP        5   // D5
#define PIN_TMC_DIR         6   // D6
#define PIN_TMC_EN          9   // D9  (active LOW)
#define PIN_TMC_DIAG        A2  // DIAG — StallGuard (abandoned on #6121; diagnostic-only)

// -- TMC2209 UART (Serial1) --
// The S2 Feather has fixed default Serial1 pins on the RX/TX header, so passing
// them explicitly to Serial1.begin() is a no-op that documents the wiring.
#define SERIAL1_RX_PIN      RX
#define SERIAL1_TX_PIN      TX

// -- Endstop pins (FEEDBACK_LIMIT_DISTANCE) --
// Both required. Which one is "home" is chosen at setup (the user's LEFT);
// see g_homeIsMaxEndstop in firmware.ino.
#define PIN_ENDSTOP_HOME   10   // D10 — NC switch, pulls LOW when triggered
#define PIN_ENDSTOP_MAX    11   // D11 — NC switch, pulls LOW when triggered

// -- Status LED --
// -- Status indicator --
// This board has BOTH: a plain red LED on D13 and an onboard NeoPixel. Use the
// pixel — colour beats blink rate (see utils/StatusLed.h) — and leave D13 for
// whatever a bench session wants to scope. Pin numbers match the Arduino core's
// own variant (PIN_NEOPIXEL 33 / NEOPIXEL_POWER 21 in
// variants/adafruit_feather_esp32s2/pins_arduino.h); the pixel's rail is behind
// a GPIO switch, hence PIN_PIXEL_POWER.
#define PIN_PIXEL          33
#define PIN_PIXEL_POWER    21

// -- External status pixel (optional, brighter) --
// Same idea and the same pad number as both QT Py headers, deliberately: MOSI is
// free on every S2/S3 board here, so ONE external-pixel loom fits all of them.
// StatusLed.h drives it in parallel with the onboard pixel at PIXEL_EXT_GAIN
// (4×); solder nothing and it costs a few bytes into open air.
//
// Wire: 5V -> pixel VDD, GND -> GND, GPIO35 -> 330R -> pixel DIN (WIRING.md §1).
#define PIN_PIXEL_EXT      35   // MOSI

// -- Board capabilities --
#define BOARD_HAS_NATIVE_USB 1  // native USB-CDC Serial; wait for the monitor at boot
