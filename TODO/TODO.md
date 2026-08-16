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
- **The network name lost its home.** It was captioned in the board rail; the rail
  is gone and `/boards` — where you actually pair — doesn't show it. The device
  reports it as `ssid` from `/api/motion-status`.

## Carried debt

- **A board is placed but never unplaced.** `defaultBoardCell()` gives every paired
  board a cell, so the empty-cell menu's "Put a board here" is nearly always greyed
  with "every paired board is placed". Either give a board a way off the canvas, or
  fold the row into "Find boards…".

## Testing
### Bench Testing

- Servo pin strapping issues - need instructions on how to test this again please.
- Claude - update this to list bench testing steps that I need to do, including instructions

### GUI Testing

- Same here for GUI