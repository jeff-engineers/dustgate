# Putting the brains back on the canvas

**Status: DONE 2026-08-16**, in the order 1 → 2 → 4 → 3. Written 2026-08-15, at the
end of the session that built the pinned rail — which this plan largely deleted.
Verified against the demo in a browser; nothing here has run on hardware.

Kept as the record of WHY, which the diff doesn't carry: the rejected alternatives
(pinning a board to a row, a drop-check rule, letting a board float above the grid)
are argued below and should not be re-proposed without new evidence.

What the steps actually became:

| Step | Landed as |
|---|---|
| 1 | `boardCells: Map<id, Cell>`, its own map beside `cells` — see below. `BOARD_W/H` moved to `routing/geometry` and `board` joined `Glyph`, so the router sizes a board like any other piece. `boardShade()` re-keyed to pairing order. |
| 2 | `defaultBoardCell()`, called only from `ensureBoardCells()` at the three seams where a board becomes known (load, import, merge) — never from `boards()`, which renders. |
| 4 | Rail, slot pitch, pin translate, scroll listener, scrim, free lane, negative-origin viewBox, `netName`, `+ Find boards` chip. The errand moved to the toolbar overflow. |
| 3 | `onCanvasContext()` → a two-row menu: put an unplaced board here, or go pair one. |

Two things the plan didn't anticipate:

- **Boards kept their own map** rather than moving into `ui.layout`. The plan said
  "entries in the same `ui.layout` map every other piece uses"; the reason they were
  separate has not changed — board ids come from mDNS hostnames and element ids are
  minted locally, so one flat map lets a board called `sel1` take a gate's square.
  Same grid, same cells, same drop checks, two maps. The saved shape went
  `Record<id, number>` → `Record<id, Cell>`; a saved NUMBER is a dead rail slot and
  is dropped rather than converted, because a slot was an x along a strip and never
  named a row. Those boards fall through to the step-2 default.
- **W7 in the wiring spec had to be re-pitched.** It asserted every cable lane sits
  in the "top third of the drop", a bound that only held because the rail sat two
  bands above the shop. A board one row above its gates has a much shorter drop, so
  the assertion is now the upper half — which is what the claim was always about.

## Why

Boards were on the canvas once. They moved to a rail along the top because a brain
placed *below* the shop routed its wires badly. The rail fixed that and bought a
different problem: the other end of every cable is somewhere you can't see the moment
you scroll.

Four things exist only to pay for that:

| Thing | Where |
|---|---|
| The pin translate | `canvas-viewport.ts` · `pinShift()`, `scrollH` |
| The scrim, and its drag-quiet state | `build.component.html` · `#railscrim`, `.scrim` |
| The free-lane exemption | `build.component.ts` · `cableCost()` |
| Per-board cable shades | `build.component.ts` · `CABLE_SHADES` |

The original problem has a smaller fix than the rail was: **put the board top-right**.
Wires then run down-and-left into a shop that grows down-and-right, which is the one
direction the lane router already handles well. The left-rail auto-layout leaves the
top-right corner empty by construction, so there is somewhere to put it.

It is also truer to the hardware. A board is mounted where the cable run is convenient;
a strip along the top says boards live somewhere other than the ductwork does.

## The work, in order

1. **Boards get cells.** `boardSlots: Map<id, number>` becomes entries in the same
   `ui.layout` map every other piece uses. Retire `railSlot`, `slotAt`, `BOARD_SLOT`,
   `RAIL_X0`. A board drag stops being a one-axis reorder and becomes the ordinary grid
   drag `startDrag` already implements — including its drop checks, for free.

2. **Default placement: top-right of the ACTIVE SYSTEM's occupied extent** — not the
   shop's. `maxCol + 1` of that system's cells, on its top row, so a board added while
   working on the second collector lands beside *that* collector rather than back up at
   the first one's corner. `activeSystemId` already follows whatever you last touched
   (`focus()`), so there is no mode to read.

   A board is the one piece with no system of its own — it may drive selectors in any
   system (`shop.js` §controllers), and `systemOf` does not contain it. So this is a
   *placement* default only, and says nothing about membership. Nothing changes about
   which system a board's channels can reach.

3. **Right-click empty board → "Add new board".** The one genuinely new piece: the
   canvas background currently only deselects, so it needs a `contextmenu` handler that
   resolves the pointer to a cell and opens a menu there. (Right-click on a *piece*
   already opens its menu — commit `702c148`.)

4. **Delete the rail machinery.** `RAIL_H` and the negative-y band, the pin translate,
   the scrim, the free-lane branch in `cableCost()`, and `revealBoard()`'s rail
   special-casing. The viewBox stops needing a negative origin, which simplifies every
   coordinate conversion that currently subtracts it.

5. **Cables get simpler.** With a board a cell or two from the gates it drives, the
   high-lane nesting in `cableRun()` is mostly moot. Keep it — it is tested and
   harmless — but at that range the drop-column search and the device cost do the real
   work.

**Keep the per-board shades.** A short wire still benefits from being colour-coded at
40%, and they cost nothing. Note the open edge recorded when they landed: the shade
keys off the board's rail *slot*, so it will need re-keying (to the controller id, or
to the cell) once slots are gone.

*Done: keyed to the board's index in `controllers[]`, which is the order /boards
paired them in. Not the cell — a shade that moved when you dragged a board would
recolour every cable leaving it, which is a colour that means nothing.*

## Decided: a board owns its cell

**Yes — exclusively.** Decided 2026-08-15. A board is a real object on a real wall, so
it behaves like one: a piece can't be dropped on its cell, `placeBlockedBy()` treats it
as an obstacle, and ducts route around it. Being an obstacle is the whole point — it is
what stops a duct being drawn through the board.

The rejected alternative was letting it float above the grid the way a `pickup` does.
That keeps the cell's meaning unchanged but lets ductwork pass through hardware.

Consequences for the work above:

- Step 1 gets the collision rules for free — `startDrag`'s existing drop checks already
  refuse an occupied cell, so nothing board-specific is needed on either side.
- Step 2's default placement must respect occupancy like any other placement, rather
  than assuming its corner of the active system's band is free.
- Step 3's "Add new board" only offers itself on an *empty* cell, same as any other
  add.

## What not to re-buy

The reason boards left the canvas is recorded above: **brains placed low route badly.**
If step 2 is skipped or a user drags a board to the bottom of the shop, that returns.

**Decided 2026-08-15: do nothing about the drag.** Get step 2 right and leave the rest
to the user. The rail existed because the *default* was bad, not because users dragged
boards low, so fixing the default is the fix.

Nothing gets pinned to a row. The rail worked as a strip because it lived off-grid in
its own negative-y band; row 0 is ordinary canvas that shop pieces already occupy, and
pinning boards there — while a board owns its cell exclusively — makes boards and pieces
compete for one scarce row until a wide shop has nowhere legal to put one.

No drop-check rule either. What actually routes badly is a board **below the gates it
drives** — the tab sits on the gate's top edge, so a board underneath climbs past the
device bodies `cableCost()` prices at 100 apiece. That is relative, not an absolute row,
and phrasing it as a rule has two cases with no answer: an unwired board drives nothing,
and a board wired above *and* below has no legal cell. It also spends step 1's whole
return, which was that a board drag becomes an ordinary grid drag.

What's left is the drawing itself: cables render live during the drag, so a bad drop
shows as ugly wire in the same gesture. That is the feedback, and it costs nothing.

If that turns out to be too quiet, the next step up is an **advisory** guidance-bar
string — drop still succeeds. That needs a second channel: `dropBlocked` today is one
string that means *refused*. A hard rule is the step after that, and pinning is off the
table for the reason above.
