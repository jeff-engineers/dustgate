# Tool sensing & collector switching — design doc

**Status:** decided 2026-09-03, **nothing bench-tested.** Sensor plugs ordered;
everything else is reasoning from datasheets and product pages, not from a meter
in this shop. Revised the same day, twice: the collector's switching hardware was
going to be a contactor we built and isn't (§4.2, §13), and the remote's band,
encoder, address and button count are now known rather than assumed (§4.2).
**Covers:** how the firmware learns a tool is running, and how the collector is
switched, now that a smart plug can do neither for a large machine.
**Companion docs:** [`outlet-discovery-provisioning.md`](outlet-discovery-provisioning.md)
(the Shelly discovery/claim path this extends), [`shop-schema-rfc.md`](shop-schema-rfc.md)
§7 (collector health), [`architecture-rfc.md`](architecture-rfc.md) (the node contract).
Any API/model surface added here goes through the canonical-model + conformance
discipline in [`../shared/device-model/README.md`](../shared/device-model/README.md).

---

## 1. Motivation

A 1HP 120V dust collector trips the overpower protection on a Shelly Plus Plug
US. That is not an edge case — it is the *first* real machine this system was
pointed at, and every other large tool in the shop will do the same. The plug
has been the only sensor and the only switch since v1, so a plug that cannot
hold a motor invalidates both halves of the design at once.

## 2. What actually broke

Worth being precise, because the obvious diagnosis leads to the wrong fix.

The Plus Plug US is rated 15A / 1800W. **MEASURED 2026-09-09 with an inrush
clamp meter: 10 A running, ~50 A inrush.** (This section said "roughly 13A" until
then — a guess, and pessimistic.)

So 10 A continuous is comfortably inside the plug's rating, not the marginal
figure the guess implied. What trips it is **inrush**: 50 A is 5× the running
current and **3.3× the plug's entire rating**, for a few hundred milliseconds at
startup. That is locked-rotor current, and it is textbook for an induction
motor.

The overpower protection that catches this exists to protect **the relay
contacts**. Contacts are the fragile part of a smart plug: they arc on make,
they erode, and making into locked-rotor current welds them. The trip is the
device defending its own weakest component.

Two consequences follow, and both are load-bearing for everything below:

- **Disabling the protection in the app is not a fix.** It silences the
  complaint and leaves the contacts making into inrush anyway. It trades a
  visible failure for a slow invisible one.
- **A bigger plug is not the fix either.** Any relay in the sense path has the
  same problem in a larger size. The relay is the problem, not its rating.

## 3. The central decision: sensing is not switching

A tool has its own switch. DustGate never needed to turn a tool on — it only
ever needed to know *"is this drawing power."* The collector is the opposite: it
must actually be commanded, and nobody needs to sense it to decide routing.

**And for a tool, not switching is a SAFETY rule, not a convenience** (added
2026-09-09; the paragraph above was the whole argument until then, and it is the
weak version). A tool switched off at the OUTLET with its own switch left ON is
armed. Energise that outlet — remotely, on a schedule, by tapping the wrong row
in an app — and a table saw spins up with nobody's hand on it, possibly mid
blade-change. No-volt-release switches and magnetic starters exist precisely to
prevent that, and a switchable smart outlet hands it back.

So the preference for a no-relay plug is not only about inrush and cost. **It
structurally cannot start a tool** — not "will not", cannot — and that is the
property worth buying. It also cannot weld its contacts shut on a stalled tool
drawing locked-rotor current continuously, which is the failure inrush ratings
do not cover.

These were fused only because a smart plug happened to do both. Separating them
costs nothing and dissolves the problem:

| Role | Needs | Relay in the load path? |
|---|---|---|
| Tool | sense only | **no** |
| Collector | switch, plus confirmation it started | yes — but not one we own |

The canonical model already anticipated this. A tool carries `sensor.outlet`
and a collector carries `control.outlet`
([`topology.js:325`](../shared/device-model/topology.js), read by `outletFor()`
at `:333`). The split described here needs **no schema change** — only a second
kind of thing that can sit in `sensor`.

## 4. Hardware

### 4.1 Tools

| Tool | Device | ~Cost | Why |
|---|---|---|---|
| Small, corded (<10A) | Shelly Plus Plug US | $25 | Unchanged. Works today, already discovered and claimed. |
| Large, corded 120V | **Athom "No Relay Power Monitoring US Plug", Tasmota build** | **$13** | Pass-through only. 16A, 100–240V, HLW8032 metering, ESP8285. No contacts, so nothing to arc, weld, or trip. |
| 240V, or hardwired | **Split-core CT into our own ESP32, at the tool** | ~$15 | See §5. |

**Take the Tasmota build, not the ESPHome one.** Tasmota answers
`GET /cm?cmnd=Status%208` with plain JSON (`StatusSNS.ENERGY.Power`), which the
firmware can poll with the same machinery it already points at Shelly. ESPHome's
native API is protobuf and would have to be hand-rolled on the ESP32 for no gain.

Known limits of the Athom plug, stated plainly:

- **16A is its global rating; the US body is a NEMA 5-15**, a 15A device. A 13A
  continuous draw is inside spec and will run warm. This is not a licence to
  hang a 3HP collector off one.
- **Cord-connected only** — no help for a hardwired machine.
- **Certification is an open question.** Athom is a small vendor. Fine for this
  shop; a real question if DustGate ever *recommends* hardware to strangers
  running 13A through it continuously.

### 4.2 The collector — the switch already exists

**The contactor build described in the first draft of this document is not
happening, and that is the most valuable outcome of the day.** A commercial dust
collector remote is exactly the object we were about to fabricate: a mains-rated
relay in a listed enclosure, with a wireless input and someone else owning the
liability. There is already one in the shop.

[Rockler Dust Collector Remote Switch](https://www.rockler.com/rockler-dust-collector-remote-switch):

| | |
|---|---|
| Rating | **15A, 110V, up to 1.5HP** — measured collector: 10 A running, 50 A inrush, so it fits with room |
| Install | Plug-in pass-through, 6' grounded cord |
| Remote | 12V "23A" battery, ~50 ft, through walls |
| Pairing | **8-position DIP switch** in both fob and receiver |

This deletes roughly $105 of parts and an afternoon of mains wiring.

**It is RF, not IR.** Shop dust collector remotes are near-universally
315/433MHz; the IR assumption that opened this line of thinking was wrong.
Convenient, since RF needs no line of sight to a receiver bolted behind a
collector.

#### Driving it

Eight DIP positions in *both* halves means a fixed-code encoder. Read off the
hardware 2026-09-03, so none of this is inference any more:

| | |
|---|---|
| Band | **315 MHz** — FCC ID `VFWPD5T`, JYH CHENG ELECTRONICS, Part 15.231, grant reads 315.0–315.0 |
| Encoder | **HT12E** — 12 bits, 8 address (A0–A7) + 4 data (AD8–AD11) |
| Address DIP | rockers **1, 6, 8 on**, rest off |
| Buttons | **one** |

**KEYED ON THE BENCH 2026-09-06 — the first verified thing in this document.**
An HT12E of our own, address strapped to match, switched a lamp through the
Rockler receiver. What that settles, and the two things that were not obvious:

| | |
|---|---|
| Rosc | **1.0 MΩ at 3.3 V → ~3.5 kHz measured**, and it works. Holtek's ~3 kHz is a *reference point*, not a requirement — the HT12D's capture window is wider than the datasheet's example implies. An earlier draft of this section treated 2.4–3.6 kHz as pass/fail; it is not. |
| Data word | **`data 14`** (`0b1110`) — **AD8 low; AD9, AD10 and AD11 are all don't-cares.** A full 16-value sweep keys on every EVEN value (0,2,4,6,8,10,12,14) and nothing odd, which is exactly "bit 0 clear". One button, one pin. 14 leaves the other three open, so it is what the fob actually sends. <br><br>One guess died here properly: that the receiver toggled on VT and ignored the data bits entirely. `data 15` does nothing, so it does not. (An intermediate reading suggested AD8 **and** AD9 were both needed; the operator thinks that was a mistype, and the full sweep does not support it. Recorded only so the number is not re-derived from a half-remembered result.) |
| **TE hold** | **≥ ~500 ms. This is the one that bites.** 120 ms keyed the receiver only intermittently while the fob was rock solid. Four words is the HT12E's documented *minimum* transmission because the decoder validates by seeing the same frame more than once — so a truncated group produces **no output rather than a wrong one**, which is indistinguishable from a range problem. Default is now 400 ms; 500 is proven. |
| Antenna | **Not needed at bench range** — an unfitted module radiates plenty across a bench, which is its own trap in the other direction. Fit the 23.8 cm quarter-wave at install. |

### The encoder comes out (2026-09-06)

**An RMT-generated frame keys the receiver, with no HT12E in the circuit.** The
ESP32 clocks the waveform out of hardware straight into the transmitter's data
pin. That drops the encoder, the address DIP and the oscillator resistor from the
BOM, and — the actual prize — **makes the address a software value**, so one
board works with any receiver without a jumper to set or document.

RMT is what makes it viable. Option B was rejected earlier because bit-banging
270 µs pulses on a primary running WiFi means FreeRTOS stretches one and the
command is silently lost. RMT clocks the train out in hardware; nothing the CPU
does afterwards can disturb it.

**The tick turns out to be nearly a free parameter, and that was a surprise.** A
sweep found **85 µs to at least 400 µs all work** — a 5:1 window. The reason is
that an HT12D does *ratio* detection: each bit is 1:2 or 2:1 within itself, so
the decoder compares the two halves of a symbol and never needs to know the rate.
The 50:1 fOSC rule is an oversampling requirement, not a matched-frequency one.

That retires a whole thread of anxiety. It is why 1.0 MΩ giving 3.5 kHz instead
of Holtek's nominal 3.0 never caused trouble, and why copying the fob's resistor
would have been solving a non-problem.

**Settled values: tick 270 µs, 24 repeats.** 270 is the middle of the proven
range and is what real HT12E parts produce; 24 repeats is ~473 ms of airtime,
which lands on the 500 ms that was independently proven with the encoder. Those
two numbers agreeing from opposite directions is the useful part — **reliability
is about total airtime, not tick length.** The decoder needs to see the same code
several times, and that is the only timing constraint that ever mattered.

The receiver is genuinely decoding, not detecting carrier: `data 15` and every
odd value still fail. A receiver that toggled on any 315 MHz energy would be a
hazard, and this one is not.

**Range is solved, and more cheaply than expected (2026-09-06).** ~40 ft through
several interior walls, at **3.3 V, with nothing in the ANT pad** — near the
50 ft Rockler claims for the fob, which runs at 12 V.

So the 12 V supply and the quarter-wave wire recommended earlier are **headroom,
not requirements**, and this document previously said otherwise. The breakout
drops to **three pins: 3.3 V, GND, DATA** — one rail, no level shifting, no
antenna.

There is a case for leaving it that way rather than banking the headroom. At
315 MHz with an 8-bit fixed code, extra range is extra opportunity to key a
neighbour's gear or be keyed by theirs, and 40 ft already covers the shop.

**Still to check:** a house is a kinder RF environment than a shop. The receiver
will sit beside a large grounded steel collector, and a universal motor's brushes
are a broadband noise source. The test that counts is at the install location
with the collector running.

**Design consequence: do not block for 500 ms.** `setSwitch()` on a primary
cannot sit in a busy wait that long — it would stall the web server and the
servo update pass for half a second on every collector change. Assert TE, record
the time, release it on a later loop pass. The hold is a duration to *schedule*,
not to *wait out*, and that is a nicer shape anyway: it composes with the
read-compare-pulse loop the toggle already forces on us.

No rolling code, no pairing handshake — the fob sends the same word every time.
Two paths:

- **A — tap the fob.** Two wires across the button, driven from a GPIO through
  an optocoupler. No RF knowledge required; guaranteed to work. Leaves a coin
  cell in the control loop forever.
- **B — transmit the code ourselves.** A ~$2 315MHz module on the primary plus
  `RCSwitch`; capture the code once with the matching receiver. No fob, no
  battery, nothing soldered into a thing we might destroy.

  **HT12E is `RCSwitch` protocol 11** — `{ 270, { 36, 1 }, { 1, 2 }, { 2, 1 },
  true }`. So `setProtocol(11)` and both capture and transmit are library calls;
  this is not a reverse-engineering project. HT12E emits a 4-word burst while TE
  is held low, so keep the default repeat rather than sending once.

  **The DIP setting is a free correctness check.** The DIP grounds an address
  pin to set it, and Holtek's documented usage is "set to VSS or left open", so
  ON = 0 and rockers 1/6/8 should decode as `0,1,1,1,1,0,1,0`. A capture whose top 8 bits disagree was decoded with the
  wrong protocol, whatever else it looks like. That check is worth more than the
  capture.

  **Brute force is a real fallback, not a threat.** A word is ~20 ms and a burst
  ~80 ms, so the entire 12-bit space is 4096 x 80 ms — **under six minutes** of
  listening for the relay to click. Knowing the address cuts it to ~64 tries.
  There is no way to get stuck here.

- **C — use the same chip.** An HT12E of our own ($1, DIP-18) driving a 315MHz
  module, with its address pins strapped to match the fob. The ESP32 then does
  nothing but pull TE low for ~100 ms.

**C is preferred.** B looked best while the argument was "RCSwitch makes it
trivial", and it does — but two things outweigh that:

- **Timing.** RCSwitch bit-bangs 270µs pulses with `delayMicroseconds()`, and
  the transmitter would be **the primary** — running WiFi, the web server,
  NodeLink and Shelly polling. The WiFi stack and FreeRTOS will preempt that
  loop, and a stretched pulse is a dropped command: a well-known ESP32 failure
  mode, and an intermittent one, which is the expensive kind. With a real HT12E
  the entire timing obligation is holding a pin low, which nothing can jitter.
- **It deletes a whole unknown.** §10 lists "does protocol 11 match *this*
  HT12E's timing" as unverified. Using the same part makes the question
  meaningless. We are not reimplementing the protocol; we are using the chip
  that defines it. There is no capture step either — the receiver module drops
  to optional debugging kit.

Two things C needs that B does not:

- **The oscillator resistor.** HT12E sets its data rate from an external R
  across OSC1/OSC2, and the HT12D in the receiver has its own; the datasheet's
  recommendation is `fOSC(D) ≈ 50 × fOSC(E)`. Wrong value, silent failure.
  **Target the frequency, not the fob's resistor** — the fob runs on a 12V cell
  and fOSC depends on VDD, so copying its resistor while running at 5V lands
  ~15–20% low (see the Oscillator Frequency vs. Supply Voltage curve, datasheet
  p9). Holtek's reference point is **ROSC = 1.1 MΩ at VDD = 5V → 3 kHz**,
  which with the 50× rule is the standard 3k/150k pairing the receiver was
  almost certainly designed around. **But run the chip at 3.3V, not 5V** (next
  bullet), where the curve puts **1.0 MΩ** at roughly that same 3 kHz — and
  1.0 M is E12, so it is in any kit. That is an eyeball off a small graph;
  measure fOSC on OSC2 and adjust, which is what the bench meter's frequency
  counter is for. The part is spec'd to need "only a 5% resistor", so the window
  is forgiving. Datasheet at `~/code/Datasheets/ht12e-holtek.pdf`; p9 carries
  both the curve and Holtek's HT12E application circuit, which is this build
  drawn out.
- **Supply voltage, and why it is 3.3V.** At 5V the HT12E's VIH is 0.8 × VDD =
  **4.0V**, so a C5 GPIO driving TE high at 3.3V is not a valid logic high. The
  failure looks like intermittent or continuous transmission, and the RF gets
  blamed for it. At 3.3V, VIH is 2.64V and nothing needs level shifting.
  (Open-drain works too — drive low to send, set the pin to input and let the
  internal 1.5 MΩ pull-up idle it high — but 3.3V is simpler.) The chip is rated
  2.4–12V. Power the **TX module** at 5V for range and feed it 3.3V data;
  standard wiring, and it works. The fob itself runs at 12V, which is the other
  half of why its resistor is not ours.
- **Which data pin.** One button means the fob asserts one of AD8–AD11. Trace it
  on the fob, or try all four.

Address straps want an **8-position SPST DIP switch** (DIP-16 body, 2.54 mm
pitch — the same part the fob uses), one side commoned to GND. **Closed = VSS =
0, open = 1** — note that on the HT12E these pins have *no* internal pull-up
(that is the HT12A; on the HT12E, A0–A7 and AD8–AD11 are transmission gates with
protection diodes only), but "set to VSS or left open" is Holtek's own documented
usage, so floating is by design and not sloppiness. `TE` *does* have a pull-high
(~1.5 MΩ), so it idles high and needs no external resistor — the ESP32 only ever
pulls it down. A 4-position DIP for the data pins is worth having beside it.

Keep A alive rather than treating it as a fallback: it is the only variant that
transmits from a **certified** device, which matters if this is ever more than
one shop (§11). Note the Rockler replacement fob is $29.99, so sacrificing a
spare is not the cheap move it sounds like.

If it ever needs to be one board with no through-hole part, the middle path is
the ESP32's **RMT peripheral** — hardware-generated pulse trains, immune to
preemption, no extra chip, at the cost of reimplementing HT12E's encoding after
all. That is the productised version; C is the right answer for the bench.

The collector's switching hardware is then **about $4 of parts on a board we
already have.**

#### The feedback is no longer optional

**Confirmed: one button.** Absolute state cannot be commanded, only flipped, and
drift is not cosmetic here. Under the never-dead-head rule, gates move on the
assumption that the collector is in a known state. Believing it is off while it
runs is a real hazard.

An Athom no-relay plug **upstream of the receiver box** closes the loop:

```
want ON → read watts → already running? done. → not running? pulse.
```

Toggle-only plus feedback gives absolute control. Toggle-only alone is
unusable. So the monitor moves from "nice to have" to **required**, and it is
the only thing that has to be bought for the collector.

**And that makes the sense plug load-bearing for the control path, not just for
health reporting.** With the plug offline, the firmware cannot know which way a
pulse will flip the collector, so it cannot safely send one — a fault case that
deserves handling deliberately rather than discovering. Contrast the Shelly
collector plug, where an unreachable plug degrades reporting to `'unknown'` but
leaves commanding intact.

**Upstream, not on the load side.** `COLLECTOR_SPINUP_GRACE_MS` is 4000 ms; a
plug downstream of the receiver only gets power when the collector is commanded
on, and would spend 2–5 s of that budget booting and joining WiFi before it
could report anything. Upstream it is permanently powered and still reads the
motor, because it is in series with everything below it. The receiver's own
draw plus the plug's is well under `COLLECTOR_RUNNING_W` (50 W), so idle reads
as off.

#### The ceiling

15A / 1.5HP. That covers the current collector with margin. **Above 1.5HP this
box is out and the contactor build comes back** — it is preserved in §13 rather
than deleted for exactly that reason.

## 5. The 240V / hardwired sensor

240V single-phase in the US is two hots and no neutral, with the motor in series
between them — so the same current flows in both legs and **one CT on one leg
reads the whole machine.** No summing, no second channel.

**At the tool, never at the panel.** Panel-side CTs are cheaper per circuit
(an Emporia Vue at ~$12/circuit, ESPHome-flashable for local operation) and were
considered and rejected: touching the panel at all, even non-intrusively, is an
insurance question this project will not create for its users. That is settled,
and it is not a cost decision. Clamp inside the tool's own motor wiring
compartment, or the receptacle box behind it — premises wiring stays untouched.

### 5.1 Keep the enclosure low-voltage

A NEMA 6-20/6-30 has no neutral, so there is no 120V at the tool. The tempting
move is an AC-DC module (HLK-PM01 takes 100–265VAC) inside the box. **Don't.**

Power the ESP32 from a USB brick in a nearby 120V outlet instead. A CT is
galvanically isolated by design, so the enclosure then holds a sensor lead, an
ESP32, and 5V — **nothing we build touches mains at all.** That is a materially
different object, for certification, for insurance, and for a user opening it.

### 5.2 Parts

- **SCT-013-030 specifically, not the SCT-013-000.** The -030 has its burden
  resistor and a TVS built in, so an accidentally unplugged connector cannot
  develop dangerous voltage on the secondary. The -000 can. That safety margin
  is worth more than the convenience of choosing our own burden. 30A covers up
  to roughly a 5HP 240V tool.
- Confirm the listing says **30A/1V** — the 30A/1A variants are the
  burden-less ones and defeat the point above.
- The jaw is 13 mm. Fine for a 12AWG conductor or a 14/3 cord, not for anything
  fatter.
- It ships with a 3.5 mm plug. Cut it off and use the bare leads.
- Bias network: 2× 10k from 3.3V/GND for a midpoint, 10µF to ground, CT across
  the midpoint and the ADC pin.

### 5.3 Do not fight the clipping

The C3/C5 ADC tops out near 2.5V and a 1V-RMS CT on a mid-rail bias will clip
its peaks. **It does not matter.** `DEFAULT_THRESHOLD_W` is 5 — the question is
"is this a hundred times the noise floor", not "how many watts". Sample RMS over
a couple of line cycles, learn the idle baseline at boot, trip on a multiple. No
calibration constant, no user-facing amps, no accuracy claim to defend. A
clipped waveform still reads as unambiguously on.

### 5.4 Open: does clamping the *whole cord* work?

Everything above assumes the CT goes around exactly one conductor, because hot
and neutral in an intact cord carry equal and opposite current and their fields
cancel. **That is true for measuring current and may be false for detecting
one.**

iVAC's Pro Tool Plus is a shipping product that determines whether a tool is on
"by sensing the magnetic field surrounding the cord," clamped onto an intact
cord. The residual field of an imperfectly balanced pair is evidently enough for
a threshold decision — which is all this system has ever needed.

If it holds, the whole §5 install story collapses to *clip it onto the cord and
open nothing*, and the 240V and hardwired cases stop being the awkward ones.
That is a large enough prize to test before committing to the split-core path.
**Untested.** §9 says how.

### 4.3 Direction: Shelly for collectors, Tasmota for tools (2026-09-09)

Jeff, leaning: **stop allowing Shelly on tools entirely**, keep it for collectors
where switching is the job — a small or medium collector with a relay big enough
for its inrush is a legitimate use, and §3's safety argument does not apply to a
blower the way it applies to a table saw.

The schema already encodes half of this: a `tasmota` under a collector's
`control.outlet` is a validation error, because a plug with no relay cannot
switch. **The mirror rule is `shelly` under a tool's `sensor.outlet`**, and it
can become a hard error the same way — but not yet, because every layout written
so far has Shellys on tools and there is no migration path. Deprecate first,
enforce after.

**Not before Tasmota is proven.** Jeff has one plug and wants confidence before
buying a shop's worth. What "proven" means, concretely:

| | |
|---|---|
| **Polling load** | **The one to watch.** See below — it is the risk this whole direction rests on |
| A real session | Drives a real tool for a full session without dropping out or missing a start |
| Threshold behaviour | No false trips on standby, no missed starts. The measured plug reports 26 W idle on a massager — a real tool's standby is what matters |
| Power cycle | Survives one, and reconnects to WiFi without help |
| The claim persists | `Mem1` still holds our hostname afterwards |
| Thermal | Warm, not hot, at the tool's actual current over a long cut |

#### The polling load, which §6 did not account for

**A Shelly pushes. A Tasmota cannot.** `Ws.SetConfig` points a Shelly at us and it
reports itself — that is why `readPushConfig()` is the ownership authority in the
first place. Tasmota has no equivalent, so every Tasmota is **polled at
`OUTLET_POLL_INTERVAL_MS` (500 ms), permanently**.

At `SMART_OUTLET_COUNT` = 7 that is **14 requests/second** leaving the ESP32, and
**2 req/s arriving at each ESP8285**, forever, for the life of the shop.

§6 argued for one driver rather than two and that argument still holds — but it
was about the DRIVER, and losing push changes the TRAFFIC PROFILE, which is a
different thing entirely. Unmeasured. If it does not hold up, the fix is a
slower cadence for `kind: tasmota` specifically: a tool starting is not a
500 ms-latency event, and the collector's own spin-up grace is already 4 s.

That would make the poll interval kind-dependent, which is a small change and an
honest one — the two device types have genuinely different economics.

## 6. One seam, not two: emulate Tasmota

The homemade 240V sensor **serves the same endpoint as the Athom plug** —
`GET /cm?cmnd=Status%208`, same JSON, same `StatusSNS.ENERGY.Power` field.

This is the most valuable decision in the document, because of what it avoids:

- **One new driver in the firmware, not two.**
- **No new NodeLink frame**, and specifically no new *direction* of travel in a
  protocol where nodes have only ever received already-resolved numbers.
- **No new JS↔C++ constant pair** to register in the anti-drift table.
- The DIY sensor and the $13 plug are indistinguishable to the primary,
  discovered the same way, and exercised by the same conformance path.

Cost on the sensor board: a few hundred bytes of `WebServer` handler on top of
the ADC loop.

## 7. One node per tool?

Since the CT already forces a powered, WiFi-connected box at each tool, the gate
servo is one more GPIO and a ~$2 increment. **The marginal cost runs backwards
from the intuition:** we are paying for the sensor and getting the actuator
nearly free.

It also serves an existing goal. [`architecture-rfc.md`](architecture-rfc.md) §1
wants gates "spread over multiple ESP32 nodes (no long cable runs)"; four servos
on one shared node means four three-conductor runs across the shop, against one
short servo lead per tool. And the failure mode is more coherent — a dead node
takes out one tool's gate *and* its sensing together, rather than four unrelated
tools' gates.

**But it is a deployment shape, not a schema.** Two standing counterexamples:

- **The slider.** One ST3215 node drives 4 gates on a rack serving 4 different
  tools. One node, many tools, and it is a shipping configuration.
- **A trunk gate belongs to no tool at all.**

So actuator addressing and sensor addressing stay **independent host+channel
pairs**. "One node per tool" is then simply the case where both channels land on
the same host, and nothing in the model needs to know that happened.

**It does add a direction to NodeLink** — a node pushing a reading upstream,
which none has done. That is a real protocol addition, but not a violation of
the node contract: the node ships a raw number and the **primary** judges it
against `thresholdW`. "Already-resolved numbers, never state names" survives
intact, and this is a smaller departure than the slider's local homing loop
already was.

**The cost is fleet management.** Fifteen boards to flash, provision and OTA
instead of four, which lands directly on the open question in
`productizing-open-questions` about telling boards apart as a non-technical
user. That question gets harder here, not easier.

## 8. Firmware work

| Work | Notes |
|---|---|
| `TasmotaOutlet : SmartOutlet` | `poll()` hits `/cm?cmnd=Status%208`; `setSwitch()` unsupported; new `generation()` value. Structurally simpler than `ShellyGen2Outlet`. |
| Discovery | **Built 2026-09-09, and it will usually find nothing.** The ownership half is solved (`Mem1`, §11). The mDNS half is worse than this row assumed: Tasmota does not advertise a service of its own, so discovery asks `_http._tcp` — every HTTP responder on the LAN — and keeps only hits carrying a `devicetype=tasmota` TXT key. That key makes a hit unambiguous, which is the job `_shelly._tcp` does for Shelly. **But Tasmota's mDNS needs `USE_DISCOVERY` compiled in and is not in the precompiled builds**, upstream having left it out because "mDNS generates more problems than it solves". A stock Athom plug is very likely silent. Zero hits means *none advertised*, never *none present*, and the fallback — a hand-entered IP — works today. A subnet sweep would find them regardless; it is 254 blocking HTTP requests and wants chunking across `loop()` passes, so it is not built. |
| RF collector control | A `SmartOutlet` whose `setSwitch()` **pulses a GPIO** (HT12E `TE`) rather than making an HTTP call, and whose `poll()` is **not its own** — the reading comes from the upstream Athom plug. The driver is nearly nothing; the interesting part is the pairing, which is new: a control device and a sense device describing the same collector. |
| Toggle reconciliation | The fob is a single toggle, so `setSwitch(on)` is **not idempotent** and must be *read, compare, pulse if different*. The collector's sense plug is therefore a hard dependency of its control path: **plug unreachable → refuse to command**, because the direction of a pulse is unknowable. Needs a distinct fault state; `'unknown'` currently means "we can't report", not "we can't act". |
| Sense-only capability | `setToolManual()` — and its `* 3` / `manualWattsFor()` constant pair — cannot turn on a tool with no relay. Manual override for such a tool must mean "open the gate, run the collector", not "run the tool". |
| Model | No `sensor`/`control` schema change (§3). A `generation`-like discriminator on the outlet is enough. |

## 9. Bench plan

Ordered so each test unblocks the next. The C5s and the Rockler receiver are
already on hand; the Athom plugs are ordered.

| # | Test | Needs | Answers |
|---|---|---|---|
| 1 | ~~Read the fob~~ | — | **Done 2026-09-03.** One button; HT12E; 315 MHz per FCC ID `VFWPD5T`; address DIP 1/6/8. |
| 2 | Athom holds the collector | nothing | Does a pass-through plug survive what a relay plug wouldn't? Watch case temperature after 20 min. |
| 3 | Collector ground truth | clamp meter | Actual FLA and actual inrush — the numbers §2 and §4.1 are currently guessing at. |
| 4 | RF replay | 315MHz TX/RX kit | Can the primary drive the receiver with a $2 module, on protocol 11, with the address cross-check passing? |
| 5 | Fob tap | optocoupler | Fallback if 4 is fiddly. |
| 6 | CT threshold | SCT-013-030 | Is baseline-and-multiple solid with no calibration? |
| 7 | **Whole cord vs one conductor** | CT + line splitter | §5.4. Same CT, same load, 1X loop vs intact cord, back to back. |
| 8 | Closed loop | 2 + 4 | Toggle remote + Athom feedback → absolute state. |

Parts for the above, beyond what is on hand: an **HT12E** (~$1), a **315MHz** TX
module (~$3), an 8-position and a 4-position DIP switch (~$8 as an assortment),
a 315MHz RX module (~$2, optional now that option C needs no capture),
2× SCT-013-030 (~$14), an AC line splitter with **1X and 10X** loops (~$25 — the
1X loop is the one test 7 needs), a clamp meter with a true **INRUSH** mode
(~$50), 4N35 optocouplers (~$8). **Do not buy the RXB6** that an earlier draft
of this section hedged on: it is 433-only and the band is now known.

Run the RF receiver module at **3.3V**: its data pin outputs at VCC and 5V into
a C5 GPIO costs a pin. The transmitter is the reverse — feed it 5V for range,
its data input takes 3.3V logic happily. Quarter-wave antenna at 315 MHz is
**23.8 cm** of solid core wire (not the 17.3 cm that 433 wants).

## 10. What is unverified

Everything. Specifically:

- No Athom plug has held the collector's 10 A continuously. One has been talked
  to over HTTP (§4.1) but nothing has been plugged into it.
- No CT has been clamped on anything, and §5.4 is a hypothesis with one piece of
  commercial evidence behind it.
- Nothing has been transmitted to the Rockler receiver. Band, encoder, address
  and button count are now **known** (§4.2); what remains unproven is that a $4
  transmitter actually keys it — which under option C reduces to matching the
  fob's oscillator resistor and picking the right data pin.
- The claim that a pass-through plug solves the trip is **reasoning from why the
  protection exists**, not an observation.
- Thermal behaviour of a 15A-body plug at **10 A** continuous in a dusty shop is
  exactly the kind of thing that looks fine on a bench and fails in year two.
  Less marginal than the 13 A guess made it, but still the failure that would
  take two years to find.

## 11. Open questions

- ~~**Ownership without `Ws.SetConfig`.**~~ **ANSWERED 2026-09-09: Tasmota's
  `Mem1`.** It is free text, persists across reboot, and is read and written over
  the same `/cm?cmnd=` endpoint as everything else — so it is the same shape of
  thing a Shelly's push target is: device state we set and can verify, rather
  than a name a user might edit. We write our hostname into it.

  The claim model gained the "second, weaker mode" this bullet anticipated:
  `claimOfMarker()` / `plugclaim::decideMarker()`, same four states, paired
  across both engines. **It is weaker in two specific ways and both are
  load-bearing:**

  1. **It is advisory, not enforced.** A Shelly can push to exactly one place, so
     writing that config *is* the claim; Mem1 is a note we agree to read. Two
     brains can poll the same Tasmota plug and neither will notice.
  2. **There is no `foreign` state, and cannot be.** `foreign` is visible on a
     Shelly only because Home Assistant had to write the push config to receive
     anything. HA *polling* a Tasmota writes nothing, so a plug reading
     `unclaimed` may well be in use and we cannot tell.

  The compensation: the marker is a hostname rather than a dialable address, so
  it does not go stale when DHCP moves us, and the whole `stale`/repair branch
  `claimOf()` needs has no counterpart.

  This unblocks discovery, which is still not built — the mDNS half (Tasmota
  advertises differently from Shelly) is untouched.
- **A collector described by two devices.** Control is an RF code; sense is a
  plug at a different address. The model has never had a system whose
  `control.outlet` and health reading come from different places. §8 lists it as
  work; the schema question is whether that is a `control` variant or something
  new.
- **Certification**, if hardware is ever recommended rather than merely
  supported (§4.1). The RF path adds a second flavour of this: the fob is
  certified under Part 15.231 and a bare $2 module on our board is not. A brief
  burst that replays an existing remote's word is squarely what 15.231
  contemplates and is a non-issue for one shop, but *shipping* a product with an
  uncertified 315MHz transmitter is not — that would want a pre-certified
  module, or option A (tapping a fob that is already certified), which is one
  more reason to keep A alive rather than treat it as a mere fallback.
- **Whether the DIY sensor should ever be a real NodeLink node** rather than a
  Tasmota impersonator (§6). Emulation is right for now; it may stop being right
  if sensors need anything push-shaped or anything the Tasmota JSON cannot say.
- **Whether DustGate should bring its own network** rather than live on the
  user's (raised 2026-09-09, deliberately not pursued yet). It would dissolve
  §12 rather than work around it: on a network we own, nothing is blocked, DHCP
  is ours, and mDNS either works or is unnecessary. The C5 can run AP and
  station at once, so it is not obviously infeasible. The costs are the reason
  it needs real thought rather than a paragraph — a plug on our AP has no route
  to the internet (which is fine for a sense-only plug and fatal for one wanting
  OTA updates), and the woodworker's phone would have to leave the house WiFi to
  reach the UI, which is a worse daily experience than any of the problems §12
  describes. Routing between the two interfaces is the thing that would decide
  it, and none of it is understood yet.

  The adjacent question is **whether the radio should be WiFi at all**. ESP-NOW
  is the interesting one for our own hardware, because it is the same radio with
  no AP, no DHCP and no addresses to go stale — every §12 problem gone by
  construction. It only reaches devices we build (§6's CT board), never a
  Tasmota, so it is a second path rather than a replacement. Zigbee and
  Thread/Matter are the standards-shaped answers and each want a coordinator and
  a certification story. **Explicitly parked**: the near-term job is working
  reliably on one shop's guest network, and none of this helps with that.

## 12. Staying reachable

Three questions asked together on 2026-09-09 — how a paired plug survives an IP
change, whether to push instead of poll, and whether to write our own Tasmota
firmware — turn out to be one question with one governing rule.

**The rule, and it is now a design constraint: never make a working shop depend
on something a network is allowed to block.** Multicast is the specific hazard.
mDNS is off by default on plenty of guest networks, most IoT VLANs, and a fair
number of mesh routers, and the woodworker whose shop stops working has no way
to know that is why. A feature may *use* mDNS as a fast path; nothing may
*require* it.

### 12.1 Where we lean on mDNS today

| Path | What mDNS does | If multicast is blocked |
|---|---|---|
| Shelly discovery (`_shelly._tcp`) | the only way to find them | wizard finds nothing → hand-typed IP |
| Tasmota discovery (`_http._tcp` + `devicetype`) | known to usually fail anyway | covered — the IP sweep finds them |
| Node discovery (`_dustgate._tcp`) | the only way to find nodes | hand-typed IP |
| `RemoteActuatorBus::resolveAndDial()` | **runtime**, on every reconnect | a node paired by *hostname* never comes back; one paired by IP is fine (`_hostIsIp`) |
| `ShellyGen2Outlet::reresolve()` | DHCP-change recovery | stale IP is unreachable, permanently |

Every *discovery* path degrades to a hand-typed IP, which is tedious but not
broken. Both *recovery* paths degrade to nothing — and recovery is the one that
fails months later, in a shop nobody is watching, on a router reboot.

**SETTLED ON HARDWARE, 2026-09-09.** Two questions that had been reasoned about
rather than tested, both answered against the Athom plug and the shop network:

- **Tasmota's mDNS is genuinely absent, not merely switched off.**
  `SetOption55 1` answers `{"SetOption55":"ON"}` — which looks like success and
  is not. Tasmota stores and echoes any SetOption in the valid range whether or
  not compiled code reads it, so **a set bit is not a feature**. After a
  confirmed restart (`UptimeSec` 131) the plug appears in neither
  `dns-sd -B _http._tcp local.` nor `_arduino._tcp`. `USE_DISCOVERY` is not in
  the build, there is no command that adds it, and the subnet sweep is therefore
  the only first-contact route. This is also why `TasmotaOutlet::reresolve()`
  stays deleted: there is no name to resolve.

- **Multicast is NOT blocked on this network.** The same browse showed
  `dustgate` immediately. Shelly discovery and `ShellyGen2Outlet::reresolve()`
  are on solid ground here — which is the opposite of what the contaminated test
  below suggested, and the reason it is kept as a warning.

**A NETWORK TEST THAT PROVED NOTHING, recorded so it is not repeated.** On
2026-09-09 `dustgate.local` failed to resolve from the Mac, and that was read as
evidence multicast was blocked on the shop network. The board was simply powered
off. There is still **no evidence either way** about mDNS on that network; the
valid test is `mdnsprobe` on a running ESP32, which asks the querier that
actually matters rather than the Mac's.

### 12.2 Tasmota recovery: re-sweep, identify by Mem1

`TasmotaOutlet::reresolve()` was a verbatim copy of the Shelly one and could
never have worked: Tasmota does not advertise over mDNS in a stock build, and
`_host` is never populated for a Tasmota because the sweep finds these by
address. It returned false on its first line every time. **Removed 2026-09-09** —
a dead branch that reads like recovery is worse than no recovery, because it
answers "what happens when the address changes?" wrongly and stops anyone
asking again.

The replacement uses what pairing already writes. **`Mem1` is a durable
identifier**: it holds our hostname, it survives reboots and lease changes, and
it is exactly the thing an IP is not. So a plug that goes missing is found again
by sweeping the subnet and reading `Mem1` for the one that says it is ours.

This is a strictly better answer than the alternatives, and it is better
*because of* §12's rule:

| Option | Why not |
|---|---|
| DHCP reservation | Router-side, per-user, and not something the product can do. The right advice for an advanced user; not a mechanism. |
| Static IP on the plug at pairing (`IPAddress1`) | We would be choosing an address on someone else's subnet with no way to know what is free. A collision is worse than a lease change, and harder to explain. |
| mDNS | The thing this section exists to stop relying on. |

Shape of the work, none of it built:

- It is **~60s of blocking HTTP over 254 addresses**, so it must never run from
  the poll task on a failed read. That is why `poll()` now simply fails.
- It is a **deliberate, user-visible action** with a progress report — the same
  machinery the layout-page discovery sweep needs (see the two-phase scan
  mockup), which is the argument for building that first and getting this second
  nearly free.
- A sweep may find a plug whose `Mem1` names a *different* brain. That is
  `plugclaim::decideMarker()`'s existing job and it needs no new policy.
- **Unresolved:** two plugs claiming the same owner name, after a plug is
  swapped without being released. The claim identifies the *brain*, not the
  *tool*, so it cannot tell two of our own plugs apart. Writing `Mem2` with the
  tool id at pairing time would fix it and costs nothing — worth doing when this
  is built.

### 12.3 Push instead of poll — yes, and as configuration

The polling load recorded in §4.3 is real: every Tasmota is polled at
`OUTLET_POLL_INTERVAL_MS` (500 ms), permanently, and at seven plugs that is 14
requests/second the primary is making forever.

Tasmota can push, with no custom firmware, via a rule:

```
Rule1 ON Energy#Power DO WebSend [<primary-ip>] /push?w=%value% ENDON
Rule1 1
```

`WebSend` sends a Tasmota *command* by default and does a raw HTTP GET when the
argument starts with `/`. **`WebQuery` is the better fit** — same idea, shaped
for non-Tasmota targets, which is what we are. Either way this is *setup*, not
code: it is written once at pairing over the same `/cm?cmnd=` endpoint that
already writes `Mem1`, `PowerOnState` and `PowerLock`.

**Push the wattage, not the verdict.** The tempting alternative is to put the
threshold in the plug so it only calls when a tool actually starts. Rejected:
`thresholdW` is a topology value the user edits, so every edit would mean
rewriting a device's rule, and the judgement would move out of the primary.
`TopologyRuntime`'s contract is that a plug *reports* and never *judges*, and
that is what makes one brain possible. The plug sends a number; we decide.

(`PowerLow`/`PowerHigh` looked like the elegant version of this and are not:
they emit **MQTT telemetry only** and raise no rule trigger, so with no broker
they do nothing. Corrected here because the first version of this section said
otherwise.)

What it costs:

- **Latency.** `Energy#Power` fires roughly every 2s, against a 500ms poll.
  Acceptable — a gate takes ~2s to move and the collector's spin-up grace is 4s,
  so the sensing is no longer the slow part of the chain.
- **Reliability.** Tasmota's own docs are blunt that `WebSend` error handling is
  poor and MQTT is the reliable transport. A dropped push is a tool that looks
  idle while it runs, which is the wrong way to fail.

So push does not *replace* polling — it lowers its rate. Keep a slow poll as the
floor (a few seconds), let push carry the fast path, and the 14 req/s becomes a
fraction of that without inventing a new failure mode. **And it changes the IP
problem's shape:** a plug that calls us does not need us to know where it is, so
a push arriving from an unexpected address is itself a re-resolve — free, if the
handler reads `Mem1` to confirm who it is before believing it.

### 12.4 Custom Tasmota firmware — no, and the reason is not difficulty

It is entirely possible. Athom's plugs are not locked down: they are documented
as compatible with the official Tasmota OTA server, people have flashed custom
builds (mensi.ch documents the process), and the constraints are ordinary —
`tasmota-minimal.bin` first if flash space is tight, DOUT flash mode on the
ESP8285.

Rejected anyway. The two things custom firmware would buy are **mDNS**
(`USE_DISCOVERY`, confirmed absent from the Athom build on 2026-09-09 — see
§12.1) and **push** — and push is available above as configuration. That leaves mDNS, which §12's own rule says we
must not depend on, so building our own way to depend on it is backwards.

Against that: it trades Tasmota's mature OTA, WiFi reconnection, energy
calibration and setup UI for a maintenance burden on a **mains device that has
to be opened to be recovered** if a bad flash bricks it. That is a bad trade for
a feature we have decided not to rely on.

**What would reopen it:** needing something the Tasmota JSON genuinely cannot
say. §6 already contemplates the reverse direction — our own CT board
*impersonating* Tasmota — and the day emulation stops being expressive enough is
the day both questions get asked again together.

## 13. Rejected

| Option | Why not |
|---|---|
| Disable the plug's overpower protection | Hides the failure, keeps making into inrush. §2. |
| A larger smart plug / the [Ogemray 25A relay](https://us.shelly.com/products/ogemray-25a-smart-relay) | 25A/6000W is a **general-use** rating with no published motor or HP rating and no inrush spec. Probably fine at 1HP; not *rated*. And it still puts a relay we own in the load path. |
| **Building the contactor box** — Shelly 1 Mini dry contact driving a 30A definite-purpose contactor (120V coil), snubber across the coil, manual bypass toggle in parallel, in a NEMA 1 enclosure | Correct engineering, ~$105 and an afternoon of mains wiring, and **entirely redundant with a $40 listed appliance already in the shop** (§4.2). **Kept here deliberately:** it is the answer again above 1.5HP, where the Rockler box runs out. If it ever gets built, note that a contactor is not overload protection — a motor without an internal thermal cutout wants a magnetic starter instead — and that this is licensed-electrician territory in many jurisdictions. |
| Converting the collector to 240V | Halves the current and would make everything easier, but needs a 240V circuit run — panel work, which §5 rules out on the same grounds. |
| Shelly EM Gen3 + CT per tool | Works, and its contactor-control output was genuinely well-matched to the build above. But ~$30/tool, and it buys nothing the $13 Athom plug does not for the common corded case. |
| Panel-side CTs (Emporia Vue, IoTaWatt) | Cheapest per circuit and the only thing that covers hardwired tools — but it means panel work. Rejected on insurance grounds; see §5. Also ambiguous when two tools share a circuit. |
| Stick-on accelerometer / vibration sensing | The genuinely cheap idea: ~$5, no mains contact, no enclosure, no electrician, works on hardwired and 240V tools, installs by peeling a sticker. Rejected **for now** only because of cross-talk: once the collector runs the whole shop shakes, and the tool's own gate and duct are physically coupled to it. Very likely separable by magnitude and spectrum; entirely unproven. If §5.4 pans out it is probably moot, since clipping a CT to an intact cord is nearly as easy and gives a number we already know how to interpret. |
| HLK-PM01 mains supply inside the DIY sensor | Turns a low-voltage gadget into a homemade mains device for the sake of avoiding a USB brick. §5.1. |
