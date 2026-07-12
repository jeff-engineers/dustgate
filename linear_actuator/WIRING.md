# Wiring Reference — DustGate

**Target hardware:**
- Adafruit ESP32-S2 Feather ([#5000](https://www.adafruit.com/product/5000))
- Adafruit TMC2209 Stepper Driver Breakout ([#6121](https://www.adafruit.com/product/6121))

> **Carrier board planned.** A custom PCB (ESP32 Feather + BTT TMC2209 StepStick on 2.54mm headers, screw terminals) will replace the breadboard/breakout assembly. This document reflects the current breadboard wiring.

**All GPIO is 3.3V logic.** Do not connect 5V signals directly to Feather pins.

---

## 1. Motor — TMC2209

### Adafruit TMC2209 Breakout (#6121)

```
Feather                      TMC2209 Board (#6121)
  D5  (STEP) ─────────────── STEP
  D6  (DIR)  ─────────────── DIR
  D9  (EN)   ─────────────── EN      (active LOW; add 10kΩ pull-down to GND)
  TX  (Serial1) ── 1kΩ ──┬── UART   (single-wire half-duplex)
  RX  (Serial1) ──────────┘
  3V3            ─────────── VDD     (3.3V logic — board supports 3.3–5V)
  GND            ─────────── GND

Motor power supply (12–24V DC, ≥2A):
  V+ ──── + terminal block on TMC2209 board
  V- ──── - terminal block (share GND with Feather)

Stepper motor:
  Coil A+ ── A1 terminal
  Coil A- ── A2 terminal
  Coil B+ ── B1 terminal
  Coil B- ── B2 terminal
```

**UART detail:**
The Adafruit #6121 exposes a single `UART` pin for half-duplex communication.
Wire Feather TX through a 1kΩ series resistor to this pin, then connect RX to
the same node (after the resistor). This lets `Serial1` both write config
registers and read back driver status.

**Current limit:**
The board has an onboard current-limiting potentiometer that sets a hardware
ceiling. `TMC2209_CURRENT_MA` in config.h sets the UART software target.
Effective current = lower of the two. Set the pot first, then tune the config
value.

---

## 2. Home Limit Switch (FEEDBACK_LIMIT_DISTANCE) — active mode

Single NC (normally closed) limit switch on the left side of travel.
The carriage contacts it at the home position during homing.

```
Feather D10 ──── [NC limit switch, C terminal]
                 [NC limit switch, NC terminal] ──── GND
Feather D10 ──── INPUT_PULLUP (no external resistor needed)
```

**Pin states:**

| Carriage position          | Switch contacts | D10 voltage | `readHomeSwitch()` |
|----------------------------|-----------------|-------------|---------------------|
| Away from switch (normal)  | Closed (NC)     | LOW → GND   | `false`             |
| Contacting switch (homing) | Open            | HIGH (pull) | `true`              |

**Fail-safe:** a broken or disconnected wire pulls D10 HIGH → reads as
triggered → motor stops. This is the correct safe-fail behavior.

**Mounting:** the switch must be positioned so the carriage triggers it
slightly before the true mechanical hard stop. `ENDSTOP_MARGIN_STEPS` in
config.h captures the measured step offset between trigger point and gate 1.

### Max endstop (optional safety limit)

A second NC switch can be wired on D11 as a hard travel limit at the far
end of the rack. Not currently installed — the pin is INPUT_PULLUP and reads
safe (not triggered) when floating.

```
Feather D11 ──── [NC limit switch, C terminal]
                 [NC limit switch, NC terminal] ──── GND
Feather D11 ──── INPUT_PULLUP
```

---

## 3. Sensorless Homing (FEEDBACK_SENSORLESS) — disabled

StallGuard was evaluated on the Adafruit #6121 and abandoned. `SG_RESULT`
returned 0 at all times regardless of SGTHRS, TCOOLTHRS, or SpreadCycle mode.
Root cause is likely the onboard current pot forcing `I_scale_analog=1`,
preventing UART current control which StallGuard requires. No additional
hardware is needed if this mode is ever revisited — StallGuard reads back
over the existing UART connection.

The sensorless homing code remains in the codebase under `#ifdef
FEEDBACK_SENSORLESS` for reference and potential future use with a different
driver board.

---

## 4. E-Stop Button

NC momentary pushbutton. Fails safe on open circuit or broken wire.
Currently wired but the interrupt is disabled in firmware (use the `estop`
serial command instead until switch placement is finalised).

```
Feather A3 ──── [NC E-Stop button, one terminal]
                [NC E-Stop button, other terminal] ──── GND
Feather A3 ──── INPUT_PULLUP
```

| Button state            | D voltage | Action                                |
|-------------------------|-----------|---------------------------------------|
| Not pressed (NC closed) | LOW       | Normal operation                      |
| Pressed / wire cut      | HIGH      | RISING interrupt → STATE_ERROR        |

**Recovery:** type `home` in the serial terminal. The actuator re-homes before
accepting position commands.

---

## 5. Dust Collector

The dust collector is switched by a dedicated Shelly smart plug over WiFi — no
local wiring to the Feather. See the main README for configuring the plug; it
turns on automatically when a gate is open and can also be toggled from the
dashboard.

---

## 6. Rotary Switch — Resistor Ladder (CONTROL_ROTARY, budget option)

SP8T rotary switch, common to GND. Each position connects A0 to GND through a
different resistor. A 10kΩ pull-up from A0 to 3V3 completes the divider.

```
3V3 ──── 10kΩ ──── A0
                    │
Rotary positions (common to GND):
  Pos 0 (Home)  ── 0Ω (wire) ──┤
  Pos 1         ── 1kΩ ─────── ┤
  Pos 2         ── 2kΩ ─────── ┤
  Pos 3         ── 3kΩ ─────── ┤
  Pos 4         ── 4kΩ ─────── ┤
  Pos 5         ── 5kΩ ─────── ┤
  Pos 6         ── 6kΩ ─────── ┤
  Pos 7         ── 7kΩ ─────── ┘
                           │
                          GND
```

**Expected ADC readings (12-bit, 0–4095, 3.3V reference):**

| Position | Resistance | Voltage | ADC (~) |
|----------|------------|---------|---------|
| 0 (Home) | 0Ω         | 0.00V   | 0       |
| 1        | 1kΩ        | 0.30V   | 372     |
| 2        | 2kΩ        | 0.55V   | 683     |
| 3        | 3kΩ        | 0.76V   | 946     |
| 4        | 4kΩ        | 0.94V   | 1170    |
| 5        | 5kΩ        | 1.10V   | 1365    |
| 6        | 6kΩ        | 1.24V   | 1537    |
| 7        | 7kΩ        | 1.36V   | 1688    |

> **Calibrate before use.** The ESP32-S2 ADC is non-linear near 0V and 3.3V.
> Switch to `CONTROL_SERIAL_DEBUG`, rotate through each position, print
> `analogRead(A0)` each loop, and update `ROTARY_THRESHOLDS[]` in
> `RotaryControl.cpp` to the midpoints between your observed values.

### Toggle switch (CONTROL_ROTARY)

```
Feather D12 ──── [Toggle switch] ──── GND
Feather D12 ──── INPUT_PULLUP
```

Switch closed = LOW = system enabled.

---

## 7. Smart Outlet Control (CONTROL_SMART_OUTLET)

No additional wiring required. The ESP32 communicates with Shelly smart outlets
over your home WiFi network using their local HTTP API. Requires:

- ESP32 connected to your home network in station mode (handled automatically
  via the `DustGate-Setup` captive portal on first boot, or by setting
  `WIFI_STA_SSID` / `WIFI_STA_PASS` in config.h)
- One Shelly outlet per blast gate position, on the same local network
- "Local control" enabled on each Shelly (on by default — no cloud required)

Outlet-to-gate mappings are configured by the setup agent and stored in NVS.

---

## 8. Pin Budget

| Signal                    | Pin          | Mode(s)                              |
|---------------------------|--------------|--------------------------------------|
| TMC2209 STEP              | D5           | All                                  |
| TMC2209 DIR               | D6           | All                                  |
| TMC2209 EN                | D9           | All (active LOW)                     |
| Home limit switch         | D10          | FEEDBACK_LIMIT_DISTANCE / _DETENT    |
| Max limit switch          | D11          | FEEDBACK_LIMIT_DISTANCE / _DETENT    |
| Toggle switch             | D12          | CONTROL_ROTARY                       |
| Status LED                | D13          | All (onboard LED)                    |
| E-stop button (NC)        | A3           | All                                  |
| Rotary switch (ladder)    | A0           | CONTROL_ROTARY (analog)              |
| Detent switches (ladder)  | A1           | FEEDBACK_LIMIT_DETENT (analog)       |
| TMC2209 UART TX           | TX (Serial1) | All (hardware UART)                  |
| TMC2209 UART RX           | RX (Serial1) | All (hardware UART)                  |

**Active config pins: 8** (D5, D6, D9, D10, D13, A3, TX, RX)  
Unused in current build: D11 (max endstop), D12 (toggle), A0 (rotary), A1 (detent)  
Free for expansion: SCL, SDA, A2, A4, A5

---

## 9. Power Supply

| Rail          | Source                   | Notes                                   |
|---------------|--------------------------|-----------------------------------------|
| Motor 12–24V  | Separate DC supply, ≥2A  | Connect to TMC2209 + and − terminals    |
| Feather 3.3V  | USB or LiPo via Feather  | All GPIO logic; powers TMC2209 VDD      |
| Common GND    | Shared across all rails  | Connect Feather GND to motor supply GND |

Do **not** power the motor from the Feather 3.3V or USB 5V rail.
Always common the grounds.
