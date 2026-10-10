# Done

Finished work, moved out of [`TODO.md`](TODO.md) so that file stays a list of
things still to do rather than a scroll of things that are not.

**Why this file exists:** the Done section lived at the bottom of TODO.md and
grew to 144 lines — longer than most of the active sections above it — which is
how a parked item stops being read. Split 2026-09-17.

**What belongs here:** an item whose reasoning was CONTESTED, or that a still-open
item leans on. Anything else can just be deleted when it lands — `git log` is the
record, and TODO.md's own header says so. Keep the dated "LANDED" line: the value
of this file is being able to answer "did we already decide this, and why".

Newest first.

### A board says whether a clamp is plugged in; the clamp switch is gone, 2026-10-09 (branch `clamp-detect`)

- **The per-board "clamp" switch (D-76, `clamp: true` in the layout) is replaced by plug detection.** LANDED 2026-10-09,
  host-tested, not on a board. Jeff's call: the switched jack already tells an empty jack from a clamp (WIRING.md §8), so a
  person should not have to. A node probes D0 once a second (`sensing/ClampJack.h`, shared with the ESP32 primary) and
  sends `CLAMP {in}` on a change, on a new link and every SENSE_REPEAT_MS, whether or not the layout uses the clamp. The brain
  keeps it per board (`NodeSession`, -1 = not said, never read as unplugged), serves `clampIn` on /api/nodes, and raises
  `clamp-unplugged` for a clamp the layout uses that reads empty (`DeviceProblems.h`). The pickers offer a board's clamp once
  one is plugged in, or while the layout already uses it (`clampOffered`, shop-doc.ts). D0's internal pull-up is enabled so
  a board with nothing on D0 reads as no clamp — the one part a bench must confirm. No mockup (jeff).

### The rest of the 2026-10-06 bug search, 2026-10-09 (branch `bug-search-fixes`)

All found by reading, none seen to fail on hardware; host-tested (native suites, the Pi cross-build, the UI suites), not yet
on the Pi. Left open in TODO.md: DNS rebinding (needs a decision on which local names to accept) and the /24 beacon.

- **A throw in a network handler no longer restarts the brain** — `io.run()` is retried with the error logged; the
  `remote_endpoint()` and `make_address()` calls use their non-throwing overloads.
- **The knock list is network-thread only**; the Boards scan gets a copy. Expires at 2 min, capped at 32.
- **A collector's control plug is re-sent its state until it takes it**, as the ESP32 does (it was sent once and forgotten).
- **`NodeWs::write()` writes from `_hold`**, not from a local it then moved — a short frame would have been sent from a dead
  stack frame.
- **macOS mDNS names that are not plain host labels never reach the shell.** Chosen over `posix_spawn`: a DustGate board's
  name is its hostname, so a whitelist closes it with nothing lost and no change to the timeout-and-kill shape.
- **`deploy.sh` cannot install an old binary as the new one** (deletes it first, checks make's status); the Makefile depends
  on every header directory and on the commit, so `/api/info` is never stale; `--state` uses `ssh -t`; `setup.sh` names
  `$SUDO_USER`.
- **The Plugs page re-reads the layout before pairing or releasing** — it wrote back the copy it loaded.
- Smaller: a wrong-NodeLink-version JOIN says "reflash it"; plug labels are cut on a UTF-8 boundary; the link log rotates at
  256 KB; reset and DELETE /api/topology share `clearLayout()`; the dead `discoverNodes()` override is gone.

### Servo range, optional boards and faster collector starts, 2026-10-08 (branch `servo-range`)

- **One gate move per BOARD, not per shop.** LANDED 2026-10-08, host-tested, not yet in the shop. The shop-wide mutex
  (`NodeBus::busy()` gating the whole queue) came from boards that drove several servos off one supply; a board's supply is its
  own, so each board now runs one move at a time and different boards move together (`TopologyRuntime::update`, `issueFrom`).
  MAKE-BEFORE-BREAK still binds across boards: a closing move waits until every opening move of its system has landed,
  wherever it is (`makeOwed`). The old nodebus test "busy local bus blocks a remote move" now pins the new rule instead.

- **The blower starts at once when its system already has an open gate; every system settles with one open at boot.** LANDED
  2026-10-08, host-tested. The blower used to wait for the new gate to land on every start (2 s sweep + 1 s hold). The shop
  rests open and make-before-break keeps the old gate open meanwhile, so only a SEALED system waits now. `settleAtBoot()` (called
  by both brains, the mock and the demo when a layout loads from nothing) opens the path to each system's first machine and
  commands every other gate closed, once that system's boards are linked; a tool or a hand start taking the system first ends it.
  Never dead-head is unchanged. `test_moves.cpp`; the layout-save pair; topology conformance now expects the settle.

### Pi bring-up and the bug search, 2026-10-06 / 07 (branch `pi-brain-bringup`)

- **A layout save is not a reboot.** LANDED 2026-10-07, host- and e2e-tested, not yet seen in the shop. Every adopt used to start
  the brain from nothing, on both brains and in the JS model: the blower the brain had started then read as one a PERSON had
  started (its press bookkeeping reset), so nothing ever pressed it off; a hand-switched tool kept its "manual" flag and lost its
  watts; every running tool read as switching on again. Now `TopologyRuntime::adopt` carries what the new layout leaves standing —
  readings and switch-on order by machine, each blower's state by system, the press bookkeeping for the same remote (both shells),
  and a gate's position only when it is the same physical gate (`sameHardware()`: everything but its name). A gate the save
  changed is seeded closed, as before, so a blower whose open gate was edited mid-cut still gets the dead-head stop. Mirrored in
  `createTopologyDevice(doc, prev)`; `layout-save.test.js` ↔ `test_layout_save.cpp` are a new pair, and `e2e.sh` saves mid-run
  and expects the OFF press (it fails on the old brain). This reverses a deliberate earlier rule ("physical position is unknown
  after a config change") for the gates a change did NOT touch — that is the contested part.

- **The ESP32 no longer lets any web page read its API key.** LANDED 2026-10-06. `GET /api/info` hands out the key by design (the
  app bootstraps from it), and every response said `Access-Control-Allow-Origin: *`, so any page open on a phone on the shop WiFi
  could read the key and drive the shop. The CORS headers (and the preflight answer) are gone from the ESP32, the native brain and
  the mock: the app is same-origin and `dev.sh live` proxies, so nothing needed them. Do not add them back for a dev convenience.
  DNS rebinding is still open (TODO.md).

- **Plug requests one at a time.** LANDED 2026-10-06. The Plugs page pinged every paired plug at once; the ESP32 serves those from
  ONE pending slot and ONE reply, so all but one row read "not answering" (built against the native brain, which did not mind).
  The page now asks one plug at a time, the ESP32 answers a second concurrent ping / rename / release / plug scan with a 429
  instead of a wrong or cut body (as node discovery and the servo jog already did), and the canvas releases removed plugs one at
  a time. The page also stopped leaking its 2 s poll when left before it finished loading.

- **A dead Shelly no longer stalls every other plug.** LANDED 2026-10-06. Shelly GETs passed no connect timeout, so an unplugged
  one cost the platform's whole budget (5 s on the native brain) on every poll pass, delaying every other polled plug's reading.
  `plughttp::get()` now has no default for the connect timeout — every call names one — and the Shelly calls pass their read
  timeout as both, as Tasmota always did. A candidate for "collector slow to start", still open in TODO.md.

- **A slider on a node can be set up from the app.** LANDED 2026-10-07, host-tested, NOT yet run on a slider. The slider page and
  the Gates list's Test drove only the brain's own rack (`/api/home`, `/api/jog`...), so on the Pi every step said "Couldn't
  reach the gate". `POST /api/linear/goto` / `GET /api/linear/state` (api/ApiCore.h) drive a slider by selector id with an
  ordinary SET carrying the distance (`TopologyRuntime::driveLinearTo`); the node homes itself on the first move. The page skips
  home / side / measure for a node, seeds each outlet from the manifold spacing, and saves `calibration.setUpOn: 'node'`.
  Afterwards routing treats that gate's position as unknown, so the next tool moves it.

- **The Pi's plug picker shows live draw, and not the Pi.** LANDED 2026-10-07. On the native brain `discover` returned the last
  sweep's rows, frozen, so "switch it on and look" could not work; it now re-asks each found plug in parallel, and sweeps first if
  nothing has been swept since boot. The sweep no longer knocks on the brain's own address and port (it answered every path with
  the app and was listed as an unclaimed plug).

- **A board too old to dial in says so.** LANDED 2026-10-07. Found with `dustgate-planer-sensor` (`8d58d3c`): it takes the claim and
  then waits to be dialled, which the native brain never does, so it "paired and lost connection". The claim reads `caps.join`,
  logs "too old to dial a brain … Reflash it by USB" and raises a `board-fault` until it links.

- **The canvas's tool sheet asks a yes/no question.** LANDED 2026-10-07. It asked "How does DustGate know it's running?" and offered
  Yes / No; it now asks "Is it on a smart plug?" (Yes, a metering plug / No, I switch it on myself), and its badge no longer says
  DustGate "switches" the tool. The three-way question (plug / clamp / nothing) stays on the Tools screen; making the two one
  component is still the outlet-picker duplication item in TODO.md.

- **The GUI sweep that "did not find a Tasmota" (2026-09-16).** Settled by `Find plugs` (since 2026-10-06 the Plugs page), which
  sweeps regardless of the layout and shows claimed plugs with their owner rather than hiding them — the open question in that
  entry. Sweeps on the Pi have since found both shop Tasmotas.

- **Smaller:** the `[RF] collector … pressed by RF through board …` line printed at every layout load now says "its remote is keyed
  through board …" (it read as a press at the Pi's first boot); the Gates list's save error no longer claims the setup was "saved on
  the gate".

### Cleanup 2026-10-06 — legacy API, schemaVersion 1, shared constants

- **Clamp switch, delete a system, Clear shop, and the Plugs page.** LANDED 2026-10-06 from the mockup
  `docs/mockups/plugs-clamp-delete.html` (decisions D-76 to D-78). Checked against a scratch native brain in the browser pane,
  not on a board. Clear shop and system delete release the plugs of the machines they remove once the layout is SAVED.

- **An interrupted gate move is sent again.** LANDED 2026-10-06, host-tested only (`test_deviceproblems.cpp` "an interrupted move
  is sent again"); not run on a board. When a node reset or lost its link mid-move, the brain believed the gate had got where it was
  sent (`_hwStates` is set when a move is COMMANDED) and left it. Now `DeviceProblems` acts on each board's `moveFault` EDGE (not the
  level — the fault stays up until the next move, so the level would loop) and calls `TopologyRuntime::reassertBoard()`, which owes
  the selector LAST commanded on that board a fresh command and replans. Only that one selector: the others were not in flight.
  A re-send that is itself interrupted raises a new edge and goes again, bounded by the move timeout, not a tight loop.

- **The legacy single-system API is gone, and so is schemaVersion 1.** LANDED 2026-10-06 (branch `cleanup-2026-10-06`).
  Deleted layer by layer, with the suites run after each: the firmware routes and consumers (`/api/estop`, `/api/config/gates`,
  `/api/config/port-role`, `/api/config/idle-timeout`, `PUT/DELETE /api/outlets/:slot`, `PUT/DELETE /api/dustcollector`), the
  mock routes, the model functions, the conformance cases that drove them, and the app's `configureOutlet`. **Kept on purpose:**
  `/api/motion` (the conformance suites' status view, and the slider's), the slider routes the app still calls
  (`home`, `move`, `jog`, `setstop`, `calibrate`, `config/orientation`), `/api/dustcollector/switch`, and the serial `estop`.
  Not removed: the saved idle-timeout NVS value, which `/api/info` still reports, though nothing can set it any more.

  schemaVersion 1: `validateShop()` and the firmware's `validateMinimal()` now refuse it with the sentence the runtime already used
  ("layout is from an older version (v1) — re-save it"), so a PUT of one answers 400 rather than being stored and rejected on load.
  `asShop`/`migrateToShop`/`isShop` are gone from the model, the UI's `toShop()` no longer migrates (it returns null for a non-shop),
  and `Shop.h` has no v1 branches. **What looks like v1 and is not:** `systemView()`/`viewOf()` hand the router ONE system's body
  (`controllers`, `elements`, `ducts`), which is how the per-system validator and the router/sequencer tests take a system. That
  shape stays. The single-system JS and firmware fixtures survive as compact ways to write one system; `shopFromV1()` (in
  `topology.fixtures.js`, test-only) lifts one into a shop, and `starShop.json` / `feedChainShop.json` are those lifted.
  `topology-conformance.js` now pushes shops and asserts a v1 document is a 400.

- **Every shared constant is paired or checked.** LANDED 2026-10-06. `shared/device-model/constant-pairs.json` lists each plain
  number that two builds hold; `tools/check-constant-pairs.js` (`npm run model:test`) reads each out of its source and fails when
  a group disagrees. The compiled-into-the-app copies are covered by the model hash baked into the bundle at build time
  (`tools/check-ui-fresh.js`, a CI step after the UI build). A new pair goes in BOTH the CLAUDE.md table and the JSON.

- **The outlet sheet's doubled "Scanning…"** LANDED 2026-10-06: the empty-state line went; the rescan button carries it.

### Routing — close every gate a tool doesn't need (2026-09-28)

- **A machine switching on re-asserts every servo gate in its system.** LANDED
  2026-09-28. Asked by jeff: a gate opened by hand stayed "closed" in our books,
  was never closed again, and the collector pulled through two tools.

  Routing already asked for CLOSED on every off-path gate; the gap was
  `planTransition()` skipping any gate BELIEVED to be at its target. Servos are
  open-loop, so belief is all there is. Decisions that are easy to relitigate:

  - **Only on a machine's RISING EDGE**, never per tick — `ingest()` runs every
    poll, and re-asserting there would move every gate several times a second.
    Not "when the decision changes" either: tool on → off (idle holds) → hand-open
    another gate → same tool on again is the SAME decision, and only the edge
    catches it.
  - **Servos only.** A slider cannot be turned by hand, and re-sending its stop
    could start a homing sweep nobody asked for.
  - **Pending in the runtime (`_reassert`), not in one plan** — the queue is
    rebuilt every tick, so a re-assert living only in its plan was dropped before
    it ran. Cleared as each move issues, and for a system that goes idle.
  - **The blower starts once its system's OPENS have landed**, not once the whole
    queue is empty. A queued close cannot seal a path that is already open
    (make-before-break), and waiting for five re-closes one servo at a time would
    leave the tool cutting without extraction.

  JS `sequencer.js` / `topology-device.js` ↔ C++ `TopologySequencer.h` /
  `TopologyRuntime.h`; paired cases in topology.test.js ↔ test_topology_controller.cpp.

### Firmware — control and comms (2026-09-17)

- **A node's firmware is finished — the CT trip numbers were the last thing that
  wasn't.** LANDED 2026-09-17. Jeff's goal, stated plainly: *"I want to make sure
  the firmware for the nodes is finalized, so we don't have to keep updating it
  unless we add new features."*

  Swept the whole node program and everything it includes for compile-time policy
  that a primary might want to change. Almost all of it was honest — protocol
  constants move on both sides together, pin maps and rack geometry change when a
  board is rewired, and hostname / WiFi creds / owner claim / `caps` / what a
  board is wired to already arrive at runtime. **A schema change has never
  reached a node**, which is the CONFIG frame doing its job.

  One row was not honest: `kTripRatio` and `kMinTripCounts` in `sensing/CtTrip.h`
  were labelled PROVISIONAL, are known to need re-deriving against the rebuilt
  1k/1k dividers, and had nothing to do with how any particular board is wired.
  They now ride the CONFIG frame's `SensorSpec`; the primary sends its own
  compiled-in values and a node that is told nothing keeps what it was built
  with. Retuning a shop is a primary reflash.

  **Why this is not a breach of the node/primary boundary, since that will be
  asked again.** `nodelink.js` used to say CONFIG carries "no threshold". That
  sentence was written when the only threshold in the system was `thresholdW`,
  and THAT one is still barred permanently: watts name a MACHINE, they come out
  of the document, and a node acting on one would be interpreting the schema. A
  multiple of a board's OWN learned noise floor, and a guard in that board's OWN
  ADC counts, mean nothing off the board they describe. The test for the next
  field that wants in is not "is it a number the primary chose" but **"could a
  node act on it without reading the document"**. The comment in nodelink.js was
  rewritten to say that rather than the shorter thing it used to say.

  Two things fell out of the work that were not the point of it:

  - **There was no hysteresis at all.** The decision was a bare
    `rmsCounts > trip`, so a tool sitting near its trip point did not report a
    state, it reported a stream of them — and every flip is a SENSE frame, which
    the primary reads as a tool starting or stopping, which is a gate move. The
    ~80x quiet-to-running gap means this never bites a table saw; it bites the
    marginal load nobody is sure about, which is the exact case Jeff hit on
    2026-09-16. `kClearRatio` 0.75 is the release point. **Provisional and
    unmeasured** — see TODO.
  - **The C++ and JS validators disagreed, and the pair caught it.** C++
    validated the tuning fields only when non-zero; zero is the sentinel for "not
    sent", so an explicit `"minCounts":0` was waved through as silence, leaving
    the primary believing it had set a guard the board never applied. JS had it
    right (validate on PRESENCE). Fixed in C++. This is the second time in two
    days that writing both halves of a pair found a real defect rather than
    merely confirming agreement.

  Rejected alternative: **measure the numbers once and freeze them.** Cheaper —
  no protocol change at all — and genuinely tempting, since `ct_bench` exists for
  exactly that. Declined for the reason the partition-table entry already argues:
  the shop is not in service, so a retune costs nothing today and costs a ladder
  per node once the boards are mounted. Freezing assumes the first measurement is
  right.

  Jeff also asked whether nodes could just stream RAW readings and let the
  primary decide. Not implemented, and bandwidth was never the objection — 4 Hz
  of one float per clamp is nothing. The reason it stayed on the node is written
  up in TODO under the OTA entry's neighbours; the short version is that the
  60 Hz RMS window cannot round-trip, and moving only the COMPARISON buys less
  than sending the numbers down does.

- **No control from the GUI of servo movement on nodes.** LANDED 2026-09-17
  (`ae5d3e4`). Jeff's note, found at the bench. The cause was a THIRD copy of a
  lookup that already had one home: the sketch resolved the jog's board with a
  literal `==` against each bus's `nodeId()`, while every gate MOVE went through
  `NodeBus::busForController()` and its alias map plus `bareHost()`
  normalisation. The two agree only while a controllerId is spelled exactly like
  its paired host — so renaming a board left gates working and every jog
  failing, which reads as "the configurator is broken on nodes".
  `jog()` is on the `ActuatorBus` seam now and the sketch just asks NodeBus.

Landed, and kept because the shape of the problem is worth not re-deriving —
or because the open remainder above only makes sense next to the record of
what closed. Anything with nothing left to say gets deleted from here; the git
history is still the record.

### Canvas — overlap and legibility (2026-09-07)

- **Two ducts must NEVER overlap.** LANDED 2026-09-07. Three separate holes, all
  closed; kept as a record because the shape of the problem is worth not
  re-deriving.
  - The branch-dot menu greys a splice that would make it worse
    (`spliceOverlaps()` — a dry run of the splice, rolled back). RELATIVE, not
    absolute: a shop that already overlaps somewhere is not one where every splice
    must be refused.
  - The guide bar names the runs that had to share a lane, at `info` — the shop
    still works, the picture is what suffers (`Router.shared()`).
  - **Overlap is judged on what is DRAWN, not on the lattice.** The edge
    bookkeeping missed two shapes that reach the screen anyway: a sub-cell stub
    between adjacent glyphs claims no lattice edge at all, and two runs through
    one port are one line until they separate.
  - **And two legs off a tee now leave by different ports** (`PORT_REUSE`), which
    is what actually stopped the second kind happening rather than merely
    reporting it. `laneOffset` used to stagger collinear runs and was deleted in
    the A* rewrite; the note that introduced it admitted it left "only a tiny
    shared stub" near the source, and that stub is exactly what survived. A port
    choice, not an offset, is the fix in this architecture.
  - Still true: every path into an overlap other than the branch-dot splice only
    REPORTS — dragging a piece, filling an end, adding at an outlet.

- **A cable must never run ALONG a duct.** LANDED 2026-09-07. Crossing one is
  fine and stays cheap; riding one is priced (`CROSSING_COST.ductShare`), because
  a 2px cable on a 6px duct is swallowed by it. The port stub is exempt by
  construction — a cable leaves the underside of its port whatever is below it,
  so a board standing over a trunk always puts its first 18px on that trunk.
  Covered by W14/W15; not yet seen on real hardware's screen.

- **The stock layout looks bad, and the overlap rules are why (2026-09-07,
  jeff).** LANDED 2026-09-07 — the offset is back, as `separateLanes()` on top of
  A* rather than instead of it. Two runs that would share a lane are nested
  LANE_STEP apart, symmetrically, the way the wiring layer has nested cables since
  boards went on the grid; `shared` is now a genuine fallback nothing on these
  boards reaches. The grid went to CELL 126 and a bend to TURN 48 alongside it, and
  ducting is now walled off above the collector's outlet height (D-67).

  Kept because the reasoning was contested and is worth not re-deriving: the
  offset was NOT ruled out by the routing rewrite.

  `laneOffset` was deleted in the routing
  rewrite because the LOCAL router was being replaced by A*, not because staggered
  parallel runs are a bad idea. The plan's line that the used-edge cost "replaces
  laneOffset's stagger" is about the mechanism, and reading it as "offsets are
  ruled out" is wrong — jeff, who made the call, says so. An offset applied to a
  path A* has already solved is a different animal from the stack of local guesses
  that came out.

  Where it would pay: two runs that must share a corridor could be drawn a few px
  apart and both stay legible, instead of one of them touring the board to find a
  lane it does not need.

- **Consider more room on the grid (2026-09-07, jeff).** LANDED — CELL 108 → 126:
  the pitch at which the reference scene's overlaps go away, with 144 and 162
  identical to it. Glyph sizes were left alone; whether `CLEARANCE` is the better
  knob is still open. Re-measure with `npm run bench:routing`.

  Original note: Rearranging the demo
  layout by hand meant leaving empty cells around things to get a clear view — so
  the spacing the canvas ships with is tighter than the one a person chooses.
  Either bigger glyphs generally, or more likely just more padding between cells.

  It belongs beside the offset work rather than after it: most of what makes a run
  ugly is having nowhere to go, and the same is true of an overlap. Cheapest
  version is `CELL` and the clearance margins, and the measurement to take first is
  what the demo layout's elbow count and overlap count do as those grow — both are
  now countable.

### Canvas — bands and the seam (2026-09-08)

- **Dropping a piece into the void between two systems should push the shop down
  (2026-09-07, jeff).** LANDED 2026-09-08 (D-71). A drop at the seam OPENS a row
  instead of spending one: everything at or below moves down by whatever count
  leaves an empty row between the piece's band and the neighbour's, so the gesture
  repeats. Boards ride along — they own a cell while belonging to no system.
  Silent and one undo step, both jeff's calls.

  Kept because the diagnosis is the part worth not re-deriving: **the gap was a
  consumable and nobody had noticed it was being spent.** The first drop grew the
  band over the empty row, so every drop after was refused because the bands then
  touched, and nothing anywhere put the row back — "the extra row vanishes once
  you do that, meaning that you can't keep moving things down" (jeff). Half of it
  had already been met in the DRAWING, which is why `systemSeparators()` exists;
  that kept the picture honest and never gave the row back.

  Explored in [`archived/seam-insert.html`](../docs/mockups/archived/seam-insert.html),
  whose demo runs the real rule.

  **Still open, and deliberately:** the push is DOWNWARD only. A piece dragged UP
  into the seam is itself what closes the gap, so no push below it can reopen one —
  that needs to move the system ABOVE, which is the seam-drag idea below.

### Canvas — marking and tracing (2026-09-07)

- **Highlight a validation problem ON THE CANVAS.** LANDED 2026-09-07 (D-66): an
  orange halo, always drawn, on every piece an airflow leak or a validation failure
  names. Explored in `archived/problem-marking.html`.

  What is deliberately NOT marked, and is the open half: **an overlapping duct.**
  That is two runs with one hidden under the other, which a ring round a box
  cannot express — the two treatments explored (a bracket over the doubled
  stretch, peeling the buried run clear on focus) both lost to *not having the
  overlap*. The guide bar still says one exists; nothing on the canvas points at
  it. Come back here only if the prevention work — the lane offset and the grid
  pitch, both above — runs out of road.

- **Trace a run from the piece you picked (2026-09-07, jeff).** LANDED (D-68).
  Selecting a machine, valve or duct lights every run back to the collector plus the
  cable of every gate on the way, as a rim of light in each line's own colour, with
  everything else dimmed. Upstream only — downstream was built and cut.

  **Hover is decided against (2026-09-08, jeff)** — selection is the whole
  gesture, on desktop as well as on the phone, and a separate "highlight ducts
  and wires on hover" item was deleted with it. Flow-direction marks were argued
  against rather than forgotten. `archived/path-highlight.html`.

### Bench

**1. A node drives a real servo — no primary needed.** ✅ **DONE** — all four PWM
channels drive real servos (`firmware/WIRING.md#1-the-board-and-its-one-pin-map` §6).

Kept for the technique, which the ST3215 slider node will want again: a node has
**no serial console** — it only acts on HELLO/PING/SET over its `/nodelink`
WebSocket — so `servo 1 90` on the primary's console moves the PRIMARY's pins.
The cheap isolated test is to *be* the primary, by pointing the conformance
runner at the real node:

```bash
bash dev.sh flash-node dustgate-node
bash dev.sh monitor node          # watch the other side while it runs
node shared/device-model/nodelink-conformance.js ws://dustgate-node.local/nodelink http://dustgate-node.local
```
Pass: the suite is green AND servos physically move. Green with nothing moving
means the link works and the actuator doesn't — exactly the split this test
exists to make visible.

## Moved from TODO 2026-10-05

- **loop() is still one enormous function — LANDED 2026-09-14, keep watching.**
  A stack protection fault on a collector board at boot, 2026-09-12:
  `SP 0x4085d060` against bounds `0x4085d068`, canary `0xabba1234` on the
  pointer, a half-built status JSON in the stack dump.

  The mechanism is worth keeping even though this is fixed, because it will
  recur: **loop() is a single function, so every `StaticJsonDocument` declared
  anywhere inside it reserves space in the SAME frame whether or not that branch
  runs.**

  Done: `SET_LOOP_TASK_STACK_SIZE(16 * 1024)` (was 8 KB), `sweepProbeOne()`, and
  then ping, rename and release pulled out into their own functions — a `<512>`
  and two `<256>`s that had been resident on every pass to serve requests that
  arrive a handful of times in a shop's life. **loop() now declares no
  StaticJsonDocument at all**; the four remaining documents in it are
  `DynamicJsonDocument`, which are heap.

  Also done, and the part that matters from here: the boot banner prints
  `uxTaskGetStackHighWaterMark()`. **Baseline measured on hardware 2026-09-14:
  13644 bytes free of 16384**, on a primary with a screen. A number that shrinks
  release over release is the warning nobody used to get — that is the whole
  reason it is printed, so compare it rather than glancing at it.

  What is NOT done: loop() is still ~1800 lines and will keep growing, and
  nothing enforces any of this. The next thing to extract when it bites is
  whatever has grown a document since.

- **~~Re-measure the CT on the rebuilt divider~~ DONE 2026-09-16.** Scale held
  (+1.50% vs the Tasmota, against +0.94% before), the screen's contribution fell
  from ~80% of the floor to 11%, and the floor underneath is the C5 ADC's own
  noise — shorting the CT out does not move it. **§5.5 is closed** (§5.5b), and
  the `Hz` column was found to report a fraction of the sample rate when fed
  noise, which wasted an hour. `ct-bench.md`'s parts table is no longer stale.

  What is left is optional and deliberately unbuilt: a 60 Hz demodulator would
  take the floor down 10-20x, and nothing needs it while a running collector
  sits 63x above it.

- **`POST /api/dustcollector/switch` is dead under a shop.** It drives collector
  slot 0 directly (`SmartOutletControl::setDcManual`), and with a topology loaded
  the main loop re-asserts every slot from `g_topoRuntime.collectorOn(systemId)`
  on every pass — so the switch is undone microseconds after it lands. It reports
  success the whole time, which is the worst way for an endpoint to be broken.
  `POST /api/collector {systemId?, on}` replaced it (D-59) and goes through the
  routing runtime, where the decision survives. Deleting the old route is
  phase-1 cleanup: the route, `_dcSwitchPending`/`consumeDustCollectorSwitchRequest`,
  its consumer in `firmware.ino`, and `ApiService.setDustCollector()` +
  `DemoApiService`'s override. `setDcManual`/`setCollectorManual` themselves STAY —
  the runtime is what calls them.

- **`/api/dustcollector/switch` on the ESP now goes through the routing runtime (2026-10-05)**, so it is no longer undone on the next loop pass; the native brain and `ApiCore` already did. The old slot-0 plumbing can still be deleted.
