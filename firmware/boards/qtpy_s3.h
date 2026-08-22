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
// Channel order (A0..A3, channel 1 = first pad of the block) is the same rule
// every board header here follows, so a topology's servo.channel means the same
// physical gate on any node board and they are swappable without re-pinning.
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
// a COLOUR, not a blink pattern. See utils/StatusLed.h.
//
// The S3's pixel needs its power rail driven HIGH before it will light; the C3's
// is always powered, which is why PIN_PIXEL_POWER is board-specific.
#define PIN_PIXEL            39
#define PIN_PIXEL_POWER      38

// -- External status pixel (optional, brighter) --
// The onboard pixel is a bench indicator: it is 2mm across and, once this board
// is screwed to a joist above a manifold, invisible from anywhere you'd stand.
// This pad takes a second WS2812 you can actually see across a shop. StatusLed.h
// drives both in parallel with the same colour, the external one at
// PIXEL_EXT_GAIN (4×) — so the bench view is unchanged and the remote one is
// readable. Solder nothing here and this costs a few bytes clocked into open air.
//
// WHY MOSI: it is free on BOTH QT Py headers (the node uses A0–A3 for servos, the
// linear build uses A0–A3 + SDA/SCL/TX), so one loom fits either, and on this
// board it sits on the same edge as 5V and GND — all three pixel wires leave from
// one corner instead of straddling the board. Nothing here uses SPI.
//
// GPIO35–37 are the octal-PSRAM pins on some ESP32-S3 modules. Both variants this
// header covers (N4R2 quad, and the no-PSRAM 5426) leave them free — but that is
// the thing to re-check before assuming this pad on some other S3.
//
// Wire: 5V -> pixel VDD, GND -> GND, GPIO35 -> 330R -> pixel DIN.
// See WIRING.md §1 for the level-shift and decoupling notes.
#define PIN_PIXEL_EXT        35   // MOSI

// -- Reserved: status screen (SSD1306 OLED) --
// Optional, and untested ON THIS BOARD — a panel works on the DevKitC
// (2026-08-21), but nothing has been wired to a QT Py and no env sets
// -DHAS_STATUS_SCREEN for this one yet.
//
// This board has the easiest version of the problem on the whole fleet: the
// STEMMA QT connector is a SECOND I²C bus (SDA1/SCL1) with its own 4-pin JST
// SH socket, so a screen on a STEMMA cable costs no header pad, no soldering
// and nothing this node already uses. The servo block (A0-A3) and both pixels
// are untouched, which is what makes a screen retrofittable on a node already
// screwed to a joist.
//
// The bare SDA/SCL pads (GPIO7/GPIO6) are the alternative for a module with
// loose wires rather than a STEMMA cable; they are free on a node build for the
// same reason MOSI is (see PIN_PIXEL_EXT above). Swap the numbers below if you
// wire it that way — Wire.begin() takes whichever pair this names.
#ifdef HAS_STATUS_SCREEN
#define PIN_OLED_SDA    41   // SDA1 — STEMMA QT
#define PIN_OLED_SCL    40   // SCL1 — STEMMA QT

// -- Wake button (fitted with the screen) --
// The screen sleeps after two minutes and nothing relights it on its own, so
// this is the ONLY way a person gets it back. See utils/WakeButton.h. Momentary to GND, internal pull-up — the S3 has one on
// every pad here, unlike the DevKitC.
//
// MISO (GPIO37) rather than the SDA/SCL pads: those two are the fallback wiring
// for a bare panel with flying leads (see above), and a button that only works
// when the screen came in on a STEMMA cable is a trap. Nothing uses SPI, and
// this sits beside PIN_PIXEL_EXT so the whole indicator group leaves from one
// corner. Same GPIO35-37 octal-PSRAM caveat as the pixel: free on both variants
// this map covers, worth re-checking on a different S3.
#define PIN_WAKE_BTN    37   // MISO
#endif

// PIN_LED deliberately NOT defined: there is no plain user LED to fall back to,
// and defining one would make StatusLed.h blink a pin that lights nothing.

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
