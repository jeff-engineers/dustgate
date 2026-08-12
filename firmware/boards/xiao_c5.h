// =============================================================================
// boards/xiao_c5.h — Seeed Studio XIAO ESP32C5 pin map (SERVO-ONLY)
//
// ⚠️⚠️ THIS IS A SPIKE, NOT A SUPPORTED TARGET. ⚠️⚠️
// Nothing here has been on hardware, and unlike the other unvalidated headers
// this one also rides a DIFFERENT PlatformIO platform and a NEWER Arduino core
// than every other env (see the xiao_c5 env in platformio.ini). Its purpose is
// to answer one question — does this firmware's library stack compile for a
// RISC-V dual-band part at all — not to be flashed and trusted.
//
// WHY THIS BOARD IS INTERESTING
//   - Real Espressif module in a thumb-sized package, USB-C, native USB.
//   - 8 MB flash + 8 MB PSRAM against the DevKitC's 4 MB and no PSRAM.
//   - Dual-band WiFi 6, so the controller's own link can leave a 2.4 GHz band
//     that a shop full of motors and Shelly plugs has already crowded.
//
// WHY IT IS SERVO-ONLY, AND WHY THAT IS THE POINT
//   The C5 is SINGLE-CORE. The primary's design leans on two cores: the Shelly
//   poller and the NodeLink client are pinned to core 0 specifically so their
//   blocking HTTP can never disturb step generation on core 1 (see the comment
//   at control/SmartOutletControl.cpp:300). Software step pulses are the one
//   workload here that turns scheduler jitter into physical error — lost steps,
//   a rough-sounding rail.
//
//   Servos have no such problem. PWM is LEDC hardware and a bus servo is a UART
//   write; both are fire-and-forget, and neither cares that something else is
//   using the CPU. So single-core is a bad trade for a stepper primary and a
//   non-issue for a servo-only board. That is the whole reason this header
//   defines no motor pins.
//
//   The same reasoning already lives in boards/qtpy_c3.h for the single-core C3.
//
// PIN NUMBERS ARE FROM SEEED'S PUBLISHED PINOUT, NOT FROM A MULTIMETER.
//   D0..D10 map to GPIO 1, 0, 25, 7, 23, 24, 11, 12, 8, 9, 10. Before trusting
//   this on hardware, confirm against the ESP32-C5 datasheet which of those are
//   STRAPPING pins — a servo signal idling on one can stop the board booting,
//   which is exactly the trap boards/qtpy_c3.h had to dodge on the C3 (GPIO2/8/9
//   there). GPIO8 and GPIO9 are used below and are the first two to check.
// =============================================================================
#pragma once

// Build-target identity, reported in the NodeLink WELCOME frame and matching
// the topology schema's controllers[].board values (docs/topology-schema.md).
#define BOARD_NAME "xiao_c5"

// -- Servo PWM block (the only actuators this board drives) --
// D7..D10 — four adjacent pads on one edge, same physical-grouping rule as every
// other board here, so a servo loom can be built once and moved between them.
// Channel order matches boards/qtpy_c3.h and qtpy_s3.h (channel 1 = first pad of
// the block), so a topology's servo.channel means the same gate on any node.
#define SERVO_PWM_PIN_1    12   // D7
#define SERVO_PWM_PIN_2     8   // D8
#define SERVO_PWM_PIN_3     9   // D9
#define SERVO_PWM_PIN_4    10   // D10

// -- Status pixel (external) --
// The XIAO's onboard indicator is a plain yellow user LED on GPIO27, not an RGB
// pixel, so a colour indicator means adding one. GPIO25 (D2) is a plain pad with
// no bus function to give up.
//
// If you would rather not spend a pad: delete PIN_PIXEL and define
// `PIN_LED 27` instead — StatusLed.h falls back to blink patterns on the onboard
// LED. Strictly worse (that ambiguity is why the pixel exists) but free.
//
// Wire: 5V -> pixel VDD, GND -> GND, GPIO25 -> 330R -> pixel DIN.
// See WIRING.md §5 for the level-shift and decoupling notes.
#define PIN_PIXEL          25   // D2

// -- Reserved: serial-servo bus --
// D6/D7 are the board's hardware UART (GPIO11/GPIO12). D7 doubles as
// SERVO_PWM_PIN_1 above, so a build that drives a serial bus servo has to give
// up PWM channel 1 — which is the right trade, since one bus replaces the whole
// four-channel block and lifts the SERVO_COUNT ceiling with it. Left commented
// until there is a bus servo on the bench to talk to.
// #define SERIAL1_TX_PIN  11   // D6
// #define SERIAL1_RX_PIN  12   // D7

// -- Board capabilities --
// USB Serial/JTAG built into the chip, no bridge. Requires
// -DARDUINO_USB_CDC_ON_BOOT=1, and the port disappears on every reset (dev.sh's
// monitor handles the reconnect).
#define BOARD_HAS_NATIVE_USB 1

// -----------------------------------------------------------------------------
// Motor / endstop pins are DELIBERATELY ABSENT.
//
// config.h derives HAS_LINEAR from whether PIN_TMC_STEP is defined, so leaving
// these out is what makes this a servo-only build — the stepper driver, the
// feedback system and the endstop supervisor all compile out. Defining them
// here to "keep the interface uniform" would silently re-enable code that has
// no hardware behind it, and on this part would also re-introduce the
// single-core step-timing problem described at the top.
// -----------------------------------------------------------------------------
