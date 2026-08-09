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

## v2 firmware + multi-node — DONE (2026-08-07), hardware-UNTESTED

The whole star topology landed this session. Read
[`docs/v2-architecture-rfc.md`](docs/v2-architecture-rfc.md) §6 for the design.

- ~~Need a way to configure an element~~ DONE — all three gate kinds
  (ball valves + manifolds 2026-08-06, sliding gates 2026-08-07).
- ~~need to be able to tie elements to esp32's~~ DONE — board + channel pickers.
- ~~A cheaper variant - qtpy based?~~ DONE — `pio run -e dustgate_node`, a
  servo-only build at 27.7% of a 4MB board. Default node is the **QT Py
  ESP32-S3 (N4R2)**, pin map in `boards/qtpy_s3.h`; the ESP32-C3 stays building
  as `dustgate_node_c3` (`boards/qtpy_c3.h`). **Both pinouts reviewed, neither
  bench-tested with servos attached** — the S3's chip/flash identity is
  confirmed, its A0–A3 assignments are not.
- **The dispatch seam** — `ActuatorBus` / `NodeBus` / `TopologyRuntime`. Before
  this, the routing brain existed only for host tests: nothing in the sketch
  included it and `/api/v2/status` was an idle stub. Now tool watts drive real
  valves, one move at a time (which IS the servo current mutex).
- **NodeLink** — versioned WS protocol, `RemoteActuatorBus` client, `/nodelink`
  secondary endpoint, hold-on-link-loss.
- **`/boards`** — scan, add, rename, remove secondary boards; live online state.
- **`dev.sh flash-node`** — flashes a node and pushes WiFi creds over serial.
  (The primary *can't* provision a node over the network — the node isn't on the
  network yet. Serial at flash time is the answer.)

## What's actually left

### 0. Optional / someday — suggest better plumbing, don't impose it
Redundant-gate flagging shipped 2026-08-09 (`redundantSelectors()` in
`shared/device-model/topology.js`, surfaced as an amber "redundant" sublabel plus a
guide-bar offer to delete). The natural follow-on is the *positive* version: suggest a
cheaper arrangement rather than only naming dead weight.

The motivating case: a branched system running an individual ball valve to every tool,
where one manifold alternating between two legs would do the same job with fewer
actuators, less wiring and one less thing to calibrate.

Constraints if this gets picked up:
- **Suggestion only, never automatic.** Real shops have ceiling joists, benches and
  existing pipe runs the model knows nothing about. A layout that's worse on paper may
  be the only one that physically fits.
- Reuse the redundancy machinery's shape: derive it from the doc, never store it, and
  keep it out of `airflowIssues` so nothing refuses to run over a style note.
- Wants a way to dismiss a suggestion per-gate, or it becomes nagging.

### 1. Bench validation — do this before anything else
Networking is now real: primary↔node link is up and green (2026-08-08). The rest
below is still compile/host-test only. Expect bench work to generate its own
backlog and reorder this list.

**Bench note (2026-08-09): the layout now configures the plugs.** `syncTopologyOutlets()`
rebuilds the poller's outlet slots from `tool.sensor.outlet` on every topology adopt, so
a tool that isn't paired on the canvas is no longer watched — even if the old setup
wizard had a slot for it. Re-pair anything that goes quiet. The collector is the one
exception: a layout with no `collector.control.outlet` keeps the stored plug rather than
un-configuring the blower. Watch for `[Outlets] Layout plugs registered: N` at boot.

#### PICK UP HERE — BTT TMC2209 V1.3 driver swap (paused 2026-08-09)
Swapping the Adafruit #6121 breakout for a BigTreeTech TMC2209 StepStick V1.3.
Wiring is documented in `linear_actuator/WIRING.md` §1.

Verified on the actual module, so these are settled:
- sense resistors read **R110 → 0.11 Ω**, which already matches `TMC2209_R_SENSE`
- no pull-down on EN, and no pull-up either — EN floats undriven, so the external
  **10 kΩ EN→3V3 is required**, and nothing on the module fights it
- **no firmware changes needed.** Address 0 (MS1/MS2 floating), external Rsense,
  UART current control and unused DIAG all match config.h as it stands

Left to do, in order:
1. fit the 10 kΩ EN→3V3; with the ESP32 unplugged, EN→3V3 should read ~10 kΩ
2. set the VREF pot to ~1.2–1.4 V (wiper→GND, VMOT powered, motor disconnected) —
   the ceiling; UART sets 800 mA underneath it
3. power up with **no motor connected** and confirm a clean TMC2209 UART handshake
   in the boot log. A failure prints `[INIT] … motor(TMC2209 UART)` and every later
   refusal repeats it. Usual causes: the 1 kΩ UART series resistor missing or
   doubled (some BTT revisions fit their own), or MS1/MS2 jumpered
4. only then connect coils — **1A/1B and 2A/2B are adjacent pairs**, not the
   breakout's A+/A-/B+/B- order. Ring out each pair first
5. re-run the steps/mm check; nothing about the driver swap should move it, which
   is the point of checking

```
node shared/device-model/conformance.js http://<primary-ip> <key> --force
node shared/device-model/topology-conformance.js http://<primary-ip>
node shared/device-model/nodelink-conformance.js ws://<node>.local/nodelink
```

### 2. Known correctness gaps
- **Sliding gate on a SECONDARY board** — calibration drives the v1 motion
  endpoints. Sound while only the primary had a stepper; `/boards` makes it
  user-reachable. Add v2 motion endpoints, or block the combination in the picker.
- **Rockler even-gate-count** — odd counts misplace gates. Now *warns* instead of
  silently rounding, but the wizard/spacing fix isn't finished.
- **Conflicts aren't surfaced** — firmware reports
  `{selectorId, winner, winnerState, losers[]}`; `live.component.ts` reads only
  `reachable`. So the UI says a tool isn't pulling but not why. Data done, UI not.
- **Servo backlash** — approach each target from one direction; the coupling has
  slop. Affects valve repeatability. Size it on the bench.
- **A stepper fault shouldn't disable the servos** — `g_hardwareFault` is one
  latched boolean covering all three `begin()` stages, and every motion path
  checks it, so a failed TMC2209 UART handshake refuses servo gate moves too.
  Those gates don't touch the stepper. Make the fault *per-capability*
  (`g_faultStages` already records which stage failed): refuse linear motion on a
  motor fault, refuse servo motion only on a servo fault, and leave the rest of
  the shop working. This bites on the bench constantly — a board wired up on the
  desk loses stepper power far more often than the whole system is actually
  broken, and right now that takes every gate down with it.

- **The tool status light can't go red** — a tool paired to a plug that stopped
  answering is indistinguishable from an idle one. `/api/v2/status` reports
  `tools[id] = {watts, active}` and nothing about the sensor; the firmware knows
  (`SmartOutlet::isReachable()`) but `linear_actuator.ino:638` only forwards
  `getPowerW()`. Plumb reachability through `setToolPower`, emit
  `tools[id].sensor = {paired, reachable}` from `TopologyRuntime::writeStatus`,
  mirror it in `topology-device.js` `statusView()` so mock and firmware still
  agree, and add a conformance scenario. Until then the shop list shows green and
  orange only, and a dead plug reads as a tool nobody switched on.

- **No inrush filter on tool-on** — v1 debounced a rising tool for
  `OUTLET_ON_DEBOUNCE_MS` (1 s) so startup inrush couldn't false-trigger a gate
  move. That debounce lives in the v1 stop-selection path, which a loaded topology
  bypasses, so v2 acts on the first over-threshold reading. The coast-down now
  covers the falling edge; this is the rising one. Same shape of fix (a delay in
  `TopologyRuntime`), but it needs a schema field or a constant first.

- **A UI deploy erases the saved shop** — `topology.json` lives in the same
  LittleFS partition as the Angular bundle, and `deploy.sh`'s `--target uploadfs`
  writes a fresh image built from `linear_actuator/data/`. So every full
  `dev.sh flash` silently wipes the user's layout, calibration and node links.
  Bit us during bring-up and read as a node-pairing failure. Either move the
  topology to NVS/its own partition, or have deploy.sh GET it before uploadfs and
  PUT it back after.

### 3. Completion
- **Clean up `/tools`** — its whole job was the outlet-pairing pass, which now
  lives in the build canvas inspector (`Set up smart outlet` → the tool sheet).
  The route is a wizard with nothing left to ask. Either point it at `/build` or
  rebuild it as a genuine review pass ("check every tool at once"), which is a
  different thing from configuring one. Also fold the v1 `OutletConfiguratorComponent`
  onto the shared `<app-outlet-picker>` while in there — it kept its own copy of
  the identify-by-power flow because it's welded to the slot/stop model.
- **Retire the v1 flat path** — 36 `/api/*` routes still live beside v2, and both
  control paths coexist (v1 stop-following is *suppressed* when a topology loads,
  which is a guard, not a resolution).
- **Navigation** — `/boards`, `/gates`, `/tools`, `/shop` exist but there's no
  coherent path through them beyond the Build toolbar.
- Add manual override buttons to ballvalves/manifolds, wire in to esp32
- ESD safety and power safety
- Consider renaming linear actuator to something...more accurate?

### 4. Canvas polish
- **Duct line routing / A\*** — deferred deliberately. It's cosmetic (odd
  doglegs, nothing malfunctions) and `clearLaneY` got it mostly right; only the
  collector case is suspect. If it's worth doing, the cheap version first: keep
  the dogleg router and only fall back to a search when the simple route
  collides, so the expensive code only runs on the cases that look wrong.
- We should have a way to warn users that devices are unnecessary (two ballvalves
  inline, a ballvalve preceding a manifold with a capped line)
- Don't allow duplicate named tools
- improve 'dont overlap ducts' rules
- Do we really need editable names for manifolds/gates?
- word wrap/fit names into the tool icons (maybe enlarge icons a bit)
- right click actions?
- Get rid of auto-arrange or make it work better?
- Edit name by double clicking on text? or just clicking? unsure.
- click and drag ducts?
