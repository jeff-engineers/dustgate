# Putting the brains back on the canvas

**Status: planned, not started.** Written 2026-08-15, at the end of the session that
built the pinned rail — which this plan largely deletes.

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

2. **Default placement: top-right of the occupied extent** — `maxCol + 1, row 0`.

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

## Decide before starting

**Does a board occupy its cell exclusively?** Recommended: **yes** — a piece can't be
dropped on it, `placeBlockedBy()` treats it as an obstacle, and ducts route around it.
It is a real object on a real wall, and being an obstacle is what stops a duct being
drawn through it. The alternative is that it floats above the grid the way a `pickup`
does. This changes what a cell means, so it wants an explicit call rather than a
default.

## What not to re-buy

The reason boards left the canvas is recorded above: **brains placed low route badly.**
If step 2 is skipped or a user drags a board to the bottom of the shop, that returns.
Worth deciding whether the drop check should discourage it, or whether it is simply the
user's business.
