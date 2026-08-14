# Wiring — Seeed XIAO ESP32C5 (servo-only node)

> ## ⚠️ NOTHING HERE HAS BEEN ON HARDWARE
>
> This board is a **spike**, not a supported target. It compiles; no board has
> been flashed, and **the pin numbers below come from Seeed's published pinout,
> not from a multimeter.** Two of them (GPIO8, GPIO9) are the ones most likely to
> be wrong in a way that stops the board booting — see [§5](#5-before-you-trust-this).
>
> A board HAS now been flashed and booted (2026-08-13): full boot log, WiFi
> joined, `ready` at 1986 ms. Nothing has been wired to a servo yet, so the pin
> numbers below are still unproven — see [§4](#4-if-it-looks-dead) first if it
> appears not to boot, because the first time, it was.
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

The same reasoning already governs [`boards/qtpy_c3.h`](../boards/qtpy_c3.h) for
the single-core C3.

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
   GPIO0   D1 │  o  │               │  o  │ GND     ← servo/pixel ground
  GPIO25   D2 │  o  │    XIAO       │  o  │ 3V3
   GPIO7   D3 │  o  │    ESP32C5    │  o  │ D10  GPIO10   ── servo ch 4
  GPIO23   D4 │  o  │               │  o  │ D9   GPIO9    ── servo ch 3  ⚠
  GPIO24   D5 │  o  │  (top view)   │  o  │ D8   GPIO8    ── servo ch 2  ⚠
  GPIO11   D6 │  o  │               │  o  │ D7   GPIO12   ── servo ch 1
              └─────┴───────────────┴─────┘
                 ▲                     ▲
                 │                     └── servo block: the FOUR pads
                 │                         furthest from the USB-C end,
                 │                         channel 1 nearest the corner
                 └── D2 = status pixel DIN
```

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

XIAO silkscreen pads D0–D10 map to GPIO **1, 0, 25, 7, 23, 24, 11, 12, 8, 9, 10**.

| Signal              | Pad | GPIO | Notes |
|---------------------|-----|------|-------|
| Servo PWM channel 1 | D7  | 12   | Also the hardware UART TX — see §3 |
| Servo PWM channel 2 | D8  | 8    | ⚠ verify strapping duty before wiring |
| Servo PWM channel 3 | D9  | 9    | ⚠ verify strapping duty before wiring |
| Servo PWM channel 4 | D10 | 10   | |
| Status pixel (DIN)  | D2  | 25   | External part; onboard LED is plain yellow, not RGB |
| Onboard user LED    | —   | 27   | Yellow, single colour. Fallback only |

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
[`boards/qtpy_c3.h`](../boards/qtpy_c3.h) and [`qtpy_s3.h`](../boards/qtpy_s3.h)
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

## 4. If it looks dead

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

## 5. Before you trust this

### Confirm the strapping pins first

**GPIO8 and GPIO9 are the two to check**, against the ESP32-C5 datasheet, before
a servo is wired to either. A servo signal idling on a strapping pin can stop the
board booting — the trap [`boards/qtpy_c3.h`](../boards/qtpy_c3.h) had to dodge on
the C3 (GPIO2/8/9 there). Symptom is a board that flashes fine and then appears
dead, which reads as a bad flash rather than a pin choice.

Also worth confirming while you have the datasheet open: whether four ADC pads are
genuinely free, since the published pinout is the only source so far.

### It rides a different platform

The C5 needs the **pioarduino** fork (official `espressif32` has no C5) and a
newer Arduino core than every other target here. Both platforms publish a package
called `framework-arduinoespressif32` into one shared directory, so only one core
can be installed at a time — whichever env built last owns it, and the other dies
with an opaque SCons `TypeError: ... not NoneType` that names no package.

**It does not recover on its own.** PlatformIO leaves the wrong core in place and
keeps failing identically until the directory is cleared. `dev.sh` and `deploy.sh`
now clear it for you and stash the evicted core, so the first swap downloads and
later ones are a rename — go through the scripts and you will never see this.

**Flash it** (picks the env, the native-USB port and the right DTR/RTS
convention, then prompts for WiFi credentials and a hostname):

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

Build only, no flashing:

```bash
pio run -e xiao_c5
```

**Never combine this env with another in one `pio` command.** `[env] platform` is
version-pinned so the collision can't happen by accident, but
`pio run -e xiao_c5 -e esp32dev_wroom32` will still fail. Building this one also
means the next DevKitC build re-installs its core — slow, not broken.

Adopting the C5 properly means migrating every target to pioarduino / core 3.x —
ESP32Servo 1.x → 3.x, and me-no-dev AsyncTCP + ESPAsyncWebServer → the ESP32Async
3.x forks — which re-validates all four supported targets. That, not the 8 MB of
flash, is the real price of this board.

### What the spike already fixed

Two portability fixes fell out of it and are in the tree:
[`utils/Watchdog.h`](../utils/Watchdog.h) (IDF 5 changed `esp_task_wdt_init()` to
a config struct) and the `rgbLedWrite`/`neopixelWrite` guard in
[`utils/StatusLed.h`](../utils/StatusLed.h).
