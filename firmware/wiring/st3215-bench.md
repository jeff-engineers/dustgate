# Wiring — ST3215 bus servo on the bench (XIAO ESP32C5)

> ## Nothing here has been done yet.
>
> No bus servo has been talked to from this project. Every number below is from
> the vendor documentation or from the ESP32-C5 datasheet, and the two marked ⚠️
> are **unconfirmed and can destroy the board if they are wrong**. Meter first,
> connect second. Written 2026-08-26, the day the parts landed on the bench.
>
> Authoritative pin source: [`firmware/boards/xiao_c5.h`](../boards/xiao_c5.h)
> under `-DDUSTGATE_SERVO_BUS`. If this file disagrees with that header, the
> header is right — the build reads it.

## 1. Power, which is not the board's problem

The servo runs on **12 V at the servo**, its own supply, its own wires. It never
comes through the XIAO, and in particular never through the `5V` pad — that pad
is raw VBUS, bidirectional, and a bench supply against a plugged-in USB port is
two supplies shorted together.

| Servo lead | Goes to |
|---|---|
| V+ (red) | 12 V bench supply, current-limited if it can be — a stall is ~2.7 A |
| GND (black/brown) | supply ground **and** a XIAO GND pad — see below |
| Signal (yellow/white) | D6/D7, through §2 |

**The common ground is not optional.** A single-ended TTL bus has no other
reference; without it the servo sees the signal swinging around an arbitrary
offset, and the usual symptom is a scan that finds nothing at any baud.

## 2. ⚠️ Logic level — measure before connecting

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
harder, wins. This is the arrangement the firmware is written for — it hears its
own transmission echoed back on RX and drains it (`ST3215Bus::drainEcho`).

Two things that are also fine, and one that is not:

- **Seeed's XIAO Bus Servo Adapter** — it does this properly, with a buffer. Use
  it if it is on the bench; the firmware does not care, the echo drain is
  harmless either way. ⚠️ Unconfirmed whether its 5–12 V jack regulates down to
  the servo socket — meter the socket before trusting it with a 12 V servo.
- **A 74LVC1G125 with a direction pin** — what a shop install should have, since
  it protects the GPIO pad from a metre of unshielded wire beside charged
  ductwork. Needs a pin and a code change; not needed for first contact.
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
ping            # again, at the target id, with its status byte spelled out
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
2. **The signal wire and the common ground** (§1, §3).
3. **The baud.** The factory rate is 1 000 000, but a servo that has been
   configured before keeps whatever it was given: `baud 115200`, `scan`, then
   `baud 500000`, `scan`. Register 6 is a baud *index*, not a rate.
4. **`trace on`**, then `ping`. TX bytes with no RX bytes is a bus that is not
   hearing us or a servo that is not there; RX bytes that fail the checksum is
   the wrong baud, or the two drivers of §3 fighting.

If the numbers from `read` are nonsense but the checksum passes, try
`endian hi` — the ST/STS series is little-endian and the SCS series is big, and
parts get sold under the wrong name. Everything below `move` will start working
the moment that is right.

## 6. What this bench does not answer

Multi-turn (`mode 3`) is where the slider actually lives, and its zero does not
survive a power cycle — which is why the endstops stay on the rail and the
homing sweep of `docs/` remains the calibration path. None of that is here yet.
This program exists to prove a shaft turns when told and stops when told.
