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
//   STRAPPING PINS ARE CHECKED: on the C5 they are GPIO2, 3, 7, 25, 26, 27 and
//   28 (datasheet §2.3.4, in the priority-3 caution list). GPIO8 and GPIO9 are
//   NOT among them — that was C3 muscle memory (straps are GPIO2/8/9 there), and
//   the servo block below is clear.
//
//   Corrected 2026-09-04: this note used to read "GPIO25, 26, 27, 28, 7, MTMS
//   and MTDI", which omitted GPIO2 and GPIO3 and named two signals the datasheet
//   does not list as straps. Nothing was ever wired wrong — GPIO2 and GPIO3
//   reach no XIAO pad — but the omission mattered the moment someone went
//   looking for a free pad, which is exactly what happened.
//
//   The one strap we drive is GPIO25, the status pixel, and it is harmless: it
//   selects the SDIO sampling edge, a peripheral this build never uses, and a
//   WS2812 DIN is high-impedance so nothing holds the line at reset.
//
//   ⚠️ D3 IS GPIO7, AND GPIO7 IS A STRAP. It is the one free-looking pad on this
//   board that is not free. Nothing uses it today, and nothing should use it for
//   an input that can be held LOW at reset — see PIN_BIN_SENSOR below for the
//   case that nearly landed there.
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
// boots. The C5's straps are GPIO2/3/7/25/26/27/28 (datasheet §2.3.4; see the
// corrected list at the top of this file), so neither of these is one.
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

#elif defined(DUSTGATE_COLLECTOR)

// -- COLLECTOR board: the PWM block, minus the two pads the collector needs --
//
// A collector board drives NO GATES (docs/tool-sensing-rfc.md §6.2), so the four
// PWM pads are free to be spent on collector jobs instead. Two go to fob servos,
// one to the transmitter, and one is spare:
//
//   D7  fob servo, ON button   \  still PWM channels 1 and 2, same driver
//   D8  fob servo, OFF button  /   and the same move-then-detach
//   D9  315 MHz transmitter        (PIN_RF_TX, below)
//   D10 spare                      lamps, or a third fob button
//
// SERVO_COUNT IS 2 HERE, and that is the whole difference from the gate build.
// The fob servos ARE channels 1 and 2 — same ServoActuator, same `servo` and
// `stroke` bench commands — so nothing new drives them. What changes is that
// channels 3 and 4 do not exist, because D9 is the transmitter.
#define SERVO_PWM_PIN_1    12   // D7 — fob servo, ON (or the only button)
#define SERVO_PWM_PIN_2     8   // D8 — fob servo, OFF, on a two-button fob

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

// -- Optional: dust-bin level sensor (collector boards) --
//
// One input pin, and that is the whole feature — which is why bin sensing is a
// CAPABILITY rather than a board role: it collides with nothing, so it needs no
// env of its own and works on a primary and a node alike. See
// docs/shop-schema-rfc.md §7.5, which supersedes §7.4's "new node type".
//
// Banner QS18VN6D diffuse photoelectric through a PC817 optocoupler. The `VN` is
// load-bearing: NPN, sinking, open-collector. The `VP` variant SOURCES +12 V and
// would kill this pin — check the part stamped on the sensor, not the notes.
//
// ⚠️ THE OPTOCOUPLER INVERTS THE SENSE: this pin reads LOW when the bin is FULL.
// That reads as a wiring fault at the bench if you are not expecting it, and it
// is why the schema carries `bin.sensor.invert` — someone who wires the sensor
// straight to a pull-up instead (simpler, less isolation, rejected in §7.4) gets
// the opposite polarity and should not need a reflash to say so.
//
// WHY D6 (GPIO11). It is an ordinary pad, and it is free on every build except
// the slider — where D6/D7 are the servo bus — and a board at a collector will
// never also be driving a rack. On a primary already driving four PWM gates with
// a screen, D0/D3/D6 are the only pads left, D3 is a strapping pin (see above),
// and D0 is the only analog pad on the edge and is spoken for by a future CT.
// That leaves exactly this one.
//
// GPIO11 is UART0 TX, which this build never uses — the console is USB
// Serial/JTAG. It is also PIN_SERVO_BUS_TX on a slider build, which is why
// config.h derives HAS_BIN with the bus excluded rather than trusting the pin
// to be free.
//
// DEFINED means "this board COULD have one", not "one is fitted" — the same
// contract as HAS_LINEAR. Whether a given board actually watches a bin is a
// TOPOLOGY fact (`bin.sensor.controllerId`), because the primary is the only
// thing that knows. There is no probe for it the way an I2C ACK settles the
// screen: a digital input cannot be asked whether anything is on the other end.
//
// So the pin is read with INPUT_PULLUP, and an unwired board reads HIGH — which
// with the inversion above means "bin OK". A board with nothing connected must
// not scream, and topology gates it regardless.
//
// Wire — a DISCRETE 4N35, TWO SIDES THAT NEVER MEET:
//   12 V side:  QS18 brown -> +12 V, blue -> 12 V GND.
//               +12 V -> 1 kOhm -> 4N35 pin 1 (LED anode).
//               QS18 black (output) -> 4N35 pin 2 (LED cathode).
//   ESP32 side: 4N35 pin 4 (emitter) -> ESP32 GND.
//               4N35 pin 5 (collector) -> D6, plus 10 kOhm from D6 to 3V3.
//               Pins 3 and 6: leave open.
//
// NOT a PC817 breakout board. One measured 4 V on its output (2026-09-12),
// above this part's absolute maximum on a GPIO — those boards commonly pull the
// output up to the INPUT side's supply, which overvolts the pin AND shorts out
// the isolation. A discrete part has no hidden pull-up.
//
// The 1 kOhm gives ~10.8 mA, the 4N35's rated test point — a partly-on
// phototransistor is what produced an unusable 2 V reading first time round.
// The 10 kOhm is not optional either: INPUT_PULLUP's internal ~45 kOhm is
// feeble against leakage. firmware/wiring/collector-node.md §2.
//
// ⚠️ DO NOT TIE THE 12 V GROUND TO THE ESP32 GROUND. This comment said to, for
// weeks, and it was wrong (corrected 2026-09-11 — Jeff asked whether BOTH sides
// grounded to the ESP32, which is the question that exposed it). Joining them
// shorts across the optocoupler and throws away the only thing it does.
//
// The module's GND pin IS the ESP32 ground — that is the output side's
// reference, and it is already connected. The 12 V ground is the INPUT side's
// reference and belongs to the 12 V supply alone. Nothing floats: each side has
// its own return.
//
// Why it matters here specifically. The 12 V supply sits next to a dust
// collector — a large induction motor, feet away from a CT clamp whose noise
// floor is already unresolved (wiring/ct-bench.md §5.5). A deliberate ground
// loop between that supply and the ADC's reference is the last thing this board
// needs. And a fault on the 12 V side would have a path straight through the
// ESP32's ground rather than staying on its own side of the barrier.
//
// ⚠️ AND IT IS MOOT IF THE BOARD IS POWERED OFF THE SAME 12 V. A plain buck
// converter shares its input and output ground by definition, so a 12->5 V
// regulator ties the two grounds upstream and there is no barrier left to
// short. On that build the opto is a LEVEL SHIFTER, not an isolator — still
// earning its place, because the QS18 swings to 12 V and 12 V on a 3.3 V pin
// destroys it. wiring/collector-node.md §6 has the three power topologies and
// which one to pick; one brick at the collector is the better install, so
// expect the common-ground case to be the normal one.
//
// A non-isolated build IS allowed — sensor straight to a pull-up, which §7.4 of
// the schema RFC rejected — and that one has a single shared ground by
// definition. It also has the opposite polarity, which is what
// `bin.sensor.invert` is for. What is not allowed is paying for the isolated
// wiring and then shorting the barrier out on purpose.
#if !defined(DUSTGATE_SERVO_BUS)
#define PIN_BIN_SENSOR  11   // D6, opto output, LOW = bin full
#endif

// -- 315 MHz transmitter: pressing the collector's remote --
//
// The DATA line of a 315 MHz OOK transmitter module, keyed with the HT12E frame
// the dust collector's own fob sends (control/RfCollectorPresser.h,
// docs/tool-sensing-rfc.md §4.2). One pin, output only — the module needs no
// enable and we never receive.
//
// D9, NOT D6 — corrected 2026-09-10, hours after being written wrong.
//
// This first sat on D6 with a note saying the clash with PIN_BIN_SENSOR was
// fine "because they never share a board". That was wrong the moment the
// collector board was defined: bin level, CT and transmitter all live on it
// (§6.2), so they share a board by design and D6 can only be one of them.
//
// The pin budget on a COLLECTOR board, which drives no gates:
//
//   D0   CT clamp        the only analog pad on the edge
//   D6   bin sensor      opto output; it had this pad first
//   D7   fob servo ON    \  the PWM block, free because no gates
//   D8   fob servo OFF   /
//   D9   RF transmitter  <- here
//   D10  spare           lamps, or a third fob button
//
// ⚠️ ON A GATE-DRIVING BOARD D9 IS SERVO CHANNEL 3. Nothing detects that: which
// jobs a board does is a topology fact, so a layout naming both a servo gate on
// channel 3 and `control.rf` without an explicit pin on the same controller is
// a wiring conflict that will simply not work. Give control.rf an explicit
// `pin` on any board that also drives gates — or better, do not ask a gate
// board to transmit, which is what §6.2 decided.
//
// RANGE argues the same way: the transmitter wants to be near the receiver, and
// the routing brain may be across the shop.
//
// NOT WIRED BY DEFAULT. Defining the pin says "this is where it would go", the
// same contract as PIN_BIN_SENSOR — whether a board actually transmits is
// decided by the layout's `control.rf`, which carries its own pin and overrides
// this. This is the fallback the serial `press` command uses so the radio can
// be tested before any layout names it.
//
// Wire: module VCC -> 5V (the cheap modules want 5V for useful range; the DATA
//       line is 3.3V-tolerant as an input), GND -> GND, DATA -> D6.
//       A 17 cm wire on the module's ANT pad is a quarter wave at 315 MHz and
//       is worth more than anything else on this list.
#if defined(DUSTGATE_COLLECTOR)
#define PIN_RF_TX        9   // D9 — free here because SERVO_COUNT is 2
#elif !defined(DUSTGATE_SERVO_BUS)
// On a GATE board D9 is servo channel 3. Defined anyway so the bench `press`
// command works on an ordinary primary with nothing on channel 3 — which is how
// the transmitter was first proven — but a board actually driving four gates
// must give control.rf an explicit pin, or it will fight channel 3.
#define PIN_RF_TX        9   // D9 — ⚠️ also SERVO_PWM_PIN_3 on this build
#endif

// -- CT clamp: the collector's own draw --
//
// D0/GPIO1 is THE ONLY ANALOG PAD on this edge, which is why nothing else may
// have it and why the wake button's note says D0 is deliberately left alone.
//
// Collector builds only. A gate board has no use for it and defining it there
// would imply the bias network is fitted, which on a gate board it is not.
// See firmware/wiring/collector-node.md §3 for the divider — and its warnings,
// because the noise floor is unresolved and the screen is part of it.
#if defined(DUSTGATE_COLLECTOR)
#define PIN_CT              1   // D0, the only ADC pad on this edge
#endif

// -- Fob servos: pressing the collector's remote mechanically --
//
// TWO, and the second one is not a spare. One servo presses one button, and the
// Rockler's single button is a TOGGLE — stateless, so a missed or doubled press
// inverts what the system believes and only the blower's draw can correct it.
// A fob with SEPARATE ON AND OFF buttons is momentary to press but IDEMPOTENT in
// meaning: pressing ON twice leaves it on, so a missed press self-corrects and
// no belief can invert. That is a strictly better control, and it costs exactly
// one more pad (docs/tool-sensing-rfc.md §4.2b).
//
// So: two pads reserved. A single-button fob uses the first and leaves the
// second unwired; a two-button fob uses both and stops needing the feedback loop
// to stay correct — though it still wants it, to notice a flat fob battery, a
// tripped breaker or an arm that has drifted out of alignment.
//
// THESE ARE SERVO CHANNELS 1 AND 2. Same pads, same PWM bank, same
// move-then-detach behaviour — what differs is who commands them: the press
// policy rather than the router. That is only free because a COLLECTOR BOARD
// DRIVES NO GATES (§6.2), so nothing else wants those channels. On a board that
// also drives gates these are the first two gates, and a layout claiming both is
// a conflict nothing currently detects.
//
// A four-button fob (power plus fan speeds, like a WEN air cleaner) would want
// four, which is the whole PWM block — possible on a collector board, and
// exactly the point at which "one arm that travels between buttons" starts
// looking cheaper than a servo per button.
#if defined(DUSTGATE_COLLECTOR)
// Aliases, not a second definition — these ARE servo channels 1 and 2. Named so
// the intent is readable where a press is commanded rather than a gate move.
#define PIN_FOB_SERVO_ON   SERVO_PWM_PIN_1   // D7
#define PIN_FOB_SERVO_OFF  SERVO_PWM_PIN_2   // D8
#endif

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
