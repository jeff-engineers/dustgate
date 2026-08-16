# TODO

Jeff's parking lot. Add anything here rather than derailing whatever is in
flight — a line is enough, context can come later.

Anything with a plan behind it lives in `docs/` and is linked from here rather
than restated. Delete an item when it lands; the git history is the record.

## Bugs

- **A system with a single tool and no gate is perfectly valid.** That's a tool
  the user wants driven automatically — say a sander with a shopvac connected.
- **All dialogs should have an [X] box**, so the user can close out of one
  without saving.
- **Wire mapping should penalize crossing over wires slightly more.**
- **Secondary ports are not being tied to secondary ports.** If a secondary port
  is added but the primary doesn't enter from the top, the second port gets
  centered.
- **Ducts should prefer to enter from the top of a tool.** Don't block the side
  entrances, just prefer the top.
- **A tool is overlapping a gate** — see `Screenshot 2026-08-16 at 7.06.02 AM.png`
  next to this file.

## UI

- **Show free ports in the board dropdown**, and sort boards by the one already
  driving this system. `boards/board-setup.component.ts` already computes
  `gatesOn()`; the picker in `gates/selector-config.component.ts` labels free
  *channels* but says nothing about a board before you select it.
- **Hide the left port on dust collectors** when the collector is in the
  leftmost column.

## Carried debt

- **`boardShade()` keys off the rail slot**, so it needs re-keying to the
  controller id or the cell once `boardSlots` is gone —
  `build.component.ts:boardShade`. Noted in the boards-on-canvas plan.
- **Boards back onto the canvas** — [boards-on-canvas-plan.md](../docs/boards-on-canvas-plan.md).
  Decided, not started. Step 4 deletes the rail machinery, so it wants a clear run.
