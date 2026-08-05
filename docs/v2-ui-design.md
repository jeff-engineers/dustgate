# DustGate v2 interface design

**Status:** all three surfaces built (2026-07-30); remaining = glyph-orientation polish (pass 2b), firmware topology-awareness, and the `source: 'sensed'|'manual'` override-latch model field
**Companions:** [`v2-architecture-rfc.md`](v2-architecture-rfc.md), [`v2-topology-schema.md`](v2-topology-schema.md)
**Mockups:** four-surface showcase artifact (published from the design session)

The v2 configurator and daily driver. Started as design intent; the three surfaces
(Live view, build canvas, tool-tagging) are now BUILT and browser-verified in demo
mode. The shared model + `/api/v2/*` layer they consume already exist and are tested;
this document is the contract for the front-end on top of them.

Demo mode note: `?demo=true` is now persisted in `sessionStorage` (app.config.ts),
because hash routing rewrites the address bar to `/#/route` and drops the pre-hash
query on every navigation. Once set it sticks for the tab; clear with `?demo=false`.

## Who it's for

Woodworkers running a small-to-midsize hobby shop: **one dust collector, one
person, one tool active at a time**. Not very tech-savvy, but geeky enough to be
3D-printing and automating their shop. That audience sets the tone — plumbing-
schematic glyphs over node-graph jargon, "turn the tool on and watch it glow"
over IP entry, plain-English thresholds over raw watts.

## Two surfaces, opposite jobs

| Surface | When | Shape |
| --- | --- | --- |
| **Live view** | Every day, at the machine | Dead-simple mobile tool list + toggles |
| **Setup** | Once, when plumbing changes | Spatial "build your shop" canvas + a tool-tagging pass |

The early instinct to make the graph the primary surface was wrong (jeff
corrected it): the graph is a *setup* tool. The thing you touch daily is a list.

---

## Live view — the daily driver

A plain list. Answers "what's collecting right now?" at a glance; everything else
is one thumb-tap away.

- **Collector card** on top: its name, what it's currently pulling from
  (`Collecting · Table saw`), and a **master on/off** that kills or enables the
  whole system regardless of what's sensed.
- **One row per tool**, the single active tool highlighted green with a
  `Collecting` pill. Because routing is most-recent-wins, only the green marker
  moves when you switch tools — no queue, no multi-select.
- Each row shows its **input source**:
  - **Auto** — the tool has a smart outlet. Row reads "sensing power" and flips
    itself when the tool powers on. The toggle is mostly a status mirror.
  - **Manual** — no outlet, so no way to sense it. Gets a real toggle you flip.

### Manual override of an auto tool (essential, not optional)

> "sometimes you gotta clear a clog" — jeff, 2026-07-30

Auto tools **must** be manually forceable. A pure auto-sensing system would lock
you out exactly when you need to run the collector against a tool that's switched
off (clearing a clog).

**Model implication — one flag, no new routing path.** A manual tap and a
smart-outlet power-on are the *same kind of event* on one most-recent-wins
recency timeline. Auto vs manual is only about the input **source**, not routing
precedence. The one subtlety the clog case forces: an activation must remember
its source, because an overridden tool draws **0 W**:

- **`source: 'sensed'`** → auto-clears when watts drop below threshold.
- **`source: 'manual'`** → ignores that tool's own 0 W reading; clears only on
  tap-off or a newer request (a power-on or another tap supersedes it).

In the UI an override reads as **`Collecting · manual`** vs `Collecting` for
sensed. Implementation is one `source` field on the active-tool record in
`topology-device` and one clause in the "should this clear?" check — reuses the
existing recency + make-before-break sequencer untouched.

---

## Setup, part 1 — the build canvas

Place the dust collector anywhere on a not-to-scale grid, then **pull a duct out
of any of a node's four sides and drop a fitting** from a palette. Chaining
fittings mirrors physically assembling ductwork — constrained enough to not be
scary free-form node-wiring (you attach to an existing point; the palette is a
short list of real parts), spatial enough to match the shop on your wall.

**The 4-way spatial placement is pure presentation.** Node `x/y` + orientation
live in a **parallel `layout` map keyed by node id**; the topology model stays
the clean presentation-free tree from [`v2-topology-schema.md`](v2-topology-schema.md).
Routing and the sequencer never see the drawing.

### Component palette (woodworker names — no "actuator/selector/feed")

| Palette name | What it is | Glyph |
| --- | --- | --- |
| Dust collector | The source (root) | Cyclone spiral, teal |
| Ball valve | Binary open/closed, 1 downstream | Circle + bore |
| **Sliding gate** | Rack-and-pinion one-of-N selector (was "linear actuator") | **Comb** (see below) |
| Manifold | Left/right/closed, 2 teeth | Body + 2 bores |
| Y-split | Passive merge | Three lines meeting |
| Tool | A machine | Rounded square |
| End cap | Capped stub, extend later | Short stub + cap bar |

### Visual language

Schematic valve-symbol glyphs, half-familiar from plumbing. **Green stroke = the
open/flowing path.** The *same* glyph animates between states (bore rotates,
puck slides, manifold bore swings).

**Selector glyphs are one family, distinguished by outlet count** — this was the
key rethink (2026-07-30):

- ball valve = 1 bore / 1 downstream (binary)
- manifold = 1 body / 2 teeth (left/right)
- **sliding gate = a comb** — full-width track, one tooth per outlet, a sliding
  puck lit green at the currently-open stop (one-of-N).

The old single-cell-plus-arrow sliding-gate glyph is **dropped** — it implied
binary and hid the fact that one gate serves several tools. Comb tooth count is
**even** (Rockler manifolds ship in pairs → 2/4/6). **Each outlet gets its own
short duct stub** (tools don't hang directly off a tooth) so any outlet stays
extensible — can take an end cap or Y later.

### Canvas rules

- **No collisions** — each node owns one grid-cell footprint. A drop shows a
  green ghost on a free cell, a red ✗ ghost when it would overlap; ducts route
  around occupied cells.
- **Labels** — every node has a tap-to-rename label above its glyph (collector
  defaults to "Dust collector"); the collector also shows a running/off pill.
- Duct runs stay orthogonal and grid-snapped.

## Setup, part 2 — tag the tools

After the pipe is drawn, walk the tool list once (progress dots, one card each).

- **Identify by power** — the woodworker-friendly trick from Phase 1. To link a
  smart plug you don't type IPs or match hostnames: switch the tool on and watch
  which `shelly-xxxx` jumps to green. Two-tier dots: green ≥5 W running, amber
  1–5 W standby, gray idle (same thresholds as the Phase 1 outlet configurator,
  so a standby-draining plug doesn't masquerade as the live one).
- **"Smart outlet?" defaults to Yes.** Toggle off → the scan/threshold block
  collapses to "you'll switch this one manually," and the tool becomes a Manual
  row in the Live view.
- **Gate chip is read-only** (`Sliding gate · outlet 2`) — derived from the
  canvas (which stub the tool hangs off), shown for confirmation, never retyped.
- **Threshold slider** with a smart default and a plain-English "why" (catches
  the motor, ignores standby).

---

## Flash budget (checked 2026-07-30)

Concern: would building this UI run the ESP32 out of flash? **No.** The UI and
the firmware live in different partitions ([`partitions-devkitc-noota.csv`](../partitions-devkitc-noota.csv)),
and the UI one is barely touched.

| Partition | Size | Used | Free | Headroom |
| --- | --- | --- | --- | --- |
| `app0` (firmware) | 1.75 MB | 1.20 MB | ~600 KB | ~33% free |
| `ffat` (UI bundle, LittleFS `data/`) | 2.19 MB | 436 KB | ~1.76 MB | **~5×** |

The v2 canvas editor is maybe +100–300 KB of minified JS — it lands in `ffat`,
which you'd have to ~5× to fill. Watch `app0` as firmware grows (servo driver +
topology-JSON consumer are tens of KB, not hundreds). Free win if ever tight:
the bundle is stored **uncompressed** — gzipping drops 436 KB → ~120 KB.

---

## Build order (when we start)

Deployed to Vercel live but **silently there** — lazy routes `/shop`, `/build`,
`/tools`. As of 2026-07-31 the home page (dashboard) shows Live/Build/Tools nav
buttons that link to them, but **only on localhost or a real device** — gated by
`isLocalOrDevice` (exported from `app.config.ts`, host-based: LAN/mDNS/localhost →
show, `*.vercel.app` → hide). So the public demo still doesn't surface the WIP
pages; dev + on-device do. Build/Tools carry a `BETA` tag.

1. **Persistence** — the parallel `layout` map (node `x/y` + orientation)
   alongside the topology tree; PUT/GET through the existing `/api/v2/*`.
2. **Reusable SVG glyph set** — the seven fittings as components, state-driven
   (green = open), animating between states.
3. **Live view first** — ✅ BUILT (2026-07-30). `live/live.component.ts` on the
   silent lazy route `/shop`; consumes `getTopology` + `getV2Status` + `simTool`
   (polls every 2 s). Reads the routing winner from `reachable` (the single green
   "Collecting" tool), auto/manual from `sensor.outlet` presence, master kill via
   stop-all. DemoApiService seeds `DEMO_TOPOLOGY` (`services/demo-topology.ts`) so
   the route shows a shop in demo/Vercel mode. Still TODO: the
   `source: 'sensed' | 'manual'` model field for true override latching (right now
   an overridden auto tool would clear on its own 0 W reading — fine in the sim,
   matters on real hardware).
4. **Build canvas** — the big one. **Pass 1 BUILT (2026-07-30)**:
   `build/build.component.ts` on silent route `/build`. Renders the topology
   spatially (schematic glyphs per type/kind, orthogonal ducts, labels,
   auto/manual sublabels), lights the live airflow path green from `reachable`,
   drag-to-reposition with grid-snap + collision + duct reroute, auto-arrange,
   and Save (persists node cells in `topology.ui.layout` — a key the model and
   validator ignore, so it round-trips through PUT/GET). Auto-layout (tree from
   the collector) seeds positions when no saved layout exists. On a FRESH system
   (getTopology 404s or has no collector), the canvas seeds a blank topology with a
   lone collector (+ primary controller) so the page is usable immediately — you
   select the collector and build outward; left un-dirty so Save stays disabled
   until the first real edit (added 2026-07-31). The left fitting legend was removed
   (mobile screen-space) and replaced by a **contextual guide bar** pinned under the
   toolbar: one line that follows state — onboarding (empty) → progress nudge → live
   airflow problems ("<tool> can't be selected … Add a gate, delete it, or [Cap them]",
   warn-styled). Toolbar status spans (err/leak/note/hint) folded into it.
   **Duct-first flow (2026-07-31)**: the fitting menu gained a **Duct** option that
   lays bare pipe — a childless `junction` = an OPEN END — instead of forcing a tool.
   Open ends are selectable and take their own +handles (`canAddChild` now true for
   junctions), so you populate them later (tap the end → +→ Tool/gate), matching the
   "plumb first, drop tools onto ends" model. Open runs are highlighted (dashed accent
   duct + accent end-dot); the guide bar nudges "N open duct end(s) — tap it, then a +".
   Same primitive works off the collector, a gate outlet, or another duct end (unifies
   add + branch). KNOWN WRINKLE: dropping a tool on an open end leaves a redundant
   pass-through junction (harmless — routing ignores junctions); collapsing 1-in/1-out
   junctions is a follow-up.
   **Batch polish (2026-07-31)**:
   • **Pass-through collapse** — a junction with exactly one child is now removed
     (reconnecting the child to the grandparent), so populating an open end / chaining
     ducts stays clean. A junction only survives as a tee (≥2 legs) or an open end (0).
   • **Cap** — an open end can be sealed via a "Cap" button (a `capped:true` junction,
     validator-tolerated); renders a bar glyph, drops the dashed highlight + nudge.
   • **Inline controls** — the rename/outlets/Cap/Delete panel now floats anchored
     above the selected element (via getScreenCTM) instead of a bottom bar; the
     collector is now selectable (rename). **Delete/Backspace** deletes the selection
     (guarded against text fields).
   • **Manifold glyph** — rounded pill + input hub fanning lines to each outlet, so it
     reads as a rotary valve distinct from the rectangular sliding gate.
   • **Duct routing** — rewrote the elbow to **drop-jog-drop** (leave the parent's
     bottom, jog, enter the child's top) so ducts stop entering from the sides and
     looking like phantom branches. Refactored to a segment model and added
     **crossover hops**: where a duct's horizontal crosses another's vertical it arcs
     over (electrical-diagram convention) so overlapping lines never read as merged;
     the fat hit-target keeps the plain path.
   • **Sibling fanning** — ducts off one parent (collector or tee) now leave at
     staggered x and jog at staggered rows (ordered by target x), so two runs never
     share a stub/row (collinear overlap). Verified: 3 tools off a collector fan to
     distinct exits (154/172/190) and rows; hops still fire on crossings.
   • **Grid-snapped branch dots (2026-07-31, mocked-first w/ jeff)** — replaced the
     imprecise fat invisible duct-hit-path with subtle always-on dots at each grid
     step along every duct segment (`branchDots()`; deduped per cell; hidden while
     dragging). Hovering brightens the dot to accent; clicking opens the fitting menu
     and `branchDuct` now drops the junction AT the clicked cell (not the child's
     cell + subtree-shift — `shiftSubtree` removed). Verified: click dot at (1,2) →
     tee lands at (1,2), new leg sprouts. This fixes "branching mid-run misbehaves".
   • **Two-section branch menu + insert-inline (2026-07-31)** — clicking a branch dot
     now offers **Insert a gate here** (ball valve / sliding gate / manifold → splice
     INTO the run via `insertInline`: the downstream reconnects to the gate's first
     outlet, so a manifold becomes a real 2-way with one leg used + one free, a ball
     valve is a plain inline on/off; other outlets stay capped-but-available) vs
     **Branch a new leg** (tool / duct → tap a parallel run). Branch legs now come off
     **perpendicular**: a junction routes the continuation straight down and taps
     branches off horizontally (`ductPoints` junction case + leg placed to the side),
     so a tee reads as a real tee, not two parallel verticals. Verified: insert ball
     valve → collector→gate→saw (leak cleared); branch tool → horizontal tap off the tee.
   • **DUCT-FIRST rewrite — the + handles are RETIRED (2026-08-02, jeff-steered).**
     The canvas is now "plumb first, populate later" as the ONLY model — no more
     selecting a node and clicking a `+` on its side. Two decisions from jeff: (1)
     *replace* the + handles outright, (2) *draw from ends only* — a node BODY drag
     still repositions; drawing pipe starts only from an open end. Concretely:
       – A fresh shop seeds the collector **plus one bare open run** (`blankTopology`),
         so there's always an anchor. Guide copy rewritten around drag/tap.
       – **Drag an open end** = draw/extend pipe: it's just node-drag on the terminus,
         the duct auto-routes (verified: dragging end0 (0,1)→(2,2) draws an L-run, menu
         stays shut).
       – **Tap an open end** (pointer-up with no move, detected in `onUp`) opens an
         **"At the end of this run"** menu: Tool / Sliding gate / Ball valve / Manifold /
         Cap. `populateOpenEnd` splices the fitting where the end was (the 1-child open
         end then collapses onto its parent) and **re-seeds a fresh open end on EVERY
         outlet** — ball valve 1, manifold 2, sliding gate N — so each outlet is itself a
         drawable open run (no + handles to reach them). Verified valid-by-construction,
         no leaks: collector→manifold with one leg→Tool (terminates) and one leg→Ball
         valve→fresh open end.
       – **Branch dot gesture** (`onBranchDotDown` + threshold): a plain **click** still
         opens the two-section menu (insert gate / branch leg); a **click-drag** (>8px)
         calls `insertManifoldLeg` — splices a manifold at that point (existing run
         carries on via outlet 1, a fresh open-end leg sprouts on outlet 2). "Dragging =
         splitting flow = needs a manifold." Verified both paths.
     Removed: `onHandle`, `addFitting`, `addToOutlet`, the `handles` array + `.handle`
     glyphs, `Dir`/`DIRS`/`Handle`. `refreshHandles` kept as an empty hook so call sites
     didn't churn. Prod build type-checks clean; NOT committed (jeff commits).
   • **Duct-first round 3 — rounded corners, lanes, device/duct block, delete-heal (2026-08-02, jeff).**
       - **Rounded duct corners** (`ductD`, Q-arcs radius 12, clamped to half-segment) so two
         ducts whose corners land near the same point curve apart instead of forming an X.
         Crossover hops kept (radius 5). Verified arcs render.
       - **Injective lanes + no-cross-device (2026-08-02, jeff round 4).** The first lane
         pass keyed on `|colDist|`, so two runs off one source heading OPPOSITE ways shared
         a lane and still overlapped. `laneOffset` is now monotonic/injective in SIGNED
         colDist (`14 + (colDist+5)*7`) → every distinct target column gets its own lane;
         leftward runs jog shallow, rightward deep, crossings interlock (hops handle the X).
         Verified two swapped gate-outlet runs now sit at y=249 vs 263 (was identical).
         **No duct crosses a device**: `canPlace` now tests the CANDIDATE position (so the
         moved node's own ducts reroute) and rejects if any device ends up on any foreign
         duct — catches both "device dropped on a run" and "moved run now passes through
         another device"; verified both blocked, empty cells allowed. **Branch placement**
         (`clearCellNear`, used by branchDuct + output-dot add/drag) now prefers a cell whose
         leg won't land on a device/duct, falling back to merely-unoccupied; verified it
         skips an occupied side cell.
       - **Obstacle avoidance (2026-08-02, jeff round 5).** Ducts now route AROUND
         devices instead of over them. `ductPoints` = `baseDuctPoints` (the lane/jog
         geometry) wrapped by `avoidDevices`: for each polyline segment that runs through a
         device box (`deviceBoxes`, inflated ~15px, endpoints + junctions exempt) it jogs
         out past an edge and back (`detourSeg`), picking the side with the most canvas room
         whose detour lane is clear of other devices; iterates so a detour meeting a second
         device routes too (bails after 5). Clear routes pass through untouched (verified 2-pt
         straight runs stay 2-pt). Everything reads `ductPoints`, so the drawn path, branch
         dots, hit target, and the block all stay consistent. Consequence: the device-drop
         block was retuned — `canPlace` now uses `deviceCrossed` (foreign duct vs the TIGHT
         glyph box) instead of the old 0.4-cell test, so a device you drop where the run can
         bow around it is now ALLOWED (the duct reroutes); the block fires only when a run is
         genuinely boxed in. Verified: drop-on-run-with-room allowed + duct detours; jointer
         on a run gets skirted on the open side.
       - **Stable lane offsets** for collinear runs (jeff picked this over bands/router).
         `ductPoints` regular + gate-outlet cases now jog NEAR THE SOURCE at a lane height
         staggered by column distance (`laneOffset`), so the LONG vertical lands on the
         child's own (distinct) column instead of the shared parent column — and the short
         horizontals get distinct lanes too. Pure fn of the duct's own endpoints → adding a
         sibling never reroutes it (the trap the old fanning fell into). Verified: 3 runs off
         one collector land on cols 64/280/388 (was all 64); only a tiny shared stub remains
         at the source. Pathological same-column siblings can still overlap (accepted).
       - **Block device-on-duct drops**: `canPlace` now also rejects a target cell whose
         centre lies within ~0.4·CELL of another run's duct (`cellOnDuct`/`ptSegDist`), ducts
         touching the moved node exempted. A blocked drop just snaps back. Verified on/off run.
       - **Delete-heal**: `deleteSelected` now runs `collapsePassThroughJunctions`, so deleting
         a branched leg collapses the leftover tee and the run heals — fixes jeff's bug where
         the branch point became un-addable after add-then-delete. Verified back to `dc→g→saw`.
     Prod build clean; NOT committed.
   • **Duct-first round 2 — passive-branch drag + output add-dots + alignment (2026-08-02, jeff).**
     Three corrections after jeff tried the flow:
       1. **Branch-dot drag = PASSIVE branch, not a manifold.** Dragging off a mid-run
          branch dot now tees in a plain junction + open-end leg (`branchDuct(_, 'duct')`)
          you can extend/populate — the forced manifold was too opinionated. `insertManifoldLeg`
          removed. Click still opens the two-section menu. Verified: `g→wye→saw` + open-end leg.
       2. **Output add-dots restore "add off a node".** Retiring the + handles lost the
          ability to add runs off the collector or off a gate/manifold/slider's spare
          outlets. Fixed with `outputDots()` — a hollow ⊕ ring at every FREE output
          (collector always; each `blocked` selector outlet). CLICK → "Add here" menu
          (Duct/Tool/Sliding gate/Ball valve/Manifold/Cap); DRAG → passive open-end leg.
          `addAtOutput` reuses the open-end path (`addOpenEndOn(parentId, branchId?)` →
          `fillEnd`/`capEnd`). Consequently `fillEnd` (ex-`populateOpenEnd` core) now seeds
          just ONE continuation open end; a gate's other outlets stay blocked and surface as
          add-dots (a fresh sliding gate = 1 open run + 3 ⊕, not 4 dashed stubs). Verified:
          2 runs off the collector, tool onto a spare gate outlet, all valid + no leaks.
       3. **Alignment pass.** A gate/manifold now takes its trunk on the TOP, in line with
          its FIRST outlet (`ductPoints` unit case), so a parent above drops STRAIGHT in
          (was left-edge entry → left-then-down jog). Manifold glyph hub moved to the top
          (over outlet 0), fanning to the bottom outlets. Auto-layout places a lone unit
          child directly above its outlet 0 (not the span midpoint). Verified: cyclone→gate→
          wye→manifold→saw is one straight vertical. (Multi-run-off-collector auto-layout is
          valid but still spreads awkwardly — deeper layout work, drag to tidy.)
     All UI-verified + prod build clean; NOT committed (jeff commits).
   • **Open-run highlight relaxed to the END only (2026-08-02).** An open run used to
     render the whole duct as a dashed accent line, drowning the branch dots. Now the
     run is a plain pipe and only the LAST ~30px is a dashed-accent stub (`openStubD`) +
     an accent end-ring on the junction. Also **duct routing is now sibling-independent**
     (`ductPoints` drops the fanning that keyed off sibling count) — adding an element no
     longer reroutes existing ducts (verified: a duct's `ductD` is byte-identical before/
     after adding a third leg to its parent). Crossover hops still separate lines that
     actually cross.
   **Pass 2 BUILT (2026-07-30)**: snap-a-fitting mutation. Select a node → "+"
   handles appear on its free sides → a menu adds a fitting into the adjacent
   cell. **Valid by construction**: new selectors (sliding gate / ball valve /
   manifold) are added with all outlets CAPPED (`role:'blocked'`, needs no child);
   attaching a tool flips one cap to a tool branch (verified with a node harness —
   every op keeps `validateTopology` green). Inspector panel does rename + delete;
   delete is guarded (can't remove a selector that still has children; deleting a
   tool re-caps its branch). Save runs `validateTopology` client-side first and
   shows a friendly error instead of PUTting an incomplete graph. Defaults:
   sliding gate = 4 outlets (even), ball valve = 1, manifold = 2.
   **Sliding-gate widget rework (2026-07-30)**: a linear selector is no longer a
   single node — it renders as one horizontal UNIT spanning N cells with an outlet
   on the bottom of each cell (combs on the bottom only). Its tools LOCK into the
   cell directly below their outlet (drag the whole unit; tools are select-only,
   not independently draggable); the trunk to the collector enters from the LEFT.
   Per-outlet "+" handles (only under free/capped outlets) add a tool to that
   specific branch. Auto-layout went top-down to suit this (collector on top,
   tools hang below — matches jeff's original hand-drawn sketch). Verified: add to
   a specific outlet, delete frees an outlet, drag-unit moves tools in formation,
   Save validates.
   **Pass 2b TODO (polish)**: per-connection glyph orientation for the OTHER
   selectors (ball valve bore / manifold), and possibly give the manifold the same
   unit treatment; plus a chooser for sliding-gate outlet count (default 4).
5. **Tool-tagging pass** — ✅ BUILT (2026-07-30). `tools/tool-setup.component.ts`
   on silent route `/tools`. Stepper (one tool per card, progress dots), editable
   name, read-only gate chip derived from the canvas (`<selector> · outlet N`),
   "Smart outlet?" Yes/No (default Yes), identify-by-power scan list reusing the
   Phase 1 `drawLevel` tiers (green ≥5 W / amber 1–5 W / gray idle) + `discoverOutlets`,
   threshold slider (default 50 W, auto-suggests from a running plug's draw).
   Finish writes `sensor.outlet` onto auto tools / strips it from manual ones, then
   `putTopology`. Verified: picking a plug → Auto, No → Manual, round-trips to the
   Live view. Demo note: `discoverOutlets` reports 0 W (tools off) so the green
   tier only lights on real hardware — the tier logic is the tested Phase 1 code.

Retire the flat `device-model.js` once the v2 surfaces cover everything v1 did.

## Canvas backlog (jeff, 2026-07-30 — captured, not yet built)

1. **Export / import shop JSON** — ✅ BUILT (2026-07-30). Export/Import buttons on
   the build-canvas bar. Export downloads the full doc (topology + `ui.layout`) as
   `<shop-name>.json`; Import reads a file → JSON parse → `validateTopology` →
   `putTopology` → reloads the canvas. Bad JSON / invalid topology are rejected with
   a friendly error and leave the canvas untouched; an unsaved-changes confirm
   guards Import. Verified round-trip + both rejection paths in demo mode.

2. **Branch off an existing tube (duct)** — today you branch off a NODE; jeff wants
   to click a DUCT segment and insert a fitting mid-run (split the duct, drop a Y).
   Needs duct-segment hit-testing + a "split this duct" op (insert element between
   child and parent, re-wire the two ducts).

3. **Insert Y-junctions (passive splits)** — the `junction` element type already
   exists in the schema (passive merge/split, no actuation). Add it to the palette
   and the mutation ops. A Y just fans one inlet to N always-open outlets.

4. **"Unselectable" / always-open gates** — RESOLVED (2026-07-30). Decision: an
   always-open segment (a tool with no actuated selector between it and the
   collector) is a **permanent suction leak = an invalid saved config**, not a
   feature. It's fine transiently while editing, but a HARD BLOCK on save. Built:
   - `airflowIssues(topology)` in the shared model (`topology.js`) — returns the
     ungated tools; reused by configurator/firmware/conformance. 6 model tests.
   - Canvas Save runs it after `validateTopology`; if any leak, blocks with a
     red banner naming the outlets and offers **Cap them** — a bypass that inserts
     a closed ball valve above each leak, saves, and tells the user to wire a servo
     or delete the tool.
   - Tool-tagging needs no always-open handling: it only ever loads a *saved*
     topology, which by definition can't leak.
   Also fixed here: a gate/manifold outlet's "+" now opens the full fitting menu —
   an outlet can feed a Tool OR another selector (`feed` role: gate→ball valve,
   gate→gate), not just a tool. Feed sub-selectors are free-draggable; tools lock
   under their outlet.

## Branch off a tube = insert a Y (unified, BUILT 2026-07-30)

jeff's call: a Y-junction only ever exists AS a tee on a run, so there's no
standalone "Y" to place — **branching a duct IS inserting a junction.** One gesture,
no palette item. Built:
- Duct segments have an invisible wide hit-path; clicking one opens the fitting menu.
- Picking a fitting inserts a `junction` on that duct (rewiring `child→J`, `J→parent`)
  and hangs the chosen fitting off it as a new leg. If the duct's parent is already a
  junction, it just adds another leg.
- Works on any duct: the trunk (parent = collector, plain duct) tees two gated banks;
  a **gated outlet** (parent = selector) → the branch role flips to `feed` and the
  junction splits that outlet into a co-open group behind the one gate.
- Enabled by relaxing the schema: a `feed` branch may now feed a `selector` **or a
  `junction`** (routing already passes through junctions — verified). Junctions are
  delete-guarded (strip their legs first). The airflow check still governs — branch a
  bare tool onto an ungated point and Save blocks it.
- 71/71 model tests pass (incl. the feed→junction relaxation + airflow cases).

**Canvas drag/insert reworked (2026-07-30, jeff feedback):**
- **Locking removed** — every node (including tools under a gate's outlets) is now
  freely draggable. The old "tools lock below their outlet, drag the unit as a whole"
  model was too rigid; ducts elbow from each outlet to wherever the tool sits, so
  tools can go anywhere. Dragging a gate moves the gate alone (tools stay; ducts
  re-route). Auto-arrange still tidies everything back to below-outlet.
- **Junction glyph is now a small unlabeled dot** (schematic pipe-junction mark),
  not a "Wye" icon+label — a tee shouldn't shout.
- **Insert placement fixed** — branching a duct puts the junction in the child's slot
  (directly below its parent, i.e. UPSTREAM of what it feeds) and pushes the child +
  its subtree down one row (`shiftSubtree`), instead of scattering the tee below the
  tool it feeds. New leg lands beside the pushed-down child.

## Canvas backlog (remaining)

- **Duct routing / collisions** — ducts are drawn as simple elbows (child cell →
  midpoint → parent cell) and can cross or overlap other ducts and nodes now that
  nodes move freely. Revisit with real orthogonal routing that avoids occupied
  cells (jeff flagged 2026-07-30).
- **Convert a junction → a gate** — a passive tee should be promotable to an
  actuated selector in place (you tee in passively, later decide to gate it),
  without redrawing. Swap the `junction` element for a `servoGate`/etc., keep the
  legs (they become the selector's branches/feeds) (jeff flagged 2026-07-30).
- Per-connection glyph orientation (pass 2b) — rotate bores/combs to face their duct.

## Firmware topology-awareness (staged)

The ESP32 firmware is still entirely Phase-1 (flat stops+outlets, one linear
stepper, `SmartOutletControl` polling Shelly plugs → a flat `stopIndex`, NVS
config). v2 needs a richer layer: multiple selectors (stepper + servo bank),
tools mapped to branches, the routing engine, and the make-before-break sequencer.

Staged plan:
1. **`TopologyRouter` (routing brain)** — ✅ BUILT (2026-07-30).
   `linear_actuator/control/TopologyRouter.h`: a pure, ArduinoJson-based C++ port
   of `routing.js` — `computeRouting(topology, activeToolsInPriorityOrder)` →
   per-selector target stateId + reachable map, plus `servoCommandAngle()`. No
   Arduino.h, so it host-compiles; `test/test_topology_router.cpp` cross-checks it
   against JS-derived expectations on the real fixtures (star/twoGates/feedChain) —
   15/15, run via `npm run firmware:router:test` in `tools/`. NOT yet wired into
   the firmware build (nothing includes it yet).
2. **Topology storage** — ✅ BUILT (2026-07-31).
   `linear_actuator/control/TopologyStore.h` persists `/topology.json` on the
   LittleFS ("ffat") partition — atomic temp-write + rename, a minimal structural
   check (parseable, one collector, one primary; the UI's `validateTopology` stays
   authoritative), 24 KB cap. Three routes in `HttpApiServer.cpp` mirror the
   `tools/mock-api.js` contract: `GET /api/v2/topology` (verbatim, 404 if none),
   `PUT /api/v2/topology` (chunked-body accumulate → validate → persist), and
   `GET /api/v2/status` (404 until configured, else the `statusView` shape at IDLE
   — every selector at its closed state, no tool drawing; live routing arrives in
   stage 3). Both boards build; flash 69.4% (router still unwired). NOTE: the
   status stub is the ONLY place the idle shape is hand-rolled without the router —
   stage 3 replaces it.
3. **`TopologyController`** — split into a desk-verifiable brain (3a) and a
   hardware cutover (3b):
   - **3a — decision core.** ✅ BUILT (2026-07-31). `control/TopologySequencer.h`
     (pure C++ port of `sequencer.js` — make-before-break `planTransition`, linear
     never breaks, dead-head detection) + `control/TopologyController.h` (pure port
     of `topology-device.js` — tracks tool watts → active set most-recent-wins,
     `reconcile()` → `{routing, plan}`, idle-HOLD, `toolForOutlet(host,ip)` maps a
     Shelly plug back to its tool). Both host-compile (no Arduino.h). This is where
     `TopologyRouter.h` finally gets wired into a consumer.
     `test/test_topology_controller.cpp` replays the JS device sim's stateful power
     sequences value-for-value — 23/23, run via `npm run firmware:controller:test`
     (or `npm run firmware:test` for router+controller). NOT yet in the device build.
   - **3b — hardware cutover (needs the rig).** Feed live power in by toolId (map
     each Shelly plug via `toolForOutlet`, reusing `SmartOutletControl`'s poll/push),
     then drive the plan's moves: linear selectors → the existing stepper
     (`positionMm` from the target state), servo selectors → `g_servos[servo.channel]`
     via `servoCommandAngle`. `deadHeadRisk` → switch the collector off instead of
     sealing. Replace the flat `stopIndex` path in `loop()`. Address linear selectors
     by `linear.channel` (parallel to `servo.channel`) — the controller must NOT assume
     a single stepper. Today only one stepper driver is wired (channel 0), so a shop
     with >1 linear selector is a provisioning/wiring task, not a code or model cap.
4. Retire the flat path once v2 drives hardware.

## Open questions (for feedback)

1. Does the sliding-gate comb read as one gate feeding several tools?
2. Is "turn the tool on, watch it glow green" an obvious way to link a plug?
3. Is the Live view's Auto vs Manual distinction clear without explanation?
