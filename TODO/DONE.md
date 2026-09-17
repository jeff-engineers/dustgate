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
