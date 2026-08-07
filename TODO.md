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
High priority — DONE (2026-08-05)

All in `dustgate-ui/src/app/build/build.component.ts` unless noted.

- ~~Single context window with the 5 options, invalid ones greyed out~~ DONE.
  One `.menu` body for all three add points (open end / free outlet / mid-run
  branch); `resolveOptions()` resolves the list once when the menu opens and greys
  what doesn't apply, with the reason inline ("drag the end", "ends a run",
  "not mid-run", "no room").
- ~~Creating ducts/tools happens at the clicked node; (no room) when full~~ DONE.
  `roomAt`/`roomNear` replace the old 10-cell spiral: the fitting lands on the
  clicked cell or one immediately adjacent, else the option greys "(no room)".
- ~~Linear slider must respect 'dont overlap devices'~~ DONE — `roomNear` is
  span-aware (`spanFor`), so a 4-wide slider greys out when its footprint collides.
- ~~Grid/build area occupies the whole visible screen and extends~~ DONE —
  `recomputeExtent` maxes content extent against the wrap's client size, plus a
  window:resize listener.
- ~~Cap/delete dialog replaced with a red (−) icon~~ DONE — badge on the selected
  node; capping stays in the context menu (it's a terminal action, not a removal).
- ~~Deleting an element with 1 downstream element converts it back to a duct~~
  DONE — `removeElement` splices any 1-in-1-out element out and reconnects its
  single leg to the parent, whatever that leg is (tool, manifold, slider).
- ~~Convert a ball valve to a manifold or slider~~ DONE — `convertKind` swaps any
  gate ↔ any gate, keeping id/name/cell/feed; targets grey out when their outlet
  count can't hold the legs, or there's no room beside it.

Medium
- ~~blocking ducts that would try to grow from the top of a manifold/slider~~ DONE —
  no branch dots on the final drop into a unit's top; add-dots were already
  bottom-only.
- ~~Don't block saving - just flag as 'work in progress'~~ DONE, with the
  enforcement moved rather than dropped: `save()` always persists, and the live
  Shop view (`live/live.component.ts`) greys itself out and refuses to drive any
  gate or the collector until the layout is whole, linking back to the builder.
  (The controller still 400s a structurally invalid doc, so that one case keeps
  the draft in the UI and says so — it can't be PUT.)

- ~~lines routed oddly~~ MOSTLY DONE (2026-08-05). `clearLaneY` picks the
  horizontal traverse lane by checking the WHOLE dogleg (drop → across → drop) is
  free of devices, preferring the old near-source lane when it's already clear and
  otherwise traversing below the obstructions. Fixes runs that lassoed around a
  tool sitting between a manifold outlet and its target. Re-check the collector
  specifically if it still looks off.
Do we really need editable names for manifolds/gates?
Don't allow duplicate named tools
improve 'dont overlap ducts' rules

Low
word wrap/fit names into the tool icons (maybe enlarge icons a bit)
- ~~control Z -> undo (maybe add undo button with history?)~~ DONE — snapshot
  undo/redo over every mutation (Ctrl/Cmd-Z, Ctrl/Cmd-Shift-Z or Ctrl-Y, plus
  toolbar buttons). Renames coalesce so a typed name undoes in one step.
right click actions?
Get rid of auto-arrange or make it work better?
Edit name by double clicking on text? or just clicking? unsure.
click and drag ducts?

## Round 2 (2026-08-05) — placement is now EXACT

Design change: the canvas no longer searches for a nearby cell when the clicked
one is taken. An action happens exactly where the user clicked, or the option is
greyed out. `freeCellNear` and the neighbour-ring search are gone; `placeAt` sets
the cell and `targetCells`/`roomAt` decide up front whether that's allowed —
one source of truth for both the greying and the placement.
- Branch legs run PERPENDICULAR to the run: horizontal → down then up, vertical →
  right then left, never onto another duct, greyed if both are taken (`BDot.axis`,
  `legCellFor`).
- Run ends (open and capped) carry no (−) badge and no inspector; tapping one
  opens its menu — fittings plus Cap/Reopen and Delete.
- Gate conversion moved out of the inspector's glyph strip into the same popover
  menu, opened by tapping the gate.


We should have a way to warn users that devices are unecessary (two ballvalves inline, a ballvalve proceeding a manifold with a capped line)

Next steps

~~Need a way to configure an element~~ DONE — all three gate kinds.
  Ball valves + manifolds (2026-08-06): `/gates` setup pass, plus the same sheet from the
  build canvas. Jog the valve, watch the handle, capture each position; no degrees ever
  shown. New firmware endpoint `POST /api/v2/servo/jog`, and the servo sweep is now
  travel-proportional so a nudge lands at once.
  Sliding gates (2026-08-07): manifold → home → home-side → reference sweep → per-outlet
  nudge-and-capture, writing `linear.calibration` + `states[].positionMm`. Drives the v1
  motion endpoints, which is sound while only the primary board has a stepper; a slider on
  a SECONDARY board would need v2 endpoints first. The odd-outlet Rockler case now warns
  instead of silently rounding.
  Live view refuses to run until every gate is calibrated; the canvas shows a green check
  when set up, amber when not.
~~need to be able to tie elements to esp32's~~ DONE (2026-08-06) for servo gates — board +
  channel pickers in the config sheet, with taken channels greyed out. `validateTopology`
  now also rejects two gates sharing a channel on one board.
A cheaper variant - qtpy based?
Consider renaming linear actuator to something...more accurate?
Add manual override buttons to ballvalves/manifolds/wire in to esp32
ESD safety and power safety
