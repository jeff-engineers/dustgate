// =============================================================================
// boards/qtpy_s3_linear.h — Adafruit QT Py ESP32-S3 pin map, LINEAR variant
//
// ⚠️ NO FIRMWARE EXISTS THAT CAN USE THIS HEADER YET. It is a validated PIN
// BUDGET, not a working target. See "WHAT IS MISSING" at the bottom before
// wiring anything — a build that includes this file will either not compile or
// will refuse every linear move at runtime, depending on which role you build.
//
// Same physical board as boards/qtpy_s3.h (Adafruit 5700, ESP32-S3 N4R2), but
// mapped for a rack: TMC2209 + two endstops instead of a four-servo bank. It
// exists to answer "can one QT Py drive a slide gate?" — the answer on pins is
// yes, with four pads to spare.
//
// MOTIVATION (2026-08-09): the DevKitC carries a stepper AND a servo block on
// one board, which makes every node pay the BOM of the biggest node. The idea
// this header serves is IDENTICAL HOST BOARDS with different breakouts —
// a 5V/servo carrier and a USB-PD 15V→5V linear carrier — where flashing, not
// part number, decides primary vs node. Hypothetical; nothing depends on it.
//
// ⚠️ MAY BE OBSOLETE BEFORE IT IS USED. Also on 2026-08-09, closed-loop SERIAL
// BUS servos (Feetech ST3215) were evaluated as a replacement for both the
// stepper slide AND the PWM servo bank — one actuator type over a 2-pin
// half-duplex UART, no TMC2209 and no 7-pin rack map at all. That option was
// liked but explicitly PARKED, not adopted. If it is ever revived, delete this
// file rather than maintaining it.
//
// Selected by -DBOARD_QTPY_S3_LINEAR (no PlatformIO env ships with it yet, and
// config.h's board #if chain does not reference it — both are deliberate, so
// this file cannot be pulled into a build by accident).
// =============================================================================
#pragma once

// Build-target identity, reported in the NodeLink WELCOME frame and matching
// the topology schema's controllers[].board values (docs/topology-schema.md).
// Distinct from "qtpy_s3" on purpose: same silicon, different capabilities, and
// the topology needs to tell a servo bank from a rack.
#define BOARD_NAME "qtpy_s3_linear"

// -- TMC2209 control pins --
// QT Py silkscreen A0/A1/A2 — three adjacent pads on one header edge.
#define PIN_TMC_STEP       18   // A0
#define PIN_TMC_DIR        17   // A1
#define PIN_TMC_EN          9   // A2  (active LOW)
// PIN_TMC_DIAG intentionally NOT defined — StallGuard abandoned, same as the
// DevKitC. Its uses in motor/StepperTMC2209Driver.cpp are #ifdef-guarded.

// -- TMC2209 UART (Serial1) --
// Freely remappable through the S3's GPIO matrix, and StepperTMC2209Driver.cpp
// already passes both pins to Serial1.begin(), so no pad is privileged here.
// Single-wire half-duplex at the driver end: TX through the 1k series resistor
// to the breakout's UART pad, RX tied to the same node (see WIRING.md).
#define SERIAL1_TX_PIN      8   // A3
#define SERIAL1_RX_PIN      7   // SDA

// -- Endstop pins (FEEDBACK_LIMIT_DISTANCE) --
// NC switch to GND, INPUT_PULLUP, HIGH = triggered. Both required — the far
// endstop is the reference for the self-calibrating sweep, not just over-travel
// safety (docs/dual-endstop-calibration.md).
//
// The NC wiring holds these lines LOW at rest, which is exactly why the pad
// choice matters: on a strapping pin, an idle endstop would stop the board
// booting. ESP32-S3 straps are GPIO0/3/45/46 and NONE of them reach a QT Py
// header pad, so every pin in this map is safe on that count. (The C3 QT Py is
// NOT — GPIO8/9 are straps there and 9 is the BOOT button, which is the main
// reason this variant is S3-only.)
#define PIN_ENDSTOP_HOME    6   // SCL
#define PIN_ENDSTOP_MAX     5   // TX pad

// -- Status LED --
// No plain user LED, only a NeoPixel — status is a COLOUR (utils/StatusLed.h).
// Both pixel pins are on-board and consume no header pad. The S3's pixel needs
// its power rail driven HIGH before it lights.
#define PIN_PIXEL            39
#define PIN_PIXEL_POWER      38

// PIN_LED deliberately NOT defined. This board is a plausible PRIMARY (it has
// the stepper pins), and the primary's status used to be single-LED blink codes
// that were simply invisible here. That gap is closed: StatusLed.h drives the
// pixel above for every state, primary and secondary alike, so there is nothing
// left that needs a plain LED.

// -- Native USB --
// USB Serial/JTAG built into the chip, no bridge. Requires
// -DARDUINO_USB_CDC_ON_BOOT=1 and monitor_dtr/rts = 1 (see platformio.ini's
// dustgate_node env for why 0 makes a live board look dead).
#define BOARD_HAS_NATIVE_USB 1

// -- No servo block --
// config.h derives HAS_SERVO from ENABLE_SERVO && SERVO_PWM_PIN_1, so omitting
// these keeps this a rack-only board. Four pads remain free (16/RX and
// 35/36/37 = MO/SCK/MI) if a mixed rack+servo node is ever wanted — but see the
// PSRAM caveat below before committing to 35/36/37.

// -----------------------------------------------------------------------------
// PIN BUDGET (why this map, and what is left)
//
//   Header pads on a QT Py ESP32-S3: 11 GPIO
//     A0 18, A1 17, A2 9, A3 8, SDA 7, SCL 6, TX 5, RX 16, MI 37, MO 35, SCK 36
//   Plus 2 more on the STEMMA QT connector: SDA1 41, SCL1 40
//   Used here: 7.  Spare: 16, 35, 36, 37 (+ the STEMMA pair).
//
// 35/36/37 ARE DELIBERATELY UNUSED. Those are the octal flash/PSRAM pins on
// ESP32-S3. The N4R2 in hand is QUAD PSRAM, so they should be free and Adafruit
// breaks them out — but the map does not need them, so it does not bet on it.
// If you later want servos on this board, verify those three on real hardware
// first; 16 and the STEMMA pair are unconditionally safe.
//
// 3V3 rail: the QT Py's regulator feeds TMC2209 VIO fine. MOTOR supply (VM,
// 12–24V) is separate and does NOT come from this board — that is the linear
// carrier's job (USB-PD 15V → 5V logic + VM), which is the whole point of
// splitting the breakouts.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// WHAT IS MISSING — read before trying to build this
//
// The pins are the easy half. Defining PIN_TMC_STEP flips HAS_LINEAR to 1
// (config.h), which pulls in the stepper driver, LimitSwitchDistance and the
// endstop supervisor. Neither existing role can then do anything useful:
//
//   1. As a SECONDARY (-DDUSTGATE_SECONDARY, build_src_filter +<node/>):
//      node/dustgate_node.cpp hard-refuses linear moves —
//        "[SET] REFUSED — linear move, no stepper on this node."
//      and its NodeLink SET frame carries a channel + ANGLE, not a position in
//      mm. Homing, the reference sweep and all calibration state live on the
//      primary. Making this work is not a pin map: it needs a linear-capable
//      node role — SET-position frames, per-node calibration in NVS, and a
//      homing state machine that survives the primary going away.
//
//   2. As a PRIMARY (the full sketch): compiles conceptually, but brings the
//      topology store, router, Shelly poller and the web UI onto a 4MB N4R2.
//      The DevKitC uses a no-OTA ffat partition to fit that; the node envs use
//      huge_app.csv, which has no ffat for the UI. A primary env for this board
//      needs its own partition table, and the flash budget is unverified.
//
//   3. config.h's board #if chain does not include BOARD_QTPY_S3_LINEAR, and no
//      PlatformIO env defines it. Adding both is step one of actually doing
//      this — left out so an unfinished target can't sneak into a build.
//
// Also unresolved, and bigger than this file: if identical host boards replace
// the DevKitC, "primary" stops being a board and becomes a flashing-time role.
// That is a firmware/provisioning change (role in NVS, or first-flashed-wins),
// not a pin change.
//
// ⚠️ EVERY PIN HERE IS UNVALIDATED ON HARDWARE. Assignments follow the
// published QT Py ESP32-S3 pinout and the S3 strapping-pin rules; no board has
// driven a TMC2209 from these pads. The chip identity and flash geometry in
// boards/qtpy_s3.h were measured — this map was not.
// -----------------------------------------------------------------------------
