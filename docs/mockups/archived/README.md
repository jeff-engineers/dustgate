# Archived mockups

Designs that were **decided against**, built and later removed, or **superseded by
the canonical page**. Kept because the reasoning is worth more than the file:
knowing a thing was tried and why it lost is what stops it being proposed again in
six months.

Nothing in here is current. If you are looking for what the UI should do, there is
exactly one answer: **[`docs/mockups/canvas.html`](../canvas.html)** — the canonical
canvas design, with a dated decision log at the bottom.

Each page opens with a banner saying when it was archived and what replaced it.
Add that banner when you move something in; a page that looks current is exactly
the trap this folder exists to avoid.

## Why everything moved here on 2026-08-20

Seven mockups had accumulated, several of them holding decisions nothing else
referenced. Two questions got re-litigated because of it:

- **Cross-system runs.** `outlet-docks-multi-system.html` had already settled the
  treatment — a supplemental run leaves its machine's system drawn grey, dashed and
  thinner — and had it applied to the overarm pickup. Nothing else pointed at it, so
  the rule was re-derived twice and got cut entirely once before being restored.
- **What a dashed line means.** `secondary-ports.html` asserted that dashes were
  "spoken for" by unfinished runs. They are not: an open end's stub is accent
  orange. That wrong sentence survived because it lived in a page nobody re-read
  against the code.

Hence one page, updated in place.

| Page | Archived | Why |
|---|---|---|
| `board-rail-pinning.html` | 2026-08-16 | The board rail itself was deleted — boards became ordinary pieces on the grid, so there is nothing to pin and no band to fade. Replaced by `docs/boards-on-canvas-plan.md`. Moved out of `secondary-ports.html`, whose other panels were still current at the time. |
| `secondary-ports.html` | 2026-08-20 | Option A (one machine, two spigots) is now `canvas.html` §2. Its "when the pickup is on the other collector" section was partly wrong; the cross-seam rule in §2 supersedes it. |
| `outlet-docks-multi-system.html` | 2026-08-20 | The interactive plug/dock prototype. Its cross-seam secondary-run treatment is now `canvas.html` §1. |
| `outlet-dock.html` | 2026-08-20 | Superseded by `outlet-docks-multi-system.html` at the time, and now by `canvas.html`. |
| `multi-system.html` | 2026-08-20 | System grounds and the band rule are now `canvas.html` §1 and §3. |
| `two-system-shop.html` | 2026-08-20 | Early two-system prototype, overtaken by `multi-system.html` and then `canvas.html`. |
| `vertical-layout.html` | 2026-08-20 | Column-count study for the phone. The layout it settled is in the app; nothing here is still open. |
| `side-entry-hood.html` | 2026-08-20 | Exploration: where a pickup's hood is drawn when its run arrives from the side. Option A won (D-41) — both ports slide to the edge their duct arrives at, for consistency. Predates the primary/secondary port rename: "spigot" and "hood" here mean primary and secondary port. |
| `second-duct-gesture.html` | 2026-08-20 | Folded into `canvas.html` §4. Its claim that dashes collided with "unfinished" was wrong. |
| `outlet-rename-unpair.html` | 2026-08-21 | Built. The rename/unpair flow it settled is in the app. |
| `problem-marking.html` | 2026-09-07 | Exploration: how the canvas points at a validation problem. The halo won (D-66) over a corner badge and dim-the-rest, and it is orange rather than a new hue. Its proposal to mark overlapping DUCTS lost to preventing them instead. |
| `seam-insert.html` | 2026-09-08 | Exploration: what a drop in the gap between two systems should do. Accepted and built (D-71) — the drop opens a row rather than spending one. Two things changed on the way in: the push is not always one row (two when the bands already touch), and it is downward only, because a piece dragged UP is itself what closes the gap. Its demo runs the real rule, so it is still worth clicking. |
| `path-highlight.html` | 2026-09-07 | Exploration: tracing the selected piece's run back to its collector. The halo in the line's own colour won (D-68), which also beat this page's own opening argument for brightening. Continuing downstream was cut and is still switched on in the page, because the reason it lost is easier to see than to describe. |
