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
[`docs/architecture-rfc.md`](docs/architecture-rfc.md) §6 for the design.

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
  included it and `/api/status` was an idle stub. Now tool watts drive real
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
a tool that isn't paired on the canvas is no longer watched — even if an older config
had a slot for it. Re-pair anything that goes quiet. The collector is the one
exception: a layout with no `collector.control.outlet` keeps the stored plug rather than
un-configuring the blower. Watch for `[Outlets] Layout plugs registered: N` at boot.

#### PICK UP HERE — BTT TMC2209 V1.3 driver swap (paused 2026-08-09)
Swapping the Adafruit #6121 breakout for a BigTreeTech TMC2209 StepStick V1.3.
Wiring is documented in `firmware/WIRING.md` §1.

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
- **Sliding gate on a SECONDARY board** — calibration drives the motion endpoints,
  which address the stepper directly rather than by selector id. Sound while only the
  primary had a stepper; `/boards` makes it user-reachable. Add selector-addressed
  motion endpoints, or block the combination in the picker.
- **Rockler even-gate-count** — odd counts misplace gates. Now *warns* instead of
  silently rounding, but the spacing fix isn't finished.
- **Conflicts aren't surfaced** — firmware reports
  `{selectorId, winner, winnerState, losers[]}`; `live.component.ts` reads only
  `reachable`. So the UI says a tool isn't pulling but not why. Data done, UI not.
- **Servo backlash** — approach each target from one direction; the coupling has
  slop. Affects valve repeatability. Size it on the bench.
- ~~**A stepper fault shouldn't disable the servos**~~ **DONE 2026-08-13,
  compile + host tests only** — new `firmware/control/FaultPolicy.h` owns the
  mapping from "which `begin()` stage failed" to "what is refused", with
  `firmware/test/test_faults.cpp` (17) over the truth table.

  **Correction to what this entry claimed:** servo gate moves were never gated
  by `g_hardwareFault`. Checked every consumer — the latch has no readers outside
  `firmware.ino`, and nothing in `LocalActuatorBus` / `TopologyRuntime` /
  `NodeBus` consults it, so a dead TMC2209 already left routing and ball valves
  working. What the single latch *did* couple was real, just different:
  - an **outlets (WiFi/Shelly) failure refused to home the rack** — `ok = okMotor
    && okFeedback && okControl` gated motion on a subsystem that shares no wire
    with the motor. Now it gates nothing: the link recovers unattended via
    `WiFiProvisioner::maintain()`, so a boot-time latch outlived its own fault.
  - **`STATE_ERROR` and the red pixel** were set for any stage, so a rackless
    servo node pulsed red forever over a TMC2209 it was never built with. Error
    state is now motor-or-endstops only, and the `NO_LINEAR_FITTED` special case
    that used to patch this up afterwards is subsumed by the policy.

  Follow-up worth doing: expose the three flags over `/api/motion` so the UI can
  say *which* capability is down instead of showing state `ERROR`.

- **The tool status light can't go red** — a tool paired to a plug that stopped
  answering is indistinguishable from an idle one. `/api/status` reports
  `tools[id] = {watts, active}` and nothing about the sensor; the firmware knows
  (`SmartOutlet::isReachable()`) but `firmware.ino:638` only forwards
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

- ~~**A UI deploy erases the saved shop**~~ **MITIGATED 2026-08-13** —
  `topology.json` still shares the LittleFS partition with the Angular bundle, so
  `--target uploadfs` still erases it; `deploy.sh` now GETs the document before
  anything is built (§0) and PUTs it back after provisioning (§5), with
  timestamped copies in `.dustgate-backups/` and `tools/restore-topology.sh` for
  the manual retry. An unreachable board **aborts the deploy** rather than
  wiping a layout it couldn't save (`--no-topology-backup` overrides); a board
  with no shop yet is a no-op. Exercised end-to-end against the mock, including
  the 404 and unreachable paths — **not yet run against hardware**.

  The real fix is still open: move the topology to NVS or its own partition so
  the copy isn't needed. Until then the restore travels over WiFi, which means a
  board that doesn't rejoin the network keeps its layout only in the backup dir.

### 3. Completion
- **Clean up `/tools`** — its whole job was the outlet-pairing pass, which now
  lives in the build canvas inspector (`Set up smart outlet` → the tool sheet).
  The route has nothing left to ask. Either point it at `/build` or rebuild it as
  a genuine review pass ("check every tool at once"), which is a different thing
  from configuring one. **Its back button doesn't work** (noted 2026-08-12) — fix
  that whichever way the route ends up going.
- **One control path, not two** — the endpoints are merged (2026-08-12: no more
  `/api/v2/*`; routing status is `/api/status`, the motion blob moved to
  `/api/motion`), but the *behaviour* still forks: stop-index following is
  *suppressed* when a topology loads, which is a guard, not a resolution. Delete
  the stop-index automation once topology routing has driven real hardware.
- **Navigation** — `/boards`, `/gates`, `/tools`, `/shop` exist but there's no
  coherent path through them beyond the Build toolbar.
- Add manual override buttons to ballvalves/manifolds, wire in to esp32
- ESD safety and power safety

#### XIAO ESP32C5 spike (2026-08-12) — compiles, but the platform is the cost
`boards/xiao_c5.h` + the `xiao_c5` env build clean (1.22 MB of a 3 MB app
partition, 53 KB RAM), servo-only. Nothing has been flashed. What the spike
actually established:

- **The two platforms cannot coexist.** The C5 needs the pioarduino fork
  (official `espressif32` has no C5); both publish a package named
  `framework-arduinoespressif32` into one shared directory, so whichever env
  builds last owns the core and the other dies with an opaque SCons
  `TypeError: ... not NoneType`. Build `xiao_c5` alone. `[env] platform` is now
  version-pinned so this can't happen by accident again.
- **So adopting the C5 = migrating every target to pioarduino / core 3.x**,
  which means ESP32Servo 1.x → 3.x and me-no-dev AsyncTCP + ESPAsyncWebServer →
  the ESP32Async 3.x forks (the migration platformio.ini's comment declined on
  regression-risk grounds). That re-validates all four supported targets. It is
  the real price of the board, not the 8 MB of flash.
- Two portability fixes fell out and are already in: `utils/Watchdog.h` (IDF 5
  changed `esp_task_wdt_init()` to a config struct) and the
  `rgbLedWrite`/`neopixelWrite` guard in `utils/StatusLed.h`.
- Before flashing one: confirm which C5 GPIO are **strapping pins** and whether
  four ADC pads are actually free — the pin map is from Seeed's published pinout,
  not from hardware.

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

### 5. Shop schema v2 (`schemaVersion: 2`) — RFC drafted 2026-08-12
Design settled in [`docs/shop-schema-rfc.md`](docs/shop-schema-rfc.md).
Nothing below is implemented. Ordering matters: the shared model first, then
firmware, then canvas — the contract discipline means a half-migrated model
breaks all three at once.

- ~~**`systems[]` container**~~ **DONE 2026-08-12, model layer only** —
  `shared/device-model/shop.js` + `shop.d.ts` + `shop.test.js` (71 tests).
  `validateShop` / `routeShop` / `planShopTransition` sit above the per-system
  functions, which are untouched: `systemView()` reshapes a system into a plain
  topology (controllers spliced back in) so `validateTopology` and the whole of
  `routing.js` / `sequencer.js` run per system exactly as written — the RFC §4.2
  claim, now under test. Also `migrateToShop` / `isShop` / `asShop`, with a test
  asserting a migrated v1 doc routes identically.
- ~~**`machines[]`**~~ **DONE 2026-08-12, model layer only** — ports carry
  `machineId`; the plug, trip point and name live on the machine. `supplemental`
  and `enabled` are implemented, including the routed/partial/stripped verdict
  (RFC §10.3) and the all-ports-disabled error (RFC §6.6).

- ~~**Firmware speaks shop**~~ **DONE 2026-08-12, compile + host tests only** —
  new `firmware/control/Shop.h` mirrors `shop.js`; `TopologyRouter`/`Sequencer`
  take a `SystemView` (three `JsonArrayConst` handles into the one parsed doc —
  a view, not a copy, because materialising N systems on an ESP32 would double
  the largest allocation for nothing). `TopologyController` is machine-based with
  a per-system collector; `TopologyRuntime` has a per-system blower, coast timer
  and dead-head verdict over a still-shop-wide serial move queue.
  `SmartOutletControl` grew `COLLECTOR_COUNT` plug slots (slot 0 keeps NVS + the
  legacy stop-index automation; the rest are RAM-only and layout-driven).
  `validateMinimal` accepts both shapes. New `firmware/test/test_shop.cpp` (56)
  cross-checks routing/planning against the JS on the same fixture; the runtime's
  per-system blower behaviour is covered in `test_nodebus.cpp` (63). All four
  boards build. **No hardware has run any of it.**

- ~~**The UI speaks shop**~~ **DONE 2026-08-12** — new
  `dustgate-ui/src/app/services/shop-doc.ts` is the typed editing seam; documents
  are migrated on read at every entry point (canvas, `/tools`, Live, the entry
  redirect) and never on the device. Two seams, deliberately: `elementsOf()` /
  `ductsOf()` FLATTEN across systems for shop-wide questions, while the canvas
  works through the active system because it draws one duct tree at a time.
  A tool's plug moved to its machine, so the plug sheet is now handed the machine
  itself and needed no changes. `topology-device.js` is per-system like its C++
  twin; the mock validates both shapes and serves back the RAW document it was
  PUT (the firmware's store/runtime split, for the same reason). The demo seed is
  a real v2 shop.

  Verified in the browser on the demo shop: Live lists machines and routes a tool
  end-to-end, the canvas draws and saves through `validateShop`, adding a tool
  creates its machine, deleting the last port takes the machine with it, and the
  per-system leak check still fires on an ungated tool.
- ~~**UI test runner**~~ **DONE 2026-08-12** — `cd dustgate-ui && npm test`.
  Plain tsc → node, matching how `test:routing` / `test:wiring` and the shared
  model suites already work; no Karma, no Vitest, no headless Chrome in CI.
  73 new checks over `shop-doc`, `shop-ready` and the flattening readers, wired
  into the ui-build job. Constraint that comes with it: anything a spec reaches
  must be Angular-free, so component behaviour still relies on a browser pass.
- **Multi-port UI** — "add another port to this machine" on the canvas, with
  size/use suggested in the port-name field (`Cabinet · 4"`). No `diameter`
  field; sizing stays the user's business (RFC §6.5). Bounded by the cardinality
  rule below: the add-port affordance disappears at three ports, and the primary
  has no delete on it.
  - **Primary reads heavier, supplementals are ghosted** (RFC §9.1) — lighter
    stroke and a thinner duct, visible at rest, never hover-only, and distinct
    from the *dimming* that already means `enabled: false`.
- **`enabled` per port, sticky** — device-owned, persisted locally, merged over
  any adopted topology so a configurator push can never re-arm a port whose hood
  is on the bench. **Supplemental ports only**: the primary is always enabled and
  gets no disable control (RFC §6.6, validated in `shop.js`). Watch the merge for
  that — a stored `false` must never land on a port that has since become a
  primary. All-ports-disabled stays a guide-bar error.
- **Start stagger** — `onDelayMs` (tool sensed → its collector on) and
  `collectorStaggerMs` (collector → next collector), 2 s defaults, both
  configurable. Measure the real gap on the bench: the delay runs from *sensing*,
  so a poll cycle is already spent (RFC §10.1).
- ~~**Plug claims**~~ **DONE 2026-08-13 (model + firmware), UI PENDING** —
  `shared/device-model/plug-claim.js` + `firmware/outlets/PlugClaim.h`, four
  states (ours/unclaimed/dustgate/foreign), 54/54 paired tests each side.
  `Ws.GetConfig` is read before any write; a plug owned by another controller is
  paired read-only by polling. Takeover is `POST /api/outlets/takeover` — a
  separate endpoint precisely so no background pass can reach it.
  **Still to do: the UI half** — claim state in the picker, and the consequence
  sentence (`takeoverWarning`) in front of the user before offering takeover.

- ~~**Node claims (multi-primary safety)**~~ **DONE 2026-08-13, UI PENDING** —
  a node belonged to whichever primary connected LAST, so a bench brain and a
  shop brain could both drive one servo bank with neither told. Now: HELLO
  carries `primaryId` as a claim, first completed handshake wins, the owner is
  persisted in NVS (a claim that evaporated on a power cut would be re-raced at
  every brownout), and a non-owner's SET is refused per frame. A refusal keeps
  the socket OPEN and names `claimedBy` — a closed socket is indistinguishable
  from a dead board. NOT released on disconnect: gates hold on link loss, so
  releasing would let a neighbour adopt shop hardware during a reboot.
  Takeover rides one HELLO (`takeover:true`, armed by `POST /api/nodes/pair`
  with `takeover`), one-shot so a reconnect loop can never escalate itself.
  `/api/nodes` reports `claimedBy` + `takeable`. The whole two-shop conversation
  is pinned end-to-end in `nodelink-conformance.js` (33/33) against the mock
  node. **UI: the boards screen still needs to show "owned by X" + offer the
  confirmed takeover.**
- **"WiFi devices" tray** — rename/extend the boards tray to hold secondary
  controllers *and* unclaimed outlets; foreign-owned plugs shown locked with
  their owner named (RFC §9). Mockup: [`docs/mockups/outlet-dock.html`](docs/mockups/outlet-dock.html).
- ~~**Supplemental ports + partial routing**~~ **DONE 2026-08-12 (model +
  firmware)** — a port declares `supplemental: true` (the overarm); routing has
  three answers instead of two: *routed*, *partial* (a supplemental port lost),
  *stripped* (a **primary** port lost). Stripped is the alarm case — a saw
  drawing 1.8 kW with its cabinet gate shut.

  **Correction:** an earlier version of this line said "arbitration itself does
  not change". That was wrong — RFC §11.3 rule 1 is *primary beats supplemental,
  whatever started more recently*, and both engines shipped without it before
  2026-08-12. Now implemented in `shop.js` and `Shop.h`: ports are ordered
  primaries-then-supplementals into the greedy router, recency preserved within
  each class. Also added the validation the RFC lists and the first cut missed —
  home-system rule, ≥1 primary port, two-primaries-on-one-selector, ducts within
  one system, machine/element id collisions.

  **Tightened 2026-08-13 to exactly one primary + 0–2 supplementals** (RFC §6.3).
  The old "≥1 primary" allowed shapes that only existed to be validated against:
  primaries straddling systems (the home-system check) and two required ports on
  one single-open selector. Both are now unrepresentable, so those two checks are
  gone and `MAX_SUPPLEMENTAL_PORTS` is in. The delete rule moved with it: a
  primary port is **not deletable** — `removePort` takes supplementals only and
  refuses the primary, and the new `removeMachine` is what "delete this tool"
  calls. The primary is also **never disabled** (`canDisablePort` is
  supplemental-only). Model + UI seam + tests; firmware routing was unaffected
  (it reads the flags, never counts them).
- **Shop-wide move queue** — plans are per-system, execution is not. One serial
  queue on the primary, or two systems transitioning at once break the
  one-servo-at-a-time current mutex (RFC §10.2).
- **Boards are not pinned to a system** — a controller drives selectors in any
  number of systems, and the **wiring layer ignores system focus** so the cable
  that proves it stays visible (RFC §13).
- **Reserved, not built** — `collector.bin` (level sensor) and shop `indicators`
  / `alerts` with `own` vs `shop` scope (RFC §7).

#### Firmware testing debt this creates
Accepted deliberately (2026-08-12) — the design is worth the extra bring-up.
Nothing is hardware-tested yet, so these land on top of an unvalidated base.

- [x] ~~`syncTopologyOutlets()` walks `machines[]` instead of elements.~~ Written
      2026-08-12; it now iterates `machineIds()` so a two-port machine registers
      ONE plug instead of burning two slots on the same IP. **Still the highest
      risk item in the migration** — it is the code that decides which plugs get
      watched, and getting it wrong makes tools silently stop being sensed. It has
      no host test (it needs `control`, WiFi and NVS). **Re-run the bench note in
      §1 against it before trusting it.**
- [x] ~~Per-system iteration in `TopologyStore.h` / `TopologyRouter.h`~~, and
      `/api/status` now reports `systems` (per-blower on/coasting/dead-head),
      `machines` (routed/partial/stripped) and port-keyed `reachable`. All
      additive: `collectorOn` at the top level still means "is any blower
      running", which is what a one-system shop has always read.
- [ ] `COLLECTOR_COUNT` slot↔system pairing is by DOCUMENT ORDER
      (`systemIds()[i]` ↔ slot `i`), asserted nowhere. Reordering `systems[]` in a
      saved layout re-points a physical plug; adopt clears the assert cache so it
      re-commands, but confirm on the bench that the right blower moves.
- [ ] Sticky `enabled` persistence — survives reboot, WiFi drop, node reconnect
      **and** topology re-adopt. Verify the re-adopt case explicitly; it is the
      one a naive implementation gets wrong.
- [ ] Two collectors on one bench setup: confirm both can run, and that
      `collectorStaggerMs` actually separates the switch-ons.
- [ ] `onDelayMs` end-to-end: measure sensed→blower-on against the configured
      value and record the poll-cycle overhead.
- [ ] Multi-port machine: both gates open together, arbitration never lets one
      machine's ports fight, disabling one port closes only that gate.

#### UNRESOLVED: `Wrong boot mode (0x13)` when seated on a carrier (2026-08-12)
Cost most of a bench session and was never actually root-caused. Worth resolving
before a carrier gets designed around a guess.

What was **observed**:
- Flashing failed with `Wrong boot mode detected (0x13)` — GPIO0 high at reset.
- A direct probe showed **RTS→EN works** (the chip resets on command) but
  **DTR→GPIO0 does not** pull the strap low. The onboard BOOT button also failed
  to force download mode.
- It flashed fine with the DevKitC lifted **off** the carrier.
- The carrier's NeoPixel was found installed **backwards**.
- Separately and probably unrelated: the macOS port node wedged
  (`termios.error: (22, 'Invalid argument')` at every baud, nothing holding it),
  cleared by an unplug/replug. That masked the boot-mode issue for several
  attempts and made the timeline hard to read.

What was **not** shown: that any of the above caused the others. The carrier was
dismantled before a measurement isolated anything, so the 5V-peripheral-backfeed
story in WIRING.md §8 is a hypothesis with a plausible mechanism and no evidence.
A reversed WS2812 is an equally good suspect by itself.

- [ ] Rebuild a minimal carrier and reproduce deliberately: seat the board, confirm
      `boot:0x13`, then pull one cross-rail signal at a time and re-probe. The probe
      is: strap GPIO0 low over a reset and check for `boot:0x03 (DOWNLOAD_BOOT)`.
- [ ] Measure the 3V3 rail with USB unplugged and the external supply on. Anything
      above ~0.3V is backfeed, and the voltage says how hard.
- [ ] Then either confirm WIRING.md §8's hypothesis or replace it with what actually
      happened. Do not leave it as a maybe — it is currently the first thing the
      next person will read when their board won't flash.

#### Found on the FIRST hardware boot (2026-08-12)
The board came up: WiFi, mDNS, HTTP API, LittleFS, outlet poll task, servo bank,
NodeLink all started on a bare DevKitC. These are what the boot log exposed.

- [ ] **Serial output from two tasks interleaves mid-line.** Observed:
      `[Outlets] Provisioning plugs (free heap ` … the whole debug menu …
      `216848  0-7  Select position` … ` bytes)...`. The outlet poll task is
      pinned to core 0 and prints while the main task prints; nothing serialises
      `Serial`. Harmless to operation, but it corrupts the one record we have of
      a boot, which matters a lot during bring-up. Wants a print mutex or a
      single logging task — not per-call-site fixes.
- [ ] **`/littlefs/topology.json does not exist` logs an ESP error 4× per boot**,
      and `[API] topology stored:` prints `no` on a separate line because of the
      interleaving above. First boot with no layout is the EXPECTED state, so it
      should read as a fact, not as four errors. Check why it is opened 4 times.
- [ ] **`nvs_open failed: NOT_FOUND` ×2** on first boot — same story: an empty
      NVS namespace is normal before setup has run.
- [x] ~~"System halted — fix wiring and reset" on TMC2209 UART failure.~~ It does
      not halt (firmware.ino:659 deliberately continues), so this sent the reader
      hunting a crash that never happened. Now "Motion disabled — the rest of the
      board still runs." (2026-08-12)
- [x] ~~Boot banner said `Target: ESP32 + TMC2209` on every board~~, including
      ones with no stepper. Now reports BOARD_NAME + what is actually fitted.
- [x] ~~Status-pixel legend only existed in WIRING.md §5~~ — added to the serial
      `help` menu, where you are when you're squinting at a blinking light. Also
      split the WIRING.md table's single "Orange" row: solid = moving, blinking =
      WiFi lost. Same colour, and only the rate tells them apart.

#### Make `HAS_LINEAR` load-bearing (deferred stopgap, 2026-08-12)
A master's job is the routing brain; the linear rack is one KIND of gate, and the
direction of travel is servo ball valves. So a primary must not require a TMC2209
— but on 2026-08-12 it turned out a stepper-less PRIMARY has never been built at
all. `HAS_LINEAR` is derived in `config.h` and **read by nothing**; every
servo-only board so far has been a NODE, which sidesteps the question by
compiling from `firmware/node/` instead.

Shipped instead: `-DNO_LINEAR_FITTED` (env `esp32dev_servo`, now the default),
which changes only how the sketch REACTS to a missing driver — the TMC2209 UART
health check still runs and still prints. Motion stays locked via the existing
`g_hardwareFault` latch; what goes away is `STATE_ERROR` and the permanently red
status pixel. That was chosen over the real fix to avoid touching motion code
mid-bench-bringup.

What the stopgap does NOT do, and what the real fix should:
- [ ] Guard `motor/StepperTMC2209Driver.cpp` and `feedback/LimitSwitchDistance.cpp`
      with `#if HAS_LINEAR`, and gate the endstop pin reads in `firmware.ino`
      (~8 sites) and `control/SerialDebugControl.cpp` (4 sites). Today these
      reference `PIN_TMC_*` / `PIN_ENDSTOP_*` unguarded, which is exactly why a
      board header that simply omits the motor pins does not compile.
- [ ] Add a null `MotorDriver` behind the existing interface so the sketch's
      motion paths still link on a servo-only build without `#ifdef`s woven
      through them — the same objection `config.h` already records against
      `#ifdef`-ing the secondary out of the primary sketch.
- [ ] Then reclaim the flash: the stepper, feedback system and endstop supervisor
      are still compiled into the servo build today (71.4% either way).
- [ ] And drop the remaining cosmetic lies: unwired endstop pins still print
      `D10: TRIGGERED` at boot (open reads as triggered on an NC switch), and the
      motion-refusal messages still say "fix wiring and reset" when there is
      nothing to fix. `g_faultStages` is set to "no rack fitted (by build)" so the
      two messages that print it read correctly; the other ~5 sites don't.

Do this before the serial-servo migration, not during a bench session.

#### Canvas chrome (small, independent of the schema work)
- [x] Promote **Done** out of the overflow menu into its own toolbar button;
      Import/Export stay in the overflow (2026-08-12).
- [ ] **Settings** entry point for shop-wide globals — `collectorStaggerMs`,
      default trip point, units. Lives in the overflow menu next to
      Import/Export and links out to its own screen; the canvas toolbar should
      not grow a fourth button.
- [ ] **WiFi devices tray above the canvas**, replacing the boards rail:
      controllers and unclaimed outlets in one compact strip, wrapping rather
      than scrolling. Mockup: [`docs/mockups/two-system-shop.html`](docs/mockups/two-system-shop.html).
- [ ] **System scope dropdown** — one control that stays the same size at four
      systems; focus dims other systems rather than hiding them, so a machine
      with ports in two systems is never cut in half.
- [ ] **Add a system** — draws a collector, switches focus to the new system,
      leaves an open end to plumb from. A system *is* its collector (RFC §13).

#### Deferred — do not start
- **Clog detection from collector wattage** (RFC §7.1). Unverified whether a
  sustained spike separates a blocked duct from a full bin from a bag that needs
  shaking. `health` stays reserved with null thresholds; nothing reads it.
