# Dual-endstop calibration — Phase 1 spec

**Status:** draft (2026-07) — **partially implemented.** Canonical model + both
mocks + conformance and the firmware *foundation* (far-endstop polarity fix,
config profiles, `CalibrationData` v2) are done; the reference-sweep motion,
`/api/calibrate` + `/api/config/port-role` endpoints, and status/info fields are
pending and should be built with hardware to validate the motion. New manifold-cal
wizard UI is not built (4" is disabled in the existing UI).
**Scope:** the existing linear-actuator DustGate (v1). Independent of the v2
servo/multi-node work — but the API/model changes here go through the same
canonical-model + conformance discipline (see `shared/device-model/`).

## Motivation

Today the actuator is **open-loop**: it homes to a single endstop, zeroes, then
trusts the step count forever. A missed/added step (stall under load, a bump,
belt slip) silently sends the blade to the wrong gate, and `steps/mm` is a
nominal computation never validated against a real build.

Adding a **second endstop at the far end** turns the system **span-referenced**:
the endstop-to-endstop distance is a fixed physical constant, so one sweep makes
the device self-calibrating and self-checking. Because most users run a known
manifold (Rockler Dust Right 2.5"/4") with gates added in pairs and an endstop at
each end, we can go further and **auto-place every gate** from a stored profile.

## Locked decisions

- **Dual endstop is REQUIRED.** No `hasFarEndstop` flag, no single-endstop
  fallback. Backwards compatibility is explicitly dropped (only one v1 unit
  exists and it will be retrofitted). One code path.
- **Positional servos / stepper** unchanged; this is purely about homing,
  calibration, and gate placement.
- **The reference sweep is THE calibration path.** Manual per-gate jog demotes to
  an optional trim/override, not a parallel mode.
- **Nominal `steps/mm` is a plausibility bound, not the source of truth.** The
  measured span is authoritative.
- Capabilities delivered (all from the one sweep): over-travel safety, lost-step
  detection, manifold auto-calibration, auto motor-direction detection, and
  empirical `steps/mm` calibration.

## Hardware requirements

- Second **NC** limit switch on its **own GPIO** (not in series with the near
  one — the firmware must distinguish which end fired). NC + pull-up is
  fail-safe: a broken wire reads as triggered.
- **Repeatable trigger geometry.** Proportional placement and `steps/mm` cal both
  depend on `span_mm` being a real constant, so the endstop trigger points must be
  at a fixed, documented distance baked into the printed assembly — not a
  hand-positioned switch that varies per build. This is a mechanical-design
  requirement, not just firmware.

## Data model

### Manifold profile (new)

A profile maps a known manifold + gate count to mm geometry, all referenced to
the **near endstop trigger point**:

```
profile(model, gateCount) -> {
  spanMm:  number,      // near endstop trigger → far endstop trigger, in mm
  gatesMm: number[],    // absolute mm position of each gate (length === gateCount)
}
```

For the Rockler manifolds this is `firstGateOffsetMm + (i-1) * gatePitchMm` for
each gate, with `spanMm` derived from the same geometry (they're modular — span
grows with gate count). Ship profiles for `rockler-2.5` and `rockler-4`; keep a
`custom` model that skips auto-placement and uses manual jog.

> Gate count is even for these manifolds (gates added in pairs), but the user may
> wire an **odd number of tools** — extra gates are placed but simply left
> unassigned (no outlet). Nothing special needed; auto-placement creates all
> `gateCount` positions and the wizard assigns outlets only to the ones in use.

### Calibration storage (extends `CalibrationStore`)

```
measuredSpanSteps: long     // steps counted near→far during the sweep
stepsPerMm:        float     // = measuredSpanSteps / spanMm  (derived, for jog/UI)
stopStepPositions: long[]    // per-gate absolute step positions (proportional)
manifoldModel:     string    // which profile produced these
```

## The reference sweep (setup calibration)

Run once at setup; steps 2 and 6's checks re-run on every subsequent home.

1. **Auto motor-direction.** Nudge a few steps and observe which endstop's state
   changes; set direction so "toward the near endstop" is negative travel. Drops
   the manual direction step.
2. **Home** to the near endstop → position 0 (the zero datum). Note that home is
   backed off `HOME_BACKOFF_STEPS` from the near *trigger* point.
3. **Sweep** to the far endstop, counting `sweepSteps` (home → far trigger).
4. **Reconstruct + calibrate.** Home sits `HOME_BACKOFF_STEPS` inside the near
   trigger, so add it back to recover the full trigger-to-trigger span, then
   divide by the measured `profile.spanMm` (84.9 mm at 2 gates for rockler-2.5):

   ```
   measuredSpanSteps = sweepSteps + HOME_BACKOFF_STEPS
   stepsPerMm        = measuredSpanSteps / profile.spanMm
   ```

   Backoff does NOT enter the gate-to-gate pitch (it shifts every gate equally
   and cancels) — only this steps/mm span. Cross-check `stepsPerMm` against the
   nominal computation; if off by more than a tolerance (say ±15%), abort with
   "wrong manifold selected or mechanical fault" instead of trusting a bad
   measurement.
5. **Auto-place gates** by proportion of the measured span (immune to steps/mm
   error and per-unit variance):

   ```
   gateSteps[i] = round( measuredSpanSteps * (profile.gatesMm[i] / profile.spanMm) )
   ```
6. **Store** `measuredSpanSteps` for future span checks; persist gate positions.

## Runtime behaviours

- **Over-travel safety.** During ANY move, either endstop triggering unexpectedly
  halts motion immediately (→ ERROR / re-home required). The far endstop is now a
  physical backstop past the last gate.
- **Lost-step / span check.** On every home, after re-finding the near endstop,
  optionally confirm the far endstop is still at `measuredSpanSteps ± threshold`
  (a periodic full sweep, or opportunistically when travelling to the last gate).
  Beyond threshold → surface "recalibrate — possible lost steps" rather than
  moving to a now-wrong position.
- **Backlash.** Approach gates from a consistent direction (as today) so the
  measured span and gate positions share the same backlash null.

## Wizard / UX

New happy path replaces the gate-by-gate jog walk:

1. Pick manifold model (Rockler 2.5" / 4" / custom).
2. Pick gate count (even; stepper).
3. "Calibrate" → runs the reference sweep (auto-direction, home, sweep,
   place gates). ~30 seconds, no jogging.
4. Assign outlets to the gates that have tools (skip unused gates).
5. Optional: manual jog-trim any individual gate if a print tolerance needs it.

`custom` model → fall back to today's manual jog-per-gate flow, but still get the
sweep's `steps/mm` calibration, over-travel safety, and span check for free.

## Firmware changes (sketch)

- `config.h`: add `FAR_ENDSTOP_PIN`; remove single-endstop assumptions; add
  `SPAN_CHECK_TOLERANCE_MM`, `STEPS_PER_MM_PLAUSIBILITY_PCT`. Nominal
  `STEPS_PER_REV/MICROSTEPS/PINION_TEETH/RACK_PITCH_MM` stay only as the
  plausibility bound.
- Homing/limit logic: read both endstops; direction auto-detect; sweep routine;
  both-endstops-as-limits in the motion state machine.
- `CalibrationStore`: persist the new fields above.
- API/status: expose far-endstop state, `measuredSpanSteps`, calibrated
  `stepsPerMm`; add a "run calibration sweep" command and a
  "set manifold profile + gate count" command.
- UI: new calibrate step in both wizards; show calibrated `steps/mm` and span in
  Settings; a "recalibrate" affordance when the span check trips.

## Canonical-model + conformance impact

Every new API surface here (far-endstop state field, sweep command, manifold
profile config, calibrated span/stepsPerMm in status) must be added to
`shared/device-model/device-model.js` (+ `.d.ts`) and covered by
`shared/device-model/conformance.js`, so the mock, demo, and firmware stay in
lockstep — same discipline as the rest of the device API. The manifold profiles
(spanMm/gatesMm math) are pure and belong in the shared model too.

## Open questions

- **Profile numbers.** Rockler 2.5" MEASURED: span 84.9 mm @ 2 gates, pitch
  82.9 mm, offset 1 mm/side. Rockler 4" still needs hardware to measure. Also
  confirm the 2.5" pitch stays uniform beyond 2 gates.
- **Span-check cadence.** Full sweep on every home (safe, slower) vs opportunistic
  check only when travelling to the end gate (faster, looser)? Leaning
  opportunistic + an explicit "recalibrate" in Settings.
- **Retrofit.** The existing unit needs the second switch + a firmware flash; the
  printed endstop mount must fix `span_mm` repeatably before the numbers are
  trustworthy.
