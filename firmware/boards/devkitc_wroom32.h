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

// -- Status screen (SSD1306 OLED), optional --
// VERIFIED 2026-08-21: a 0.96" panel answered at 0x3C on these two pins and the
// firmware drew to it. NOT the usual GPIO21/22 — those are PIN_TMC_EN and
// PIN_TMC_DIR above, so copy-pasted I2C example code would drive the stepper's
// enable line. GPIO16/4 are the board's last two general-purpose spares and sit
// next to the pixel's GPIO17 on the V4 right header, keeping the indicator group
// together on a carrier. Fitting a screen therefore fills this board.
//
// WROVER WARNING: GPIO16/17 are the PSRAM interface on WROVER modules — that is
// SDA *and* PIN_PIXEL, on a module that drops into the same footprint, with no
// spares left to move them to. See firmware/wiring/devkitc.md §5.
// Declared by the BUILD, not probed for: -DHAS_STATUS_SCREEN (env
// esp32dev_screen) fits one, and which pins that means stays this file's
// business. Still unbuilt hardware — no panel has been connected to this board.
#ifdef HAS_STATUS_SCREEN
#define PIN_OLED_SDA    16
#define PIN_OLED_SCL     4

// -- Wake button (fitted with the screen) --
// The screen blanks after two minutes so it doesn't burn a static layout in; a
// button is how you get it back without walking to a phone. See WakeButton.h.
//
// GPIO34 because fitting the screen took the last two general-purpose pins and
// this is what's left: input-only, no output, and — the part that matters —
// NO INTERNAL PULL-UP. INPUT_PULLUP is accepted here and does nothing, so this
// pin needs an external 10kOhm to 3V3 and the plain INPUT below. Wiring is in
// firmware/wiring/devkitc.md §5.
#define PIN_WAKE_BTN        34
#define WAKE_BTN_INPUT_MODE INPUT   // external 10kOhm pull-up — 34 has none
#endif

// -- Reserved: servo PWM outputs (4 in a row) --
#define SERVO_PWM_PIN_1    25
#define SERVO_PWM_PIN_2    26
#define SERVO_PWM_PIN_3    27
#define SERVO_PWM_PIN_4    14

// -- Board capabilities --
#define BOARD_HAS_NATIVE_USB 0  // USB-serial via CP2102/CH340; Serial is always ready
