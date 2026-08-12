// =============================================================================
// boards/qtpy_s3.h — Adafruit QT Py ESP32-S3 pin map (SERVO-ONLY NODE)
//
// The secondary node board actually in hand. Like the C3 variant this exists to
// answer "what is the least hardware that can still be a DustGate node?" —
// because a secondary receives already-resolved SET frames (control/NodeLink.h),
// a channel and an angle rather than a topology, the answer is "a WiFi MCU with
// four PWM pins". No stepper, no TMC2209, no endstops, no web UI in flash.
//
// Selected by -DBOARD_QTPY_S3, normally paired with -DDUSTGATE_SECONDARY (see
// config.h) which compiles out the primary-only subsystems.
//
// PART: this header targets the **N4R2** (4 MB flash + 2 MB PSRAM, Adafruit
// 5700), confirmed against the board on the bench via `esptool.py flash_id`:
//   Chip is ESP32-S3 (QFN56) revision v0.2
//   Features: WiFi, BLE, Embedded Flash 4MB (XMC), Embedded PSRAM 2MB (AP_3v3)
// The no-PSRAM QT Py S3 (8 MB flash, Adafruit 5426) shares this exact pin map —
// only the platformio.ini `board` id and flash size differ, so if you mix the
// two variants on one shop network this header still applies to both.
//
// DUAL-CORE, unlike the C3. Irrelevant here: a secondary has no Shelly poller,
// no routing and no web UI, so it stays a plain single loop answering a
// WebSocket. Nothing in the node firmware pins tasks to a core.
//
// ⚠️ PIN MAP IS UNVALIDATED WITH SERVOS ATTACHED. The chip identity and flash
// geometry above are measured; the servo assignments follow the published QT Py
// ESP32-S3 pinout and are believed correct but have not driven a real servo yet.
// =============================================================================
#pragma once

// Build-target identity, reported in the NodeLink WELCOME frame and matching
// the topology schema's controllers[].board values (docs/topology-schema.md).
#define BOARD_NAME "qtpy_s3"

// -- Servo PWM block (the only actuators this board drives) --
// QT Py silkscreen A0/A1/A2/A3 — four adjacent pins on one header edge.
// Channel order is deliberately identical to boards/qtpy_c3.h (A0..A3), so a
// topology's servo.channel means the same physical pin on either node board and
// the two are swappable without re-pinning the gates.
//
// None of these are ESP32-S3 strapping pins (those are GPIO0, 3, 45, 46), so a
// servo signal idling here cannot hold the board out of boot — the constraint
// that forced the C3's map to dodge GPIO2/8/9 does not bite on the S3, where 8
// and 9 are ordinary GPIO.
#define SERVO_PWM_PIN_1    18   // A0
#define SERVO_PWM_PIN_2    17   // A1
#define SERVO_PWM_PIN_3     9   // A2
#define SERVO_PWM_PIN_4     8   // A3

// -- Status LED --
// No plain user LED on this board, only a NeoPixel — so the status indicator is
// a COLOUR, not a blink pattern. See node/NodeStatusLed.h.
//
// The S3's pixel needs its power rail driven HIGH before it will light; the C3's
// is always powered, which is why NODE_PIXEL_POWER_PIN is board-specific.
#define NODE_PIXEL_PIN       39
#define NODE_PIXEL_POWER_PIN 38

// PIN_LED still points at a harmless pin so shared code that does a plain
// digitalWrite(PIN_LED, …) can't land on a servo signal line. Nothing in the
// node firmware uses it — blink codes are a primary-side feature.
#define PIN_LED            38   // (not a plain LED — see NODE_PIXEL_* above)

// -- Native USB --
// The S3 has USB Serial/JTAG built in, no USB-serial bridge chip. This is why
// the env sets -DARDUINO_USB_CDC_ON_BOOT=1, and why the port disappears on
// every reset (see run_monitor's reconnect note in dev.sh).
#define BOARD_HAS_NATIVE_USB 1

// -----------------------------------------------------------------------------
// Motor / endstop pins are DELIBERATELY ABSENT.
//
// config.h derives HAS_LINEAR from whether PIN_TMC_STEP is defined, so leaving
// these out is what makes this a servo-only build — the stepper driver, the
// feedback system and the endstop supervisor all compile out. Defining them
// here to "keep the interface uniform" would silently re-enable code that has
// no hardware behind it.
// -----------------------------------------------------------------------------
