// =============================================================================
// boards/qtpy_c3.h — Adafruit QT Py ESP32-C3 pin map (LOW-COST SERVO-ONLY NODE)
//
// The cheap secondary. This board exists to answer one question: what is the
// least hardware that can still be a DustGate node? Because a secondary receives
// already-resolved SET frames (control/NodeLink.h) — a channel and an angle, not
// a topology — the answer is "a WiFi MCU with four PWM pins". No stepper, no
// TMC2209, no endstops, no web UI in flash.
//
// Selected by -DBOARD_QTPY_C3, and normally paired with -DDUSTGATE_SECONDARY
// (see config.h) which compiles out the primary-only subsystems.
//
// ⚠️ PIN MAP IS UNVALIDATED ON HARDWARE. The assignments below follow the
// published QT Py ESP32-C3 pinout and the C3's strapping-pin constraints, but
// no board has been bench-tested yet. Verify before trusting a build.
//
// SINGLE-CORE: unlike the WROOM-32/S2 targets, the C3 has one core, so the
// "Core 0 / Core 1" task pinning the primary uses does not apply. That's fine
// here — a secondary has no Shelly poller, no routing and no web UI; it runs a
// plain loop and answers a WebSocket.
//
// Strapping pins deliberately avoided on the servo block: GPIO2, GPIO8 and
// GPIO9 are sampled at reset on the ESP32-C3, and a servo signal line idling on
// one of them can stop the board booting.
// =============================================================================
#pragma once

// Build-target identity, reported in the NodeLink WELCOME frame and matching
// the topology schema's controllers[].board values (docs/topology-schema.md).
#define BOARD_NAME "qtpy_c3"

// -- Servo PWM block (the only actuators this board drives) --
// QT Py silkscreen A0/A1/A2/A3 — four adjacent pins on one header edge, none of
// them strapping pins.
#define SERVO_PWM_PIN_1     4   // A0
#define SERVO_PWM_PIN_2     3   // A1
#define SERVO_PWM_PIN_3     1   // A2
#define SERVO_PWM_PIN_4     0   // A3

// -- Status LED --
// No plain user LED on this board, only a NeoPixel on GPIO2 — so the status
// indicator is a COLOUR, not a blink pattern. See utils/StatusLed.h.
//
// Unlike the S3, the C3's pixel has no separate power-enable pin: it is always
// powered, so PIN_PIXEL_POWER is deliberately left undefined.
//
// GPIO2 is a C3 strapping pin, but it is sampled only at reset — driving it as
// the pixel's data line after boot is what the board is designed to do.
#define PIN_PIXEL            2

// PIN_LED deliberately NOT defined: StatusLed.h prefers PIN_PIXEL and only
// falls back to a plain LED when no pixel exists, so defining one here would be
// dead weight at best and a servo signal line driven as an LED at worst.

// -- Native USB --
// The C3 has USB Serial/JTAG rather than a USB-serial bridge chip.
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
