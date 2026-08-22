# Wiring Reference — DustGate

Shop-wide wiring: the things that are true whatever board is in the box. Per-board
pin maps live in their own files, because the one question this document kept
failing to answer quickly was "which pin, on the board in my hand".

## Which board are you wiring?

| Board | Role | Wiring |
|-------|------|--------|
| **Espressif ESP32-DevKitC** (WROOM-32) | Primary, with the linear rack, stepper and endstops | [`wiring/devkitc.md`](wiring/devkitc.md) |
| **Seeed XIAO ESP32C5** | Servo-only node | [`wiring/xiao-c5.md`](wiring/xiao-c5.md) |
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
| **Green** | Ready. Node: primary linked. Primary: routing live **and every paired board answering**. |
| **Blue** | On WiFi but not ready. Node: no primary linked. Primary: no layout stored yet, or a board it's paired with is dark. |
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
| Seeed XIAO ESP32C5 | GPIO25, external | Onboard LED is plain yellow, not RGB |

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

---

## 6. Status Screen (SSD1306 OLED) — optional

> **WORKING ON ONE BOARD, as of 2026-08-21.** A 0.96" SSD1306 answered at 0x3C on
> a DevKitC's GPIO16/4 and the firmware drew to it — the first display any
> DustGate board has driven:
>
> ```
> [SCREEN] panel answered at 0x3C — drawing
> [I2C] Scanning SDA=GPIO16 SCL=GPIO4 at 100kHz...
>   0x3C  SSD1306 OLED (the expected status screen)
> ```
>
> What that verifies: **these pins, this address, on this board.** Not verified:
> the sleep/wake behaviour over hours, whether every layout reads well on real
> glass, and the pins proposed for the two node boards — those are still
> transcribed from published pinouts and have driven nothing.
>
> **It took a swapped pair to get there**, which is the failure this chapter
> should make you check first: SDA and SCL reversed scan exactly like a dead
> module. GPIO16 is SDA, GPIO4 is SCL.

A 0.96" 128×64 SSD1306 on I²C, so *"is it connected?"* has an answer you can read
at the machine instead of on a phone. The layouts — what each screen says, and
the eight-row budget they have to say it in — are in
[`docs/mockups/oled-status.html`](../docs/mockups/oled-status.html); this chapter
is only the wire.

### It never replaces the pixel

The pixel is readable across a dusty shop; the screen is readable at arm's
length. That is the whole division of labour. §1 stays fitted on every board,
and the screen — where one is fitted at all — mirrors the *same*
`statusled::Status` state the pixel is showing rather than inventing a second
vocabulary. Two indicators that could disagree with each other would be worse
than one.

### Wiring

Four wires, and no level shifting: run the module from **3V3**, and its I²C lines
are then already at the ESP32's logic level.

```
3V3 ─────────────── VCC
GND ─────────────── GND   ── common with the board's ground
SDA pin ─────────── SDA
SCL pin ─────────── SCL
```

Which pins is per board, and the three boards are not equally lucky:

| Board | SDA / SCL | How it goes |
|---|---|---|
| [QT Py ESP32-S3](wiring/qtpy-s3.md) | GPIO41 / GPIO40 | **Easiest.** The STEMMA QT socket is a second I²C bus — a cable, no soldering, no header pad, nothing else disturbed |
| [XIAO ESP32C5](wiring/xiao-c5.md) | GPIO23 / GPIO24 (D4/D5) | The XIAO-standard I²C position, so Seeed's own accessories land on it; free in either node role |
| [ESP32-DevKitC](wiring/devkitc.md) | **GPIO16 / GPIO4** | The awkward one — and it spends the board's last two spare pins |

**The DevKitC cannot use GPIO21/22**, which is what every ESP32 I²C example
assumes: those are the TMC2209's `EN` and `DIR` on that board. I²C remaps through
the GPIO matrix, so the fix is one `Wire.begin(16, 4)` — but copy-pasted example
code will silently drive the stepper's enable line instead of a display. GPIO16
and GPIO4 also sit next to the pixel's GPIO17 on the V4 right header, keeping the
whole indicator group as one block on a carrier.

Two consequences worth knowing before a carrier is fabbed:

- **Fitting a screen fills the DevKitC.** Everything left after GPIO16 and GPIO4
  is input-only or strapping — fine for a wake button, which only needs reading,
  with an external 10kΩ pull-up standing in for the one those pins lack.
- **WROVER warning:** GPIO16/17 are the PSRAM interface on WROVER modules. That
  is SDA *and* `PIN_PIXEL`, on a module that drops into the same footprint, with
  no spares left to move them to. "Add PSRAM later" is exactly the kind of
  decision that looks free.

**Do not run it from 5V.** These modules will take it, but then their SDA/SCL
idle at 5V through the onboard pull-ups, and no ESP32 here is 5V tolerant. The
3V3 rail has ample headroom for it — the panel draws under 20mA with every pixel
lit, and far less showing text.

**Address is 0x3C** on essentially all of the 4-pin 0.96" modules (0x3D exists on
some 128×64 parts; if it scans up as 0x3D, that is why). No pull-up resistors to
add — the modules carry their own.

### Finding out whether it is wired right

`i2c` on the serial console scans the bus and says what answered, at what
address, and what it probably is:

```
i2c              # scans the pins this build declares for the screen
i2c 16 4         # scans a pair you name — the useful form when hunting
```

It separates the three failures that look identical from outside: nothing
powered, wrong address (0x3D rather than 0x3C), and SDA/SCL swapped. It also
names a **PCF8574 character-LCD backpack** (0x27/0x3F) if that is what is on the
bus, because that is a different part this firmware cannot drive at all — not a
setting to change.

It **refuses** to scan `PIN_TMC_EN`/`PIN_TMC_DIR`. EN is active LOW, so a scan
pulling it down is a scan that silently energises the driver — the same trap the
DevKitC's pin map dodges, and a debug command is exactly where someone walks back
into it. On a bench board with no driver fitted those are just pins, so the
refusal can be lifted deliberately: `i2c 21 22 force`.

Scans at 100kHz rather than the screen's 400kHz: a marginal pull-up or a long
dupont run fails at 400k and answers fine at 100k, and "the module is alive" is
the thing you need first.

### Burn-in, and why the screen sleeps

An OLED with fixed labels lit 24/7 burns them in — the ghost of `gates` and
`nodes` etched across every later screen. So the intended behaviour is:

- **Blank after a couple of minutes idle, always.**
- **Wake on events** — a gate moving, a tool drawing power, a node dropping, any
  fault.
- **Nothing holds it awake** (changed 2026-08-22). A fault used to, on the
  reasoning that nobody should have to press anything to find out what broke.
  But a fault is exactly the state that *lasts*: a node goes dark on a Friday
  and the panel spends the weekend burning "NODE DARK" into itself with nobody
  in the shop. The alarm still lights the screen when it happens — it just
  doesn't hold it. Anyone arriving later presses the button.

Which makes a lit screen mean *something happened*, instead of becoming wallpaper
you stop reading. That is a firmware behaviour, not a wiring one, but it is the
reason the wake button below is a **requirement** on any board with a panel and
not the convenience it started as.

That policy is `statusscreen::awake()` in
[`utils/StatusScreenModel.h`](utils/StatusScreenModel.h), and it is a decision
rather than a comment — the captive portal blanks along with everything else, so
a stranger being asked to join an AP may have to press the button too.

### The wake button

Since the timer has no exceptions, the button is the **only** way to see a state
the screen has already slept through — a fault raised on Friday, a portal waiting
on Saturday, or just a healthy board you want to read from two feet away.
Ordinary momentary switch to GND, read with `INPUT_PULLUP`:

```
GPIO ──── [momentary NO] ──── GND
```

Same NC-vs-NO caution as the endstops in reverse: this one is **normally OPEN**,
so the pin idles HIGH and a strapping pin is harmless here — nothing holds the
line at reset unless someone is pressing the button while the board boots.

**It is fitted with the screen, not separately, and now enforced.**
`-DHAS_STATUS_SCREEN` defines `PIN_WAKE_BTN` alongside the I²C pair, because a
board with no panel has nothing for a button to wake — and `WakeButton.h` fails
the build with an `#error` on the reverse case, a panel with no button. Per
board:

| Board | Pin | Pull-up |
|---|---|---|
| DevKitC | GPIO34 | **External 10kΩ to 3V3** — the pin is input-only and has none |
| QT Py S3 | GPIO37 (MISO) | Internal |
| XIAO C5 | GPIO0 (D1) | Internal |

The DevKitC row is the one that bites: `INPUT_PULLUP` on an input-only pin is
accepted and does nothing, and a floating input reads as phantom presses — a
screen that lights by itself. Its header pairs the pin with
`WAKE_BTN_INPUT_MODE INPUT` so the external resistor is the only option.

The driver is [`utils/WakeButton.h`](utils/WakeButton.h): a debounced poll and
one call to `statusscreen::note()`. **One edge, one wake, and nothing else** —
no long-press, no double-tap, no menu. A button that could change what the shop
*does* would need every confirmation the web UI has, and that is a different
part with a different name.

> **UNVERIFIED.** No button has been wired to any board. The screen it wakes has
> run on a DevKitC; this half has not.

### Fitting one is a build-time fact

A DustGate ships without a screen unless somebody fits one, so the display has to
compile out completely when its pins aren't defined — the same seam `HAS_LINEAR`
and `PIN_PIXEL` already use. No display, no library, no flash spent. That guard
sits on the *driver*: the layout model is pure C++ that touches no pin, so a
board with no screen pays only what the linker drops.

**Settled 2026-08-20: declared by the build — and then probed to check the
declaration is true.** Not probe-to-discover; probe-to-verify. An env sets
`-DHAS_STATUS_SCREEN`, the board header turns that into its own two pins, and
`pio run -e esp32dev_screen` is a DevKitC with a screen fitted. Probing at 0x3C
would have been friendlier to someone adding one later; declaring is how every
other fitted-or-not part on these boards is decided, and one seam beats two.

A screen that is *declared and not actually there* is handled, because that is
the mistake someone will make: `StatusScreen::begin()` does a zero-length write
at 0x3C first, and an ACK is the whole test. No ACK → it says so and disables
every later call, so a missing panel cannot hang the brain in Wire's timeout once
per pass of `loop()`.

**That probe is ours, and it has to be.** `Adafruit_SSD1306::begin()` does not
check anything — read it: the only `return false` is a failed `malloc` of the
1 KB framebuffer. It clocks the init sequence into open air and reports success
regardless. Trusting it printed `SSD1306 up` on a board with an empty bus
(2026-08-21), which is a worse failure than no message at all.

**And pass `periphBegin = false`.** Left at its default the library calls
`wire->begin()` with no arguments, which on an ESP32 re-initialises I²C on the
core default pins — **GPIO21/22**. On the DevKitC those are the TMC2209's EN and
DIR. Everything above about keeping I²C off those pins is undone by a default
argument in a display library, silently: the panel talks to nothing while the
stepper's enable line gets driven as a clock.

The two Adafruit libraries (SSD1306 + GFX) are why that env is separate rather
than a flag on the default one. A board with no panel should not download,
compile or store a display driver — the screen env costs ~32 KB of flash over
`esp32dev_servo`, which is exactly what an unfitted board declines to pay.
