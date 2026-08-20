# Wiring — ESP32-DevKitC (the rack primary)

**Board:** Espressif ESP32-DevKitC (ESP32-WROOM-32). The supported primary, and
the only board in the project with a linear rack, a stepper and endstops.

Board-independent chapters — the status pixel's colour vocabulary and its
external wiring, smart plugs, power supply, decoupling — live in
[`WIRING.md`](../WIRING.md). This file is only what is specific to this board.

Also covered here: the **Adafruit ESP32-S2 Feather** ([#5000](https://www.adafruit.com/product/5000)),
a legacy variant that still builds. It shares this design; where its pin differs
it is listed in its own column.

**Stepper driver** — either:
- Adafruit TMC2209 Stepper Driver Breakout ([#6121](https://www.adafruit.com/product/6121)), or
- BigTreeTech TMC2209 StepStick V1.2/V1.3 — same chip, no firmware change ([§2](#2-motor--tmc2209))

> **Carrier board planned.** A custom PCB (ESP32-DevKitC + TMC2209 driver on 2.54mm headers, screw terminals) will replace the breadboard/breakout assembly. Standardize the DevKitC socket footprint on **1.0" (25.4mm) row spacing** — the official DevKitC spec; most cheap clones are 0.9" and will not seat. This document reflects the current breadboard wiring.

**All GPIO is 3.3V logic.** Do not connect 5V signals directly to the ESP32 pins.

---

## 1. Board pin map

The firmware picks the pin set per board (`firmware/boards/*.h`, selected by the
`-DBOARD_*` build flag). Diagrams below use **DevKitC GPIO numbers**; the Feather column
is the equivalent pin on that legacy board.

| Signal                 | DevKitC (WROOM-32)   | Feather S2   |
|------------------------|----------------------|--------------|
| TMC2209 STEP           | GPIO23               | D5           |
| TMC2209 DIR            | GPIO22               | D6           |
| TMC2209 EN (active LOW)| GPIO21 **+10kΩ→3V3** | D9           |
| TMC2209 UART TX        | GPIO19               | TX (Serial1) |
| TMC2209 UART RX        | GPIO18               | RX (Serial1) |
| Home endstop           | GPIO32               | D10          |
| Far endstop            | GPIO33               | D11          |
| Status pixel           | GPIO17 (external)    | GPIO33 (onboard) |
| Status screen SDA *(opt)* | GPIO16            | —            |
| Status screen SCL *(opt)* | GPIO4             | —            |

**Reserved for servo gates:** GPIO25, GPIO26, GPIO27, GPIO14 — a contiguous 4-pin block
for servo PWM outputs. Unused by the sliding gate; do not repurpose them on the carrier.
**Spare:** none left once a status screen is fitted — GPIO16 and GPIO4 were the
last two, and the optional OLED ([§5](#5-status-screen-ssd1306-oled--optional)) takes both.

### Status pixel (DevKitC needs an external one)

The official Espressif DevKitC V4 has **no user LED** — its only onboard LED is the
always-on red power LED, so the indicator is a part you add either way. (NodeMCU-style
clones do have a user LED on GPIO2; that is where the old `PIN_LED 2` default came from.)

It is a WS2812 pixel rather than the single-colour LED this used to be — see
[`WIRING.md` §1](../WIRING.md#1-status-pixel-ws2812--neopixel) for the colour
vocabulary and the wiring, including why running the pixel from 3V3 rather than
5V is the recommended option here.

GPIO17 is deliberately not GPIO2: GPIO2 is a strapping pin, so anything wired there that
holds the pin high blocks download mode at boot.

> **DevKitC pin cautions:** avoid the strapping pins (GPIO0, 2, 5, 12, 15) for driven
> outputs. GPIO6–11
> are the internal flash and are not usable. The endstops sit on GPIO32/33 because they need
> internal pull-ups; the input-only pins (GPIO34/35/36/39) have **no** pull-up and cannot be
> used for the NC-switch inputs.

---

---

## 2. Motor — TMC2209

### Adafruit TMC2209 Breakout (#6121)

```
ESP32-DevKitC                TMC2209 Board (#6121)
  GPIO23 (STEP) ──────────── STEP
  GPIO22 (DIR)  ──────────── DIR
  GPIO19 (TX) ──── 1kΩ ──┬── UART    (single-wire half-duplex)
  GPIO18 (RX) ───────────┘
  3V3           ──────────── VDD     (3.3V logic — board supports 3.3–5V)
  GND           ──────────── GND

  GPIO21 (EN)   ─────┬────── EN      (active LOW)
                     │
                   [10kΩ]  <-- REQUIRED pull-up. Read the note below before
                     │          you wire this: to 3V3, NOT to GND.
                    3V3

Motor power supply (12–24V DC, ≥2A):
  V+ ──── + terminal block on TMC2209 board
  V- ──── - terminal block (share GND with the ESP32)

Stepper motor:
  Coil A+ ── A1 terminal
  Coil A- ── A2 terminal
  Coil B+ ── B1 terminal
  Coil B- ── B2 terminal
```

#### ⚠ The 10kΩ pull-up on EN is a safety part, not an optional one

**Fit it to 3V3, not to GND.** Getting this backwards energizes your motor at
unpredictable current every time the board boots.

EN is **active LOW** — LOW turns the driver's output stage on. The resistor decides
what EN does in every moment the ESP32 is *not* driving it: the whole reset and boot
window, and permanently if the firmware hangs or the MCU dies. WiFi provisioning runs
before `motor.begin()`, so that window is **seconds**, not microseconds.

| Resistor to | EN floats | Driver state when MCU isn't driving |
|-------------|-----------|-------------------------------------|
| **3V3 (pull-up)** ✅ | HIGH | Outputs off, coils floating, zero current |
| GND (pull-down) ❌ | LOW | **Energized** at whatever the VREF pot is set to |

10 kΩ is weak on purpose: the ESP32 drives through a few tens of ohms and overrides it
easily, and the resistor only costs 3.3 V / 10 kΩ = 0.33 mA when driven LOW. GPIO21 is
not a strapping pin, so there's no boot-mode side effect.

**Check your carrier board first.** Some StepStick-style carriers fit their own
pull-down on EN, which will fight a 10 kΩ pull-up. With the ESP32 removed, measure EN
to GND — if it reads low, lift their resistor or use a stronger pull-up.

#### Don't switch VDD/VIO from a GPIO to de-energize the motor

It doesn't work and it damages parts. The coil output stage runs off **VM**, not VIO,
so cutting logic power leaves the power stage undefined rather than off. Meanwhile the
ESP32 keeps driving STEP/DIR/EN into an unpowered input, back-feeding through the ESD
clamp diodes.

Driving `EN` HIGH — `motor.enable(false)` — is the supported way to reach zero coil
current. The firmware already does it on idle timeout, e-stop, and hardware fault.

**UART detail:**
The Adafruit #6121 exposes a single `UART` pin for half-duplex communication.
Wire the ESP32 TX (GPIO19) through a 1kΩ series resistor to this pin, then connect RX
(GPIO18) to the same node (after the resistor). This lets `Serial1` both write config
registers and read back driver status. On the classic ESP32 these UART pins are remappable
and are set explicitly in `Serial1.begin(...)`; on the Feather they are the fixed TX/RX
header pins.

**Current limit:**
The board has an onboard current-limiting potentiometer that sets a hardware
ceiling. `TMC2209_CURRENT_MA` in config.h sets the UART software target.
Effective current = lower of the two. Set the pot first, then tune the config
value.

---

### BigTreeTech TMC2209 StepStick (V1.2/V1.3)

The common 3D-printer driver module, and a drop-in alternative to the Adafruit
breakout. Same chip, same firmware, **no code changes** — but the module is a
16-pin StepStick rather than a breakout with labelled headers, so the wiring and
two of its jumpers need attention.

Everything below assumes you've read the EN pull-up and VDD notes above; they
apply here unchanged.

```
ESP32-DevKitC                BTT TMC2209 V1.2  (StepStick, 2x8 pins)
  GPIO23 (STEP) ──────────── STEP
  GPIO22 (DIR)  ──────────── DIR
  GPIO19 (TX) ──── 1kΩ ──┬── PDN/UART   (see the UART note below)
  GPIO18 (RX) ───────────┘
  3V3           ──────────── VDD        (logic side — NOT VMOT)
  GND           ──────────── GND        (logic GND, left column)

  GPIO21 (EN)   ─────┬────── EN         (active LOW)
                     │
                   [10kΩ]  <-- to 3V3, NOT GND. See the warning above.
                     │
                    3V3

  (leave unconnected)        MS1, MS2   -- UART address, see below
  (leave unconnected)        DIAG, INDEX, SPREAD

Motor power:
  12–24V +  ──────────────── VMOT       (right column, top)
  12–24V -  ──────────────── GND        (right column — common with ESP32 GND)
  100µF+ electrolytic across VMOT/GND, close to the module.

Stepper motor (note the ORDER — it is not A+/A-/B+/B-):
  Coil A ── 1A, 1B
  Coil B ── 2A, 2B
```

#### Confirm the pinout against your own silkscreen

StepStick clones vary, and pin order differs from the Adafruit breakout's screw
terminals. Two mistakes here are expensive:

- **VMOT is not VDD.** Putting 24 V on the logic pin destroys the module and can
  take the ESP32 with it. VMOT/GND are the pair at the top of the *right* column;
  VDD/GND are at the bottom of the same column on most V1.2 layouts.
- **Coil pairs are 1A/1B and 2A/2B**, adjacent — not interleaved. Getting one coil
  reversed just runs the motor backwards (fixable in config); mixing wires *between*
  coils shorts a phase through the driver.

Ring out each coil pair with a multimeter before powering up: the two wires of one
coil read a few ohms to each other and open-circuit to the other pair.

#### MS1/MS2 set the UART address, not microstepping

This is the difference that catches people coming from A4988/DRV8825 boards. On the
TMC2209 in UART mode, microstepping is a register (`MICROSTEPS` in config.h, set by
`StepperTMC2209Driver::begin()`) and the MS pins do something else entirely:

| MS2 | MS1 | UART address |
|-----|-----|--------------|
| low | low | **0** ← what config.h expects |
| low | high | 1 |
| high | low | 2 |
| high | high | 3 |

BTT modules leave both pins floating with pull-downs, so **address 0 is the default
and you should leave MS1/MS2 unconnected.** If you jumper them for microstepping out
of habit, the driver stops answering and the firmware reports the TMC2209 UART
handshake as failed at boot. `TMC2209_ADDRESS` in config.h must match whatever the
pins say.

#### UART: one wire, and check for the on-board resistor

The chip's PDN_UART pin is single-wire half-duplex — same arrangement as the Adafruit
board's `UART` pin. Wire ESP32 TX through 1 kΩ to it, and tap RX on the driver side of
the resistor.

Some BTT revisions ship a 1 kΩ resistor already fitted between the UART pad and the
PDN pin (it's there for the printer-mainboard use case). **Look for it before adding
your own** — two 1 kΩ resistors in series still usually works, but if reads come back
garbled, that's the first thing to check.

#### VREF pot: set it as a ceiling, then forget it

Same relationship as the Adafruit board — the pot is a hardware ceiling and UART sets
the working value, with the *lower* winning. The firmware calls
`I_scale_analog(false)` at init so current comes from `TMC2209_CURRENT_MA` rather than
the pot, but the analog ceiling still clamps it.

For a BTT V1.2 (R_SENSE **0.11 Ω**, which matches `TMC2209_R_SENSE` already):

```
Irms ≈ Vref × 1.77 / 2.5      →  Vref 1.2 V ≈ 0.85 A RMS
```

Set the pot to roughly 1.2–1.4 V measured between the pot's wiper and GND with the
motor idle and VMOT powered, which leaves comfortable headroom over the 800 mA
`TMC2209_CURRENT_MA` default.

#### Verify R_SENSE on the board, don't trust the model number

`TMC2209_R_SENSE` in config.h **must** match the physical sense resistors. Read them
rather than looking them up: two small resistors sit beside the driver chip, one per
coil, and the marking is the value —

| Marking | R_SENSE | config.h |
|---------|---------|----------|
| `R110`  | 0.11 Ω  | `0.11f` ← current default; **confirmed on a BTT V1.3** |
| `R150`  | 0.15 Ω  | `0.15f` |
| `R100`  | 0.10 Ω  | `0.10f` |

Don't try to measure them with a multimeter; at these values your lead resistance
swamps the reading.

Getting it wrong scales every current setting by `(R_config + 0.02) / (R_real + 0.02)`,
and the two directions are NOT equally forgiving:

- **config lower than reality** (say 0.11 set, 0.15 fitted) → ~24% *less* current than
  asked for. Shows up as stalls under load. Annoying, harmless.
- **config higher than reality** (0.15 set, 0.11 fitted) → ~31% *more* current than
  asked for, silently, with the pot as the only ceiling. This is the one that cooks a
  motor.

When unsure, the low guess is the safe guess.

#### Pins we deliberately don't use

- **DIAG** — StallGuard output. Unused; homing is on physical limit switches.
  StallGuard was evaluated on the Adafruit #6121 and abandoned: `SG_RESULT`
  returned 0 at all times regardless of SGTHRS, TCOOLTHRS, or SpreadCycle mode,
  most likely because the onboard current pot forces `I_scale_analog=1` and
  blocks the UART current control StallGuard needs. `PIN_TMC_DIAG` is
  intentionally undefined on the DevKitC.
- **INDEX** — step-position pulse output. No use here.
- **SPREAD** — chops between StealthChop and SpreadCycle. Left floating; the
  firmware sets the chopper mode over UART.

---

---

## 3. Home Limit Switch (FEEDBACK_LIMIT_DISTANCE) — active mode

Single NC (normally closed) limit switch on the left side of travel.
The carriage contacts it at the home position during homing.

```
GPIO32 ──── [NC limit switch, C terminal]
            [NC limit switch, NC terminal] ──── GND
GPIO32 ──── INPUT_PULLUP (no external resistor needed)
```

**Pin states:**

| Carriage position          | Switch contacts | GPIO32 voltage | `readHomeSwitch()` |
|----------------------------|-----------------|----------------|---------------------|
| Away from switch (normal)  | Closed (NC)     | LOW → GND      | `false`             |
| Contacting switch (homing) | Open            | HIGH (pull)    | `true`              |

**Fail-safe:** a broken or disconnected wire pulls GPIO32 HIGH → reads as
triggered → motor stops. This is the correct safe-fail behavior.

**Mounting:** the switch must be positioned so the carriage triggers it
slightly before the true mechanical hard stop. `ENDSTOP_MARGIN_STEPS` in
config.h captures the measured step offset between trigger point and gate 1.

### Far (max) endstop — REQUIRED

A second NC switch on GPIO33 at the far end of the rack. Wired identically to the
home switch (NC, INPUT_PULLUP, **HIGH = triggered**, fail-safe). It serves two
purposes (see [`docs/dual-endstop-calibration.md`](../../docs/dual-endstop-calibration.md)):

1. **Over-travel safety** — halts motion if the carriage ever runs past the last
   gate.
2. **Reference for self-calibration** — the reference sweep measures the
   endstop-to-endstop span to derive steps/mm and place gates by proportion.

```
GPIO33 ──── [NC limit switch, C terminal]
            [NC limit switch, NC terminal] ──── GND
GPIO33 ──── INPUT_PULLUP (no external resistor needed)
```

**Pin states:**

| Carriage position          | Switch contacts | GPIO33 voltage | `readMaxSwitch()` |
|----------------------------|-----------------|----------------|--------------------|
| Away from switch (normal)  | Closed (NC)     | LOW → GND      | `false`            |
| Contacting switch (far end)| Open            | HIGH (pull)    | `true`             |

> **Must be installed on new builds.** Because GPIO33 is an active NC input with
> `HIGH = triggered`, a **floating/unwired pin reads HIGH → triggered**, which the
> firmware treats as "at the far limit" (fail-safe halt). This is intentional —
> an absent far endstop fails safe rather than allowing an un-limited far end —
> but it means the switch has to be present. Mount it so the carriage triggers it
> slightly before the true mechanical hard stop, same as the home switch.

---

---

## 4. Remote Boot & Reset Buttons (enclosure-mounted, optional)

The ESP32's onboard EN (reset) and BOOT buttons become unreachable once the
electronics are enclosed. Both lines are broken out to the header, so a pair of
panel-mount momentary pushbuttons (normally-open, wired the same way as the
onboard buttons) lets you reboot or force bootloader mode from outside the
enclosure — useful for recovering from the WiFi/serial issues covered in the
README's troubleshooting section without opening the case.

```
DevKitC EN   ──── [Reset button] ──── GND      (Feather: RST)
DevKitC GPIO0 ─── [Boot button]  ──── GND      (Feather: IO0)
```

**Reset button (EN):** momentary short to GND restarts the board — equivalent to
power-cycling, but non-destructive to WiFi credentials/calibration (both live
in NVS/EEPROM, not RAM).

**Boot button (GPIO0):** momentary short to GND, held while EN is also pressed and
released, forces the ROM bootloader (used for flashing over USB — see the
manual BOOT+RESET sequence in the README's troubleshooting section). Not
needed for day-to-day use; only matters if you'll be reflashing with the
enclosure closed. Note the classic ESP32 (unlike the S2's native USB) always
flashes through the onboard CP2102/CH340 USB-serial chip.

> **Caution:** GPIO0 is a strapping pin — it's sampled at boot to decide whether
> to enter the bootloader. Wire the button exactly as above (momentary to GND,
> floating/HIGH otherwise) and don't tie anything else to this pin, or you risk
> the board booting into flash mode unexpectedly.

---

---

## 5. Status Screen (SSD1306 OLED) — optional

> **UNBUILT**, like the whole chapter it belongs to. See
> [`WIRING.md` §6](../WIRING.md#6-status-screen-ssd1306-oled--optional) for what the
> screen is for, the burn-in/sleep behaviour and the 3V3 rule; the layouts are in
> [`docs/mockups/oled-status.html`](../../docs/mockups/oled-status.html). Only the
> DevKitC-specific part is here.

| Signal | DevKitC pin | Note |
|---|---|---|
| SDA | GPIO16 | labeled spare, non-strapping |
| SCL | GPIO4  | labeled spare, non-strapping |

```
3V3 ────────── VCC        (never 5V — see WIRING.md §6)
GND ────────── GND
GPIO16 ─────── SDA
GPIO4 ──────── SCL
```

**Not GPIO21/22, which is what every ESP32 example uses.** Those are the TMC2209's
EN and DIR here. I²C is fully remappable through the GPIO matrix, so the fix is one
`Wire.begin(16, 4)` — but it does mean copy-pasted example code will silently drive
the stepper's enable line instead of a display. GPIO16 and GPIO4 also sit adjacent to
the pixel's GPIO17 on the V4 right header, so the whole indicator group is one block
on the carrier.

> On a **servo-only** build (`esp32dev_servo`) no TMC2209 is fitted and GPIO21/22 are
> electrically free — but the pin map is deliberately the same across both envs, so a
> carrier built for one board works on the other. Use 16/4 anyway.

### This spends the board's last two spare pins

The DevKitC is full. GPIO16 and GPIO4 were the only general-purpose pins left, and
fitting a screen takes them both. Everything after this is input-only (GPIO34/35/36/39,
no output and no internal pull-up) or a strapping pin.

Which is fine for a **wake button**, since a button only needs to be read: put it on
an input-only pin with an **external 10kΩ pull-up to 3V3** (the internal one those
pins lack), momentary to GND.

```
3V3 ──[10kΩ]──┬── GPIO34
              └── [momentary NO] ──── GND
```

### ⚠ If you ever swap to an ESP32-WROVER

**GPIO16 and GPIO17 are the PSRAM interface on WROVER modules.** That is the status
pixel *and* the display's SDA line, both gone, on a module that drops into the same
footprint — and there are no spare pins left to move them to. A WROVER swap is
therefore a repin of the whole indicator group, not a drop-in. Worth knowing before
the carrier is fabbed, because "add PSRAM later" is exactly the kind of decision that
looks free.

---

## 6. Pin Budget

| Signal                    | DevKitC pin   | Notes                                |
|---------------------------|---------------|--------------------------------------|
| TMC2209 STEP              | GPIO23        | All                                  |
| TMC2209 DIR               | GPIO22        | All                                  |
| TMC2209 EN                | GPIO21        | All (active LOW; **10kΩ pull-up to 3V3**) |
| TMC2209 UART TX           | GPIO19        | All (hardware UART, remappable)      |
| TMC2209 UART RX           | GPIO18        | All (hardware UART, remappable)      |
| Home-side limit switch    | GPIO32        | FEEDBACK_LIMIT_DISTANCE (required)   |
| Far-side limit switch     | GPIO33        | FEEDBACK_LIMIT_DISTANCE (required)   |
| Status pixel (WS2812)     | GPIO17        | External; 330R in series on DIN — see [WIRING.md §1](../WIRING.md#1-status-pixel-ws2812--neopixel) |
| Status screen SDA         | GPIO16        | Optional — see [§5](#5-status-screen-ssd1306-oled--optional) |
| Status screen SCL         | GPIO4         | Optional; **not** the usual GPIO21/22 — those are TMC EN/DIR |
| Remote reset button       | EN            | Optional, not code-visible           |
| Remote boot button        | GPIO0         | Optional, not code-visible           |

Both limit switches are required. Which one acts as the home datum is decided at
setup time (always the user's LEFT end) — see `g_homeIsMaxEndstop`.

**Active header pins: 8** (GPIO23, 22, 21, 19, 18, 32, 33, 17), or **10** with a
status screen fitted (+ GPIO16, 4). The status pixel costs a header pin on this
board — the DevKitC has no onboard user LED to ride.

**Reserved for servo PWM:** GPIO25, 26, 27, 14 (contiguous 4-pin block).
**Free for other expansion:** GPIO16 and 4 — **but those are the status screen's
I²C pair if one is fitted**, and nothing general-purpose remains after that. Beyond
them only the input-only GPIO34/35/36/39 (input/ADC only — no output, no internal
pull-up), which will do for a button with an external pull-up and nothing else.

EN/GPIO0 aren't GPIOs the firmware reads — they're the hardware reset/bootloader
lines, listed here only so the pin budget stays accurate if you wire the
enclosure buttons from [§4](#4-remote-boot--reset-buttons-enclosure-mounted-optional).

---
