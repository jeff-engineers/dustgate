# Attic: the linear rack (TMC2209 stepper + limit switches)

**Nothing here is compiled** — `build_src_filter` excludes `attic/`, and no board
header defines the pins that would switch it on. Kept to be repurposed for the
ST3215 serial bus slider, not maintained. No compiler has checked it since
2026-08-23; treat the first build as a port, not a resume.

| File | Fate |
|---|---|
| `LimitSwitchDistance.{h,cpp}` | **The reason this exists.** Homing sweep, datum selection, span-based stop placement. The endstops stay on the slider — multi-turn zero doesn't survive a power cycle — so the sweep is still the calibration path. Its step-count arithmetic is the part that goes; position comes from the servo's encoder. |
| `StepperTMC2209Driver.{h,cpp}` | Reference only. Read it for the move/settle/arrive contract in `MotorDriver.h` that a new driver has to satisfy. |
| `tmc2209-params.h` | Bench-tuned current/standstill numbers, out of `config.h`. |
| `devkitc_wroom32.h`, `devkitc-wiring.md` | The one rack pin map that moved a real carriage — endstop wiring, and GPIO34 being input-only with an external pull-up. |

`MotorDriver.h` / `FeedbackSystem.h` stayed in `motor/` and `feedback/`: they are
the seam a serial-servo driver plugs into, and `NullMotorDriver`/`NullFeedback`
still implement them.

`HAS_LINEAR` now derives from the serial-bus pins, so reviving the stepper *as a
stepper* needs a capability of its own. Includes were rewritten to `../../` on
the way in; nothing else was touched.
