# Wiring — Seeed XIAO ESP32C5 (servo-only node)

> ## ⚠️ NOTHING HERE HAS BEEN ON HARDWARE
>
> This board is a **spike**, not a supported target. It compiles; no board has
> been flashed, and **the pin numbers below come from Seeed's published pinout,
> not from a multimeter.** Two of them (GPIO8, GPIO9) are the ones most likely to
> be wrong in a way that stops the board booting — see [§4](#4-before-you-trust-this).
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

## 4. Before you trust this

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
