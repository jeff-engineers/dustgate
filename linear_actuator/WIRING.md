# Wiring Reference — DustGate

**Target hardware:**
- **Espressif ESP32-DevKitC** (ESP32-WROOM-32) — primary/supported board
- Adafruit ESP32-S2 Feather ([#5000](https://www.adafruit.com/product/5000)) — legacy variant, still builds but unadvertised
- Adafruit TMC2209 Stepper Driver Breakout ([#6121](https://www.adafruit.com/product/6121))

> **Carrier board planned.** A custom PCB (ESP32-DevKitC + TMC2209 driver on 2.54mm headers, screw terminals) will replace the breadboard/breakout assembly. Standardize the DevKitC socket footprint on **1.0" (25.4mm) row spacing** — the official DevKitC spec; most cheap clones are 0.9" and will not seat. This document reflects the current breadboard wiring.

**All GPIO is 3.3V logic.** Do not connect 5V signals directly to the ESP32 pins.

---

## Board pin map

The firmware picks the pin set per board (`linear_actuator/boards/*.h`, selected by the
`-DBOARD_*` build flag). Diagrams below use **DevKitC GPIO numbers**; the Feather column
is the equivalent pin on that legacy board.

| Signal                 | DevKitC (WROOM-32)   | Feather S2   |
|------------------------|----------------------|--------------|
| TMC2209 STEP           | GPIO23               | D5           |
| TMC2209 DIR            | GPIO22               | D6           |
| TMC2209 EN (active LOW)| GPIO21               | D9           |
| TMC2209 UART TX        | GPIO19               | TX (Serial1) |
| TMC2209 UART RX        | GPIO18               | RX (Serial1) |
| Home endstop           | GPIO32               | D10          |
| Far endstop            | GPIO33               | D11          |
| Status LED             | GPIO2 (onboard)      | D13 (onboard)|

**Reserved for Phase 2 (servo nodes):** GPIO25, GPIO26, GPIO27, GPIO14 — a contiguous
4-pin block for servo PWM outputs. Unused by the v1 actuator; do not repurpose them on the
carrier. **Spare:** GPIO17, GPIO16, GPIO4.

> **DevKitC pin cautions:** avoid the strapping pins (GPIO0, 2, 5, 12, 15) for driven
> outputs — GPIO2 is used here only as the onboard LED, which is safe post-boot. GPIO6–11
> are the internal flash and are not usable. The endstops sit on GPIO32/33 because they need
> internal pull-ups; the input-only pins (GPIO34/35/36/39) have **no** pull-up and cannot be
> used for the NC-switch inputs.

---

## 1. Motor — TMC2209

### Adafruit TMC2209 Breakout (#6121)

```
ESP32-DevKitC                TMC2209 Board (#6121)
  GPIO23 (STEP) ──────────── STEP
  GPIO22 (DIR)  ──────────── DIR
  GPIO21 (EN)   ──────────── EN      (active LOW; add 10kΩ pull-down to GND)
  GPIO19 (TX) ──── 1kΩ ──┬── UART    (single-wire half-duplex)
  GPIO18 (RX) ────────────┘
  3V3            ─────────── VDD      (3.3V logic — board supports 3.3–5V)
  GND            ─────────── GND

Motor power supply (12–24V DC, ≥2A):
  V+ ──── + terminal block on TMC2209 board
  V- ──── - terminal block (share GND with the ESP32)

Stepper motor:
  Coil A+ ── A1 terminal
  Coil A- ── A2 terminal
  Coil B+ ── B1 terminal
  Coil B- ── B2 terminal
```

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

## 2. Home Limit Switch (FEEDBACK_LIMIT_DISTANCE) — active mode

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
purposes (see [`docs/dual-endstop-calibration.md`](../docs/dual-endstop-calibration.md)):

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

## 3. Sensorless Homing (FEEDBACK_SENSORLESS) — disabled

StallGuard was evaluated on the Adafruit #6121 and abandoned. `SG_RESULT`
returned 0 at all times regardless of SGTHRS, TCOOLTHRS, or SpreadCycle mode.
Root cause is likely the onboard current pot forcing `I_scale_analog=1`,
preventing UART current control which StallGuard requires.

Because StallGuard is abandoned, **the TMC2209 DIAG pin is not wired** on the
DevKitC (the Feather build still defines it on A2 for continuity, but nothing
depends on it). If this mode is ever revisited, StallGuard reads back over the
existing UART connection — no DIAG wire is strictly required — or route DIAG to
one of the spare GPIO (17/16/4) and define `PIN_TMC_DIAG` in the board header.

The sensorless homing code remains in the codebase under `#ifdef
FEEDBACK_SENSORLESS` for reference and potential future use with a different
driver board.

---

## 4. Dust Collector

The dust collector is switched by a dedicated Shelly smart plug over WiFi — no
local wiring to the ESP32. See the main README for configuring the plug; it
turns on automatically when a gate is open and can also be toggled from the
dashboard.

## 5. Smart Outlet Control (CONTROL_SMART_OUTLET)

No additional wiring required. The ESP32 communicates with Shelly smart outlets
over your home WiFi network using their local HTTP API. Requires:

- ESP32 connected to your home network in station mode (handled automatically
  via the `DustGate-Setup` captive portal on first boot, or by setting
  `WIFI_STA_SSID` / `WIFI_STA_PASS` in config.h)
- One Shelly outlet per blast gate position, on the same local network
- "Local control" enabled on each Shelly (on by default — no cloud required)

Outlet-to-gate mappings are configured by the setup agent and stored in NVS.

---

## 6. Remote Boot & Reset Buttons (enclosure-mounted, optional)

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

## 7. Pin Budget

| Signal                    | DevKitC pin   | Notes                                |
|---------------------------|---------------|--------------------------------------|
| TMC2209 STEP              | GPIO23        | All                                  |
| TMC2209 DIR               | GPIO22        | All                                  |
| TMC2209 EN                | GPIO21        | All (active LOW)                     |
| TMC2209 UART TX           | GPIO19        | All (hardware UART, remappable)      |
| TMC2209 UART RX           | GPIO18        | All (hardware UART, remappable)      |
| Home-side limit switch    | GPIO32        | FEEDBACK_LIMIT_DISTANCE (required)   |
| Far-side limit switch     | GPIO33        | FEEDBACK_LIMIT_DISTANCE (required)   |
| Status LED                | GPIO2         | Onboard LED (no header pin)          |
| Remote reset button       | EN            | Optional, not code-visible           |
| Remote boot button        | GPIO0         | Optional, not code-visible           |

Both limit switches are required. Which one acts as the home datum is decided at
setup time (always the user's LEFT end) — see `g_homeIsMaxEndstop`.

**Active header pins: 7** (GPIO23, 22, 21, 19, 18, 32, 33) — the status LED rides
the onboard GPIO2, so it costs no header pin.

**Reserved for Phase 2 (servo PWM):** GPIO25, 26, 27, 14 (contiguous 4-pin block).
**Free for other expansion:** GPIO17, 16, 4, plus the input-only GPIO34/35/36/39
(input/ADC only — no output, no internal pull-up).

EN/GPIO0 aren't GPIOs the firmware reads — they're the hardware reset/bootloader
lines, listed here only so the pin budget stays accurate if you wire the
enclosure buttons from section 6.

---

## 8. Power Supply

| Rail            | Source                    | Notes                                     |
|-----------------|---------------------------|-------------------------------------------|
| Motor 12–24V    | Separate DC supply, ≥2A   | Connect to TMC2209 + and − terminals      |
| ESP32 5V        | USB, or 5V/VIN header pin  | Onboard AMS1117 regulates it to 3.3V      |
| ESP32 3V3       | Regulated output pin      | Powers GPIO logic + TMC2209 VDD           |
| Common GND      | Shared across all rails   | Connect ESP32 GND to motor supply GND     |

Do **not** power the motor from the ESP32 3.3V or USB 5V rail.
Always common the grounds.

> **Note (vs. the Feather):** the DevKitC has **no onboard LiPo charger** — power it
> from USB or a regulated 5V source on the 5V/VIN pin. The Feather's battery/charging
> features do not apply to the primary build.
