# Bill of materials

Started 2026-09-17, when the power question was settled (12 V barrel jack →
MPM3610) and there was finally a stable thing to build on perfboard.

**What's here:** boards, modules, sensors, and the passives whose VALUE matters.
**What's not:** wire, connectors, headers, hookup, heatshrink, standoffs,
perfboard itself. Those are consumables and listing them buries the parts you
actually have to choose.

**Every part traces to a section of [`../firmware/WIRING.md`](../firmware/WIRING.md)**,
which carries the reasoning and the pin-level detail. This file is the shopping
list, not the design — when the two disagree, WIRING.md is right and this needs
fixing.

⚠️ **Quantities are PER BOARD unless a row says otherwise.** One board is one
gate under the model this is heading towards (one gate = one board), so a shop
is this list times the number of gates, plus one collector unit.

---

## 1. Core — on every board, whatever job it does

| Part | Qty | Notes |
|---|---|---|
| **Seeed XIAO ESP32C5** | 1 | THE board. Primary or node by flash, not by part. 8 MB flash, 8 MB PSRAM. Needs its own `PLATFORMIO_CORE_DIR` (pioarduino) — official espressif32 has no C5 |
| **MPM3610 buck breakout**, 12 V → 5 V | 1 | Rated 1.2 A. Confirmed adequate 2026-09-17 against an isolated DC-DC; see §2 of WIRING.md |
| **DC barrel jack**, 2.1 × 5.5 mm | 1 | The supply input. 12 V, decided 2026-09-17 |
| **Schottky diode** — 1N5817 or SS34 | 1 | Between the buck and the XIAO's `5V` pad. NOT optional: that pad is raw bidirectional VBUS, so without it the buck backfeeds a laptop's USB port. ~0.3 V drop; a silicon diode's 0.6 V is too much |
| 100–220 µF electrolytic | 1 | ESP32 VIN/5V ↔ GND. Rate ≥2× the rail |
| 10 µF ceramic + 0.1 µF ceramic (X5R/X7R) | 1 ea | ESP32 3V3 ↔ GND |
| 0.1 µF ceramic | 1 per electrolytic | In parallel with each bulk cap — the electrolytic carries the energy, the ceramic the edge its ESR cannot |
| **WS2812B pixel** | 1 | External, on D2. The XIAO's onboard LED is plain yellow and cannot show status |
| 330 Ω resistor | 1 | Series on the pixel's DIN. A current limit, not decoration — see the strapping-pin note in WIRING.md §3 |
| **SSD1306 OLED**, I²C, 0x3C | 1 | D4/D5. Optional in software — the firmware probes at boot and runs fine without one |
| Momentary push button | 1 | D1, wake. Lights the screen; on a slider node a 1 s hold triggers homing |

## 2. Driving gates — PWM boards

| Part | Qty | Notes |
|---|---|---|
| Servo, ~6 kg·cm metal gear | 1 per gate, max 3 | Ball-valve gates. Channels 0–2 = D7/D8/D9. **Only one ever moves at a time** — the firmware serialises them, which is what makes the 1.2 A buck viable |
| 470–1000 µF low-ESR electrolytic | 1 | Servo power rail. At the servo terminals, not at the board |

## 3. Sensing a tool — the CT clamp

| Part | Qty | Notes |
|---|---|---|
| **SCT-013-030** current clamp | 1 | 30 A : 1 V. **Voltage output — the burden resistor is INSIDE the plug. Do not add one.** Terminates in a 3.5 mm plug |
| **3.5 mm jack**, switched (NC contact) | 1 | So the install is "plug it in". **Buy the switched variant** — an unplugged clamp otherwise reads the bias midpoint, which is indistinguishable from an idle tool, and that gate silently never opens. The NC contact to a spare GPIO gives plug-detect for free |
| 1 kΩ resistor | 2 | The bias divider, 1k/1k off 3V3. **Not 10k/10k** — that was the old build and it coupled the screen's charge pump into the floor |
| 100 µF electrolytic | 1 | Divider midpoint to GND. With 1k/1k this settles in ~250 ms |
| 0.1 µF ceramic | 1 | Across the analog node |

Measured floor on this arrangement: **~0.195 A** (6.2–7.0 ADC counts), stable
across four sessions and two supply topologies. Trip lands at ~0.8 A.

## 4. The collector unit — extras beyond the core

A collector board is an ordinary primary or node that a LAYOUT points at a bin,
a clamp and a remote. No separate firmware, no separate pin map.

| Part | Qty | Notes |
|---|---|---|
| **Banner QS18VN6D** beam sensor | 1 | Bin full. 10–30 V DC, NPN sinking output |
| **4N35 optocoupler**, DISCRETE DIP-6 | 1 | **Not a PC817 breakout module.** Those pull their output to the INPUT side's supply — measured 4 V on the bench, above a C5 GPIO's absolute maximum, and it shorts out the isolation the part exists for |
| 1 kΩ resistor | 1 | 4N35 LED series, off 12 V. ≈10.8 mA, the part's rated test point |
| 12 V green pilot lamp | 1 | Existing shop part, stays on 12 V |
| 12 V red strobe | 1 | ditto. Measured contribution to the CT floor: ~2.7 counts, a tenth of the trip point |
| **315 MHz TX module** | 1 | Keys the collector's own remote. Data input takes 3.3 V logic; module runs 5 V on the bench, 12 V for a real install. D10 |
| **HT12E** encoder | 1 | 12-bit, 8 address + 4 data |
| 8-position SPST DIP switch | 1 | Address. DIP-16 body, 2.54 mm |
| 1.0 MΩ resistor | 1 | HT12E Rosc |
| 9 g servo (SG90 class) | 1–2 | ALTERNATIVE to the RF path: an arm on a printed fixture pressing the fob's own buttons. This is the SHIPPING answer — it needs no soldering inside a certified remote. Plastic gear is fine; a button press is not a stall |

**RF and servo-on-fob are alternatives, not both.** RF is proven on the bench
and is the only route for a collector with no fob; the servo is what ships,
because "open the fob and solder across the button" is an install step a
woodworker cannot perform.

## 5. The slider — only on a rack build

| Part | Qty | Notes |
|---|---|---|
| **ST3215** serial bus servo | 1 | 12 V, 30 kg·cm. Its own board riding with the slider. **Never shares a board with PWM servos** — `config.h` `#error`s if a pin map claims both |
| Endstop switch, NC | 2 | D8/D9. Wired normally-CLOSED so a snapped lead reads as triggered and stops the carriage |
| 1 kΩ resistor | 1 | Series on the ST3215 bus TX (D6) |

---

## Deliberately NOT on this list

Recorded so nobody re-buys something that was reasoned away.

| Part | Why not |
|---|---|
| **HUSB238 / any USB-PD trigger** | USB-PD dropped 2026-09-17 for a 12 V barrel jack. The XIAO's USB-C is a device port and will not negotiate anyway, so PD always meant an extra module |
| **Traco TMR 6-1211** (isolated 12→5 DC-DC) | Measured unnecessary 2026-09-17: a plain buck costs 4% of the CT's noise floor with the lamps running. WIRING.md keeps its part table only for the sizing reasoning |
| **Burden resistor for the CT** | The SCT-013-**030** is a voltage-output clamp — it is already inside the plug. Adding one divides the signal and creates the open-secondary hazard the part was chosen to avoid |
| **PC817 optocoupler breakout** | See §4. Measured 4 V out, above the C5's absolute maximum |
| **SCT-013-100** (100 A clamp) | Rejected 2026-09-13. It would spend the only margin that matters — 25 mV vs 83 mV for a small 240 V tool — to buy headroom during inrush, when nothing is measuring anyway |
| **TMC2209, stepper, 24 V supply** | Stepper deleted 2026-08-28. The ST3215 owes it nothing but the `MotorDriver` contract |
| **Panel-side CTs** | Need an electrician and void insurance. An install step the owner cannot perform is not a cheaper option, it is a different product |
| **Shelly Plus Plug US on a collector** | A 16 A relay met 45–50 A of inrush and tripped, 2026-09-03. Nothing in the control path carries motor current now |

---

## Open

- **Inrush protection for the CT.** 45–50 A through a 30 A clamp puts ~4 V on a
  3.6 V-max pin, and `isRailed()` cannot see it (clipping is symmetric, so the DC
  mean does not move). The direction is settled — series R + Schottky clamp to
  the rails — but the values are not, and nothing is fitted today.
- **A smaller clamp for small tools.** An SCT-013-005 is 5 A full scale and six
  times the resolution. Nothing needs it yet: every tool measured clears the trip
  by 6× or better.
