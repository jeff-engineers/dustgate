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
#define PIN_LED            13   // D13 — onboard LED on ESP32-S2 Feather

// -- Board capabilities --
#define BOARD_HAS_NATIVE_USB 1  // native USB-CDC Serial; wait for the monitor at boot
