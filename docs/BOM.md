# Bill of materials

Started 2026-09-17, when the power question was settled (12 V barrel jack →
MPM3610) and there was finally a stable thing to build on perfboard. **Merged
with the sourcing guide on 2026-10-08** (the "DustGate Sourcing Guide" doc, 2026-10-07):
its part numbers, prices and traps are here now, so this file is the one list.

**What's here:** boards, modules, sensors, the passives whose value matters, and
— since the sourcing guide — the headers and connectors a PCB needs, because on a
PCB they are footprints you have to choose, not consumables.
**What's not:** wire, hookup, heatshrink, standoffs, perfboard itself.

**This file is the shopping list. The wiring is drawn in [`carrier-wiring.html`](carrier-wiring.html)**
(both builds), and the pins are the board headers in `firmware/boards/`. When they disagree with this list, they
win and this needs fixing. The reasoning behind most parts is in the archived wiring notes,
[`archive/WIRING.md`](archive/WIRING.md): history, not a current reference.

⚠️ **Quantities are PER BOARD unless a row says otherwise.** One board is one
gate (one gate = one board), so a shop is this list times the number of gates,
plus one collector board. The board is UNIVERSAL: a gate board, a CT-sensing node
and the collector board are the same carrier with different parts fitted; an
unfitted footprint costs nothing.

### Two builds, and where they differ

| | **Perfboard** — what is built and running (2026-10-08) | **PCB** — V1, laid out 2026-10-29, not yet built |
|---|---|---|
| Status pixel | Treedix WS2812B board, 5 V side of the Schottky, 330 Ω on DIN | **the same board** — decided 2026-10-17 over a bare 5050 (SparkFun COM-16347): it is what has run since 2026-08-23, and a panel of 48 is on hand |
| CT jack | **Adafruit TRRS Jack Breakout #5764** (switched, labels `Sleeve Right RSw LSw Left Ring`) | **CUI SJ1-3525N** (tip and ring switches). NOT the SJ1-3523N, which has no switch |
| 12 V → 5 V | Adafruit MPM3610 5 V buck breakout | the same breakout, **on a female header socket** — it is the only converter the CT floor has been measured with |
| Servo rail | **DROK mini adjustable buck** (3 A peak, 2 A long-term), trimmed to ~6 V | **the same DROK**, on five wire pads (IN+, IN−, OUT+, OUT−, EN; EN left open). A Pololu D24V22F6 can land on the same pads instead (row 6). Product path later: a discrete LM2596 |
| Passives | through-hole, from the drawer | through-hole (leaded) for this round; 1206 is a later spin |
| Barrel jack | whatever is on hand | **CUI PJ-063AH**: a 2.0 mm pin, which is what a 2.1 mm plug mates with, rated 8 A |
| CT inrush clamp | 1 kΩ + 2 × 1N5817, fitted on the cyclone and miter saw boards | 1 kΩ + 2 × 1N5817 (R6, D3, D4), with test points on the tip, the bias node and the jack switch. No BAT54S footprint on V1 |
| Input protection | none | **PTC fuse + TVS, fitted** (§1). Decided 2026-10-08: the boards live beside dust ducts, a large ESD source |
| Bin sensor | flying leads, **ferrules crimped** — never soldered or tinned: solder creeps under a screw and the joint cracks with vibration | a **4-way screw terminal**, J1: brown, black, blue, and the unused white, so no lead is left loose (ferrules on the leads too) |
| Shop light | chained off the pixel's DOUT | **J4**, a JST-XH 3-pin: 5 V from the buck, DIN through R8 (330 Ω), GND |
| Expansion | — | **J5**, a Qwiic socket on the OLED's I²C bus; **J6**, D3 on a 2-pin header; **J7 / J8**, optional sockets for panel buttons in parallel with SW1 (Wake, D1) and SW2 (Enable, D9) |
| Mounting | — | four M3 holes, one per corner |

None of the PCB column has been on a board. The perfboard column is the
reference: it works, so where the PCB differs, the PCB is the thing being tested.


### PCB order list — one table (2026-10-08)

Everything a **PCB carrier** needs, with where to buy it. Links and prices are the sourcing guide's
(2026-10-07) unless the Status says otherwise; a "search" link opens a Digi-Key search because no product
page was confirmed. **Check stock and price in the cart before ordering.** The KiCad symbols and
footprints are chosen with the board design, which is kept outside this repo; the row numbers here are
what it keys on, so **renumbering a row breaks that check** — append rather than reorder.
Qty is per board; "coll." = the collector board only.

| # | Part | Qty | Buy | Price | Status / note |
|---|---|---|---|---|---|
| 1 | Seeed XIAO ESP32C5 | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=XIAO%20ESP32-C5) · Seeed direct | ~$7–10 | Stock not confirmed at Digi-Key. Socketed (row 2), never soldered down |
| 2 | Sullins PPTC071LFBN-RC, 7-pin female, 8.5 mm | 2 | [Digi-Key S7005-ND](https://www.digikey.com/en/products/detail/sullins-connector-solutions/PPTC071LFBN-RC/810146) | $0.37 | Verified. The XIAO's socket |
| 3 | Sullins PPTC041LFBN-RC, 4-pin female | 2 + 1 coll. | [Digi-Key S7002-ND](https://www.digikey.com/en/products/detail/PPTC041LFBN-RC/S7002-ND/810144) | $0.26 | Verified. The OLED, the MPM3610 breakout (row 5), and on a collector board the 315 MHz transmitter (row 29) |
| 4 | Sullins 5-pin female (PPTC051LFBN-RC) | 0 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=PPTC051LFBN-RC) | ~$0.30 | **Not used on V1.** It was the Pololu buck's socket; the Pololu (row 6) is now the alternative and lands on U5's wire pads instead |
| 5 | Adafruit MPM3610 5 V buck breakout #4739 | 1 | [Adafruit](https://www.adafruit.com/product/4739) | ~$6 | What the CT floor was measured with. Header, from Adafruit's board file: GND, VOUT, VIN, EN, 2.54 mm (the LM1117 TO-220 order plus EN) — **go by the silkscreen**, the header is rotated on the board |
| 6 | Pololu D24V22F6, 6 V 2.5 A buck | 0 (alt.) | [Pololu #2859](https://www.pololu.com/product/2859) | $12.95–19.95 | **The alternative servo buck on V1** — the DROK (row 36) is the default. On U5's wire pads: VIN → IN+, GND → IN−, VOUT → OUT+; EN and PG open. Five pins at 0.1″: PG, EN, VIN, GND, VOUT — order from Pololu's [dimension diagram](https://www.pololu.com/file/0J1031/d24v22fx-step-down-voltage-regulator-dimension-diagram.pdf). Not a Digi-Key part |
| 7 | CUI PJ-063AH barrel jack, 2.1 × 5.5 mm | 1 | [Digi-Key CP-063AH-ND (search)](https://www.digikey.com/en/products/result?keywords=PJ-063AH) | ~$1 | 20-week lead time reported. 2.0 mm centre pin, 8 A (Same Sky datasheet, 2025-09-30) — the pin a 2.1 mm plug takes. Pin 1 centre (+12 V), pin 2 sleeve; the MS legs are mechanical |
| 8 | Bourns MF-RHT200 PTC fuse (2 A hold, 16 V) | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=MF-RHT200) | ~$0.50 | F2. Not in the guide; not looked up. The PWM board's 2 A adapter. **Slider: MF-RHT300** for its 2.7 A stall |
| 9 | P6KE18A TVS, 15.3 V standoff, unidirectional | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=P6KE18A) | ~$0.40 | D6. Approved (jeff, 2026-10-08); stock not looked up. 12 V input, after the fuse |
| 10 | MCC 1N5817-TP Schottky, DO-41 | 3 | [Digi-Key 1N5817-TPCT-ND](https://www.digikey.com/en/products/detail/mcc-micro-commercial-components/1N5817-TP/950394) | $0.21 | Verified. D7 on the XIAO's 5 V pad; D3 and D4 clamping D0 |
| 11 | BAT54S dual Schottky, SOT-23 (alternative to two of row 10) | 0 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=BAT54S) | ~$0.15 | **Not on V1**: no footprint for it. Kept for a later spin |
| 12 | Panasonic EEU-FR1V221, 220 µF 35 V | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=EEU-FR1V221) | ~$0.40 | C3, at the XIAO 5 V pad. **Check the can diameter** in the datasheet and pick the matching `CP_Radial_*` |
| 13 | Panasonic EEU-FR1V471 / EEU-FR1V102, 470–1000 µF 35 V | 1 | [471 (search)](https://www.digikey.com/en/products/result?keywords=EEU-FR1V471) · [102 (search)](https://www.digikey.com/en/products/result?keywords=EEU-FR1V102) | ~$0.50 | C6, at the servo connectors. Same diameter check |
| 14 | Panasonic EEU-FR1V100, 10 µF 35 V | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=EEU-FR1V100) | ~$0.25 | C1, the CT bias bulk: 10 µF, what the planer-sensor node runs (jeff, 2026-10-09). Part number not looked up |
| 15 | KEMET C320C104K5R5TA, 0.1 µF X7R radial | 4 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=C320C104K5R5TA) | $0.13 | C4 beside C3, C5 beside C6, C7 beside C8, and C2 on the CT bias node. Was 5 until 2026-10-17: the pixel board carries its own. **2.54 mm lead spacing**, body 5.08 × 3.18 mm max (KEMET datasheet, 2026-10-18) — not 5.08 mm, which is the C322. A footprint with both spacings takes either |
| 16 | KEMET C322C106K3R5TA, 10 µF ±10% 25 V X7R radial | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=C322C106K3R5TA) | ~$0.60 | C8, XIAO 3V3 ↔ GND, beside a row-15 0.1 µF. **Non-polarized** ceramic. 5.08 mm lead spacing, body 5.08 × 4.07 mm max, 6.60 mm tall (KEMET GoldMax 300 datasheet). The same GoldMax family as row 15, but the C322 style: at 10 µF the body is 4.07 mm thick, too wide for a 2.54 mm-pitch footprint. Stock and price not confirmed; a sibling part was listed at a 20-week lead |
| 17 | Yageo CFR-25JB-52-1K, 1 kΩ ¼ W | 3 + 1 coll. | [Digi-Key](https://www.digikey.com/en/products/detail/yageo/CFR-25JB-52-1K/96) | pennies | Verified. R1 and R3, the CT bias divider; R6, CT series; R5, the 4N35's LED, on a collector board |
| 18 | Yageo CFR-25JB-52-330R, 330 Ω | 2 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=CFR-25JB-52-330R) | pennies | R4, pixel DIN; R8, the shop light's data line at J4 — with only USB in, the ring has no 5 V while the pixel still drives it, and R8 limits that current |
| 19 | Yageo CFR-25JB-52-10K, 10 kΩ | 1 + 1 coll. | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=CFR-25JB-52-10K) | pennies | R2, CT plug-detect; R7, the bin pull-up on D6 |
| 20 | Treedix WS2812B 5050 board, snapped off a 48-board panel | 1 | [Amazon B09FSTT43H](https://www.amazon.com/dp/B09FSTT43H) | ~$0.27 ($12.99 / 48) | D1. On hand. Two rows of three 0.1″ pins, 10.16 mm apart: 5V, DIN, GND on one edge, 5V, DOUT, GND on the other (Jeff, 2026-10-17). Carries its own decoupling. GRB, the firmware's order. Was the SparkFun COM-16347 bare 5050 until 2026-10-17 |
| 21 | SSD1306 0.96″ OLED, 4-pin I²C | 1 | none confirmed at Digi-Key | ~$4–8 | U1. **Pin order varies** (VCC/GND swapped on some). V1 is drawn GND, VCC, SCL, SDA — **read your module before soldering**: the other order puts 3V3 in backwards |
| 22 | Omron B3F-1000, 6 mm tactile | 2 | [Digi-Key SW400-ND](https://www.digikey.com/en/products/detail/omron-electronics-inc-emc-div/B3F-1000/33150) | $0.24 | Verified. SW1, Wake (D1); SW2, Enable (D9) — no firmware for D9 yet. A taller plunger in the same 6 mm footprint reaches through a case lid |
| 23 | CUI SJ1-3525N, 3.5 mm switched jack | 1 | [Digi-Key CP1-3525N-ND](https://www.digikey.com/en/products/detail/same-sky-formerly-cui-devices-/SJ1-3525N/738687) | $1.00 | J2. Verified. NOT the SJ1-3523N (no switch). Meter the NC side first (archive/WIRING.md §8) |
| 24 | SCT-013-030 clamp, 30 A : 1 V | 1 per CT | Amazon / eBay | ~$8–10 | Plugs into row 23 |
| 25 | 4N35 optocoupler, DIP-6 | 1 coll. | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=4N35) | <$1 | U2. Not a PC817 breakout |
| 26 | 1N4148 signal diode, DO-35 | 1 coll. | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=1N4148) | pennies | D5. ESD: across the 4N35's LED |
| 27 | P4KE18A TVS, DO-41 | 1 coll. | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=P4KE18A) | ~$0.30 | D2. ESD: 12 V → GND at the Banner's terminal. Approved (jeff, 2026-10-08); stock not looked up |
| 28 | Screw terminal, 4-way, 5 mm (Phoenix MKDS 1,5/4) | 1 coll. | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=MKDS%201%2C5%2F%204) | ~$1.20 | J1: 1 Banner brown (12 V) · 2 Banner black · 3 Banner blue (GND) · 4 Banner white, not connected — a terminal for every lead, so none is left loose. As drawn on the wiring page. Was 3-way until 2026-10-29, and 6-way with the 12 V lamps until 2026-10-20 |
| 29 | SparkFun 10535, 315 MHz TX | 1 coll. | [Digi-Key 1568-10535-ND](https://www.digikey.com/en/products/detail/sparkfun-electronics/10535/5673762) | $5.75 | U6. Verified, 12-week lead. Pins GND, DATA, VCC, ANT (Wenshing TWS-BS-6 datasheet, the module SparkFun sells). JP1, the VCC jumper 5 V / 12 V: a 1x03 0.1″ male header + a shunt |
| 30 | Banner QS18VN6D beam sensor | 1 coll. | [Powermatic Associates](https://www.powermatic.net/part/banner/qs18vn6d/1090386) | ~$90 (eBay $27–34) | Digi-Key lists only the QS18VN6D**B** |
| 31 | Servo header — breakaway 0.1″ male, 3 pins | 1 (+1 fob) | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=PREC003SAAN-RC) | pennies | Servo_1 (gate, D7) and Servo_2 (fob, D8). What the perfboard uses (jeff, 2026-10-08). Signal, V+, GND. A standard servo plug has no latch, so no header locks it — a printed retainer in the housing is the fix if vibration ever walks one off |
| 32 | Test points | 6 | — | — | Bare 1.5 mm pads, nothing to buy: TP1 servo rail, TP2 XIAO 5 V, TP3 3V3, TP4 CT tip, TP5 CT bias node, TP6 jack switch. The GND loop is row 41 |
| 33 | Mounting holes, M3 | 4 | — | — | 3.2 mm, unplated, one per corner. Screws and standoffs are not on this list. Were M2.5 with a GND pad until 2026-10-29 |
| 34 | 12 V wall adapter, 2.1 mm | 1 | any | — | 2 A for a PWM board, 3 A for a slider |
| 35 | Bourns MF-RHT050 PTC fuse (0.5 A hold) | 1 coll. | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=MF-RHT050) | ~$0.40 | Not looked up. F1, the Banner's own fuse: F2's output → F1 → the Banner's brown lead, the 4N35's LED resistor and the TVS at the terminal. A pinched Banner lead trips F1 alone, so the board stays up. The Banner and the LED draw ~35 mA |
| 36 | DROK mini adjustable buck, 4.5–20 V in, 3 A peak / 2 A long-term | 1 | Amazon (search "DROK mini buck") | ~$3 | U5, the servo rail. On hand; the perfboard's buck. **Trim it to ~6 V before a servo goes on.** It hangs on leads to five wire pads: IN+, IN−, OUT+, OUT−, EN. EN left open — it runs without a lead there (jeff, 2026-10-25). Pololu D24V22F6 (row 6) is the alternative |
| 37 | JST BM04B-SRSS-TB, 4-pin SH 1.0 mm, vertical (Qwiic / STEMMA QT) | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=BM04B-SRSS-TB) | ~$0.50 | J5, an expansion socket on the OLED's I²C bus: GND, 3V3, SDA, SCL, the Qwiic order, so a Qwiic or STEMMA QT board plugs in with a stock cable. The board's only surface-mount part. Optional. Not looked up |
| 38 | JST B3B-XH-A, 3-pin XH 2.5 mm, vertical | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=B3B-XH-A) | ~$0.20 | J4, the shop light ring: 1 = 5 V (buck side), 2 = DIN through R8, 3 = GND. The ring's lead takes an XHP-3 housing and SXH crimps. Not looked up |
| 39 | JST B2B-XH-A, 2-pin XH 2.5 mm, vertical | 0–2 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=B2B-XH-A) | ~$0.15 | J7 (Wake, D1) and J8 (Enable, D9): panel buttons, in parallel with SW1 and SW2, for a case that puts the buttons elsewhere. Leave them out when the board buttons are used. Pin 1 the pad, pin 2 GND. Not looked up |
| 40 | Panasonic EEU-FR1E101, 100 µF 25 V | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=EEU-FR1E101) | ~$0.30 | C9, bulk on the fused 12 V beside the barrel jack: steadies the input when a servo starts. 6.3 mm can, 2.5 mm leads. Part number not looked up |
| 41 | Keystone 5001 test loop, black | 1 | [Digi-Key (search)](https://www.digikey.com/en/products/result?keywords=Keystone%205001) | ~$0.30 | TP7, GND for a scope clip. 5000–5004 are the same loop in different colours; black for ground. Not looked up |
| 42 | Breakaway 0.1″ male header, 2 pins | 1 | the row 31 strip | pennies | J6: D3 and GND. **D3 is a strapping pin** — nothing on it may hold it LOW through reset |

Not on a board: gate servos (MG995 class, or the ST3215 for a slider — Waveshare SKU 22414, $21.99), and
Shelly plugs. The JLCPCB board itself is under "Ordering the PCB" below.
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
| 10 µF + 0.1 µF ceramic (X5R/X7R) | 1 ea | same | KEMET C320C104K5R5TA (0.1 µF, radial, **2.54 mm** lead spacing); KEMET C322C106K3R5TA (10 µF, row 16) | XIAO 3V3 ↔ GND |
| 0.1 µF ceramic | 1 per electrolytic | same | C320C104K5R5TA | In parallel with each bulk cap |
| **WS2812B pixel** | 1 | Treedix WS2812B board | the same board (PCB order list row 20) | On D2. 5 V from the XIAO side of the Schottky — so it works on USB alone. The ~4.7 V after the diode is what makes 3.3 V data close to in spec (archive/WIRING.md §4). Not the 5 mm through-hole NeoPixel: that one is RGB order |
| 330 Ω resistor | 1 | same | Yageo CFR-25JB-52-330R | Series on the pixel's DIN |
| **SSD1306 OLED**, I²C, 0x3C, 4-pin | 1 | same | no Digi-Key listing confirmed (~$4–8 anywhere) | D4/D5. **3V3, never 5V** (archive/WIRING.md §5) — the perfboard runs it from 3V3 (jeff, 2026-10-08; the sourcing guide said 5 V and was wrong) — check which way round the module's VCC/GND pins are before fixing the footprint; modules differ. Optional in software |
| Momentary push button | 1 | same | **Omron B3F-1000** (SW400-ND, $0.24) | D1, wake. On a slider node a 1 s hold triggers homing |
| Second button (manual control) | 0–1 | — | B3F-1000 | SW2, "Enable", on D9, the one spare pad. No firmware yet |
| Female header, 7-pin | 2 | as used | **Sullins PPTC071LFBN-RC** (S7005-ND, ~$0.37) | The XIAO's socket. 8.5 mm tall. Gold PPPC071LFBN-RC was backordered |
| Female header, 4-pin | 1 + per socketed module | as used | **Sullins PPTC041LFBN-RC** (S7002-ND, ~$0.26) | The OLED; also the buck modules if socketed |
| **PTC resettable fuse**, ~2.5 A hold | 1 | — | **Bourns MF-RHT200** (PCB order list row 8); the slider's MF-RHT300, for its 2.7 A stall | **Fitted on the PCB.** In series with the 12 V input. See "Input protection" below |
| **TVS diode**, 15 V standoff, unidirectional | 1 | — | e.g. P6KE18A (through-hole) or SMBJ15A — not looked up | **Fitted on the PCB.** 12 V to GND, after the fuse |

#### Input protection (the reserved fuse and TVS)

Neither is on the perfboard, and nothing has failed for want of them. Both guard
against a mistake rather than a fault the design makes:

- **The fuse** limits current if something on the 12 V side shorts — a pinched
  lead to the Banner sensor, a solder bridge, a reversed buck. Without
  it, a 3 A adapter feeds the short until a trace or a wire burns; with it, the
  fuse goes high-resistance and resets once the fault is gone.
- **The TVS** clamps fast spikes on the 12 V input — hot-plugging the barrel jack,
  and spikes picked up on the Banner's lead. **With the fuse in front
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
(a series resistor, or a small ESD diode at the pad). **V1's panel-button sockets, J7 and J8, have
neither:** keep a panel button's lead short, or put a 1 kΩ in it at the button end.

## 2. Driving gates — PWM boards

| Part | Qty | Perfboard | PCB | Notes |
|---|---|---|---|---|
| Gate servo, metal gear | 1 | MG995 (DeeGoo FPV) | same | **Channel 0, D7.** One gate per board. Pulse range is a shop setting (Settings → Servos, 400–2600 µs default) |
| **Servo rail buck**, 12 V → ~6 V | 1 | **DROK mini adjustable** (4.5–20 V in, 3 A peak / 2 A long-term), trimmed to ~6 V | **the same DROK**, U5, on wire pads (PCB order list row 36); the **Pololu D24V22F6** (6 V, 2.5 A, ~$13–20, not Digi-Key) lands on the same pads as the alternative | Every servo, nothing else. Split from the 5 V rail after a stall reset the XIAO on a shared supply. **Check each servo's maximum before going above 6 V** — a 6 V micro servo can be damaged at 7 V. Recom R-78B6.5-1.5 (945-3276-ND, $13.52) is a SIP-3 alternative, 1.5 A |
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
| **3.5 mm switched jack** | 1 | **Adafruit TRRS Jack Breakout #5764** | **CUI SJ1-3525N** (CP1-3525N-ND, $1.00) | **Buy a switched one.** The switch pulls an unplugged jack to 3V3, so it reads as railed (ignored) instead of the bias midpoint — which is indistinguishable from an idle tool. Wiring and metering: archive/WIRING.md §8 |
| 1 kΩ resistor | 2 | same | Yageo CFR-25JB-52-1K | The bias divider, 1k/1k off 3V3. **Not 10k/10k** |
| Bulk cap on the bias node, 10 µF electrolytic | 1 | same | Panasonic EEU-FR1V100 (10 µF) | Bias node to GND. What the planer-sensor node runs (jeff, 2026-10-09) |
| 0.1 µF ceramic | 1 | same | C320C104K5R5TA | Bias node to GND |
| 10 kΩ resistor | 1 | same | Yageo CFR-25JB-52-10K | Plug-detect: jack switch → 3V3 |
| 1 kΩ resistor | 1 | same | CFR-25JB-52-1K | Inrush: series, jack tip → D0 |
| Schottky, 1N5817 | 2 | same | 1N5817-TP (D3, D4). A **BAT54S** (SOT-23) could replace both on a later spin; V1 has no footprint for it | Inrush: D0 → 3V3 and GND → D0. **Meter their direction before power-up** — a reversed one holds D0 low and drags 3V3 down (two boards, 2026-10-11). Margin is thin |

Fitted on the cyclone and miter saw boards (2026-10). With them, the cyclone reads a floor of **0.04 A** and trips at
0.23 A. (Before them, on the bench: ~0.195 A, trip ~0.8 A.)

## 4. The collector board — extras beyond the core

A collector board is an ordinary primary or node that a LAYOUT points at a bin,
a clamp and a remote. No separate firmware, no separate pin map.

| Part | Qty | Perfboard | PCB | Notes |
|---|---|---|---|---|
| **Banner QS18VN6D** beam sensor | 1 | same | Powermatic Associates (~$90, authorized) or eBay ($27–34). Digi-Key lists only the QS18VN6D**B** — a different model, check before swapping | Bin full. 10–30 V DC, NPN sinking output |
| **4N35 optocoupler**, DISCRETE DIP-6 | 1 | same | Vishay 4N35 (search) | D6. **Not a PC817 breakout module** — measured 4 V on its output, above a C5 GPIO's absolute maximum |
| 1 kΩ resistor | 1 | same | CFR-25JB-52-1K | 4N35 LED series, off 12 V. ≈10.8 mA |
| 1N4148 signal diode | 1 | recommended | 1N4148 (DO-35) | **ESD, added 2026-10-08.** Anti-parallel across the 4N35's LED (anode to pin 2, cathode to pin 1). The LED survives only ~6 V reverse, and the Banner's output lead runs into a collector full of charged dust |
| TVS diode, 15 V standoff | 1 | recommended | e.g. P4KE18A — not looked up | **ESD, added 2026-10-08.** At the screw terminal, 12 V to GND, so a hit on the Banner's leads is clamped where it enters rather than after crossing the board |
| **PTC fuse for the Banner** (F1) | 1 | — | Bourns MF-RHT050 (PCB order list row 35) | Between the main fuse's (F2's) output and the Banner's 12 V, so a short on its lead trips F1 rather than the whole board. Added 2026-10-20 |
| **Screw terminal** | 1 | — (flying leads) | 5 mm, 4-way, J1: Banner brown, black, blue, and white, unused (PCB order list row 28) | Not on the list before 2026-10-08 |
| **315 MHz TX module** | 1 | same | **SparkFun 10535** (1568-10535-ND, $5.75; 12-week lead) | **D10.** Keys the collector's own remote; the only part the RF path needs. 3.3 V data; 5 V on the bench, 12 V for real range — unverified, so the PCB lets either rail reach it: JP1 and a shunt |
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
| Bus interface | 1 | The **Seeed XIAO Bus Servo Adapter** (what the bench slider uses), or hand-built: a 74LVC1G125 buffer per archive/WIRING.md §7 — not just a 1 kΩ on TX |

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

**V1** (laid out 2026-10-29) is 73.3 × 89.6 mm, two layers, every part through-hole except
the Qwiic socket, with a ground pour on both sides. Trace widths are sized for 1 oz copper: 2.0 mm
for the raw 12 V, 1.5 mm for the fused 12 V and the servo rail, 1.0 mm for the 5 V. The CT
circuit sits in one corner beside D0, away from both bucks; the ports a case needs to reach — the
shop light, Qwiic, and the two panel-button sockets — are together on one edge. The silkscreen
carries "DustGate PCB v1.0".

## Traps found while sourcing

- **SJ1-3523N has no switch.** 3524N switches the tip only, **3525N** tip and ring.
- **A "2.1 mm" barrel jack has a 2.0 mm pin.** The 2.1 mm is the plug's hole, and the jack's pin has to be slightly smaller to fit it. Datasheets give the pin, so a jack for 2.1 mm plugs reads "2.0 mm": the PJ-063AH, the PJ-002AH and the PJ-102AH all take our adapters, and they differ in current — 8 A, 5 A, and less for the PJ-102AH, Adafruit's breadboard jack. The mismatch to avoid is a **2.5 mm pin** jack (CUI's B-suffix parts, such as the PJ-079BH), which a 2.1 mm plug will not go onto, or a 2.5 mm plug on a 2.0 mm pin, which goes in loose. This line said the opposite until 2026-10-18.
- **ST's 1N5817 at Digi-Key is last-time-buy**; MCC's 1N5817-TP is stocked.
- **QS18VN6D vs QS18VN6DB** — Digi-Key has only the second; check Banner's sheet.
- **The 5 mm through-hole NeoPixel is RGB order**; WS2812B 5050 parts are GRB, which is what the firmware drives.

---

## Deliberately NOT on this list

Recorded so nobody re-buys something that was reasoned away.

| Part | Why not |
|---|---|
| **12 V pilot lamp and red strobe** at the collector | Out 2026-10-20 (D-86): the shop light chained off the collector board's pixel shows a full bin, running suction and "needs a person", which two lamps on the Banner's 12 V could not |
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

- **Inrush protection for the CT** is fitted but not scoped: 45–50 A through a 30 A clamp puts ~4 V on a 3.6 V-max pin
  before the Schottkys, and nobody has watched a worst-case motor start on a scope.
- **The 315 MHz module's supply** — 5 V or 12 V. V1 lets JP1 pick either; bench both.
- **A smaller clamp for small tools.** An SCT-013-005 is six times the resolution.
  Nothing needs it yet.
- **Product spin:** drop the XIAO for the ESP32-C5-WROOM-1 module, and plan FCC.
  Not this round.
