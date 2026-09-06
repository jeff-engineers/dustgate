# Wiring — Seeed XIAO ESP32C5 (servo-only, node **or** primary)

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
> **Still unproven on this board:** the serial-servo bus pads, and every
> non-servo pad's physical position, which is still Seeed's drawing rather than
> something traced. The status screen pins (D4/D5) and the wake button (D1) both
> came off that list on 2026-08-22 — see §4. If it appears not to boot, read
> [§5](#5-if-it-looks-dead) first, because the first time, it was booting fine.
>
> Authoritative source for every number here:
> [`firmware/boards/xiao_c5.h`](../boards/xiao_c5.h). If this file and that header
> ever disagree, the header is right — the build reads it.

**Role:** either personality. Four PWM gates and no endstops, *or* one ST3215
serial-bus servo on D6/D7 with two endstops on D8/D9 — never both, and
`-DDUSTGATE_SERVO_BUS` picks. In the PWM build: no stepper, no endstops — as a secondary
node (`xiao_c5`) or as the shop's primary (`xiao_c5_primary`). **Both roles have
run on this board.** The primary was proven 2026-08-22: it joins WiFi, serves the
Angular UI well, and drives servos. Still untested in that role: talking to a
secondary node over NodeLink.

**The wiring on this page is the same either way.** That is the point of it: role
is a build-time choice, not a different board, a different carrier or a different
pad. A primary additionally serves the Angular UI and polls Shelly plugs — both
over WiFi, neither costing a pin — and has no rack, which is what needed firmware
work rather than wiring (`HAS_LINEAR`; see the note at the top of
`platformio.ini`). Nothing below changes when you flash the other one.

Board-independent chapters — the status pixel's colour vocabulary and external
wiring, smart plugs, power supply, decoupling — are in
[`WIRING.md`](../WIRING.md).

## Why this board is interesting

- Real Espressif module in a thumb-sized package, USB-C, native USB.
- **8 MB flash + 8 MB PSRAM.**
- **Dual-band WiFi 6**, so the controller's own link can leave a 2.4 GHz band
  that a shop full of motors and Shelly plugs has already crowded.

## Single core, and why it stopped mattering

The C5 has one core, and the old primary design leaned on two: the Shelly poller
and the NodeLink client were pinned to core 0 so their blocking HTTP could never
disturb step generation on core 1. Software step pulses were the one workload
here that turned scheduler jitter into physical error.

Nothing generates steps any more. PWM is LEDC hardware and a bus servo is a UART
write — both fire-and-forget, neither cares that something else is using the CPU.
That is what makes this board a primary as well as a node.

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
                             ┌─────┬─────┴───────┴─────┬─────┐
     analog in ── D0  GPIO1  │  o  │                   │  o  │  5V          ←  do NOT power servos from this pad
   wake button ── D1  GPIO0  │  o  │                   │  o  │  GND         ←  servo / pixel / screen ground
  status pixel ── D2  GPIO25 │  o  │       XIAO        │  o  │  3V3         ←  screen VCC  (never 5V)
          free ── D3  GPIO7  │  o  │      ESP32C5      │  o  │  D10  GPIO10 ── servo ch 4
    screen SDA ── D4  GPIO23 │  o  │                   │  o  │  D9   GPIO9  ── servo ch 3
    screen SCL ── D5  GPIO24 │  o  │    (top view)     │  o  │  D8   GPIO8  ── servo ch 2
ST3215 TX rsvd ── D6  GPIO11 │  o  │                   │  o  │  D7   GPIO12 ── servo ch 1  /  ST3215 RX rsvd
                             └─────┴───────────────────┴─────┘
```

**Every function is on the side its pad is on** (changed 2026-08-22, for PCB
work): left-column pads carry their label to the left, right-column pads to the
right, so a row reads straight across from net to pin without a callout arrow
crossing the board. `rsvd` marks a pad the firmware does not use today but that
`boards/xiao_c5.h` names in a commented-out block — do not spend it on the
carrier.

The screen and its button are one fitting: the board header defines all three pads
at once, so D1, D4 and D5 are spoken for on every C5 build whether or not a panel
is plugged in. What's left is D0 and D3, plus D6/D7 until the ST3215 arrives.

**D8 and D9 used to carry a ⚠ here and no longer do.** It marked a suspected
strapping-pin problem — an NC endstop or an idling servo signal holding a strap
LOW through reset. GPIO8/GPIO9 are ordinary IO on the C5 (the straps are
26/27/28) and this was closed on the bench 2026-08-19, so the mark was stale and
would have cost a PCB revision to work around nothing.

Counting rule when it's docked and you can see nothing: **hold the USB-C end
away from you.** Left column is D0→D6 running away from the connector; right
column is 5V, GND, 3V3, then D10→D7 running toward you. Servo channel 1 is the
pad in the corner *diagonally opposite* the USB-C connector.

The two buttons are at the USB-C end: **RESET** reboots, **BOOT** does nothing on
its own. Download mode is hold BOOT → tap RESET → release BOOT. Pressing BOOT
alone on a hung board gets you nothing, which is easy to mistake for a dead board.

> **Which of these pads has actually passed a signal** — the distinction that
> matters if you are about to commit them to copper. Everything here started as
> Seeed's published pinout redrawn, not a board traced with a multimeter; some of
> it has since been confirmed by hardware doing something.
>
> | Confirmed by a working signal | Still drawing-only |
> |---|---|
> | D7, D8, D9, D10 — servos moved (2026-08-21) | D0, D3 — never connected to anything |
> | D4, D5 — panel answered at 0x3C (2026-08-22) | |
> | D1 — button press lit the screen (2026-08-22) | D6 — reserved for the ST3215 bus, nothing on the bench to talk to |
> | D2 — a WS2812 lit and showed the right colours (2026-08-23) | |
>
> **Every pad this firmware actually uses is now confirmed by a working signal.**
> The servo block, the I²C pair, the button and the pixel are all as good as
> traced. What is left in the right-hand column is pads nothing has ever been
> attached to — a wrong number there costs a board spin, but no test can find it
> until something is wired to them.
>
> The physical *positions* of the pads on the edge are still Seeed's drawing
> throughout. The header [`boards/xiao_c5.h`](../boards/xiao_c5.h) is what the
> build reads; if it and this file disagree, the header is right.

### The numbers

XIAO silkscreen pads D0–D10 map to GPIO **1, 0, 25, 7, 23, 24, 11, 12, 8, 9, 10**
— confirmed 2026-08-16 against Seeed's pin-definition drawing, so this row is no
longer hearsay.

**Every pad, in silkscreen order** — the carrier has to account for all of them,
including the ones nothing uses, so this is the list to lay out from rather than
the signals-only version it replaced (2026-08-22). "Passive" is what the carrier
owes the net; where it says none, none is needed.

| Pad | GPIO | Net | Passive the carrier owes it | Notes |
|-----|------|-----|------------------------------|-------|
| D0  | 1    | *free* | — | **The only ADC pad on the edge.** Keep it free for a current sense; don't spend it on a digital function that fits elsewhere |
| D1  | 0    | Wake button | none — internal pull-up | Momentary NO to GND, `INPUT_PULLUP`. Not a strap on the C5, so safe held down through reset. Verified 2026-08-22 |
| D2  | 25   | Status pixel DIN | **330 Ω series** | External WS2812; the onboard LED is plain yellow. See §3 |
| D3  | 7    | *free, but* | — | ⚠️ **GPIO7 is a strapping pin** (JTAG source). Fine as an output or as an input that idles HIGH; never for one that can be held LOW through reset. See §6 |
| D4  | 23   | Screen SDA | none (module carries its own pull-ups) | XIAO-standard I²C. Verified 2026-08-22. If a bare panel with no pull-ups is ever used, 4.7 kΩ to 3V3 |
| D5  | 24   | Screen SCL | none (as SDA) | ditto |
| D6  | 11   | **Bin sensor in** (PWM builds) / ST3215 bus TX (slider builds) | **1 kΩ series** on the bus when fitted; none for the bin sensor | One pad, two mutually exclusive jobs — `config.h` `#error`s if a build claims both. Bin sensor: §7. Bus: hardware UART TX, half-duplex, see §2 |
| D7  | 12   | Servo ch 1 — **and** ST3215 bus RX | none | The one genuinely contended pad: a serial-bus build gives up PWM channel 1 |
| D8  | 8    | Servo ch 2 | none | Ordinary GPIO, not strapping (bench-confirmed 2026-08-19). Alt: SDIO_DATA0 |
| D9  | 9    | Servo ch 3 | none | Ordinary GPIO, not strapping (same). Alt: SDIO_CLK |
| D10 | 10   | Servo ch 4 | none | Alt: SDIO_CMD |
| 5V  | —    | Carrier 5 V in | **Schottky in series** | Bidirectional VBUS. Without the diode, carrier power and a plugged-in USB cable short two supplies together |
| GND | —    | Common ground | — | Servo, pixel and screen grounds all common here. Mandatory, not optional |
| 3V3 | —    | Screen VCC | — | Regulator output. **Never feed the screen 5 V**; never draw servos from it |
| —   | 27   | Onboard user LED | — | Not on the edge. Green, single colour, strapping but latched at reset (§6). Fallback only — see §3 |

**Servos are not powered from this board.** Every servo V+ comes off the buck
directly, with the bulk and bypass caps at the servo terminals (§2) — the pads
above carry signal and ground only. The most expensive mistake available on this
carrier is running four servos' current through the XIAO's 5V pad.

**Absent from the PWM build, present in the slider build.** `config.h` derives
`HAS_LINEAR` from whether the board header defines `PIN_SERVO_BUS_TX` — not from
`PIN_TMC_STEP`, which is what this note used to say and stopped being true when
the stepper went to the attic on 2026-08-23. So the same header presents two pin
maps and `-DDUSTGATE_SERVO_BUS` chooses:

- **without it** (`xiao_c5_primary`, `xiao_c5`): four PWM pads, no bus, no
  endstops, `HAS_LINEAR` 0. The driver, the feedback system and the endstop
  supervisor all compile out.
- **with it** (`xiao_c5_linear_primary`, `xiao_c5_linear`, `xiao_c5_bus_bench`):
  the bus on D6/D7 and the endstops on D8/D9, `HAS_LINEAR` 1, no PWM block at
  all.

Defining both "to keep the interface uniform" is not merely untidy — `config.h`
`#error`s on it. PWM and serial never share a board.

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

  D7/D8/D9/D10 ──────────────────────  servo signal (orange/yellow)
  (GPIO12/8/9/10)                       one pad per channel, ch1 = D7
  XIAO GND ──────────────────────────  common with servo GND   (REQUIRED)
```

**Never power servos from the XIAO's 5V pad.** Same rule as every other board
here, and it matters more on a part this small: feed servos from the buck
directly, and give the board its own leg off the same buck. See
[`WIRING.md` §5](../WIRING.md#5-decoupling--keeping-the-esp32-out-of-brownout).

### The serial-servo bus, and the endstops that come with it

> **A servo answered on these pads 2026-08-26**, stepped three full turns on
> 2026-08-28, and **drove a real 4-gate rack the same day**: homed to its datum,
> ran the reference sweep, and moved to every gate. The endstops below are proven
> on that rail too. What has NOT run is the same actuator as a NodeLink *node*.

D6/D7 (GPIO11/GPIO12) are the hardware UART. **D7 doubles as servo channel 1**,
so a build driving a serial-bus servo gives up PWM channel 1 — the right trade,
since one bus replaces the whole four-channel block and lifts the `SERVO_COUNT`
ceiling with it.

```
D6 (GPIO11, TX) ──┐
                  ├── ST3215 signal    (through the adapter, or a 1k on TX —
D7 (GPIO12, RX) ──┘                     see wiring/st3215-bench.md §3)
```

**The endstops are not optional, and they are not stepper leftovers.** A bus
servo in stepping mode reports how much of the last command is outstanding, never
where the shaft is, so absolute position is something the firmware counts — and
counting does not survive a power cycle. The homing sweep is still the only thing
that can put a datum on the rail.

```
D8 (GPIO8) ──── endstop 1 ──── GND      normally-CLOSED, INPUT_PULLUP
D9 (GPIO9) ──── endstop 2 ──── GND      ditto
```

Wired NC so untriggered reads LOW and triggered reads HIGH — **and so does a
broken wire**. That is the point: a snapped lead in a shop full of vibration
stops the carriage instead of letting it drive into the end of the rail. Both
reading triggered at boot is not a carriage in two places, it is a missing
ground or an unplugged loom, and the firmware says so at startup.

Which switch is the datum is not wired, it is decided: `g_homeIsMaxEndstop`,
settled by the one setup question, because **home is always the user's LEFT**.
A motor wired backwards is detected by the sweep itself (the far switch answering
first) and costs one direction flip, not a rewire.

Servo power does **not** come through the XIAO. 12V at the gate, its own supply
— see `wiring/st3215-bench.md` §0.1 for what is metered where.

---

## 3. Status pixel

The onboard indicator is a plain yellow LED on GPIO27, so a colour indicator
means adding a part. GPIO25 (D2) is a plain pad with no bus function to give up.

```
5V (or 3V3 — see WIRING.md §1) ──── Pixel VDD
GND ─────────────────────────────── Pixel GND ──── XIAO GND   (common, mandatory)
D2 (GPIO25) ──── [330R] ─────────── Pixel DIN
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

> **Verified on hardware 2026-08-22** — a panel on D4/D5 of a real C5.
>
> Since the same day there is no separate screen build: the `xiao_c5` env carries
> the driver like every other target, and probes for the panel at boot. The driver was already proven on a DevKitC
> (`../attic/linear/devkitc-wiring.md` §5); this is the second board it has run on.
>
> See
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
DevKitC's situation, where the obvious I²C pins are already the stepper's. Unlike
most numbers in this file, these two are no longer just Seeed's published
convention: a panel has answered on them.

### Turning it on

Nothing to turn on, as of 2026-08-22. `PIN_OLED_SDA`/`PIN_OLED_SCL` in
[`boards/xiao_c5.h`](../boards/xiao_c5.h) are what fit a screen; the driver is
[`utils/StatusScreen.h`](../utils/StatusScreen.h) over the host-tested layout model
beside it, and it probes 0x3C at boot rather than trusting a build flag. The two
Adafruit libraries resolve against this fork's Arduino core 3.x — a different
resolution than the DevKitC's 2.0.x line, and an uneventful one, since GFX is pure
drawing and the SSD1306 driver only needs Wire. Same isolated core dir as every C5
build:

```
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5 -t upload
```

There was a second env for this (`xiao_c5_screen`, `-DHAS_STATUS_SCREEN`) until
2026-08-22 — see the note at the top of `platformio.ini` for why one env per board
won.

**If nothing answers, suspect the pair before the panel.** The DevKitC's panel
took a reversed SDA/SCL to come up, and a swapped pair scans exactly like a dead
module — nothing answers, with no hint as to why. There is no `i2c` scan command
to lean on here: that lives in `control/SerialDebugControl.cpp`, which the node
build excludes (`build_src_filter = -<*> +<node/>`). On a node, swap the two
wires, or swap the `PIN_OLED_*` defines in the board header and reflash.

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
back without walking to a phone — and a second press puts it out early rather than
waiting out the timer. A board header that names the panel pins and not the button
won't compile. The two are fitted together, so D1, D4 and D5 go as a set.

D1 is GPIO0 — which on most ESP32 parts would be the boot strap and a bad choice,
but **not on the C5**, where the boot straps are GPIO26/27/28 (§6). A momentary
switch to GND with `INPUT_PULLUP` is safe here even at reset, since a normally-open
button leaves the pin pulled high unless someone is holding it.

```
D1 (GPIO0) ──── [momentary NO] ──── GND
```

The driver is [`utils/WakeButton.h`](../utils/WakeButton.h), and it has two gestures.
A **short press** toggles the screen — lit if it was dark, dark if it was lit — so a
finished reading ends when you say so instead of two minutes later. It doesn't latch:
the next event lights the panel anyway.

A **one-second hold** sweeps every servo, channel 1 to 4, out and back, with the
channel and commanded angle on the panel while it runs
([`motor/ServoSelfTest.h`](../motor/ServoSelfTest.h)). It is the instrument for a
failure that has no other symptom — a servo that answers once per boot and then
silently stops — and it drives the raw channels, so a board with no shop stored runs
it identically. It refuses while the collector is running. No double-tap and no menu
beyond those two: anything that changed what the shop *does* in service would need
every confirmation the web UI has, and that is a different part. **Verified here on
2026-08-22** — the only board where a wake button has been pressed. The toggle is
newer than that test: the press-to-light half ran, the press-again-to-blank half
was written afterwards.

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
so `bash dev.sh monitor node` is now correct. If you ever bypass the scripts, hold
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

Settled 2026-08-16, **corrected 2026-09-04** against the ESP32-C5 datasheet
§2.3.4, which lists them outright in its priority-3 caution list: the strapping
pins are **GPIO2, GPIO3, GPIO7, GPIO25, GPIO26, GPIO27 and GPIO28**.

The old wording here — "GPIO25, 26, 27, 28, 7, MTMS and MTDI" — named MTMS/MTDI
(which *are* GPIO2 and GPIO3, so it was right by another name) but read as though
GPIO2 and GPIO3 were something other than pads. The ADC table further down had it
right the whole time. Nothing was ever miswired; what it cost was making **D3
look free**, which it is not.

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

### It rides a different platform, in its own core directory

The C5 needs the **pioarduino** fork (official `espressif32` has no C5) and
Arduino core 3.x. It builds against its own `PLATFORMIO_CORE_DIR`
(`~/.platformio-pioarduino`, 7.6 GB, downloaded once), which `dev.sh` and
`deploy.sh` set via `use_core_for_env()` in
[`tools/boardinfo.sh`](../../tools/boardinfo.sh). By hand:

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_primary -e xiao_c5
```

Set `DUSTGATE_FORK_CORE_DIR` to put it elsewhere if `~` is tight. `~/.platformio`
holds an unused official installation plus leftovers — ~6.8 GB, all
re-downloadable, safe to delete if the disk is tight.

**Flash it** (picks the env, the core dir, the native-USB port and the right
DTR/RTS convention, then prompts for WiFi credentials and a hostname):

```bash
bash dev.sh flash-node
```

Or with the hostname up front — it must be unique per node, since that string is
the board's identity in the claim handshake and the name the primary's picker
binds to:

```bash
bash dev.sh flash-node dustgate-node-c5
```

Serial monitor for it afterwards:

```bash
bash dev.sh monitor node
```

### What this board already fixed elsewhere

Two portability fixes fell out of bringing it up, and are in the tree:
[`utils/Watchdog.h`](../utils/Watchdog.h) (IDF 5 changed `esp_task_wdt_init()` to
a config struct) and the `rgbLedWrite`/`neopixelWrite` guard in
[`utils/StatusLed.h`](../utils/StatusLed.h).

---

## 7. Dust bin level sensor — optional

**NOTHING HERE HAS BEEN WIRED.** Every number is from a datasheet or a product
page. Treat this as a plan to check against a meter, not a record of something
that worked. Design rationale and the decisions behind it:
[`docs/shop-schema-rfc.md`](../../docs/shop-schema-rfc.md) §7.4–§7.5.

One input pin, which is why this is a **capability rather than a board role** —
it needs no env of its own and works on a primary and a node alike.

### Parts

| Role | Part | Notes |
|---|---|---|
| Sensor | Banner **QS18VN6D** diffuse photoelectric, 10–30 V dc | **`VN` = NPN, sinking, open-collector.** The `VP` variant SOURCES +12 V and would kill the pin. One character apart — check what is stamped on the sensor, not these notes |
| Isolation | HiLetgo **PC817** 2-channel optocoupler board, 3.6–30 V in | Series resistor already on it. One channel spare |
| Supply | Existing 12 V rail + a 12→5 V regulator | The ESP32 can also run off USB; either way **the grounds must be tied** |

### The circuit

```
  +12V ─────┬──────────────────┬─────────────────┐
            │                  │                 │
       QS18 brown         opto IN+          (existing 12V lamps,
            │                  │             left wired as they are)
       ┌────┴────┐             │
       │  QS18   │             │
       │ VN6D    │        ╔════╧════╗
       └──┬───┬──┘        ║ PC817   ║   input side
    black │   │ blue      ║  ch 1   ║
          │   │           ╚════╤════╝
          └───┼───────────  IN─┘         sinks when the beam is made
              │
  12V GND ────┴──────────────────────────┬──  TIE THIS TO ESP32 GND
                                         │
       ╔═════════════════════════════════╧═╗
       ║ PC817 ch 1, output side           ║
       ║   VCC ── 3V3    OUT ── D6    GND ─╫── ESP32 GND
       ╚═══════════════════════════════════╝
```

XIAO side, three wires:

| Opto output pin | XIAO pad |
|---|---|
| VCC | **3V3** — not 5 V. This is what sets the logic level |
| OUT | **D6** (GPIO11) |
| GND | **GND** |

### ⚠️ The optocoupler inverts the sense

**D6 reads LOW when the bin is FULL.** Say it out loud before you debug anything,
because at the bench it reads exactly like a wiring fault.

The firmware does not hardcode it: `bin.sensor.invert` in the document decides,
defaulting `true` because the optocoupler is the documented build. Anyone who
wires the sensor straight to a pull-up instead (simpler, no isolation, rejected
in RFC §7.4) flips the flag rather than reflashing.

The pin is read `INPUT_PULLUP`, so an **unwired board reads HIGH → "bin OK"**. A
board with nothing connected must not scream, and topology gates it anyway:
nothing is reported unless a collector's `bin.sensor.controllerId` names this
board.

### Check these three things on the physical opto board

1. **Is output VCC a separate pin from the input side?** If the two are bonded,
   the isolation is decorative and you may as well use RFC §7.4's discrete
   circuit. This is the one that decides whether the module was worth buying.
2. **Does the output side carry its own pull-up?** Most do. If not, add 10 kΩ
   from OUT to 3V3.
3. **Sink budget.** The QS18 sinks 150 mA max and is now carrying the existing
   lamp load *plus* roughly 10 mA for the opto LED. Measure the strobe before
   assuming there is room — its draw is still a guess.

### Why D6 and not somewhere else

On a primary already driving four PWM gates with a screen, **D0, D3 and D6 are
the only pads left**. D3 is GPIO7, a strapping pin (§6), and the sensor pulls it
LOW exactly when the bin is full — a board that reboots with a full bin would
boot with a strap held down, which is right on an empty bin and wrong precisely
when it matters. D0 is the only ADC pad on the edge and is reserved for a current
sense. That leaves D6, by elimination rather than preference.

**D6 is the servo bus on a slider build**, so the two are mutually exclusive; the
board header guards it and `config.h` `#error`s as the backstop. A board at a
collector will never also be driving a rack, so nothing is lost.

**The RF transmitter for the collector remote does not fit on a four-gate
primary** — and belongs at the collector anyway, next to the receiver. See
[`docs/tool-sensing-rfc.md`](../../docs/tool-sensing-rfc.md) §4.2.

### The lamps stay on 12 V

The green pilot and the red strobe stay wired as they already are. Simpler, and
they keep working when the board does not.

The cost: the strobe can then only ever mean *this sensor tripped* — never a
clog, never a shop-wide alert. Two pads stay reserved for low-side FETs
(AO3400 / 2N7002 class, gate straight off a GPIO) if that changes.

### What you should see

With the firmware running and a collector configured with a bin sensor,
`GET /api/status` grows a field:

```jsonc
"systems": { "big": { "collectorOn": false, "bin": { "full": false } } }
```

**`bin` is absent entirely** if no collector names this board — that is correct,
not a fault. An unwatched bin and an empty bin are different claims.

A change takes **`kBinDebounceMs` (2 s)** to appear, so block the beam and wait
before deciding it is broken. That number is a guess picked from how a bin
behaves in the author's head; it is the first thing to revisit once a beam is
over a real bin. See [`utils/BinSensor.h`](../utils/BinSensor.h).
