// =============================================================================
// boards/devkitc_wroom32.h — Espressif ESP32-DevKitC (WROOM-32) pin map
//
// The primary/canonical DustGate target (see the target-board decision doc).
// Selected by -DBOARD_DEVKITC. Classic dual-core ESP32 — NOT an S2/S3:
//   - no native USB (onboard CP2102/CH340 USB-serial), so BOARD_HAS_NATIVE_USB=0
//   - Serial1 pins are freely remappable and MUST be passed to Serial1.begin()
//
// Raw GPIO numbers are used deliberately (not A0/A1/... aliases, which differ
// between Arduino cores). Layout targets the official DevKitC V4 silkscreen.
//
// Physical grouping (V4 header order):
//   RIGHT header — motor driver:  STEP 23, DIR 22, EN 21, UART TX 19, RX 18
//   LEFT header  — sensors+servo: HOME 32, MAX 33, then the reserved 4-in-a-row
//                                 servo PWM block 25/26/27/14 for ball-valve gates.
//
// Pin-choice rationale:
//   - Endstops on 32/33: full GPIO with internal pull-ups (the NC-switch
//     INPUT_PULLUP wiring needs them). Input-only 34/35/36/39 have NO pull-up
//     and cannot be endstops.
//   - Servo block 25/26/27/14: physically contiguous, output-capable, none are
//     strapping pins. ADC2 (irrelevant for PWM output). Currently unused — routed
//     to a servo header on the carrier for ball-valve gates.
//   - No DIAG pin: StallGuard is abandoned; PIN_TMC_DIAG is intentionally left
//     undefined here and its uses are #ifdef-guarded.
//   - Status pixel on GPIO17, a labeled spare: the official DevKitC V4 has NO
//     user LED (its one LED is the always-on power LED), so the indicator is
//     external either way. GPIO2 — the usual choice, and where this started —
//     is the onboard LED on NodeMCU-style CLONES only, and is a strapping pin,
//     so anything miswired to 3V3 there would keep the board out of download
//     mode. GPIO17 is non-strapping and has no such failure mode.
// =============================================================================
#pragma once

// Build-target identity, reported in the NodeLink WELCOME frame and matching
// the topology schema's controllers[].board values (docs/topology-schema.md).
#define BOARD_NAME "devkitc"

// -- TMC2209 control pins --
#define PIN_TMC_STEP       23
#define PIN_TMC_DIR        22
#define PIN_TMC_EN         21   // active LOW
// PIN_TMC_DIAG intentionally NOT defined — StallGuard abandoned; DIAG unused.

// -- TMC2209 UART (Serial1) — remappable on classic ESP32 --
#define SERIAL1_TX_PIN     19
#define SERIAL1_RX_PIN     18

// -- Endstop pins (FEEDBACK_LIMIT_DISTANCE) --
#define PIN_ENDSTOP_HOME   32   // NC switch, INPUT_PULLUP
#define PIN_ENDSTOP_MAX    33   // NC switch, INPUT_PULLUP

// -- Status pixel (external) --
// The official DevKitC V4's only onboard LED is the always-on power LED, so the
// indicator has to be a part you add. It is a single WS2812/NeoPixel rather than
// the plain LED this used to be: one data line either way, but colour says more
// in one glance than any blink rate can (see utils/StatusLed.h).
//
// GPIO17 is a labeled spare and non-strapping, so unlike GPIO2 nothing wired
// here can hold the board out of download mode at boot.
//
// Wire: 5V (or 3V3 — see WIRING.md §1) -> pixel VDD, GND -> GND,
//       GPIO17 -> 330R -> pixel DIN, 1000µF across the pixel's supply.
// The DevKitC is 3V3 logic; on a 5V-powered pixel read the level-shift note in
// WIRING.md §1 (status pixel) before assuming DIN is happy.
#define PIN_PIXEL          17

// This board's pixel is ALREADY the external one, so it takes no second pin —
// the brighter option here is a gain, not another pad. Uncomment to run it at
// the same 4× the QT Py/Feather external pixels use; the default 1× matches the
// bench-calibrated levels in StatusLed.h.
// #define PIXEL_GAIN      4

// No PIN_LED: the pixel covers every state. StatusLed.h's plain-LED fallback is
// for boards that have an LED and no pixel, which this one does not.

// -- Reserved: servo PWM outputs (4 in a row) --
#define SERVO_PWM_PIN_1    25
#define SERVO_PWM_PIN_2    26
#define SERVO_PWM_PIN_3    27
#define SERVO_PWM_PIN_4    14

// -- Board capabilities --
#define BOARD_HAS_NATIVE_USB 0  // USB-serial via CP2102/CH340; Serial is always ready
