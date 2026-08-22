# Wiring — Seeed XIAO ESP32C5 (servo-only node)

> ## The servo block is proven. The rest of this map is not.
>
> A supported node target as of 2026-08-14, and as of **2026-08-21 all four PWM
> channels have driven real servos** — the signal reaches the pin on every one of
> D7/D8/D9/D10. That closes the question this banner used to be about: the pin
> numbers came from Seeed's published pinout rather than a multimeter, and now
> the four that matter have been confirmed by a servo moving.
>
> Earlier: flashed and booted 2026-08-13 (full boot log, WiFi joined, `ready` at
> 1986 ms), and **the strapping-pin worry closed** 2026-08-19 — GPIO8/GPIO9 are
> ordinary IO on this part, so a servo signal idling there does not hold the board
> out of its app. See [§6](#6-before-you-trust-this).
>
> **Still unproven on this board:** the status screen pins (D4/D5 — a panel works
> on a DevKitC but nothing has been wired to a C5), the serial-servo bus pads, and
> every non-servo pad's physical position, which is still Seeed's drawing rather
> than something traced. If it appears not to boot, read
> [§5](#5-if-it-looks-dead) first, because the first time, it was booting fine.
>
> Authoritative source for every number here:
> [`firmware/boards/xiao_c5.h`](../boards/xiao_c5.h). If this file and that header
> ever disagree, the header is right — the build reads it.

**Role:** servo-only secondary node. Four PWM gates, no stepper, no endstops.

Board-independent chapters — the status pixel's colour vocabulary and external
wiring, smart plugs, power supply, decoupling — are in
[`WIRING.md`](../WIRING.md).

## Why this board is interesting

- Real Espressif module in a thumb-sized package, USB-C, native USB.
- **8 MB flash + 8 MB PSRAM** against the DevKitC's 4 MB and no PSRAM.
- **Dual-band WiFi 6**, so the controller's own link can leave a 2.4 GHz band
  that a shop full of motors and Shelly plugs has already crowded.

## Why it is servo-only, and why that is the point

The C5 is **single-core**. The primary's design leans on two: the Shelly poller
and the NodeLink client are pinned to core 0 specifically so their blocking HTTP
can never disturb step generation on core 1. Software step pulses are the one
workload here that turns scheduler jitter into physical error — lost steps, a
rough-sounding rail.

Servos have no such problem. PWM is LEDC hardware and a bus servo is a UART
write; both are fire-and-forget, and neither cares that something else is using
the CPU. So single-core is a bad trade for a stepper primary and a non-issue for
a servo-only board — which is why this pin map defines no motor pins at all.

The same reasoning governs every servo-only node header here.

---

## 1. Pin map

### Top view — the labels you can't see once it's docked

The silkscreen is on the **bottom** of the board, so every pad label disappears
the moment it's in a socket. This is that label, from above. **The USB-C
connector is the only orientation reference** — the board is otherwise
symmetrical, and it is easy to count a servo lead onto the wrong row.

```
                          USB-C
                        ┌───────┐
              ┌─────┬───┴───────┴───┬─────┐
   GPIO1   D0 │  o  │               │  o  │ 5V      do NOT power servos here
   GPIO0   D1 │  o  │               │  o  │ GND     ← servo/pixel/screen ground
  GPIO25   D2 │  o  │    XIAO       │  o  │ 3V3     ← screen VCC (never 5V)
   GPIO7   D3 │  o  │    ESP32C5    │  o  │ D10  GPIO10   ── servo ch 4
  GPIO23   D4 │  o  │               │  o  │ D9   GPIO9    ── servo ch 3  ⚠
  GPIO24   D5 │  o  │  (top view)   │  o  │ D8   GPIO8    ── servo ch 2  ⚠
  GPIO11   D6 │  o  │               │  o  │ D7   GPIO12   ── servo ch 1
              └─────┴───────────────┴─────┘
                 ▲ ▲ ▲                 ▲
                 │ │ │                 └── servo block: the FOUR pads
                 │ │ │                     furthest from the USB-C end,
                 │ │ │                     channel 1 nearest the corner
                 │ │ └── D4 = screen SDA, D5 = screen SCL  (§4, optional)
                 │ └──── D2 = status pixel DIN
                 └────── D1 = wake button, momentary to GND (§4, optional)
```

The optional parts are the screen and its button, and they are one fitting: with
`-DHAS_STATUS_SCREEN` the board header defines all three pads at once, so D1, D4
and D5 are either all spoken for or all free. What's left after fitting them is
D0, D3 and D6.

Counting rule when it's docked and you can see nothing: **hold the USB-C end
away from you.** Left column is D0→D6 running away from the connector; right
column is 5V, GND, 3V3, then D10→D7 running toward you. Servo channel 1 is the
pad in the corner *diagonally opposite* the USB-C connector.

The two buttons are at the USB-C end: **RESET** reboots, **BOOT** does nothing on
its own. Download mode is hold BOOT → tap RESET → release BOOT. Pressing BOOT
alone on a hung board gets you nothing, which is easy to mistake for a dead board.

> Same caveat as the rest of this file: the drawing is Seeed's published pinout
> redrawn, not a board traced with a multimeter. The header
> [`boards/xiao_c5.h`](../boards/xiao_c5.h) is what the build reads.

### The numbers

XIAO silkscreen pads D0–D10 map to GPIO **1, 0, 25, 7, 23, 24, 11, 12, 8, 9, 10**
— confirmed 2026-08-16 against Seeed's pin-definition drawing, so this row is no
longer hearsay.

| Signal              | Pad | GPIO | Notes |
|---------------------|-----|------|-------|
| Servo PWM channel 1 | D7  | 12   | Also the hardware UART TX — see §3 |
| Servo PWM channel 2 | D8  | 8    | Ordinary GPIO on the C5 — not strapping. Alt: SDIO_DATA0 |
| Servo PWM channel 3 | D9  | 9    | Ordinary GPIO on the C5 — not strapping. Alt: SDIO_CLK |
| Servo PWM channel 4 | D10 | 10   | Alt: SDIO_CMD |
| Status pixel (DIN)  | D2  | 25   | External part; onboard LED is green, not RGB. Strapping — see §5 |
| Onboard user LED    | —   | 27   | Green, single colour. Strapping pin, but latched at reset — see §6. Fallback only |
| Status screen SDA *(opt)* | D4 | 23 | XIAO-standard I²C — see §4 |
| Status screen SCL *(opt)* | D5 | 24 | XIAO-standard I²C — see §4 |
| Wake button *(opt)*  | D1  | 0    | Momentary to GND, `INPUT_PULLUP`. Not strapping on the C5 |

**Deliberately absent: motor and endstop pins.** `config.h` derives `HAS_LINEAR`
from whether `PIN_TMC_STEP` is defined, so leaving them out is what makes this a
servo-only build — the stepper driver, the feedback system and the endstop
supervisor all compile out. Defining them "to keep the interface uniform" would
silently re-enable code with no hardware behind it, and on this part would
re-introduce the single-core step-timing problem above.

---

## 2. Servo block

D7–D10 are four **adjacent pads on one edge**, chosen so a servo loom can be
built once and moved between boards. Channel order matches
[`boards/qtpy_s3.h`](../boards/qtpy_s3.h)
— channel 1 is the first pad of the block — so a topology's `servo.channel`
means the same gate on any node.

```
  5V/6V supply ──┬────────────┬───── servo V+   (red)
                 │            │
            [470-1000µF]   [0.1µF]      <-- AT the servo terminals
                 │            │
  GND ───────────┴────────────┴───── servo GND (brown/black)

  GPIO12/8/9/10 ─────────────────────  servo signal (orange/yellow)
  XIAO GND ──────────────────────────  common with servo GND   (REQUIRED)
```

**Never power servos from the XIAO's 5V pad.** Same rule as every other board
here, and it matters more on a part this small: feed servos from the buck
directly, and give the board its own leg off the same buck. See
[`WIRING.md` §5](../WIRING.md#5-decoupling--keeping-the-esp32-out-of-brownout).

### Reserved: serial-servo bus

D6/D7 (GPIO11/GPIO12) are the hardware UART. **D7 doubles as servo channel 1**,
so a build driving a serial-bus servo gives up PWM channel 1 — the right trade,
since one bus replaces the whole four-channel block and lifts the `SERVO_COUNT`
ceiling with it. Left commented out in the header until there is a bus servo on
the bench to talk to.

---

## 3. Status pixel

The onboard indicator is a plain yellow LED on GPIO27, so a colour indicator
means adding a part. GPIO25 (D2) is a plain pad with no bus function to give up.

```
5V (or 3V3 — see WIRING.md §1) ──── Pixel VDD
GND ─────────────────────────────── Pixel GND ──── XIAO GND   (common, mandatory)
GPIO25 ──── [330R] ──────────────── Pixel DIN
```

Colour vocabulary, the 3V3-vs-5V logic question and the bulk cap are in
[`WIRING.md` §1](../WIRING.md#1-status-pixel-ws2812--neopixel) — all of it applies
here unchanged.

**Don't want to spend the pad?** Delete `PIN_PIXEL` from the board header and
define `PIN_LED 27` instead; `StatusLed.h` falls back to blink patterns on the
onboard LED. Strictly worse — that ambiguity is exactly why the pixel exists —
but free.

---

## 4. Status Screen (SSD1306 OLED) — optional

> **NEVER TESTED ON THIS BOARD.** The driver is proven on a DevKitC as of
> 2026-08-21 (`wiring/devkitc.md` §5), but nothing has been wired to a C5 and
> D4/D5 below is still Seeed's published convention rather than a measurement.
> The build is ready and waiting:
>
> ```
> PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_screen
> ``` See
> [`WIRING.md` §6](../WIRING.md#6-status-screen-ssd1306-oled--optional) for what the
> screen is for, the burn-in/sleep behaviour and the 3V3 rule; the layouts are in
> [`docs/mockups/oled-status.html`](../../docs/mockups/oled-status.html). Only the
> C5-specific part is here.

| Signal | Pad | GPIO | Note |
|---|---|---|---|
| SDA | D4 | 23 | the XIAO-standard I²C position |
| SCL | D5 | 24 | ditto |

```
3V3 ────────── VCC        (never 5V — see WIRING.md §6)
GND ────────── GND ────── XIAO GND   (common, mandatory)
D4 (GPIO23) ── SDA
D5 (GPIO24) ── SCL
```

**These are the pads Seeed's own I²C accessories expect**, so a Grove connector or a
XIAO expansion board lands on them without a rework — which is the opposite of the
DevKitC's situation, where the obvious I²C pins are already the stepper's. Same
caveat as every number in this file: D4/D5 is Seeed's published convention, not
something traced with a meter.

### Turning it on

Declared by the build, not probed for: `-DHAS_STATUS_SCREEN` activates the
`PIN_OLED_*` block in [`boards/xiao_c5.h`](../boards/xiao_c5.h), and the driver is
[`utils/StatusScreen.h`](../utils/StatusScreen.h) over the host-tested layout model
beside it. The env is **`xiao_c5_screen`** — `xiao_c5` plus `-DHAS_STATUS_SCREEN` and the two
Adafruit libraries, which do resolve against this fork's Arduino core 3.x
(compiled clean 2026-08-21). Same isolated core dir as every C5 build:

```
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_screen -t upload
```

**On first light, check the swap before suspecting anything else.** The DevKitC's
panel took a reversed SDA/SCL to come up, and a swapped pair scans exactly like a
dead module — nothing answers, with no hint as to why. Run `i2c`; if it comes back
empty, run `i2c 24 23`.

A declared-but-absent panel is handled the same way everywhere: no ACK at 0x3C, a
line on serial, driver disabled.

### It fits in both roles, which is the point

D4/D5 are free whether this board is running four PWM gates or an ST3215 slider,
so one carrier design carries the screen in either configuration:

| | Primary / PWM node | Slider node |
|---|---|---|
| D4, D5 | **screen** | **screen** |
| D6, D7 | free | ST3215 bus TX/RX |
| D7–D10 | servo ch 1–4 | — |
| D8, D9 | — | endstops |
| D2 | pixel | pixel |
| D1 | wake button *(opt)* | wake button *(opt)* |
| D0, D3 | free | free |

**D0 (GPIO1) is deliberately left out of that**: it is the only analog pin on the
castellated edge (§6), and spending it on a digital button you could put on D1
instead would be the last thing you did before needing a current sense.

### The wake button

The screen blanks after two minutes so it doesn't burn a static layout into itself
on a node that idles for weeks — right until you want to read it, at which point
nothing is changing and so nothing wakes the glass. Since 2026-08-22 the timer makes
no exceptions (not even for a fault), so the button is the only way a person gets it
back without walking to a phone — and a screen build without one won't compile. `-DHAS_STATUS_SCREEN` fits it along with the
panel, so D1, D4 and D5 are either all spoken for or all free.

D1 is GPIO0 — which on most ESP32 parts would be the boot strap and a bad choice,
but **not on the C5**, where the boot straps are GPIO26/27/28 (§6). A momentary
switch to GND with `INPUT_PULLUP` is safe here even at reset, since a normally-open
button leaves the pin pulled high unless someone is holding it.

```
D1 (GPIO0) ──── [momentary NO] ──── GND
```

The driver is [`utils/WakeButton.h`](../utils/WakeButton.h), and it does exactly one
thing: lights the screen. No long-press, no double-tap, no menu — a button that could
change what the shop *does* would need every confirmation the web UI has, and that is
a different part. **No button has been wired to a board yet**, on this or any other.

**The onboard RESET and BOOT buttons are at the USB-C end**, and disappear the
moment the board is in a case ([§1](#1-pin-map)). Unlike the DevKitC — which breaks
EN and GPIO0 out to the header, so a panel-mount reset is just two wires — the C5's
castellated edge is D0–D10, 5V, GND and 3V3, with **no RST pad among them**. A
panel-mount reset on a C5 carrier therefore means soldering to the button itself or
finding a test pad on the underside; whether a usable one exists is **unconfirmed**,
and worth checking against a real board before a case design depends on it.

---

## 5. If it looks dead

**It probably isn't.** On 2026-08-13 this board appeared completely dead — no
serial output past the ROM banner, no pixel, BOOT button doing nothing — and was
in fact booting correctly every single time. The monitor was asserting DTR and
RTS, which on this part is the ROM's **download-mode trigger**, not CDC line
state. The board left the app for the bootloader the instant the monitor opened.

Fixed in `platformio.ini` (`monitor_dtr = 0` / `monitor_rts = 0` for this env),
so `bash dev.sh monitor c5` is now correct. If you ever bypass the scripts, hold
both lines low or you will re-run the same scare.

Reading the port directly, with the lines low, is the tie-breaker when a board
seems dead — it cannot reset anything:

```bash
python3 -c "import serial,sys,time; p=serial.Serial(); p.port='/dev/cu.usbmodem1401'; p.baudrate=115200; p.dtr=False; p.rts=False; p.timeout=0.2; p.open(); [sys.stdout.write(p.read(4096).decode('utf-8','replace')) for _ in range(60)]"
```

Known-good boot on real hardware looks like this — `ready` at ~2 s:

```
[BOOT] serial   t=  117ms heap=232632 internal=232632 largest=204788 psram=8388608
[BOOT] claim    t=  125ms ...
[BOOT] wifi     t= 1965ms heap=173784 ...
[BOOT] ready    t= 1986ms heap=156272 ...
```

One line in that log is noise, not a fault: `E (1168) MSPI Timing: Failed to
allocate dummy cacheline for PSRAM memory barrier!`. It comes from IDF's
`esp_psram` before our code runs, and the board reports all 8 MB of PSRAM working
afterwards. Unexplained, harmless so far, and **not** worth chasing when
something else is wrong — it appears on every boot.

## 6. Before you trust this

### Strapping pins — checked, and the map is clear

Settled 2026-08-16 against the **ESP32-C5 Series Datasheet v1.4, Table 3-1**. The
strapping pins are **GPIO25, GPIO26, GPIO27, GPIO28, GPIO7, MTMS and MTDI**.

**GPIO8 and GPIO9 are not among them.** That worry was C3 muscle memory — the
straps are GPIO2/8/9 on *that* part — so the whole servo block (GPIO12/8/9/10) is
ordinary IO and a servo idling there cannot hold the board out of its app.

**Confirmed on the bench, 2026-08-19.** The board boots with a servo wired to the
block; the datasheet reading and the hardware agree, so nothing here needs moving
and this question is closed.

**And the signal does reach them, 2026-08-21:** all four channels drive real
servos. Booting with a servo attached only ever proved the strap question; this
proves the map. The C5's half of bench test 1 in `TODO/TODO.md` is done — the
QT Py S3 has still never moved one.

The one strap this board's map does touch is **GPIO25, the status pixel**, and it
is benign: GPIO25 (with MTDI) selects the **SDIO sampling edge**, a peripheral
this build never brings up, and a WS2812 DIN is a high-impedance input so nothing
holds the line either way while the latches sample at reset. Boot mode lives on
GPIO26/27/28 and the JTAG source on GPIO7; none of those reach a pad we use.

Two straps are already spoken for by the board itself: **GPIO28 is the BOOT
button** (held low at reset = serial bootloader) and **GPIO27 carries the onboard
green user LED** while also selecting UART0 ROM-message printing. GPIO27 defaults
pull-up, so ROM logging stays on; and because straps are latched at Chip Reset and
the pins then revert to ordinary IO, the `PIN_LED 27` fallback in the header is
safe — it only ever drives the pin long after the latch closed.

If anything is ever wired to 27 or 28 externally, note they default **pull-up**
and the combination **27 = 0 with 28 = 0 is invalid** — the datasheet calls the
behaviour undefined.

### ADC: one pin on the edge, four on the back — and two of those strap

Also settled, from Seeed's own pin-definition drawing: the board has **1 analog
pin plus 4 analog pads on the reverse side**, not five equal ones.

| | Pad | GPIO | Also |
|---|---|---|---|
| A0 | **D0**, edge | 1 | the only analog pin on the castellated edge |
| A1 | back pad | 2 | MTMS — **strapping** |
| A2 | back pad | 3 | MTDI — **strapping** |
| A3 | back pad | 4 | MTCK |
| A4 | back pad | 5 | MTDO |

So "four free ADC pads" was too generous. A1–A4 are the **JTAG pads on the
underside** — no castellation, no header, you solder to the belly of the board —
and A1/A2 double as strapping pins. If a node ever needs analog (a current sense,
a pot), A0 is the only one you can reach from a plugged-in board.

### It rides a different platform — and now lives in its own core directory

The C5 needs the **pioarduino** fork (official `espressif32` has no C5) and a
newer Arduino core than every other target here. Both platforms publish packages
under the *same names* — `framework-arduinoespressif32`, `toolchain-riscv32-esp`
— so while they shared one `~/.platformio`, building either one broke the other.
Two distinct failures, neither of which PlatformIO detects or repairs:

1. **The Arduino core.** The loser's package is left in place, judged not to
   satisfy the spec, and the builder gets a `None` path — surfacing as
   `TypeError: expected str, bytes or os.PathLike object, not NoneType`, four
   frames deep in SCons, naming no package.
2. **The riscv toolchain**, worse. The official platform half-removed it: sysroot
   deleted, `.piopm` still claiming 14.2.0. `pio pkg install` reports "Already
   up-to-date" while every compile fails with `riscv32-esp-elf-g++: command not
   found` or `fatal error: stdint.h: No such file or directory`.

**Fixed properly on 2026-08-14: this env gets its own `PLATFORMIO_CORE_DIR`**
(`~/.platformio-pioarduino`), so the two installations never meet. `dev.sh` and
`deploy.sh` set it before every build and before the monitor —
`use_core_for_env()` in [`tools/boardinfo.sh`](../../tools/boardinfo.sh). Costs
**7.6 GB** of disk (measured 2026-08-14), downloaded once.

The old shared installation keeps the fork's leftovers — another 6.8 GB of
packages nothing builds against any more. Reclaim them if the disk is tight:

```bash
rm -rf ~/.platformio/packages/.dustgate-core-fork \
       ~/.platformio/packages/framework-arduinoespressif32-libs \
       ~/.platformio/packages/toolchain-riscv32-esp@src-* \
       ~/.platformio/packages/tool-riscv32-esp-elf-gdb \
       ~/.platformio/platforms/espressif32@src-* \
       ~/.platformio/tools/toolchain-riscv32-esp \
       ~/.platformio/tools/tool-riscv32-esp-elf-gdb
```

All of it is re-downloadable, and none of it is what the isolated directory
uses — that has its own copies.

The earlier fix swapped the core package in and out around each build. It
handled (1) and not (2), which was the lesson: a shared directory has an unknown
number of collisions in it, and each one is found the same expensive way.

**Building by hand needs the core dir set too** — this is now the only correct
way to build this env outside the scripts:

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5
```

Set `DUSTGATE_FORK_CORE_DIR` to put it elsewhere if `~` is tight. A bare
`pio run -e xiao_c5` will try to install the fork over the official
installation — the old breakage, now the only way left to reach it. If you do it
by accident, the recovery is to delete what it half-installed:

```bash
rm -rf ~/.platformio/packages/toolchain-riscv32-esp ~/.platformio/tools/toolchain-riscv32-esp
```

Mixing envs in one command is still wrong — `pio run -e xiao_c5 -e dustgate_node`
can't work, since one pio process has one core dir — but it is now a clear
failure rather than a trap that corrupts an installation.

**Flash it** (picks the env, the core dir, the native-USB port and the right
DTR/RTS convention, then prompts for WiFi credentials and a hostname):

```bash
bash dev.sh flash-node c5
```

Or with the hostname up front — it must be unique per node, since that string is
the board's identity in the claim handshake and the name the primary's picker
binds to:

```bash
bash dev.sh flash-node c5 dustgate-node-c5
```

Serial monitor for it afterwards:

```bash
bash dev.sh monitor c5
```

### Still open: one fleet or two platforms

Isolation makes the two platforms coexist; it does not make them one. Every
other target stays on espressif32 6.x / Arduino core 2.0.x, so the C5 is the only
board here running core 3.x — a second library set (ESP32Async forks, ESP32Servo
3.x) that gets exercised only when this board is built.

Collapsing that means moving **every** target to pioarduino / core 3.x, which
re-validates all of them on hardware. Worth doing eventually, deliberately, not
as a side effect of needing a node. See TODO.md §0.5.

### What this board already fixed elsewhere

Two portability fixes fell out of bringing it up, and are in the tree:
[`utils/Watchdog.h`](../utils/Watchdog.h) (IDF 5 changed `esp_task_wdt_init()` to
a config struct) and the `rgbLedWrite`/`neopixelWrite` guard in
[`utils/StatusLed.h`](../utils/StatusLed.h).
