# Wiring — HT12E injection bench (315 MHz → Rockler DC switch)

**WORKING AS OF 2026-09-06.** An HT12E on a breadboard keyed the Rockler
receiver and switched a lamp. The three numbers that took finding:

| | |
|---|---|
| **TE hold** | **≥ ~500 ms.** 120 ms keyed it only intermittently. A short burst gives NO output, not a wrong one — it reads exactly like a range problem. This is the one that costs a day. |
| fOSC | **~3.5 kHz** with 1.0 MΩ at 3.3 V, and that works. Holtek's ~3 kHz is a reference, not a target. |
| Data word | **`data 14`** — AD8 low, the other three don't-care. Every even value keys it, nothing odd. One button, one pin. |

**And the encoder is optional.** An RMT-generated frame from the ESP32 keys the
receiver with no HT12E in the circuit at all — move the TX module's DATA jumper
from HT12E pin 17 to **D4** and use `rmt`. A tick sweep found **85–400+ µs all
work**, because the HT12D does ratio detection rather than rate matching, so the
tick is nearly a free parameter. Settled at **270 µs, 24 repeats** (~473 ms
airtime, matching the proven 500 ms hold). The address becomes a software value.

No antenna was fitted for any of it.

**What it is for.** Keying the Rockler dust collector remote's receiver from an
ESP32, by generating the same HT12E code its fob does. Background and the
decision to use the encoder chip rather than bit-bang the waveform:
[`../../docs/tool-sensing-rfc.md`](../../docs/tool-sensing-rfc.md) §4.2.

**Firmware:** [`../bench/ht12e_bench.cpp`](../bench/ht12e_bench.cpp), env
`xiao_c5_ht12e_bench`. Nothing transmits until you type a command.

---

## 1. What is already known

Read off the hardware, so none of this is inference:

| | |
|---|---|
| Band | **315 MHz** — FCC ID `VFWPD5T`, JYH CHENG, Part 15.231 |
| Encoder | **HT12E** — 12 bits, 8 address (A0–A7) + 4 data (AD8–AD11) |
| Address | DIP rockers **1, 6, 8 on**, rest off |
| Buttons | **one** |

**One unknown: which of AD8–AD11 the button asserts.** That is what `scan` is
for — four candidates, one of them switches the collector.

## 2. Parts

| | |
|---|---|
| HT12E, DIP-18 | The encoder. Datasheet at `~/code/Datasheets/ht12e-holtek.pdf` |
| 315 MHz TX module | **5 V on the bench, 12 V for a real install** — see below. Its data input takes 3.3 V logic either way |
| 8-position SPST DIP switch | Address. DIP-16 body, 2.54 mm |
| 1.0 MΩ resistor | Rosc. See §5 |
| XIAO ESP32C5 | Any spare one |

Optional stage 2 (§6): HT12D + 315 MHz RX module + 33 kΩ.

## 3. HT12E pinout, and how little of it you need

**Eight of the eighteen pins are left open.** Five of the address pins, because
address 1/6/8 means only three are grounded, and three of the four data pins,
because the sweep proved only AD8 matters.

```
            ┌───────∪───────┐
  A0    1 ──┤               ├── 18  VDD    3V3
  A1    2   ┤               ├── 17  DOUT   → TX module DATA
  A2    3   ┤               ├─┐ 16  OSC1
  A3    4   ┤    HT12E      ├─┴ 15  OSC2   1.0 MΩ across 15–16
  A4    5   ┤               ├── 14  TE     ← GPIO, pull LOW ≥500 ms
  A5    6 ──┤               ├   13  AD11
  A6    7   ┤               ├   12  AD10
  A7    8 ──┤               ├   11  AD9
  VSS   9 ──┤               ├── 10  AD8    ← the button
            └───────────────┘
              1,6,8,9,10 → GND      no stub drawn = leave OPEN
```

| Pin | | Connect to | Why |
|---|---|---|---|
| 18 | VDD | **3V3** | Not 5 V — §5 |
| 9 | VSS | **GND** | |
| 1–8 | A0–A7 | **8-position DIP to GND** | The address, and **the only thing that varies per device** — every receiver has its own code. This one: rockers **1, 6, 8 closed**, rest open. Closed = 0, open = 1, and open is genuinely open (no internal pull-up; that is the HT12A) |
| 10 | AD8 | **GND** | The button. This is the only data pin that does anything |
| 11, 12, 13 | AD9–AD11 | **open** | Don't-cares. Every even data value keys the receiver |
| 14 | TE | **GPIO** | Pull LOW to transmit, hold ≥500 ms |
| 15, 16 | OSC2, OSC1 | **1.0 MΩ across them** | ~3.5 kHz at 3.3 V |
| 17 | DOUT | **TX module DATA** | |

So a permanent build is **an 8-position DIP, two wires to ground (VSS and AD8),
one to 3V3, one GPIO, one to the transmitter, and a resistor.** Only one ESP32
pad is spent — TE — which is what makes this fit on a primary with two pads free.

### As a breakout: four pins

Put the HT12E, the DIP, the resistor, the TX module and the antenna on their own
little board and the host has to supply exactly four things:

| | |
|---|---|
| **3V3** | HT12E VDD, and the TX module too — 3.3 V proved enough for 40 ft through walls |
| **GND** | common |
| **TE** | the one signal |

(Drop the HT12E for the RMT path and it is **3.3 V, GND, DATA** — three pins.)

**Both rails are needed, and 3V3 cannot be dropped.** VIH on the HT12E is
0.8 × VDD, so at 5 V it wants 4.0 V to read TE high and no 3.3 V host can produce
that. Open-drain nearly rescues it — the firmware never drives TE high, it
releases to high-Z and lets the internal 1.5 MΩ pull-up idle it — but at 5 V VDD
that pull-up parks TE at 5 V, into a C5 pin that is not 5 V tolerant. A 3.3 V
regulator on the breakout would get it to three pins; one extra wire is cheaper
than an active part.

**Give the transmitter 12 V, not 5 V.** The fob runs from a 12 V "23A" cell —
the whole thing, encoder and transmitter — and the fob is what actually achieves
Rockler's 50 ft through walls. These modules take 3.5–12 V and their output power
goes up with supply, so 12 V is the reference, not an optimisation. This shop
already has a 12 V rail for the bin sensor and the collector lamps.

**The two supplies are independent, which is why you can have both.** The
ENCODER's supply sets the data rate (fOSC against VDD, §5) and has nothing to do
with range; the TRANSMITTER's supply sets radiated power and has nothing to do
with the data rate. So the HT12E stays at 3.3 V for GPIO compatibility while the
module gets 12 V for range. Do not run the HT12E at 12 V to match the fob — VIH
would become 9.6 V and TE would be undrivable.

⚠️ **Verify at the bench:** that the HT12E's 3.3 V DOUT still keys the module's
data input when the module is on 12 V. It is normally a transistor base behind a
series resistor and 3.3 V usually drives it fine, but that is a habit, not a
datasheet reading.

**Address on a DIP, data on a strap, and the split is not arbitrary.** The
address is the per-device setting: another receiver has another code, and a DIP
is how one board design works with any of them without unsoldering. AD8 is not
per-device — it is which pin the HT12E's one button happens to be, the same on
every unit of this remote — so it is a wire to ground.

### What the bench rig adds

Two things, both of which come out once the answer is known:

- **AD9/AD10/AD11 to D8/D9/D10** — so `scan` and `sweep` could search for the
  button pin in software. That search is done; a permanent build leaves all three
  open and never touches them again.
- **OSC2 (pin 15) to D0**, so `osc` can measure the oscillator. Sense OSC2, never
  OSC1 — see §5.

## 4. The wiring

**This is the rig that worked on 2026-09-06**, not a proposal. Values in the
right-hand column are what it was actually running.

```
  3V3 ──┬─────────────── HT12E pin 18 (VDD)     3.3V, not 5V — see §5
        │
        │   ┌── 5V ───── TX module VCC          bench. 12V for real range — §3
        │   │
        │   └─ HT12E 17 (DOUT) ── TX module DATA
        │                          TX module GND ── GND
        │                          TX module ANT ── EMPTY (nothing fitted; §5a)
        │
   [1.0 MΩ] across HT12E 15 ── 16               measured ~3.5 kHz, works
        │
  GND ──┴─────────────── HT12E pin 9 (VSS) ── XIAO GND
```

**Address — the DIP switch, never the ESP32.** Eight independent SPST switches:
one side of all eight commoned to **GND**, the other side to HT12E pins 1–8.
This is the one part of the wiring that changes per receiver.

| Rocker | HT12E pin | Set to | = |
|---|---|---|---|
| 1 | 1 (A0) | **on** (closed → GND) | 0 |
| 2 | 2 (A1) | off (open) | 1 |
| 3 | 3 (A2) | off | 1 |
| 4 | 4 (A3) | off | 1 |
| 5 | 5 (A4) | off | 1 |
| 6 | 6 (A5) | **on** | 0 |
| 7 | 7 (A6) | off | 1 |
| 8 | 8 (A7) | **on** | 0 |

Closed = grounded = **0**. Open = **1** — and open is genuinely open, because
HT12E address pins have no internal pull-up (that is the HT12A). There is no
common bus inside a DIP switch; you jumper one row to GND yourself.

**Data and TE — to the XIAO:**

| XIAO pad | GPIO | HT12E pin | | |
|---|---|---|---|---|
| D1 | 0 | 14 | **TE** — pulled low to transmit | **hold ≥500 ms** |
| D7 | 12 | 10 | **AD8** | **the button.** Pull LOW to key |
| D8 | 8 | 11 | AD9 | don't-care |
| D9 | 9 | 12 | AD10 | don't-care |
| D10 | 10 | 13 | AD11 | don't-care |
| D0 | 1 | 15 | OSC2 — sense only, for `osc` | |

**D3 is deliberately unused.** It is GPIO7, a strapping pin.

The last three rows are **bench-only** — they exist so `scan` could search for the
button pin in software, and that search is finished (§3). A permanent build wires
TE and straps AD8; see §3's table.

## 5. Rosc — 1.0 MΩ, and why not the fob's

The HT12D in the receiver only latches if the incoming bit rate is near its own
oscillator ÷ 50. Holtek's reference point is **1.1 MΩ at 5 V → 3 kHz**.

**Run the HT12E at 3.3 V**, where the curve on datasheet p9 puts **1.0 MΩ** at
about the same 3 kHz — and 1.0 M is an E12 value, so it is in any kit.

Two reasons not to copy the resistor off the fob:

1. **fOSC depends on VDD.** The fob runs on a 12 V cell. The same resistor at
   3.3 V lands 15–20% low. Match the *frequency*, not the part.
2. **At 5 V the HT12E's VIH is 0.8 × VDD = 4.0 V**, so a 3.3 V GPIO driving TE
   high is not a valid logic high. At 3.3 V, VIH is 2.64 V and nothing needs
   level shifting. (The firmware releases TE to high-Z rather than driving it
   high, so 5 V would work too — but there is no reason to take the risk.)

**Sense OSC2 (pin 15), not OSC1 (pin 16).** OSC1 is the oscillator input — the
high-impedance RC node — so a jumper and a GPIO there add capacitance straight
onto the timing network and shift the frequency you are trying to read. OSC2 is
the buffered output and barely loads. Same reason a scope probe goes there.

`osc` measures it. Target 2400–3600 Hz. Even on OSC2 a wire pulls the reading
down slightly; treat a few percent as measurement, not error.

## 5a. The antenna — leave it OFF for the bench

Two different things on these boards get called "the coil", and they are not the
same part:

- a **tank or matching inductor**, part of the oscillator or output stage — sets
  or matches the frequency, and is not an antenna;
- a **loop antenna**, a trace around the PCB perimeter, which is what many key
  fobs actually use.

Look for a pad or hole marked **`ANT`** or **`A`**, usually at a corner. That is
the antenna connection, and it is separate from the coil. An empty one means the
coil you are looking at is doing the other job.

**On the bench, unconnected is fine and is the better choice.** A 315 MHz
transmitter with no antenna still radiates plenty across a few feet — it couples
through the board and the jumper wires. (The trap runs the other way: cheap
315/433 rigs work at arm's length with nothing fitted, then fail at three metres,
and the code gets blamed.)

**Deliberately weak matters here**, because `sweep` transmits sixteen different
data words at your address. Eight address bits is not many, 315 MHz is shared
with a lot of garage-door and gate hardware, and a neighbour's receiver is not
yours to key. Keep the bench range short and the question stays between you and
your own collector.

**As it turns out, you may never need to.** Measured 2026-09-06: **~40 ft
through several interior walls at 3.3 V with nothing in the ANT pad** — close to
the 50 ft Rockler claims for the fob, which runs from a 12 V cell. The 12 V
supply and the quarter-wave wire are headroom, not requirements.

Arguably leave it. Extra range at 315 MHz with an 8-bit fixed code is extra
chance of keying a neighbour's gear, and 40 ft already covers a shop.

If you do want it: 23.8 cm of solid core wire (quarter wave) in the ANT pad. And
note that if the module's coil is a loading coil forming a complete antenna
system, a wire may *detune* it — measure, fit, measure again.

## 6. Optional stage 2 — a receiver of your own

**Not needed in the end** (2026-09-06): the Rockler keyed on the first `scan`, so
the transmitter never had to be proved separately. Kept because it is the right
move if a rebuild ever goes quiet, and because it answers a question the Rockler
cannot: *is my transmitter emitting the right code at all*, as distinct from
*does the receiver like it*.

HT12D + 315 MHz RX module, address DIP set **identically** (1, 6, 8 on), Rosc
**33 kΩ** — roughly 50× the encoder, which measured ~3.5 kHz here. Wire the
HT12D's **VT** (valid transmission, pin 17) to **D6**.

VT goes high only on a valid, address-matched frame. `tx` reports it inline:

```
  data 14, TE low 500ms ... sent, HT12D decoded it
```

Address and waveform both confirmed, with the Rockler still unplugged.

## 7. Bring-up order

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_ht12e_bench -t upload
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio device monitor -e xiao_c5_ht12e_bench
```

**This was run on 2026-09-06 and it worked.** Kept as the order to repeat on a
rebuild, with what each step actually produced.

0. **No antenna yet** (§5a). Short range is what you want while sweeping codes.
1. **`osc`** — before anything radiates. Wrong here means nothing else can work.
   Sense pin 15, OSC2. **Measured ~3.5 kHz**, not the ~3 kHz the datasheet's
   example implies, and it works — so that band is a sanity check, not a gate.
2. **Stage 2 if wired: `tx`** — does the HT12D decode it? Address and waveform
   confirmed without the collector in the room.
3. **Plug the Rockler receiver in, with a LAMP in its outlet** — not the dust
   collector. Nobody has a collector at their bench, and a lamp is the better
   indicator anyway: instant, unambiguous, no spin-up to misread, and no noise.
   Any table lamp or work light; the receiver is a 15 A pass-through and does not
   care what the load is.

   **`sweep` in particular needs the lamp.** Sixteen patterns two seconds apart
   is sixteen motor starts — locked-rotor inrush every time, into relay contacts,
   for no reason. And it would not even answer the question: a 1HP blower takes
   longer than two seconds to spin up and longer still to coast down, so the
   results would smear across patterns and you could not tell which one fired it.
   A lamp makes `sweep` both harmless and legible.
   (You can also just listen for the relay click with nothing plugged in — but a
   lamp is readable across a room and does not need the shop quiet.)

   Then **`scan`** — four patterns, two seconds apart. The one that switches the
   lamp is the answer. **It was the first one: AD8, `data 14`.**
4. **`hold`** is the setting that decides whether any of this is reliable. 120 ms
   keyed the receiver only intermittently; **500 ms is solid** and is now the
   default. A short burst produces no output rather than a wrong one, so it looks
   exactly like a range problem — do not go chasing the antenna first.
5. Write the winning value into `docs/tool-sensing-rfc.md` §4.2.
6. Only then move the receiver to the collector and fit the antenna (§5a), and
   confirm range from where it will actually live. **Still outstanding.**

Lamp not switching after `scan`? Raise `hold` first — that was the answer here.
Then `sweep` (all 16; the full set is what proved only AD8 matters), then
re-check `osc` and the address DIP against §3. An address one rocker out fails
silently and looks exactly like a dead transmitter.

## 8. Console

| | |
|---|---|
| `tx` | one burst with the current data word |
| `data <0-15>` | AD8..AD11, bit 0 = AD8. **Defaults to 14**, the answer. 15 = all open = idle, and does nothing |
| `scan` | the four one-button patterns, 2 s apart |
| `sweep` | all 16, 2 s apart |
| `hold <ms>` | TE low duration. **Default 500** — measured, not derived |
| `osc` | measure OUR fOSC on OSC2 |
| `oscf` | measure the FOB's fOSC — clip D0 to its pin 15 and hold the button |
| `vt` | HT12D valid-transmission state (stage 2 only) |

Any keypress stops a `scan` or `sweep` mid-run.

## 9. What is settled, and what is not

**Settled on the bench 2026-09-06** — an HT12E on a breadboard switched a lamp
through the Rockler receiver, repeatably:

- the address (1/6/8), the data word (**14**, AD8 only), `hold` (**≥500 ms**),
  and that **1.0 MΩ at 3.3 V → ~3.5 kHz** is inside the HT12D's window.

**Not settled:**

- **Range, and the antenna.** Everything above was done at bench distance with
  **nothing in the ANT pad**. The 23.8 cm quarter-wave and a real check from
  where the receiver will live are both outstanding (§5a).
- **The fob's own fOSC** was never measured (`oscf` exists for it). Ours works,
  so it is curiosity rather than a gap — but if a rebuild ever goes quiet, it is
  the number to compare against.
- **Part 15.231.** The fob is certified; a bare module on a breadboard is not.
  A brief burst replaying an existing remote's word is squarely what that rule
  contemplates and is a non-issue for one shop — but it is not a licence to
  ship, which is why tapping a certified fob stays a live option in §4.2.
