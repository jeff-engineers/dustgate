# TODO

## Mock/demo consolidation — DONE

The shared canonical model lives at `shared/device-model/device-model.js` (+ `.d.ts`).
Both simulators are thin wrappers over it:
- `tools/mock-api.js` — HTTP/WS wrapper (owns `setTimeout` timing).
- `dustgate-ui/.../demo-api.service.ts` — async wrapper (owns `await delay`),
  imports via the `@device-model` tsconfig path alias (allowedCommonJsDependencies
  in angular.json silences the CJS warning).
Drift between the two JS mocks is now structurally impossible.

**Conformance ("sync with hardware") — DONE:** `shared/device-model/conformance.js`
runs 34 contract scenarios over HTTP against any target. Shape/validation/state
assertions hold on both mock and real firmware; sim-only details (exact wattage,
device names, dcOn flip) are shape-only or gated to localhost. DESTRUCTIVE
(homes/moves/wipes) — refuses non-localhost targets without `--force`.
- `cd tools && npm run conformance:ci` — spawns mock, runs, cleans up (CI-ready).
- `node shared/device-model/conformance.js http://<device-ip> <key> --force` — certify real hardware.

CI: `.github/workflows/ci.yml` runs three jobs on every push/PR — conformance
(mock), ui-build (Angular, also type-checks the @device-model wiring), and
firmware (`pio run`). Documented in README.md "Testing & CI" and
shared/device-model/README.md.

Follow-ups worth doing eventually (not blocking):
- Certify the current firmware against the conformance suite on real hardware
  (should be green; if not, it's found real drift — the whole point).
- Fold config constants (NUM_STOPS, MIN_STOP_SEPARATION_MM) into the contract so
  firmware config.h values are checked too.
- The CI `firmware` job hasn't run on GitHub yet (no push) — confirm `pio run`
  works in the Actions runner on first push (verified locally this session).

## Hardware calibration — steps/mm VALIDATED (2026-07)

- **Steps/mm confirmed accurate; no recalibration needed.** Bench-measured on the
  reference build: jog 50 mm → 50.15 mm actual (**0.3% error**), which is smaller
  than the mechanical slop and far inside the ~2 mm gate tolerance. So the nominal
  `STEPS_PER_REV·MICROSTEPS / (PINION_TEETH·RACK_PITCH_MM)` = 51.47 steps/mm holds
  — leave the config constants as-is. Supporting measurements: **backlash ≈ 0.6 mm**
  (< 2 mm tolerance → no compensation needed), **homing repeatable to 1 step**
  (~0.02 mm). Implication: the dual-endstop sweep's steps/mm *refinement* is
  non-critical for accuracy here; the sweep's value is auto-placement +
  over-travel safety + lost-step detection, not precision.
  Separately, `STEPS_PER_MM = 40` in `tools/mock-api.js` and
  `demo-api.service.ts` is a made-up mock-only constant for simulated
  position math — it doesn't need to match real hardware, just needs to stay
  in sync between the two mocks (already aligned as of the 2026-07 parity
  port).
