# Wiring — Adafruit QT Py ESP32-S3 (servo-only node)

> **This is the node board.** Decision 2026-08-14: nodes are developed on the QT
> Py S3; the [XIAO ESP32C5](xiao-c5.md) is a parked option. It is the board that
> has actually been flashed, joined WiFi and answered a primary.
>
> **Still unproven: the servo pins with a servo attached.** The chip identity and
> flash geometry below are measured (`esptool.py flash_id`); A0–A3 come from
> Adafruit's pinout and have driven nothing yet. Unlike the C5, none of them are
> strapping pins, so the failure mode here is a servo that doesn't move — not a
> board that won't boot.
>
> Authoritative source for every number here:
> [`firmware/boards/qtpy_s3.h`](../boards/qtpy_s3.h). If this file and that
> header ever disagree, the header is right — the build reads it.

**Role:** servo-only secondary node. Four PWM gates, no stepper, no endstops.

Board-independent chapters — the status pixel's colour vocabulary and external
wiring, smart plugs, power supply, decoupling — are in
[`WIRING.md`](../WIRING.md).

## Which QT Py S3 you have

Two Adafruit parts share this pin map exactly:

| Part | Flash | PSRAM | PlatformIO `board` |
|------|-------|-------|--------------------|
| **5700 (N4R2)** — the one on the bench | 4 MB | 2 MB | `adafruit_qtpy_esp32s3_n4r2` |
| 5426 (no PSRAM) | 8 MB | — | `adafruit_qtpy_esp32s3_nopsram` |

Only the `board` id in `platformio.ini` differs. Flashing the wrong one fails
loudly at connect (a flash-size mismatch), so a mix-up can't silently brick a
board. Confirm what's in your hand:

```bash
esptool.py --port /dev/cu.usbmodem* flash_id
```

A node needs neither the RAM nor the flash — the whole build is 27.8% of the
4 MB part, and a node's entire interface is the `/nodelink` WebSocket, so there
is no Angular bundle to store.

## Why it is servo-only

The S3 is **dual-core**, so unlike the C3 and the C5 that is not the reason. The
reason is that a secondary receives already-resolved `SET` frames — a channel and
an angle, never a topology — so the least hardware that can be a DustGate node is
a WiFi MCU with four PWM pins. No stepper, no TMC2209, no endstops, no web UI.

`config.h` derives `HAS_LINEAR` from whether `PIN_TMC_STEP` is defined, so the
motor pins being **absent** from the header is what compiles the stepper driver,
the feedback system and the endstop supervisor out. Adding them "to keep the
interface uniform" would silently re-enable code with no hardware behind it.

---

## 1. Pin map

### Top view — the labels you can't see once it's docked

Pad labels are on the **underside**, so they vanish the moment the board is in a
socket. This is that label, from above. **The USB-C connector is the orientation
reference**; the STEMMA QT connector at the opposite end is the second one.

```
                          USB-C
                        ┌───────┐
              ┌─────┬───┴───────┴───┬─────┐
   GPIO18  A0 │  o  │               │  o  │ 5V      do NOT power servos here
   GPIO17  A1 │  o  │               │  o  │ GND     ← servo/pixel/screen ground
    GPIO9  A2 │  o  │   QT Py       │  o  │ 3V      ← screen VCC (never 5V)
    GPIO8  A3 │  o  │   ESP32-S3    │  o  │ MO   GPIO35  ── external pixel DIN
    GPIO7 SDA │  o  │               │  o  │ MI   GPIO37  ── WAKE BUTTON (§4)
    GPIO6 SCL │  o  │  (top view)   │  o  │ SCK  GPIO36
    GPIO5  TX │  o  │               │  o  │ RX   GPIO16
              └─────┴───┬───────┬───┴─────┘
                 ▲      └───────┘
                 │      STEMMA QT ── SCREEN GOES HERE (§4)
                 │      GPIO41 SDA1 / GPIO40 SCL1 — a separate bus, so it
                 │      costs no pad and collides with nothing
                 └── servo block: the FOUR pads NEAREST the USB-C end,
                     channel 1 in the corner beside the connector

   The SDA/SCL pads above (GPIO7/6) are the screen's FALLBACK wiring, for a bare
   panel with flying leads rather than a STEMMA cable — see §4. Prefer the
   connector; it cannot be miscounted.

   NeoPixel: GPIO39 data, GPIO38 power — onboard, no wiring needed
   Buttons at the USB-C end: RESET, and BOOT on GPIO0 — both vanish in a case
```

The screen and its wake button are one optional fitting: `-DHAS_STATUS_SCREEN`
defines the STEMMA pair and MISO together, so either all three are spoken for or
none are. The button is on **MISO, not one of the SDA/SCL pads**, because those
two are the fallback wiring for a bare panel — a button that only worked when the
screen arrived on a STEMMA cable would be a trap.

**Counting rule when it's docked:** hold the USB-C end *toward* you. The servo
block is the near end of the left column, channel 1 closest to the connector.
That is the **opposite** end from the [XIAO C5](xiao-c5.md), whose servo block is
the far corner — worth knowing if both are on the bench, because the looms are
interchangeable and the boards are not.

Download mode is hold BOOT → tap RESET → release BOOT. Pressing BOOT alone does
nothing, which is easy to mistake for a dead board.

> Two different confidences in that drawing. Every **GPIO number** is read out of
> Adafruit's own Arduino variant header
> (`variants/adafruit_qtpy_esp32s3_n4r2/pins_arduino.h`), not transcribed from a
> picture. The **physical order of the non-servo pads down each side** follows the
> QT Py form factor and hasn't been checked against this board with a meter — it
> doesn't matter for anything DustGate drives, but don't wire I²C off this drawing
> without looking. [`boards/qtpy_s3.h`](../boards/qtpy_s3.h) is what the build
> reads.

### The numbers

| Signal              | Pad | GPIO | Notes |
|---------------------|-----|------|-------|
| Servo PWM channel 1 | A0  | 18   | |
| Servo PWM channel 2 | A1  | 17   | |
| Servo PWM channel 3 | A2  | 9    | Ordinary GPIO on the S3 — not strapping |
| Servo PWM channel 4 | A3  | 8    | Ordinary GPIO on the S3 — not strapping |
| Status pixel (DIN)  | —   | 39   | Onboard NeoPixel |
| Status pixel power  | —   | 38   | Must be driven HIGH before the pixel lights |
| External pixel (DIN)| MOSI| 35   | Optional second pixel, driven 4× brighter — see below |
| Status screen SDA *(opt)* | STEMMA QT | 41 | `SDA1` — a second bus, no header pad — see §4 |
| Status screen SCL *(opt)* | STEMMA QT | 40 | `SCL1` |
| Wake button *(opt)* | MISO| 37   | Momentary to GND, `INPUT_PULLUP`. Fitted with the screen — see §4 |
| Onboard user LED    | —   | —    | There isn't one — the pixel is the only indicator |

### The external pixel

The onboard pixel is a bench indicator. Once this board is screwed to a joist
above a manifold it is invisible from anywhere you would actually stand, so
`PIN_PIXEL_EXT` takes a second WS2812 on the **MOSI pad (GPIO35)**.
`StatusLed.h` drives both in parallel with the same colour — the onboard one at
1×, the external at `PIXEL_EXT_GAIN` (4×, so `kBright` 48 → 192). Solder nothing
and the pad costs a few bytes clocked into open air.

```
5V (or 3V3 — see WIRING.md §1) ──── Pixel VDD
GND ─────────────────────────────── Pixel GND ──── QT Py GND   (common, mandatory)
GPIO35 ──── [330R] ──────────────── Pixel DIN
```

MOSI was picked because it is free on **both** QT Py builds — the node uses
A0–A3 for servos, the linear build uses A0–A3 plus SDA/SCL/TX — so one loom fits
either without re-pinning, and it sits on the same edge as 5V and GND, so all
three wires leave from one corner. Nothing here uses SPI.

⚠ GPIO35–37 are the octal-PSRAM pins on some ESP32-S3 modules. Both variants
this map covers (N4R2 quad PSRAM, and the no-PSRAM 5426) leave them free — worth
re-checking before reusing this pad on a different S3.

**Strapping pins on this part are GPIO0, 3, 45 and 46**, none of which are used
here. That is why this map can spend GPIO8 and GPIO9 freely while the C3 and C5
maps have to dodge them.

**Deliberately absent: motor and endstop pins** — see above.

---

## 2. Servo block

A0–A3 are four **adjacent pads on one edge**, chosen so a servo loom can be built
once and moved between boards. Channel 1 is the first pad of the block on every
node header here, so a topology's `servo.channel` means the same gate whichever
board drives it.

```
  5V/6V supply ──┬────────────┬───── servo V+   (red)
                 │            │
            [470-1000µF]   [0.1µF]      <-- AT the servo terminals
                 │            │
  GND ───────────┴────────────┴───── servo GND (brown/black)

  GPIO18/17/9/8 ─────────────────────  servo signal (orange/yellow)
  QT Py GND ─────────────────────────  common with servo GND   (REQUIRED)
```

**Never power servos from the QT Py's 5V pad.** It is the USB rail through a
tiny board: four servos stalling on it browns out the MCU, and the failure looks
like a WiFi dropout rather than a power problem. Feed servos from the buck
directly and give the board its own leg off the same buck. See
[`WIRING.md` §5](../WIRING.md#5-decoupling--keeping-the-esp32-out-of-brownout).

### Reserved: serial-servo bus

TX/RX (GPIO5/GPIO16) are the hardware UART and are **not** shared with any servo
channel — unlike the C5, where the bus and PWM channel 1 are the same pad. So a
bus-servo build on this board gives up nothing. Left out of the header until
there is a bus servo on the bench to talk to.

---

## 3. Status pixel

Onboard, on GPIO39 — **nothing to wire.** The one board-specific catch is
`PIN_PIXEL_POWER` (GPIO38): the pixel's rail is gated and must be driven HIGH
before it will light at all. `StatusLed.h` does that; a hand-rolled sketch that
skips it sees a pixel that appears dead.

Colour vocabulary is in
[`WIRING.md` §1](../WIRING.md#1-status-pixel-ws2812--neopixel), unchanged here.

There is no plain user LED to fall back to, which is why `PIN_LED` is
deliberately not defined in the header.

---

## 4. Status Screen (SSD1306 OLED) — optional

> **NEVER TESTED ON THIS BOARD.** A panel works on the DevKitC as of 2026-08-21
> (`wiring/devkitc.md` §5), so the firmware path is proven — but nothing has been
> wired to a QT Py, the STEMMA pins below are still transcribed from Adafruit's
> pinout, and **no env sets `-DHAS_STATUS_SCREEN` for this board**. See
> [`WIRING.md` §6](../WIRING.md#6-status-screen-ssd1306-oled--optional) for what the
> screen is for, the burn-in/sleep behaviour and the 3V3 rule; the layouts are in
> [`docs/mockups/oled-status.html`](../../docs/mockups/oled-status.html). Only the
> QT Py-specific part is here.

**This board has the easiest version of it on the whole fleet: use the STEMMA QT
connector.** It is a *second* I²C bus on its own 4-pin JST SH socket, so a screen
on a STEMMA cable costs **no header pad, no soldering, and nothing this node
already uses** — the servo block and both pixels are untouched. That is what makes
a screen retrofittable on a node that is already screwed to a joist, which is not
true of the DevKitC (where fitting one spends its last two spare pins).

| Signal | Where | GPIO | Note |
|---|---|---|---|
| SDA | STEMMA QT | 41 | `SDA1` — the second bus, not the header pads |
| SCL | STEMMA QT | 40 | `SCL1` |

```
STEMMA QT socket ──[ 4-pin JST SH cable ]── SSD1306 with a STEMMA QT socket
                                            (power and both signals in the cable)
```

For a bare module with flying leads instead, the **header pads SDA/SCL (GPIO7 and
GPIO6)** are free on a node build — the servos take A0–A3 and the external pixel
takes MOSI, so nothing collides. Wire it the ordinary way and swap the two numbers
in [`boards/qtpy_s3.h`](../boards/qtpy_s3.h); `Wire.begin()` takes whichever pair
the header names.

```
3V3 ────────── VCC        (never 5V — see WIRING.md §6)
GND ────────── GND ────── QT Py GND   (common, mandatory)
GPIO7  (SDA) ─ SDA
GPIO6  (SCL) ─ SCL
```

⚠ Same caveat as everything in [§1](#1-pin-map): the GPIO numbers come from
Adafruit's variant header, but the **physical pad order down each side has not been
checked with a meter**. Count pads before wiring I²C off the drawing — and prefer
the STEMMA connector, which cannot be miscounted.

### The wake button

The screen blanks after two minutes so it doesn't burn a static layout into itself
on a node that idles for weeks, and since 2026-08-22 nothing — not even a fault —
holds it lit past that. The button is the only way a person gets it back without
walking to a phone: one edge, one wake, nothing else. A screen build that doesn't
define it won't compile. `-DHAS_STATUS_SCREEN` fits
the button along with the panel, since there is nothing for it to wake otherwise.

```
MISO (GPIO37) ──── [momentary NO] ──── GND
```

`INPUT_PULLUP`, which works on every pad on this part — no external resistor, unlike
the DevKitC. **MISO rather than one of the SDA/SCL pads**, because those two are the
fallback wiring for a bare panel above: a button that only worked when the screen
came in on a STEMMA cable would be a trap. Nothing here uses SPI, and it sits beside
the external pixel's MOSI, so the indicator group leaves from one corner. Same
GPIO35–37 octal-PSRAM caveat as the pixel.

The driver is [`utils/WakeButton.h`](../utils/WakeButton.h), and it does one thing:
lights the glass. There is no long-press and no menu — a button that could change
what the shop *does* would need every confirmation the web UI has. **No button has
been wired to any board yet.**

### Turning it on

Declared by the build, not probed for — the same seam that answers "is a stepper
fitted?" elsewhere. `-DHAS_STATUS_SCREEN` activates the `PIN_OLED_*` block in the
board header; the driver is [`utils/StatusScreen.h`](../utils/StatusScreen.h) over
[`utils/StatusScreenModel.h`](../utils/StatusScreenModel.h), which decides what the
screen says and is host-tested against the 21×8 character budget.

What is missing here is only the env: copy `dustgate_node` in `platformio.ini`, add
the flag, and extend its `lib_deps` with the two Adafruit libraries (SSD1306 + GFX).
Budget about **32 KB of flash** — irrelevant on this board's `huge_app` partition,
and the reason it stays opt-in rather than default.

A declared-but-absent panel is handled everywhere the same way: no ACK at 0x3C, a
`[SCREEN] declared but no ACK` line on serial, driver disabled. It must never hang a
node in Wire's timeout once per loop.

### What a node's screen actually says

A node's whole world is one question — *can the brain reach me?* — so its screens
are the two at the bottom of the layouts page: `LINKED`, naming the primary that
owns it and ageing the last command, or `UNLINKED`, which is the state the pairing
bug currently needs a serial monitor to see at all. Jeff wants screens on the nodes
in his own shop; a product cannot require one on every board in the building, which
is why this is a fitting and not a feature.

---

## 5. If it looks dead

**Check the monitor's line handling before suspecting the board.** This board's
convention is the **opposite** of the C5's, and getting it wrong produces silence
either way:

| USB kind | Boards | DTR/RTS | Symptom when wrong |
|----------|--------|---------|--------------------|
| TinyUSB CDC | **QT Py S3**, Feather S2 | hold **HIGH** | `USBCDC::write()` discards every byte — boots fine, prints nothing |
| USB Serial/JTAG | XIAO C5 | hold **LOW** | DTR+RTS *is* the ROM download-mode trigger — board leaves the app |
| Bridge chip | DevKitC | hold LOW | auto-reset loop |

`platformio.ini` sets `monitor_dtr = 1` / `monitor_rts = 1` for this env, and
`tools/boardinfo.sh` derives the kind from `BOARD_HAS_NATIVE_USB` /
`BOARD_USB_SERIAL_JTAG` in the board header, so going through the scripts is
always right:

```bash
bash dev.sh monitor s3
```

The port **disappears on every reset** — USB comes straight off the MCU, there is
no bridge chip to hold it up. That is normal, and why `dev.sh` leaves pio's
monitor reconnect enabled for this env instead of `--no-reconnect`.

Reading the port directly is the tie-breaker when a board seems dead, since it
can't reset anything (note `dtr=True` here — the inverse of the C5's recipe):

```bash
python3 -c "import serial,sys; p=serial.Serial(); p.port='/dev/cu.usbmodem1101'; p.baudrate=115200; p.dtr=True; p.rts=True; p.timeout=0.2; p.open(); [sys.stdout.write(p.read(4096).decode('utf-8','replace')) for _ in range(60)]"
```

A healthy boot prints a `[BOOT]` line per stage — `serial`, `claim`, `wifi`,
`servos`, `mdns`, `server`, `ready` — with `ready` a couple of seconds in. A
truncated trace names the stage it died in, which is the point of it.

---

## 6. Flashing it

Picks the env, the native-USB port and the right DTR/RTS convention, then prompts
for WiFi credentials and a hostname:

```bash
bash dev.sh flash-node s3
```

Or with the hostname up front — **it must be unique per node**, since that string
is the board's identity in the claim handshake and the name the primary's board
picker binds to:

```bash
bash dev.sh flash-node s3 dustgate-node-1
```

Build only, no flashing:

```bash
pio run -e dustgate_node
```

After it joins, it advertises itself over mDNS as `role=secondary` and shows up
in the primary's **Boards** screen under "Scan for boards".
