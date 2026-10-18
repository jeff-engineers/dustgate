# dustgate-ui — working here

The root `CLAUDE.md` has the project rules. This file is the UI's.

## The two canonical pages

- **[`docs/mockups/canvas.html`](../docs/mockups/canvas.html) is the canonical canvas DESIGN** (glyphs, port and system
  rules, drag rules, dated `Built` pills). Read it before changing the canvas.
- **[`docs/mockups/decisions.html`](../docs/mockups/decisions.html) is the DECISION REGISTER**, rendered from one array
  at the top of the file. Read it before re-deriving a UI decision; append when one is settled — a decision that
  touches more than one screen always goes here.
- **Update them in place.** A settled question changes the canvas section it affects and adds a register entry — never
  a new page. A mockup for a screen not yet built lives beside them (today: `collector-manual.html`) and moves to
  `archived/` with a banner once the screen ships.

## How UI work is done

- **Mockup first.** For any non-trivial change, write an interactive mockup in `docs/mockups/` styled with the app's own
  tokens (`src/styles.css`), publish it for review, iterate on the same URL, and only then build. Jeff's feedback is
  visual and specific; code written before he has seen the screen gets redone.
- **Hover is never the only trigger** — this is a phone app. A `title` tooltip on anything ambiguous, as a habit, but it
  only supplements: what a person must know to act is visible text. Label fields visibly, not by placeholder.
- **Focus dims rather than hides.**
- **A failure says what is true** (D-84): `services/failure.ts` — "didn't answer" only when nothing answered, otherwise
  the brain's own reason — and every failure is logged to the console as `[DustGate] …`. Use `failureReason()` and
  `reportFailure()` in every catch that shows something; a list that failed to load is not an empty list.
- **Verifying on a phone is two stages.** Mine: the browser pane's mobile emulation, driving touch `PointerEvent`s. It
  cannot settle focus styles or iOS long-press. Jeff's: his phone, which needs the dev server bound to the LAN
  (`npm run start:mock -- --host 0.0.0.0 --port 4300`, then the Mac's LAN address) or a Vercel preview (a pushed branch
  builds one). Production (`vercel --prod`) is the public demo and is his to run. Say plainly what emulation proved.
- `src/app/build/build.component.ts` is ~6k lines — grep it, don't read it whole.
- Tests: `npm test`. A new `*.spec.ts` runs only once it is listed in `tsconfig.spec.json` and `spec-runner.js`.

## Canvas rules (decided)

- **New pieces default into the system you're working in** (`activeSystemId`); systems own contiguous row bands
  (`bandBlockedBy()`). A board belongs to no system: placed top-right of the active system (`defaultBoardCell()`).
- **A board stands on the canvas and owns its cell** exclusively; ducts route around it. A Pi brain is not drawn and
  holds no cell (`isHiddenBoard`).
- **A machine is ONE box, however many ports it has** (square = primary port, tapered = secondary). It owns no cell.
- **A tool lives in ONE system; only a SECONDARY port's run may cross the seam**, drawn grey, dashed and thinner. An
  unfinished run's stub is accent orange (`.open-stub`), not grey dashed.
- **No duct size anywhere.** **The plug belongs to the tool**, never a port. Where air leaves a gate it is a **port**;
  "outlet" means the smart plug only (D-83).
