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

// -- What this board drives: FOUR PWM SERVOS *OR* ONE SERIAL BUS --
//
// PWM and serial never share a board (see config.h), and on this part they
// physically can't anyway: D7 is PWM channel 1 and the UART's RX. So the flag
// picks which personality the header presents, and config.h's #error stays as
// the backstop for a board header that tries to claim both.
//
// -DDUSTGATE_SERVO_BUS is the slider's build. Everything else — primary and
// PWM node — gets the four-channel block and no bus.
#if defined(DUSTGATE_SERVO_BUS)

// -- Serial-servo bus (Feetech ST3215 and friends) --
// D6/D7 are GPIO11/12, the board's only exposed hardware UART, and the pads
// Seeed's XIAO Bus Servo Adapter sockets onto. ONE WIRE, half duplex at the
// servo end; whether our own transmission echoes back depends on what is
// driving the line (see ST3215Bus::receive — it copes with both).
//
// TX IS D6. Two sources against one, checked 2026-08-26 after a bus servo
// answered nothing:
//   - The Arduino core's own variant table for this board (framework-
//     arduinoespressif32/variants/XIAO_ESP32C5/pins_arduino.h) says
//     `TX = 11, RX = 12` and `D6 = 11, D7 = 12`. That is the table the build
//     itself uses, so it is the one that decides.
//   - Every XIAO silkscreen agrees: D6 TX, D7 RX.
//   - Seeed's wiki for the Bus Servo Driver Board says the opposite ("connect
//     the RX pin on the Driver Board to the TX pin (D7) on your host"). Treat
//     that line as a typo — but the bench console can `swap` at runtime, so it
//     costs a command to find out rather than a reflash.
//
// ⚠️ The bus LOGIC LEVEL is still unconfirmed (3.3V vs 5V) and the C5 is NOT 5V
// tolerant. That is moot through the adapter, which buffers, and it matters the
// moment a servo lead meets one of these pads directly.
// See firmware/wiring/st3215-bench.md.
#define PIN_SERVO_BUS_TX   11   // D6, the pad the XIAO silkscreen calls TX
#define PIN_SERVO_BUS_RX   12   // D7, ditto RX

// -- Endstops: the two switches that make multi-turn survivable --
//
// NOT OPTIONAL, and not a leftover from the stepper. The ST3215 in step mode
// reports how much of the last command is still outstanding, never where the
// shaft is (wiring/st3215-bench.md §5.0.2), so absolute position is something
// the driver counts — and counting does not survive a power cycle. The homing
// sweep of docs/dual-endstop-calibration.md is therefore still the calibration
// path, and it needs both switches: one is the datum, the other measures the
// span and catches over-travel.
//
// D8/D9 = GPIO8/9, and they are ORDINARY PADS on this part. That needed
// checking: on the ESP32-C3 the straps are GPIO2/8/9, and a NORMALLY-CLOSED
// switch holds its pin LOW at reset, which on a strap would change how the chip
// boots. The C5's straps are GPIO25/26/27/28/7 + MTMS/MTDI (datasheet v1.4
// Table 3-1), so neither of these is one.
//
// WIRED NORMALLY-CLOSED, to GND, with INPUT_PULLUP: untriggered reads LOW,
// triggered reads HIGH — and so does a broken wire or an unplugged connector.
// That is the point. A snapped lead in a shop full of vibration stops the
// carriage instead of letting it drive into the end of the rail.
//
// HOME vs MAX is a WIRING label, not a role. Which switch is the datum is
// g_homeIsMaxEndstop, decided at setup by the one wizard question ("did it home
// to the left?"), because home is always the user's LEFT.
#define PIN_ENDSTOP_HOME    8   // D8, NC to GND, INPUT_PULLUP
#define PIN_ENDSTOP_MAX     9   // D9, ditto

#else

// -- Servo PWM block --
// D7..D10 — four adjacent pads on one edge, same physical-grouping rule as every
// other board here, so a servo loom can be built once and moved between them.
// Channel order matched the retired boards/qtpy_s3.h (channel 1 = first pad of
// the block), so a topology's servo.channel means the same gate on any node.
#define SERVO_PWM_PIN_1    12   // D7
#define SERVO_PWM_PIN_2     8   // D8
#define SERVO_PWM_PIN_3     9   // D9
#define SERVO_PWM_PIN_4    10   // D10

#endif

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
// Optional, and PROVEN on this board: a panel ran on D4/D5 of a real C5 on
// 2026-08-22, after the DevKitC proved the driver on 2026-08-21. D4/D5 are
// the XIAO-standard I2C position, so Seeed's own accessories land on them, and they
// stay free whether this board is driving four PWM gates or an ST3215 slider.
// D1 is GPIO0, which is an ordinary pad on the C5 (the boot straps are 26/27/28),
// so a momentary-to-GND button there is safe even at reset.
// See firmware/wiring/xiao-c5.md §4 and the layouts in
// docs/mockups/oled-status.html.
// Fitted by naming the pins here, and no longer by a build flag: one env per
// board as of 2026-08-22, every one of them assuming a screen (see the note at
// the top of platformio.ini). The button on D1 was pressed on a real C5 and lit
// a real panel, 2026-08-22 — the only board where that is true.
#define PIN_OLED_SDA    23   // D4
#define PIN_OLED_SCL    24   // D5

// The screen sleeps after two minutes and nothing relights it on its own, so the
// button is the only way a person gets it back without walking to a phone — and a
// second press puts it out early. See utils/WakeButton.h. D1 is GPIO0, which would
// be the boot strap on most ESP32 parts and a bad place for a switch — on the
// C5 the straps are 26/27/28, so a normally-open momentary here is safe even at
// reset. D0 is deliberately not used: it is the only analog pad on the edge.
#define PIN_WAKE_BTN     0   // D1, INPUT_PULLUP, momentary to GND

// -- The serial-servo bus moved UP --
// It is defined with the PWM block it replaces (-DDUSTGATE_SERVO_BUS), because
// the two are one choice and reading them apart is what let the pin map claim
// both. Giving up PWM channel 1 is the right trade: one bus replaces the whole
// four-channel block and lifts the SERVO_COUNT ceiling with it.

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
//   output is discarded. That was the retired QT Py S3 / Feather S2 behaviour;
//   this board is the other kind, below.
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
// config.h derives HAS_LINEAR from the serial-bus pins now, so leaving these out
// is what makes this a servo-only build — the stepper driver, the
// feedback system and the endstop supervisor all compile out. Defining them
// here to "keep the interface uniform" would silently re-enable code that has
// no hardware behind it, and on this part would also re-introduce the
// single-core step-timing problem described at the top.
// -----------------------------------------------------------------------------
