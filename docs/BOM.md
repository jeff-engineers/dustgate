# Bill of materials

Started 2026-09-17, when the power question was settled (12 V barrel jack →
MPM3610) and there was finally a stable thing to build on perfboard. **Merged
with the sourcing guide on 2026-10-08** (the "DustGate Sourcing Guide" doc, 2026-10-07):
its part numbers, prices and traps are here now, so this file is the one list.

**What's here:** boards, modules, sensors, the passives whose value matters, and
— since the sourcing guide — the headers and connectors a PCB needs, because on a
PCB they are footprints you have to choose, not consumables.
**What's not:** wire, hookup, heatshrink, standoffs, perfboard itself.

**Every part traces to a section of [`../firmware/WIRING.md`](../firmware/WIRING.md)**,
which carries the reasoning and the pin-level detail. This file is the shopping
list, not the design — when the two disagree, WIRING.md is right and this needs
fixing. **Both builds are drawn in [`carrier-wiring.html`](carrier-wiring.html).**

⚠️ **Quantities are PER BOARD unless a row says otherwise.** One board is one
gate (one gate = one board), so a shop is this list times the number of gates,
plus one collector board. The board is UNIVERSAL: a gate board, a CT-sensing node
and the collector board are the same carrier with different parts fitted; an
unfitted footprint costs nothing.

### Two builds, and where they differ

| | **Perfboard** — what is built and running (2026-10-08) | **PCB** — the next prototype round |
|---|---|---|
| Status pixel | Adafruit NeoPixel breakout, 5 V side of the Schottky, 330 Ω on DIN | **SparkFun COM-16347** — a bare WS2812B 5050 (GRB, the firmware's order). Same rail, same 330 Ω |
| CT jack | **Adafruit TRRS Jack Breakout #5764** (switched, labels `Sleeve Right RSw LSw Left Ring`) | **CUI SJ1-3525N** (tip and ring switches). NOT the SJ1-3523N, which has no switch |
| 12 V → 5 V | Adafruit MPM3610 5 V buck breakout | the same breakout, **on a female header socket** — it is the only converter the CT floor has been measured with |
| Servo rail | **DROK mini adjustable buck** (3 A peak, 2 A long-term), trimmed to ~6 V | **Pololu D24V22F6** (6 V fixed, 2.5 A), on a socket. Product path later: a discrete LM2596 |
| Passives | through-hole, from the drawer | through-hole (leaded) for this round; 1206 is a later spin |
| Barrel jack | whatever is on hand | **CUI PJ-063AH** (2.1 mm pin). NOT PJ-002AH / PJ-102AH — those are 2.0 mm |
| CT inrush clamp | not fitted | 1 kΩ + 2 × 1N5817 fitted on the first article, BAT54S footprint as the alternative |
| Input protection | none | **PTC fuse + TVS, fitted** (§1). Decided 2026-10-08: the boards live beside dust ducts, a large ESD source |
| Bin sensor / lamps | flying leads | a **screw terminal** |

None of the PCB column has been on a board. The perfboard column is the
reference: it works, so where the PCB differs, the PCB is the thing being tested.

---

## 1. Core — on every board, whatever job it does

| Part | Qty | Perfboard | PCB part (Digi-Key) | Notes |
|---|---|---|---|---|
| **Seeed XIAO ESP32C5** | 1 | same | Search "XIAO ESP32-C5" (~$7–10; Seeed $6.90) | THE board. Primary or node by flash, not by part. Needs its own `PLATFORMIO_CORE_DIR` (pioarduino). On sockets — see headers |
| **12 V → 5 V buck** | 1 | Adafruit MPM3610 5 V breakout | same breakout, socketed | Rated 1.2 A, plenty now that servos have their own rail. Alternative SIP-3: Recom R-78E5.0-1.0 (945-2201-ND, $3.92) — **unvalidated against the CT floor**, so re-measure before switching |
| **DC barrel jack**, 2.1 × 5.5 mm | 1 | on hand | **CUI PJ-063AH** (CP-063AH-ND, ~$1; long lead time, check stock) | The supply input, 12 V |
| **12 V wall adapter** | 1 | on hand | any | **2 A for a PWM board, 3 A for a slider** (an ST3215 stalls near 2.7 A). Not optional, just not a board part |
| **Schottky**, 1N5817 | 1 | same | **MCC 1N5817-TP** (1N5817-TPCT-ND, $0.21). Not ST's 1N5817 (last-time-buy) | Between the 5 V buck and the XIAO's `5V` pad, and **only that** — the pad is raw bidirectional VBUS, so without it the buck backfeeds a laptop's USB. The screen-side loads (pixel) sit on the XIAO side of it. 1 A is enough because servos are NOT behind it |
| 100–220 µF electrolytic | 1 | same | Panasonic EEU-FR1V221 (220 µF 35 V) | XIAO `5V` ↔ GND. Rate ≥2× the rail |
| 10 µF + 0.1 µF ceramic (X5R/X7R) | 1 ea | same | KEMET C320C104K5R5TA (0.1 µF, radial 5.08 mm); any radial X7R ≥25 V for 10 µF | XIAO 3V3 ↔ GND |
| 0.1 µF ceramic | 1 per electrolytic | same | C320C104K5R5TA | In parallel with each bulk cap |
| **WS2812B pixel** | 1 | Adafruit NeoPixel breakout | **SparkFun COM-16347** (1568-16347CT-ND, $0.68) | On D2. 5 V from the XIAO side of the Schottky — so it works on USB alone. The ~4.7 V after the diode is what makes 3.3 V data close to in spec (WIRING.md §4). Not the 5 mm through-hole NeoPixel: that one is RGB order |
| 330 Ω resistor | 1 | same | Yageo CFR-25JB-52-330R | Series on the pixel's DIN |
| **SSD1306 OLED**, I²C, 0x3C, 4-pin | 1 | same | no Digi-Key listing confirmed (~$4–8 anywhere) | D4/D5. **3V3, never 5V** (WIRING.md §5) — the perfboard runs it from 3V3 (jeff, 2026-10-08; the sourcing guide said 5 V and was wrong) — check which way round the module's VCC/GND pins are before fixing the footprint; modules differ. Optional in software |
| Momentary push button | 1 | same | **Omron B3F-1000** (SW400-ND, $0.24) | D1, wake. On a slider node a 1 s hold triggers homing |
| Second button (manual control) | 0–1 | — | B3F-1000 | **Reserve the footprint on D9**, the one spare pad. No firmware yet |
| Female header, 7-pin | 2 | as used | **Sullins PPTC071LFBN-RC** (S7005-ND, ~$0.37) | The XIAO's socket. 8.5 mm tall. Gold PPPC071LFBN-RC was backordered |
| Female header, 4-pin | 1 + per socketed module | as used | **Sullins PPTC041LFBN-RC** (S7002-ND, ~$0.26) | The OLED; also the buck modules if socketed |
| **PTC resettable fuse**, ~2.5 A hold | 1 | — | e.g. Bourns MF-R250 (radial) — not looked up; size the slider's for its 2.7 A stall | **Fitted on the PCB.** In series with the 12 V input. See "Input protection" below |
| **TVS diode**, 15 V standoff, unidirectional | 1 | — | e.g. P6KE18A (through-hole) or SMBJ15A — not looked up | **Fitted on the PCB.** 12 V to GND, after the fuse |

#### Input protection (the reserved fuse and TVS)

Neither is on the perfboard, and nothing has failed for want of them. Both guard
against a mistake rather than a fault the design makes:

- **The fuse** limits current if something on the 12 V side shorts — a pinched
  lead to the Banner sensor or the lamps, a solder bridge, a reversed buck. Without
  it, a 3 A adapter feeds the short until a trace or a wire burns; with it, the
  fuse goes high-resistance and resets once the fault is gone.
- **The TVS** clamps fast spikes on the 12 V input — hot-plugging the barrel jack,
  and the inductive kick from long lamp and strobe runs. **With the fuse in front
  of it, it is also the reverse-polarity protection:** a wrong-polarity adapter
  forward-biases the TVS, which conducts hard, and the fuse trips. That costs
  nothing at the logic rail, unlike a series diode — which would have to carry the
  servo rail's current (1N5817 is far too small; SS34/1N5822 class) and drop 0.3–0.5 V.

So there is no input diode on either build, deliberately. **Both are fitted on
the PCB (jeff, 2026-10-08)** — the boards live beside dust ducts, which build
static the way little else in a house does. If the fuse ever trips on a legitimate
servo stall, it is undersized, not unnecessary.

**The TVS guards the power input only.** Static that arrives on a SIGNAL lead —
the CT clamp's cable, a long button lead, the Banner's wires — does not pass it.
The CT tip already has its 1 kΩ and Schottky clamp (§3) and the Banner is behind
the 4N35; a wake or second button on a long lead would want the same treatment
(a series resistor, or a small ESD diode at the pad). Worth a footprint.

## 2. Driving gates — PWM boards

| Part | Qty | Perfboard | PCB | Notes |
|---|---|---|---|---|
| Gate servo, metal gear | 1 | MG995 (DeeGoo FPV) | same | **Channel 0, D7.** One gate per board. Pulse range is a shop setting (Settings → Servos, 400–2600 µs default) |
| **Servo rail buck**, 12 V → ~6 V | 1 | **DROK mini adjustable** (4.5–20 V in, 3 A peak / 2 A long-term), trimmed to ~6 V | **Pololu D24V22F6** (6 V, 2.5 A, ~$13–20, not Digi-Key), socketed | Every servo, nothing else. Split from the 5 V rail after a stall reset the XIAO on a shared supply. **Check each servo's maximum before going above 6 V** — a 6 V micro servo can be damaged at 7 V. Recom R-78B6.5-1.5 (945-3276-ND, $13.52) is a SIP-3 alternative, 1.5 A |
| 470–1000 µF low-ESR electrolytic | 1 | same | Panasonic EEU-FR1V471 / EEU-FR1V102 | At the servo connector, servo rail ↔ GND |
| 0.1 µF ceramic | 1 | same | C320C104K5R5TA | Beside it |
| Fob servo | 0–1 | — | 9 g (SG90 class) | **Channel 1, D8.** Only on the board that presses the collector's fob. Same servo rail |

**Two channels, not three or four** — the gate on D7 and the fob presser on D8.
D9 is the spare (second button), D10 is the transmitter. This list said "max 3,
D7/D8/D9" until 2026-10-08, from before the 2026-09-17 pin budget.

## 3. Sensing a tool — the CT clamp

| Part | Qty | Perfboard | PCB | Notes |
|---|---|---|---|---|
| **SCT-013-030** current clamp | 1 | same | Amazon/eBay (~$8–10); no Digi-Key or Mouser listing | 30 A : 1 V. **Voltage output — the burden resistor is INSIDE the plug. Do not add one.** 3.5 mm plug |
| **3.5 mm switched jack** | 1 | **Adafruit TRRS Jack Breakout #5764** | **CUI SJ1-3525N** (CP1-3525N-ND, $1.00) | **Buy a switched one.** The switch pulls an unplugged jack to 3V3, so it reads as railed (ignored) instead of the bias midpoint — which is indistinguishable from an idle tool. Wiring and metering: WIRING.md §8 |
| 1 kΩ resistor | 2 | same | Yageo CFR-25JB-52-1K | The bias divider, 1k/1k off 3V3. **Not 10k/10k** |
| Bulk cap on the bias node | 1 | as built (see Open) | Panasonic EEU-FR1V101 (100 µF) | Bias node to GND |
| 0.1 µF ceramic | 1 | same | C320C104K5R5TA | Bias node to GND |
| 10 kΩ resistor | 1 | **new** | Yageo CFR-25JB-52-10K | Plug-detect: jack switch → 3V3 |
| 1 kΩ resistor | 1 | **new** | CFR-25JB-52-1K | Inrush: series, jack tip → D0 |
| Schottky, 1N5817 | 2 | **new** | 1N5817-TP; or one **BAT54S** (SOT-23) instead | Inrush: D0 → 3V3 and GND → D0. **Margin is thin** — see WIRING.md §8 |

Measured floor without the new parts: **~0.195 A** (6.2–7.0 ADC counts), stable
across four sessions and two supply topologies. Trip lands at ~0.8 A. **Re-measure
it with the three new parts fitted** — they add source impedance and leakage on
the bias node.

## 4. The collector board — extras beyond the core

A collector board is an ordinary primary or node that a LAYOUT points at a bin,
a clamp and a remote. No separate firmware, no separate pin map.

| Part | Qty | Perfboard | PCB | Notes |
|---|---|---|---|---|
| **Banner QS18VN6D** beam sensor | 1 | same | Powermatic Associates (~$90, authorized) or eBay ($27–34). Digi-Key lists only the QS18VN6D**B** — a different model, check before swapping | Bin full. 10–30 V DC, NPN sinking output |
| **4N35 optocoupler**, DISCRETE DIP-6 | 1 | same | Vishay 4N35 (search) | D6. **Not a PC817 breakout module** — measured 4 V on its output, above a C5 GPIO's absolute maximum |
| 1 kΩ resistor | 1 | same | CFR-25JB-52-1K | 4N35 LED series, off 12 V. ≈10.8 mA |
| 1N4148 signal diode | 1 | recommended | 1N4148 (DO-35) | **ESD, added 2026-10-08.** Anti-parallel across the 4N35's LED (anode to pin 2, cathode to pin 1). The LED survives only ~6 V reverse, and the Banner's output lead runs into a collector full of charged dust |
| TVS diode, 15 V standoff | 1 | recommended | e.g. P4KE18A — not looked up | **ESD, added 2026-10-08.** At the screw terminal, 12 V to GND, so a hit on the Banner's or the lamps' leads is clamped where it enters rather than after crossing the board |
| **Screw terminal** | 1 | — (flying leads) | 3.5 or 5 mm pitch, ~5-way: 12 V, GND, Banner out, lamp, strobe | Not on the list before 2026-10-08 |
| 12 V green pilot lamp | 1 | existing | existing | Stays on 12 V |
| 12 V red strobe | 1 | existing | existing | ~2.7 counts on the CT floor |
| **315 MHz TX module** | 1 | same | **SparkFun 10535** (1568-10535-ND, $5.75; 12-week lead) | **D10.** Keys the collector's own remote; the only part the RF path needs. 3.3 V data; 5 V on the bench, 12 V for real range — unverified, so the PCB should let either rail reach it (a jumper) |
| 9 g servo (SG90 class) | 0–1 | — | — | ALTERNATIVE to the RF path: an arm pressing the fob's own buttons. D8, the servo rail |

**No HT12E, no DIP switch, no Rosc resistor.** `control/RfCollectorPresser.h`
generates the 12-bit frame itself and clocks it out of the **RMT peripheral** in
hardware — deliberately, because the primary runs WiFi and FreeRTOS would stretch
a bit-banged pulse. The fob's address is a constructor argument (`0b01011110` for
the Rockler remote measured on 2026-09-16), not a DIP setting. Those three parts
were the ORIGINAL bench rig that proved the encoding; buying them now would be
buying a chip to do what the ESP32 already does better.

**RF and servo-on-fob are alternatives, not both.** RF is proven on the bench
and is the only route for a collector with no fob; the servo is what ships,
because "open the fob and solder across the button" is an install step a
woodworker cannot perform.

## 5. The slider — only on a rack build

Its own small board: different pin map (D6/D7 bus, D8/D9 endstops) and 12 V at
the servo rather than through a buck. **Never shares a board with PWM servos** —
`config.h` `#error`s if a pin map claims both.

| Part | Qty | Notes |
|---|---|---|
| **ST3215** serial bus servo, 12 V | 1 | 30 kg·cm. Waveshare SKU 22414 ($21.99) or RobotShop ($28.24 incl. tariffs). **Not the 7.4 V variant** |
| Endstop switch, NC | 2 | D8/D9. Normally-CLOSED so a snapped lead reads as triggered and stops the carriage |
| Bus interface | 1 | The **Seeed XIAO Bus Servo Adapter** (what the bench slider uses), or hand-built: a 74LVC1G125 buffer per WIRING.md §7 — not just a 1 kΩ on TX |

---

## Ordering the PCB

A 10-board JLCPCB run of a 2-layer board under 100 × 100 mm is about $10–20
landed (reported figures, not a quote). Keep the order at $59 or less for the
cheap shipping; 1.6 mm, lead-free HASL, 1 oz, unassembled, no stencil.
**Tariffs swing it:** the exclusion that shields bare 2- and 4-layer boards (HTS
8534) is reported to expire **2026-11-10**, after which 25% applies — order before
then if possible, and read the duty line at checkout.

**First article first:** build one board, read the CT floor against ~6.7 counts
with servos idle and moving, then build the other nine.

Layout rules that come from this list (the sourcing guide's, unchanged): keep the
CT bias node to millimetres of copper and far from both bucks' switching loops;
cluster the inrush parts at the D0 socket pin; star the grounds at the supply (the
12 V load returns, both bucks' input grounds); servo power gets its own return;
test points on 5 V, the servo rail, 3V3 and the CT bias node; mounting holes and a
silkscreen revision.

## Traps found while sourcing

- **SJ1-3523N has no switch.** 3524N switches the tip only, **3525N** tip and ring.
- **PJ-002AH and PJ-102AH are 2.0 mm** barrel jacks. The adapter is 2.1 mm: **PJ-063AH**.
- **ST's 1N5817 at Digi-Key is last-time-buy**; MCC's 1N5817-TP is stocked.
- **QS18VN6D vs QS18VN6DB** — Digi-Key has only the second; check Banner's sheet.
- **The 5 mm through-hole NeoPixel is RGB order**; WS2812B 5050 parts are GRB, which is what the firmware drives.

---

## Deliberately NOT on this list

Recorded so nobody re-buys something that was reasoned away.

| Part | Why not |
|---|---|
| **A diode on the 12 V input** | It would carry the servo rail's current (a 1N5817 cannot; SS34/1N5822 class) and drop 0.3–0.5 V for nothing. Reverse polarity is the fuse + TVS's job (§1) |
| **HUSB238 / any USB-PD trigger** | USB-PD dropped 2026-09-17 for a 12 V barrel jack. The XIAO's USB-C is a device port and will not negotiate anyway |
| **Traco TMR 6-1211** (isolated 12→5 DC-DC) | Measured unnecessary 2026-09-17: a plain buck costs 4% of the CT's noise floor with the lamps running |
| **Burden resistor for the CT** | The SCT-013-**030** is a voltage-output clamp — it is already inside the plug |
| **PC817 optocoupler breakout** | See §4. Measured 4 V out, above the C5's absolute maximum |
| **SCT-013-100** (100 A clamp) | Rejected 2026-09-13. It would spend the only margin that matters — 25 mV vs 83 mV for a small 240 V tool |
| **TMC2209, stepper, 24 V supply** | Stepper deleted 2026-08-28 |
| **HT12E encoder, 8-way DIP switch, 1.0 MΩ Rosc** | The firmware IS the encoder (§4) |
| **Panel-side CTs** | Need an electrician and void insurance. An install step the owner cannot perform is not a cheaper option, it is a different product |
| **Shelly Plus Plug US on a collector** | A 16 A relay met 45–50 A of inrush and tripped, 2026-09-03 |
| **A custom buck on this round** | At 10 boards a module is cheaper than laying out and validating one. The discrete LM2596 servo rail (Vout = 1.23 × (1 + R2/R1); R1 = 1 k, R2 = 3.9 k ≈ 6.0 V) is the product path once the module has proven the voltage |

---

## Open

- **The CT bias node's bulk cap: 10 µF or 100 µF? UNCLEAR.** This list says
  100 µF; WIRING.md §8's table says 10 µF. Either works as bulk. The board to read
  it off is the **planer-sensor node** — the one CT board known to work — then make
  both files and `carrier-wiring.html` say the same thing.
- **Inrush protection for the CT** is now a proposed circuit (§3, WIRING.md §8),
  not yet fitted or scoped. 45–50 A through a 30 A clamp puts ~4 V on a 3.6 V-max
  pin, and `isRailed()` cannot see it.
- **"Clamp unplugged" is not reported.** The switched jack makes it detectable
  (D0 railed high); the firmware ignores a railed reading but says nothing.
- **The 315 MHz module's supply** — 5 V or 12 V on the PCB. Bench it before routing.
- **A smaller clamp for small tools.** An SCT-013-005 is six times the resolution.
  Nothing needs it yet.
- **Product spin:** drop the XIAO for the ESP32-C5-WROOM-1 module, and plan FCC.
  Not this round.
