# Wiring Reference — DustGate

Shop-wide wiring: the things that are true whatever board is in the box. Per-board
pin maps live in their own files, because the one question this document kept
failing to answer quickly was "which pin, on the board in my hand".

## Which board are you wiring?

| Board | Role | Wiring |
|-------|------|--------|
| **Espressif ESP32-DevKitC** (WROOM-32) | Primary, with the linear rack, stepper and endstops | [`wiring/devkitc.md`](wiring/devkitc.md) |
| **Seeed XIAO ESP32C5** | Servo-only node (spike — nothing flashed yet) | [`wiring/xiao-c5.md`](wiring/xiao-c5.md) |
| Adafruit ESP32-S2 Feather | Legacy primary variant | in [`wiring/devkitc.md`](wiring/devkitc.md) — it shares that design |
| Adafruit QT Py ESP32-S3 | Servo-only node — **the default node** | [`wiring/qtpy-s3.md`](wiring/qtpy-s3.md) |

Every board's authoritative pin numbers are its header in
[`firmware/boards/`](boards/) — the build reads those, and a wiring doc that
disagrees with one is the wiring doc being wrong.

**All GPIO is 3.3V logic.** Do not connect 5V signals directly to an ESP32 pin,
on any board here.

---

## 1. Status Pixel (WS2812 / NeoPixel)

Every board runs the same indicator now, primary and secondary alike
(`firmware/utils/StatusLed.h`). It is a **single WS2812-family pixel**, not a
plain LED: one data line either way, but a colour is readable across a dusty shop
in one glance where a blink rate is not.

| Colour | Meaning |
|--------|---------|
| **Green** | Ready. Node: primary linked. Primary: routing live. |
| **Blue** | On WiFi but not ready. Node: no primary linked. Primary: no layout stored yet. |
| **Orange, solid** | Something is moving. Solid = a move or servo sweep, slow blink = homing, fast blink = calibration sweep. Also a 400 ms flash when a command lands. |
| **Orange, blinking (~1.5×/sec)** | WiFi lost, or never joined. Shares the colour with "moving" — the rate is what tells them apart, so check whether it is steady before assuming a gate is in flight. |
| **White, blinking** | Captive portal is up, waiting for WiFi credentials. The only state that needs a human to walk over — hence the only white. |
| **Red, pulsing** | Fault. Hardware init failed or the system is in e-stop. |

Orange outranks everything except nothing-happening, deliberately: "is anything
actually moving?" is the first question every time a gate misbehaves.

### Which boards have one

| Board | Pixel | Notes |
|-------|-------|-------|
| ESP32-DevKitC | **GPIO17, external** | No onboard user LED at all — you add this part |
| Adafruit ESP32-S2 Feather | GPIO33 onboard | Power gated by GPIO21 (`PIN_PIXEL_POWER`) |
| Adafruit QT Py ESP32-S3 | GPIO39 onboard | Power gated by GPIO38 |
| Seeed XIAO ESP32C5 | GPIO25, external | Spike target; onboard LED is plain yellow, not RGB |

Boards with an onboard pixel need no wiring. Only the DevKitC and the XIAO need
a part added.

### Wiring an external pixel (DevKitC)

```
5V  ──────────────┬──── Pixel VDD
                  │
               [1000µF]        (across VDD/GND, close to the pixel)
                  │
GND ──────────────┴──── Pixel GND ──── ESP32 GND   (common ground, mandatory)

GPIO17 ──── [330R] ──── Pixel DIN
```

The 330Ω series resistor and the bulk capacitor are the two things people skip
and then chase: the resistor protects DIN against the inrush on a hot-plug, and
the cap keeps the pixel's own switching off a rail shared with servos.

**On 3V3 vs 5V logic.** The ESP32 drives 3.3V, and a WS2812 running off 5V wants
a DIN above ~0.7×VDD = 3.5V. In practice a single pixel usually latches fine at
3.3V, and this is the common hobby shortcut — but it is out of spec and shows up
as an intermittent wrong colour, not a clean failure. Two reliable fixes:

- **Run the pixel from 3V3 instead of 5V.** One status pixel draws little enough
  that the regulator won't notice, and 3.3V logic into a 3.3V pixel is in spec.
  This is the recommended option here.
- Or keep 5V and add a level shifter on DIN.

Use a **WS2812B-family** pixel (Adafruit NeoPixel breakouts, or a single pixel cut
from a strip). The firmware drives it with the Arduino core's own RMT-based
routine, so there is no library to add.

**On a servo build, mind the shared 5V rail.** If you power the pixel from the
same supply as the servos, a servo's inrush can brown the pixel into a garbage
colour — which then reads as a fault that isn't one. The bulk cap above helps;
powering the pixel from 3V3 sidesteps it entirely.

### Boards with no pixel

`StatusLed.h` falls back to blink patterns on `PIN_LED` for any board that
defines one and no pixel (fast = fault, slow = working, solid = ready). That path
exists for compatibility, not as a design goal — the ambiguity it reintroduces is
the whole reason the pixel replaced it.

---

---

## 2. Dust Collector

The dust collector is switched by a dedicated Shelly smart plug over WiFi — no
local wiring to the ESP32. See the main README for configuring the plug; it
turns on automatically when a tool is collecting and can also be toggled from
the Live view.

## 3. Smart Outlet Control (CONTROL_SMART_OUTLET)

No additional wiring required. The ESP32 communicates with Shelly smart outlets
over your home WiFi network using their local HTTP API. Requires:

- ESP32 connected to your home network in station mode (handled automatically
  via the `DustGate-Setup` captive portal on first boot, or by setting
  `WIFI_STA_SSID` / `WIFI_STA_PASS` in config.h)
- One Shelly outlet per blast gate position, on the same local network
- "Local control" enabled on each Shelly (on by default — no cloud required)

Outlet-to-gate mappings are configured during setup and stored in NVS.

---

---

## 4. Power Supply

| Rail            | Source                    | Notes                                     |
|-----------------|---------------------------|-------------------------------------------|
| Motor 12–24V    | Separate DC supply, ≥2A   | Connect to TMC2209 + and − terminals      |
| ESP32 5V        | USB, or 5V/VIN header pin  | Onboard AMS1117 regulates it to 3.3V      |
| ESP32 3V3       | Regulated output pin      | Powers GPIO logic + TMC2209 VDD           |
| Common GND      | Shared across all rails   | Connect ESP32 GND to motor supply GND     |

Do **not** power the motor from the ESP32 3.3V or USB 5V rail.
Always common the grounds.

### USB-PD instead of a DC brick — the intended supply

The barrel-jack brick above is the legacy arrangement. The direction is **USB-PD
for everything**: one USB-C charger, a PD trigger to negotiate the voltage, and a
buck to whatever the actuators want. A charger is a part people already own and can
replace anywhere, which a 15V 3A barrel brick is not.

**Pick the PD voltage from the spec's fixed list, not from what your charger
happens to offer.** USB-PD's normative fixed voltages are **5V, 9V, 15V and 20V**.
**12V is optional** — common, but not guaranteed, so a design that needs 12V is a
design that fails on some perfectly good chargers. This is the reason to prefer a
9V or 15V part over a 12V one when the choice is open.

#### Bench-validated chain (2026-08-12)

Measured on the bench, not derived: a DevKitC primary with the status pixel and one
6kg digital servo, through a deliberate stall, showed **no brownout and no reset —
with no capacitors fitted at all.**

```
USB-C charger ──> HUSB238 (PD trigger, 15V) ──> MPM3610 buck ──> 5V ──┬── DevKitC 5V/VIN
                                                                      └── servo V+
```

Read that result carefully before copying it:

- The **MPM3610 breakout is rated 1.2A**, where [architecture-rfc.md](../docs/architecture-rfc.md)
  specs an **XL4015 at 5A** for this job. A stalling 6kg digital servo can ask for
  1.5–2.5A — above the buck's limit. So "no brownout at stall" may be the buck
  **current-limiting** rather than the servo being satisfied. Both look identical
  from outside. It is a real data point about the ESP32 surviving, not proof the
  supply has headroom.
- It held with **one** servo. The design is **four per node**. What makes that
  plausible on a small buck is that `holdAtRest` defaults **false** (servos move
  then detach, so idle channels draw nothing) and only one servo is ever commanded
  at a time — see the mutex note in [§5](#5-decoupling--keeping-the-esp32-out-of-brownout). Both have to stay true for the budget to.
- **Fit the capacitors anyway** ([§5](#5-decoupling--keeping-the-esp32-out-of-brownout)). A stall transient is microseconds to
  milliseconds; you will not see it without a scope, and the first symptom is a
  reboot mid-move rather than anything legible in a log. Not having hit it is not
  evidence against it.

#### Two 5V sources at once

Bench work usually means the buck's 5V **and** the host's USB VBUS are on the board
together. Unless something isolates them, the buck feeds the host's USB port.

Check it with one measurement: **unplug the host, leave the buck running, measure
VBUS on the USB connector. It should read 0V.** Anything near 5V is backfeed into
your computer. A Schottky between the buck and the board's 5V pin is the usual fix —
put anything else on the board side of it so the diode isolates the supply, not the
peripheral.

> **Hypothesis, not a diagnosis.** A peripheral on 5V with its signal line tied to a
> 3.3V GPIO *can* push current through the ESP32's ESD clamp diodes into the chip's
> own 3V3 rail, and a partially-powered ESP32 cannot latch its strapping pins
> cleanly on reset. That is a real and well-known mechanism, and it is the reason
> the series resistors in [§1](#1-status-pixel-ws2812--neopixel) are a current limit rather than decoration.
>
> It has **not** been shown to be what happened here. What was actually observed on
> 2026-08-12: flashing failed with `Wrong boot mode detected (0x13)` while the
> DevKitC was seated on its carrier and succeeded with the board off it; the
> carrier's NeoPixel was later found installed **backwards**; the carrier was then
> dismantled before anything was isolated. A reversed WS2812 is its own fault with
> its own conduction path, and no measurement tied either one to the strapping
> failure. Treat this section as a thing to check, not a thing to conclude — see
> the open item in TODO.md.

#### Planned: 9V for serial-bus servos

The serial bus servo (ST3215-class) replaces both the stepper and the PWM servo
bank. It is a 12V-class part, but the plan is to run it from **9V PD** rather than
12V, precisely because 9V is a spec-normative fixed voltage and 12V is not — a 9V
design works off any compliant charger.

The trade is torque: the headline rating is at 12V, so expect meaningfully less at
9V. Size the gate mechanism for the 9V figure, not the datasheet's. Not yet built —
see the serial-bus notes in the board headers.

> **Note (vs. the Feather):** the DevKitC has **no onboard LiPo charger** — power it
> from USB or a regulated 5V source on the 5V/VIN pin. The Feather's battery/charging
> features do not apply to the primary build.

---

---

## 5. Decoupling — keeping the ESP32 out of brownout

> **Untested on hardware.** This is standard practice written down so the bench
> session starts from a known-good arrangement, not a measured result. The only
> value here that predates it is the 100µF at VMOT in
> [`wiring/devkitc.md` §2](wiring/devkitc.md#2-motor--tmc2209).

The WROOM-32's brownout detector resets the chip when 3V3 sags past **~2.8V**.
Nothing on the ESP32 side causes that. The loads sharing the rail do:

- **Servo inrush and stall** — a hobby servo pulls 0.5–1A+ for tens of milliseconds
  when it starts moving and when the gate hits its stop. On the v2 servo nodes this
  is the main offender. **Budget for ONE moving servo, not two:** the firmware holds
  a hard mutex — only one servo is ever commanded at a time — and the move queue is
  shop-wide and serial, so even a make-before-break transition across two systems
  concatenates its moves rather than overlapping them. (Corrected 2026-08-12; this
  previously said two could move at once, which doubled the budget for no reason.
  The invariant is in [architecture-rfc.md](../docs/architecture-rfc.md).)
- **Idle servos draw nothing.** `holdAtRest` defaults false — a servo moves, then
  detaches, and the valve holds by friction. So a four-gate node's steady draw is
  the ESP32 alone. Set `holdAtRest` true on a build that back-drives when
  de-energized and that stops being true, which changes the supply sizing.
- **Stepper coil energizing** — the TMC2209 slams current into the coils on enable
  and on the first steps after idle.
- **Long thin wire** — ~0.3Ω in 10ft of 22AWG turns a 1A transient into a 0.3V drop
  before any capacitor gets to see it.

Capacitors absorb the fast transient. They do **not** fix an undersized supply or a
wire run that's too thin, and reaching for more capacitance is the wrong move when
the rail is sagging steadily rather than dipping.

### What to fit

| Location                       | Value          | Type                    |
|--------------------------------|----------------|-------------------------|
| TMC2209 VMOT ↔ GND             | 100–220µF      | electrolytic            |
| Servo power rail, per node     | 470–1000µF     | low-ESR electrolytic    |
| ESP32 VIN/5V ↔ GND             | 100–220µF      | electrolytic            |
| ESP32 3V3 ↔ GND                | 10µF + 0.1µF   | ceramic (X5R/X7R)       |

Rate every electrolytic at **≥2× its rail** (so ≥50V on a 24V motor supply), and put
a 0.1µF ceramic in parallel with each one: the electrolytic carries the bulk energy,
the ceramic handles the fast edge its ESR can't.

### Where they go

Directly across + and − **at the load**, on the shortest leads you can manage. A bulk
cap back at the power supply does almost nothing — the wire inductance between it and
the servo is the thing you're compensating for.

```
Servo node — the one that matters:

  5V/6V supply ──┬────────────┬───── servo V+   (red)
                 │            │
            [470-1000µF]   [0.1µF]      <-- AT the servo terminals,
                 │            │              not back at the supply
  GND ───────────┴────────────┴───── servo GND (brown/black)

  GPIO25/26/27/14 ─────────────────── servo signal (orange/yellow)
  ESP32 GND ───────────────────────── common with servo GND   (REQUIRED)
```

```
ESP32 DevKitC input:

  5V buck out ──┬──────────┬───── VIN (5V pin)
                │          │
            [100µF]     [0.1µF]
                │          │
  GND ──────────┴──────────┴───── GND

  and close to the 3V3 pin:

  3V3 ──┬──────────┬── GND
     [10µF]     [0.1µF]
```

Electrolytics are polarized — stripe/short leg to GND. Backwards on a 12V rail they
vent.

### The four rules that matter more than the caps

1. **Don't power servos from the DevKitC 5V pin.** That routes servo current through
   the board's traces and its USB/regulator path, which is the fastest way to brown
   out. Feed servos from the buck converter directly; the ESP32 gets its own leg off
   the same buck.
2. **Star ground.** Servo GND, TMC2209 GND and ESP32 GND each return to one point at
   the supply. Daisy-chaining them puts the stepper's return current across the
   ESP32's ground reference.
3. **Fat wire on the power legs** — 18–20AWG for servo and motor power, short runs to
   the node. 22AWG and up is fine for signal.
4. **Common ground is mandatory.** [`wiring/devkitc.md` §2](wiring/devkitc.md#2-motor--tmc2209) already says this for
   the motor; it is equally true for every servo. Without it the PWM and STEP/DIR signals
   have no reference.

### If it still browns out

Put a scope (or a fast multimeter) on the 3V3 pin during a gate move before adding
capacitance:

- **Dips for ~10ms, then recovers** — transient. More/closer bulk capacitance helps.
- **Sags and stays down** — the supply is undersized or the run is too long. Size the
  buck for *stall* current × the number of servos that can move together, not rated
  current, and shorten or thicken the run.
