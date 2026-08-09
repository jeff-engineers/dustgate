# Duct routing: real pathfinder + stable drags + rightward growth

> **Status — implemented 2026-08-09.** `routing/{geometry,route-grid,router}.ts` landed;
> the five heuristics are deleted. Conformance cases R1–R8 live in
> `routing/router.spec.ts` (`npm run test:routing`, 27/27) and are drawn as validation
> mockups. Two decisions differ from the plan as written below:
>
> 1. **Tool ports are top, left and right — never bottom** (the plan said top only).
>    A trunk really does reach a machine from whichever side it runs down; withholding
>    the bottom port stops A* approaching from underneath, which reads wrong on a
>    top-down plan. Consequence: a run to a tool that isn't directly below now prefers
>    a 1-bend side entry over a 2-bend approach over the top.
> 2. **Costs are scaled ×4** (step 4 / turn 32 / used 24 / hug 8 / reuse −3). At the
>    plan's 1/8/6/2/−3 the reuse discount makes an edge cost −2, and a negative edge
>    weight breaks A*'s optimality guarantee.

## Context

The shop-layout editor (`dustgate-ui/src/app/build/build.component.ts`, ~1800 lines, the entire
feature) has three linked problems, all traceable to one root cause: **there is no stored route
geometry.** Only `cells: Map<id,{col,row}>` is state. Every duct polyline is re-derived from
scratch, on every change detection pass, by a stack of hand-rolled heuristics —
`baseDuctPoints` (4 hardcoded elbow cases, 364), `laneOffset` (405), `clearLaneY` (candidate-lane
scan, 423), `avoidDevices` (5-pass skirting, 477) and `detourSeg` (roomier-side guess, 517).

Symptoms the user hit:

1. **Nothing can be added to the right of the cyclone in the demo layout.** `outputDots()` line
   **985** gives the collector exactly one ⊕, drawn to its *right* but committing to cell
   `(col+1, row+1)`. In the demo the cyclone is at (0,0) and the 4-wide "Main gate" occupies
   cols 0–3 of row 1 — so the target cell is always occupied and `roomAt` (1038) greys out
   every menu item as "no room". The genuinely free cells (1,0), (2,0)… are never considered.
2. **Ducts lasso around obstacles.** Because the heuristics are local guesses, a small endpoint
   move flips which candidate lane `clearLaneY` picks, or which side `detourSeg` skirts, and the
   whole path — including the part nowhere near the obstacle — jumps.
3. **Dragging gets worse the further you drag.** `onMove` (721) writes raw pixel `dragX/dragY`
   with no snapping, and `nx()/ny()` read them, so the *entire* route re-solves continuously at
   pixel granularity. Nothing before the obstacle is held fixed, so the run degrades progressively
   as the pointer moves. On drop, `canPlace` (758) may silently refuse with no explanation.

Decisions taken: replace the heuristics with a real grid pathfinder; keep the drag permissive with
live red feedback rather than constraining where the ghost can go.

Intended outcome: ducts take sane, stable, obstacle-free orthogonal paths; dragging feels
predictable (the route ahead of the pointer changes, the route behind it does not); and the
cyclone can grow in any direction, including right.

## Approach

### 1. Extract geometry into a routing module

Pull the pure geometry out of the component into a new
`dustgate-ui/src/app/build/routing/` folder — the component is already at its size limit and
none of this needs Angular:

- `geometry.ts` — reuse as-is, moved: `segBoxHit` (497), `firstHitBox` (505), `simplifyPts` (540),
  `ptSegDist` (803). Plus the `Box`/`Pt` types.
- `route-grid.ts` — new: the lattice + A*.
- `router.ts` — new: `routeAll(scene): Map<childId, Pt[]>`, the single entry point.

`baseDuctPoints`, `laneOffset`, `clearLaneY`, `avoidDevices`, `detourSeg` are **deleted** —
`route-grid.ts` subsumes all five.

### 2. The pathfinder (`route-grid.ts`)

Half-cell lattice (`CELL/2` = 54px) over the board extent from `recomputeExtent` (1735).

- **Blocked nodes**: device footprints inflated by the existing `M = 15` clearance, built from
  `deviceBoxes` (456) / `halfW` (469) / `halfH` (620) — keep these three, they are the correct
  per-glyph extents. A duct's own parent and child are exempt, as today; junctions stay
  non-obstacles.
- **Cost function**: 1 per step, `+8` per turn (strongly prefers few bends), `+6` for a lattice
  edge already used by an earlier-routed duct (soft, not blocked — parallel runs separate on
  their own and this replaces `laneOffset`'s stagger), `+2` for edges hugging a device box.
- **Directional ports**: replaces the hardcoded parent-bottom→child-top assumption. Each endpoint
  contributes a port `{pt, dir}`; A* seeds a mandatory first/last step in `dir`. Ports per glyph:
  collector → bottom, left, right; unit (gate/manifold) → top inlet at outlet 0, plus one bottom
  port per outlet index; tool → top; junction → all four. The router picks the port pair with the
  lowest total cost. **This is what makes sideways and upward runs legal at all.**
- **Fallback**: if A* finds nothing (fully boxed in), emit the straight dogleg and let the caller
  mark the route as failed — used by the drag feedback in §4.

### 3. Determinism, stability and cost (`router.ts`)

- `routeAll` routes every duct in one pass, in a **fixed order** (sorted by duct child id), adding
  each result to the shared soft-cost map so later ducts deterministically avoid earlier ones.
- **Prior-route bias**: `routeAll` takes the previous frame's paths and gives a cost *discount*
  (`-3`) to lattice edges reused from the same duct's prior route. This is the direct fix for
  "the part before the obstacle keeps getting worse" — the solver has an explicit incentive to
  keep the already-good prefix and only re-solve near the change.
- **Memoize** on a scene hash (all cells + span + drag ghost cell). Today `ductD` (566) calls
  `ductPoints` for *every other duct* inside its own loop — O(n²) full re-routes per change
  detection pass. After this, one A* pass per actual layout change.
- `ductPoints(childId)` (449) stays as the component's accessor but becomes a map lookup into the
  memoized `routeAll` result, so `ductD`, `ductPath`, `openStubD`, `branchDots`, `cellOnDuct`,
  `deviceCrossed` all keep working unchanged.

### 4. Drag behaviour

In `onMove` (721) / `onUp` (726):

- **Snap the ghost to cell centres while moving** (round to grid, as `onUp` already does at 729)
  so routes re-solve only when the pointer crosses a cell boundary, not per pixel. The glyph may
  still track the pointer visually; the *routing* input is the snapped cell.
- **Only reroute the dragged node's own ducts.** All other ducts are held at their last committed
  paths for the duration of the drag (pass them to `routeAll` as frozen).
- **Live validity**: compute `canPlace` (758) for the hovered cell each time it changes; when
  false, tint the ghost and target cell red and show the reason in the existing hint bar
  (the "1 open end — drag it to run more pipe…" strip, template ~line 151 region). Drop on an
  invalid cell reverts as today, but the user has already seen why.
- Same treatment for the ⊕ / open-end drags: `onODotMove` (1004) and `onBDotMove` (1068) should
  track a live target cell and show the same red/valid feedback instead of silently doing nothing
  on `pointerup` (current bail at **1013**).

### 5. Growth in any direction

- **Collector add-dots**: replace the single hardcoded dot at **985** with one dot per free side
  (right, left, bottom). Each dot's candidate cell comes from a small helper
  `firstFreeCellToward(from, dir, span)` that scans outward from the node until `roomAt` (1038)
  passes, instead of a fixed `(col+1, row+1)`. In the demo this makes the right-hand ⊕ target
  (1,0) — free — so the menu enables. Same helper backs `targetCells` (832) and `legCellFor` (1050),
  so menu promises and placement stay in sync (they already share `targetCells` — keep that).
- **Negative growth**: `roomAt` rejects `col < 0 || row < 0` (1039). Keep the guard but add a
  `normalizeCells()` step (run after any mutation, alongside `recomputeExtent` 1735) that shifts
  every cell so `min(col) = min(row) = 0`. Left/up growth then works without a negative quadrant.
- **Auto-layout** (`autoLayoutInto`, 1751): keep the top-down `place(k, row+1)` shape and the
  cyclone-over-outlet-0 special case (1772) — it produces the straight drop we want. Two changes:
  route through the new ports so a wide unit's children no longer force the monotone cursor into
  cramped columns, and reserve a free cell beside the root so its side ⊕ always has somewhere to go.

## Critical files

- `dustgate-ui/src/app/build/build.component.ts` — all of the above; the only component touched.
- `dustgate-ui/src/app/build/routing/{geometry,route-grid,router}.ts` — new.
- `dustgate-ui/src/app/services/demo-topology.ts` — read-only reference for the default layout
  (it has no `ui.layout`, so it exercises `autoLayoutInto`).
- `docs/v2-ui-design.md` — append a dev-log entry for the router replacement, matching the
  existing log style (~lines 190–410).

Untouched: `shared/device-model/routing.js` is the **airflow** solver, unrelated to pixels.

## Verification

1. `cd dustgate-ui && npm run build` — must compile clean (strict mode).
2. Unit tests for the new router (no Angular needed — plain TS): a fixture scene reproducing the
   demo topology asserting (a) cyclone→gate is a straight drop, (b) no returned path segment
   intersects a non-endpoint device box, (c) re-routing after a 1-cell move of one node leaves
   every *other* duct's path byte-identical, (d) a route with an obstacle mid-span keeps its
   prefix identical to the prior route.
3. Start the dev server via `preview_start` and drive `/build` with the browser tools:
   - Load the demo layout, tap the ⊕ to the right of the cyclone — every menu item must be
     enabled, not "no room". Add a tool there and screenshot the sideways run.
   - Drag "Drum sander" slowly across the board past the tools; read the SVG paths via
     `javascript_tool` at several points and confirm the segments *before* the obstacle are
     stable and no loops appear.
   - Drag a node into a genuinely boxed-in cell and confirm the red feedback + hint text.
   - Auto-arrange, Undo, Redo, Save, then reload and confirm the layout round-trips through
     `docWithLayout` (1504) / `savedLayout` (1781).
4. Check `read_console_messages` for errors and confirm no change-detection perf regression
   (the memoization should make it strictly faster than today's O(n²)).
