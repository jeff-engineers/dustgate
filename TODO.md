# TODO

## Mock/demo consolidation (deferred long-term fix)

`tools/mock-api.js` and `dustgate-ui/src/app/services/demo-api.service.ts` simulate
the same device behavior independently and have already drifted once (see the
2026-07 parity port — mismatched `STEPS_PER_MM`, missing `discoverOutlets()`,
hardcoded `dcConfigured`). Parity has been restored for release, but the
duplication itself is still there and will drift again.

Plan: extract the state machine (home/jog/move/saveStop/configureOutlet/
ping-sim/discover-sim/clearcal/dust-collector logic) into a shared plain
module (e.g. `tools/shared/device-sim.js`) that both consumers wrap —
`mock-api.js` around HTTP/WS, `demo-api.service.ts` around async/await. Keep
timing (`setTimeout`/`delay`) owned by the caller, not the shared module, so
the two different async patterns don't fight each other.

Estimated ~4-6 hours: mechanical extraction (~1.5-2h) + reconciling drifted
fields/constants (~2-3h) + verification against both `npm run start:mock` and
`?demo=true` (~1h).

## Hardware calibration

- **Recalibrate steps/mm for real hardware.** The firmware derives
  steps-per-mm from `STEPS_PER_REV` (200), `MICROSTEPS` (16), `PINION_TEETH`
  (15), and `RACK_PITCH_MM` (4.145) in `linear_actuator/config.h`, computed in
  `linear_actuator/utils/MotionMath.h`. These are nominal part specs, not
  measured — verify actual mm traveled per step on the physical actuator
  (rack-and-pinion backlash, pinion pitch-diameter tolerance, belt/coupler
  slop, etc. can all shift the real ratio) and correct whichever constant
  accounts for the discrepancy (most likely `RACK_PITCH_MM`, since that's the
  hardest to pin down from the part spec alone).
  Separately, `STEPS_PER_MM = 40` in `tools/mock-api.js` and
  `demo-api.service.ts` is a made-up mock-only constant for simulated
  position math — it doesn't need to match real hardware, just needs to stay
  in sync between the two mocks (already aligned as of the 2026-07 parity
  port).
