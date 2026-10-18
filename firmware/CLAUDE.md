# firmware — working here

The root `CLAUDE.md` has the project rules. Constants shared with the JS model, and why: `shared/device-model/CLAUDE.md`
— **a number that also lives in `shared/device-model/` is a pair; change both, in the same change.**

## Builds

`PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e <env>` (the pioarduino platform; official `espressif32` has no
C5). `dev.sh` sets it for you. Before committing firmware: `cd tools && npm run firmware:test` and build
`xiao_c5_primary` and `xiao_c5` (the `verify` skill).

| Env | Role |
|---|---|
| `xiao_c5_primary` | an ESP32 brain: routing, 2 PWM channels, CT, bin, RF |
| `xiao_c5` | a node: 2 PWM channels, CT, bin, RF — which of them it uses is a LAYOUT fact |
| `xiao_c5_linear_primary` / `xiao_c5_linear` | the slider board (ST3215 rack) as a brain / as a node |
| `xiao_c5_bus_bench`, `xiao_c5_ht12e_bench`, `xiao_c5_ct_bench` | not roles: the bus-servo console, the 315 MHz console, a walk-around CT meter |

## Boards

- **One board, one pin map.** Primary and node are the same XIAO C5 and carrier; the difference is
  `-DDUSTGATE_SECONDARY` and the source filter. `boards/xiao_c5.h` is the truth for which pad does what. A board at the
  collector is an ordinary node the layout points at a bin, a clamp and a remote — there is no collector build.
- **PWM servos and a serial bus never share a board.** `config.h` `#error`s on a pin map claiming both;
  `-DDUSTGATE_SERVO_BUS` flips a board to the slider's bus pins (D6/D7) and endstops (D8/D9).
- **The slider node owns its homing sweep** (a closed loop that can't round-trip over WiFi), ticked from `loop()`, on
  demand — the first SET that needs a datum — never at boot. Read the CALIBRATION note at the top of
  `node/dustgate_node.cpp` before changing it.
- **Every env assumes a screen**; an I²C ACK at 0x3C at boot decides whether one is fitted.
- **Decision-making headers are pure** (STL + ArduinoJson, no Arduino.h) so host tests can drive them.
  `test/shim/Arduino.h` fakes just enough clock and ADC for `sensing/CtTrip.h`; it must not grow — split a header
  instead. CtTrip is one file shared by node and primary: never copy it.

## At the bench

- **Flashing is Jeff's call** (root `CLAUDE.md`, "Working here"); the `flash` skill has the procedure and the traps.
  **Pin the boards** (`dev.sh ports --pin`): primary and node are the same part.
- **A bench sequence sets the state it depends on.** Start it with the persistent settings it relies on (an ST3215's
  `mode`, limits, torque live in the servo's EEPROM and survive every reset): a leftover setting looks exactly like
  broken hardware.
- **When it's unclear where something fails, add logging** — both sides of a boundary, and every refusal — rather
  than reasoning toward a guess. Several failures here were silent, not wrong.
- Wiring is drawn in `docs/carrier-wiring.html` (the PWM board; the slider board and the RF transmitter are still to be
  drawn — TODO). `docs/archive/WIRING.md` is history.
