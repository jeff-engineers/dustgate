# Wiring — ST3215 bus servo on the bench (XIAO ESP32C5)

> ## First contact: 2026-08-26. A servo answered.
>
> `id 1 answered (status 0x00)` at **1 000 000 baud, TX on D6, RX on D7**,
> through Seeed's Bus Servo Driver Board with **no jumper fitted and the barrel
> jack powered**. That settles the pin order, the baud and the wiring for this
> board — the rest of this file is now a record rather than a guess.
>
> Then it moved. `move 2048` turned the shaft, the same session.
>
> `read` decodes correctly too, and the proof is a cross-check rather than a
> vibe: it reported **12.1 V** against a meter reading 12 V at the socket. A
> wrong byte order or a wrong register address cannot produce the right voltage
> by accident, so the register map in `ST3215Bus.h` and little-endian are both
> confirmed for this part.
>
> Metering advice below still applies to anything hand-wired; the ⚠️ logic-level
> warning is moot through this board, which buffers.
>
> Authoritative pin source: [`firmware/boards/xiao_c5.h`](../boards/xiao_c5.h)
> under `-DDUSTGATE_SERVO_BUS`. If this file disagrees with that header, the
> header is right — the build reads it.

## 0. On the bench: Seeed's Bus Servo Driver Board for XIAO

**The working configuration, as it actually ran:**

| | |
|---|---|
| Jumper on the front 2-pin header | **not fitted** (Seeed: "it's not shorted by default") |
| Barrel jack | **powered** — 12 V |
| XIAO pads | **TX = D6/GPIO11, RX = D7/GPIO12** |
| Baud | **1 000 000** |
| Servo | answered at **id 1**, status `0x00`, and moved on `move 2048` |
| `read` | `pos 439 (38 deg) speed 0 load 0 12.1V 23 C still` — the 12.1 V is the meter cross-check that confirms the register map |

**What the hour of silence before that was, we never established.** Every one of
those settings had already been tried, in both pin orders, at every baud
`sweep` knows — so nothing in this table is what fixed it. The only thing that
changed was that the XIAO came out of the socket for a loopback test and went
back in. Assume a seating or cable-contact problem, and reseat everything before
believing a silent bus.

The diagnostic ladder that came out of that hour is §5.1, and it is the part of
this file worth reading twice: it is what turns "nothing answered" into a
statement about *which half* of the bench is broken.

## 0.0 Notes that apply to either Seeed board

The XIAO sockets straight into it, the servo plugs into its 3-pin socket, and
its own jack feeds both. That removes most of §1–§3 — **read §0.1 anyway**,
because the adapter answers the half-duplex question and *raises* a power one.

- **It drives the line for you.** No series resistor, no direction pin to
  drive, and usually no echo: the firmware tolerates either (`ping` reports
  which wiring it is actually on — see §5).
- **TX is D6.** Settled on the bench, and by the Arduino core's own variant
  table for this board (`variants/XIAO_ESP32C5/pins_arduino.h`: `TX = 11,
  D6 = 11`), which is the table the build compiles against. Seeed's wiki line —
  "connect the `RX` pin on the Driver Board to the `TX` pin (D7) on your host" —
  is a typo; believing it costs an afternoon. `swap` flips the order live if you
  ever need to check again.
- **1 Mbps**, per Seeed's own example (`COMSerial.begin(1000000, SERIAL_8N1)`).
- **No mode jumper on this board** — "you don't need to modify any circuits".
  The bigger *Bus Servo Driver Board* is the one with a UART-vs-USB solder
  bridge, so check for one if that is the board on your bench.

Source: [Seeed's XIAO Bus Servo Adapter wiki](https://wiki.seeedstudio.com/xiao_bus_servo_adapter/).

### 0.1 Power, as measured (2026-08-26)

Metered on the bench, so these are facts rather than the datasheet's silence:

| Where | Reading | What it settles |
|---|---|---|
| XIAO `5V` pad, jack live | **5 V** | The adapter REGULATES the jack down for the XIAO. The destructive case — 12 V onto a 5 V pad — is off the table. |
| Servo socket power pins | **12 V** | The jack reaches the servo unregulated, at its full rail. A servo that answers on the bus but feels weak under load is therefore not a starved-rail story. |
| Adapter rail, powered from the XIAO's USB instead | **5 V** | USB can power the whole thing. It does NOT say whether there is a blocking diode — see below. |

**The one thing still open: is the `5V` pad isolated from USB VBUS?** That pad is
raw VBUS and bidirectional, so with jack and USB both live, two 5 V sources meet
across whatever sits between them. The third reading above does not answer it: a
diode oriented VBUS → `5V` pad passes current in exactly that direction, and what
it blocks is the reverse. What would answer it:

- Jack live, **USB unplugged**, meter VBUS at the USB-C connector itself. ~5 V
  means the pad backfeeds VBUS and there is no blocking diode; ~0 V means there
  is one.
- Or with USB alone, compare VBUS against the `5V` pad — a Schottky shows up as
  a ~0.3 V step.

It is 5 V against 5 V either way, which is the benign version of this. Cheap
insurance while it is unanswered: **pull the jack while flashing.**

## 1. Power, if you wire it yourself instead

The servo runs on **12 V at the servo**, its own supply, its own wires. It never
comes through the XIAO, and in particular never through the `5V` pad — same pad,
same reason as §0.1.

| Servo lead | Goes to |
|---|---|
| V+ (red) | 12 V bench supply, current-limited if it can be — a stall is ~2.7 A |
| GND (black/brown) | supply ground **and** a XIAO GND pad — see below |
| Signal (yellow/white) | D6/D7, through §3 |

**The common ground is not optional.** A single-ended TTL bus has no other
reference; without it the servo sees the signal swinging around an arbitrary
offset, and the usual symptom is a scan that finds nothing at any baud. The
adapter of §0 gets this right by construction, which is half of why it is worth
using.

## 2. ⚠️ Logic level — measure before connecting anything hand-wired

Moot with the adapter of §0, which is what its buffer is for. It applies the
moment a servo signal lead meets a XIAO pad directly.

The C5 is **not 5 V tolerant**. Feetech's bus is documented as TTL without
saying which TTL, and no source reachable from here settles it.

With the servo powered and the signal wire connected to **nothing**, meter the
signal pin against ground. It idles high (the bus rests at its supply level).

- **~3.3 V** — wire it straight through, per §3.
- **~5 V** — do not connect it to D7. A divider on RX (e.g. 10k/20k) or a
  level shifter, and re-check that TX still reaches a valid high at the servo.

## 3. Half duplex: one wire, two pins

The servo has one data line. The C5 has a TX pad and an RX pad, and tying both
straight onto that line means two push-pull drivers fighting on every reply.

The bench build's default assumption is the simple version:

```
  D6 (GPIO11, TX) ──[ 1k ]──┬── servo signal
  D7 (GPIO12, RX) ──────────┘
```

The 1k limits the current when both ends drive at once; the servo, driving
harder, wins. On this wiring we hear everything we say, and the firmware drops
the frame it recognises as its own. It does not *depend* on hearing it — the
adapter of §0 suppresses the echo and the same code works either way.

Two things that are also fine, and one that is not:

- **Seeed's XIAO Bus Servo Adapter** — §0. It does this properly, with a buffer,
  and it is what is on the bench.
- **A 74LVC1G125 with a direction pin** — what a hand-built shop install should
  have, since it protects the GPIO pad from a metre of unshielded wire beside
  charged ductwork. Needs a pin and a code change.
- **Tying TX and RX directly together with no resistor** — works right up until
  it doesn't, and what fails is the C5's pad.

## 4. Flash it

This env is the only one that defines the bus pins, so it is also the only one
where `HAS_LINEAR` is 1.

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_bus_bench -t upload
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio device monitor -e xiao_c5_bus_bench
```

Nothing moves on boot. Torque is not enabled and no register is written until
you type a command.

## 5. First contact, in order

```
scan            # every id 0..20 at 1 Mbps. A virgin servo answers at id 1.
ping            # status byte, and which half-duplex wiring you are actually on
read            # position, load, volts, temp — volts is the sanity check on §1
torque on
move 2048 200   # a slow half turn
stop            # goal := where it is; it keeps holding
torque off      # the shaft goes free
```

If `scan` finds nothing, in this order:

1. **Power.** A servo with no 12 V is silent, not noisy. `read` would have shown
   the voltage if anything were answering, which is the circular part of first
   contact — check it with the meter instead.
2. **The signal wire and the common ground** (§1, §3) — or, on the adapter, that
   the XIAO is seated the right way round in its socket.
3. **`sweep`.** Every plausible baud, both pin orders, staying on whatever
   answers. It exists because the two settings that can be wrong are
   indistinguishable from the console, and both have now been wrong once: a
   servo configured before keeps whatever rate it was given (register 6 is a
   baud *index*, not a rate), and the TX/RX order was inverted in this repo
   until the adapter arrived.
4. **`trace on`**, then `ping`. TX bytes with no RX bytes is a bus that is not
   hearing us or a servo that is not there; RX bytes that fail the checksum is
   the wrong baud, or two drivers fighting on a hand-wired line (§3).

If the numbers from `read` are nonsense but the checksum passes, try
`endian hi` — the ST/STS series is little-endian and the SCS series is big, and
parts get sold under the wrong name. **The part on this bench is little-endian**
(the default), confirmed by `read` agreeing with a meter on the supply voltage.

## 5.0 `suite` — run everything, get a baseline

```
suite
```

One command: it sets the state it depends on, exercises travel, speed, mid-move
retargeting, `stop`, repeatability and multi-turn, puts the servo back the way it
found it, and prints a PASS/FAIL line per check with a count at the end. **The
shaft turns**, so nothing should be bolted to it.

It exists because hand-typed sequences kept landing in state left by the command
before them, and every one of those produced a confident wrong conclusion —
"speed 0 means stop" (it means maximum; the servo was in mode 3 at the time),
"it stalled" (it was reversing), "it is straining to hold" (the load field was
being decoded with the wrong sign bit). A scripted run is a report you can diff
against the last build instead of a transcript two people have to interpret.

Anything added to it follows three rules, written at the top of `doSuite()`: set
the state you depend on, put the servo back, and don't count a measurement as a
check.

## 5.1 When nothing answers at all: prove the UART first

`scan` finding nothing is the least informative result this program can produce
— it is equally consistent with a dead servo, a deaf driver board, the wrong
pads, and a UART that never transmitted. `selftest` splits that:

```
selftest        # TX to RX inside the chip. No servo, no board, no wire.
selftest wire   # the same through a jumper from D6 to D7, XIAO out of its socket
blast 5         # transmit continuously, so a meter or scope can see the pin
```

- **Internal fails** → the peripheral, the baud or this program is broken, and
  nothing outside the chip is worth touching yet.
- **Internal passes, wire fails** → those two pads are not this UART. `swap`,
  try again; if it still fails the pin numbers in the board header are wrong.
- **Both pass, `scan` still silent** → the chip is fine and the fault is on the
  board or the servo: power, the jumper, the cable, the servo itself. That is
  where a meter earns its keep, and `blast 5` is what to meter against.

## 6. What this bench does not answer

Multi-turn (`mode 3`) is where the slider actually lives, and its zero does not
survive a power cycle — which is why the endstops stay on the rail and the
homing sweep of `docs/` remains the calibration path. None of that is here yet.
This program exists to prove a shaft turns when told and stops when told.
