// =============================================================================
// boards/xiao_c5.h — Seeed Studio XIAO ESP32C5 pin map (SERVO-ONLY)
//
// A supported node target since 2026-08-14. A board has been flashed and booted
// (WiFi joined, ready at ~2s), but ⚠️ NO SERVO HAS BEEN DRIVEN FROM THESE PINS.
// The numbers below are Seeed's published pinout, not measured — see the
// strapping-pin warning further down before wiring one.
//
// Unlike every other header here it rides a DIFFERENT PlatformIO platform (the
// pioarduino fork) and a NEWER Arduino core, so its env builds against its own
// PLATFORMIO_CORE_DIR — see the xiao_c5 env in platformio.ini.
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
//
// PIN NUMBERS ARE FROM SEEED'S PUBLISHED PINOUT, NOT FROM A MULTIMETER.
//   D0..D10 map to GPIO 1, 0, 25, 7, 23, 24, 11, 12, 8, 9, 10 — checked against
//   Seeed's pin-definition drawing on 2026-08-16, still not against a meter.
//
//   STRAPPING PINS ARE NOW CHECKED (datasheet v1.4, Table 3-1): on the C5 they
//   are GPIO25, 26, 27, 28, 7, MTMS and MTDI. GPIO8 and GPIO9 are NOT among them
//   — that was C3 muscle memory (straps are GPIO2/8/9 there), and the servo block
//   below is clear. The one strap we do touch is GPIO25, the status pixel, and it
//   is harmless: it selects the SDIO sampling edge, a peripheral this build never
//   uses, and a WS2812 DIN is high-impedance so nothing holds the line at reset.
//   The boot-mode straps (26/27/28) and the JTAG strap (7) reach no pad we use.
// =============================================================================
#pragma once

// Build-target identity, reported in the NodeLink WELCOME frame and matching
// the topology schema's controllers[].board values (docs/topology-schema.md).
#define BOARD_NAME "xiao_c5"

// -- Servo PWM block (the only actuators this board drives) --
// D7..D10 — four adjacent pads on one edge, same physical-grouping rule as every
// other board here, so a servo loom can be built once and moved between them.
// Channel order matches boards/qtpy_s3.h (channel 1 = first pad of
// the block), so a topology's servo.channel means the same gate on any node.
#define SERVO_PWM_PIN_1    12   // D7
#define SERVO_PWM_PIN_2     8   // D8
#define SERVO_PWM_PIN_3     9   // D9
#define SERVO_PWM_PIN_4    10   // D10

// -- Status pixel (external) --
// The XIAO's onboard indicator is a plain green user LED on GPIO27, not an RGB
// pixel, so a colour indicator means adding one. GPIO25 (D2) is a plain pad with
// no bus function to give up — it is a strapping pin, but only for the SDIO
// sampling edge, which nothing here uses (see the note at the top).
//
// If you would rather not spend a pad: delete PIN_PIXEL and define
// `PIN_LED 27` instead — StatusLed.h falls back to blink patterns on the onboard
// LED. Strictly worse (that ambiguity is why the pixel exists) but free. GPIO27
// is a strapping pin (UART0 ROM message printing, default pull-up), but straps
// latch at Chip Reset and the pin is then an ordinary IO, so driving it from the
// app is safe.
//
// Wire: 5V -> pixel VDD, GND -> GND, GPIO25 -> 330R -> pixel DIN.
// See WIRING.md §1 for the level-shift and decoupling notes, and
// firmware/wiring/xiao-c5.md for this board's full wiring.
#define PIN_PIXEL          25   // D2

// Like the DevKitC, this board's pixel is already external, so the brighter
// option is a gain rather than a second pad. Uncomment for the same 4× the
// QT Py/Feather external pixels run at.
// #define PIXEL_GAIN      4

// -- Reserved: status screen (SSD1306 OLED) + wake button --
// Optional, and unbuilt ON THIS BOARD — a panel works on the DevKitC (2026-08-21)
// but nothing has been wired to a C5. D4/D5 are
// the XIAO-standard I2C position, so Seeed's own accessories land on them, and they
// stay free whether this board is driving four PWM gates or an ST3215 slider.
// D1 is GPIO0, which is an ordinary pad on the C5 (the boot straps are 26/27/28),
// so a momentary-to-GND button there is safe even at reset.
// See firmware/wiring/xiao-c5.md §4 and the layouts in
// docs/mockups/oled-status.html.
// Fitted by -DHAS_STATUS_SCREEN, the same build-time switch the DevKitC uses —
// env `xiao_c5_screen`. No panel has been connected to a C5 yet.
#ifdef HAS_STATUS_SCREEN
#define PIN_OLED_SDA    23   // D4
#define PIN_OLED_SCL    24   // D5

// The screen sleeps after two minutes and nothing relights it on its own, so the
// button is the only way a person gets it back without walking to a phone. See utils/WakeButton.h. D1 is GPIO0, which would
// be the boot strap on most ESP32 parts and a bad place for a switch — on the
// C5 the straps are 26/27/28, so a normally-open momentary here is safe even at
// reset. D0 is deliberately not used: it is the only analog pad on the edge.
#define PIN_WAKE_BTN     0   // D1, INPUT_PULLUP, momentary to GND
#endif

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

// ...but NOT the TinyUSB kind. This part's USB is the chip's USB Serial/JTAG
// peripheral (Arduino builds it as HWCDC; the board file sets
// ARDUINO_USB_MODE=1, and the device enumerates as 303a:1001 "USB JTAG/serial
// debug unit"), and the two kinds want OPPOSITE host-side line handling:
//
//   TinyUSB CDC (QT Py S3, Feather S2): DTR must be ASSERTED or the firmware's
//   output is discarded — see the note in the dustgate_node env.
//
//   USB Serial/JTAG (this board): DTR/RTS are not line state at all,
//   they are the ROM's download-mode trigger. Assert both and the chip drops
//   into the bootloader — the port vanishes, the monitor exits instantly, no
//   pixel, and the BOOT button does nothing. It looks exactly like a dead
//   board, and it was the entire "C5 won't boot" bring-up scare on 2026-08-13.
//   Hold both LOW: monitor_dtr = 0 / monitor_rts = 0.
#define BOARD_USB_SERIAL_JTAG 1

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
