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



'SHOP' GUI
High priority

Adding a linear slider is not respecting the 'dont overlap devices rule'
the grid/build area needs to occupy the entire visible screen, and possibly extend automatically
creating branches should always happen at the node the user clicked on
the cap/delete dialog can be removed, we can replace it with a red (-) icon, and just use the 'add' dialog
deleting a element with only 1 downstream resource should be allowed and convert it back to a duct
It should be possible to convert a ball valve to a manifold or slider

Medium
we should consider blocking ducts that would try to grow from the top of a manifold/slider - the interface is confusing if that happens
the 'at the end of this run' dialog should just be 'add'
the cap/delete dialog can be removed, we can replace it with a red (-) icon, and just use the 'add' dialog
There's still some weirdness with lines being routed oddly - mainly off the dust collector
Don't block saving - just flag as 'work in progress' or something?
Do we really need editable names for manifolds/gates? -
Don't allow duplicate named tools
improve 'dont overlap ducts' rules

Low
word wrap/fit names into the tool icons (maybe enlarge icons a bit)
control Z -> undo (maybe add undo button with history?)
right click actions?
Get rid of auto-arrange or make it work better?
Edit name by double clicking on text? or just clicking? unsure.
click and drag ducts?


We should have a way to warn users that devices are unecessary (two ballvalves inline, a ballvalve proceeding a manifold with a capped line)

Next steps

Need a way to configure an element
need to be able to tie elements to esp32's
A cheaper variant - qtpy based?
Consider renaming linear actuator to something...more accurate?
Add manual override buttons to ballvalves/manifolds/wire in to esp32
ESD safety and power safety
