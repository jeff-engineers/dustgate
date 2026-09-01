# Attic: the linear rack (TMC2209 stepper + limit switches)

**Nothing here is compiled** — `build_src_filter` excludes `attic/`, and no board
header defines the pins that would switch it on. Kept to be repurposed for the
ST3215 serial bus slider, not maintained. No compiler has checked it since
2026-08-23; treat the first build as a port, not a resume.

| File | Fate |
|---|---|
| ~~`LimitSwitchDistance.{h,cpp}`~~ | **GONE — back in `firmware/feedback/` since 2026-08-28.** It was the reason this directory exists, and it did the job: the ST3215 slider needs the same homing sweep, datum selection and span-based placement, because a step-counting bus servo has no datum either. The port changed nothing structural — the guess below, that its step-count arithmetic would have to go, was wrong. A "step" just became an encoder count, and `utils/MotionMath.h` absorbed that one level down. |
| `StepperTMC2209Driver.{h,cpp}` | Reference only. Read it for the move/settle/arrive contract in `MotorDriver.h` that a new driver has to satisfy. |
| `tmc2209-params.h` | Bench-tuned current/standstill numbers, out of `config.h`. |
| `devkitc_wroom32.h`, `devkitc-wiring.md` | The one rack pin map that moved a real carriage — endstop wiring, and GPIO34 being input-only with an external pull-up. |

`MotorDriver.h` / `FeedbackSystem.h` stayed in `motor/` and `feedback/`: they are
the seam a serial-servo driver plugs into, and `NullMotorDriver`/`NullFeedback`
still implement them. **That seam has been used, 2026-08-28** —
`motor/st3215/ST3215LinearDriver.{h,cpp}` is the driver, and it plugged in
without either interface changing, which is the outcome those two files were
kept for. `StepperTMC2209Driver` below remains the reference for the
move/settle/arrive contract it had to satisfy.

`HAS_LINEAR` now derives from the serial-bus pins, so reviving the stepper *as a
stepper* needs a capability of its own. Includes were rewritten to `../../` on
the way in; nothing else was touched.
