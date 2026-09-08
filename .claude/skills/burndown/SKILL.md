---
name: burndown
description: Work through the findings in a TODO/review-*.md file — branch, fix in criticality order, one commit per fix, tick each off as it lands. Use when asked to "work the review", "burn down the TODO", "fix the review findings", or when there are spare tokens and no other task in flight.
---

# Burn down a review

Jeff runs this whenever there are spare tokens and nothing else in flight. The
shape is always the same, and the point is that each session leaves the tree in
a state the next one can pick up cold.

## 0. Before anything

Read the review file (`TODO/review-*.md`, newest first). It is grouped by zone
and ordered by criticality inside each zone, with `[C]`/`[H]`/`[M]`/`[L]` tags.

**A review is a snapshot, not a live list.** It names files and line numbers as
they stood at one commit. Check what has landed since:

```bash
git log --oneline <review-base-commit>..HEAD
git diff --stat <review-base-commit>..HEAD
```

If a file a finding names has changed, re-verify that finding against the
current code before touching it. Say so if it no longer applies — a finding
that fixed itself gets ticked off with a note, not a fix.

## 1. Branch

```bash
git stash push -u -m "wip"        # only if the tree is dirty
git checkout main && git pull
git checkout -b bugfix-<review-date>
git stash pop                     # if you stashed
```

Never work on `main`. If another session is running, expect the tree to move
under you — pull before branching, not after.

## 2. Pick in this order

1. **`[C]` critical** — physical damage, data loss, or a broken design rule.
2. Within a tier, **well-defined and low-test-burden first.** A one-line fix
   with a host test behind it beats a subtle one that needs Jeff at the bench.
3. **Skip anything that needs hardware Jeff has to sit in front of**, unless he
   says otherwise. Flag it in the summary instead. Homing behaviour, servo
   motion under load, and anything on a real rack are in this category.
4. Ask before anything Jeff has already decided about. A deliberate, dated
   trade-off in a comment is a decision, not a bug.

## 3. One commit per fix

Each commit does one thing and explains **why**, at the altitude of the
surrounding code — the house voice (see CLAUDE.md § Voice). A commit message
that would tell a stranger what broke and how the symptom pointed away from the
cause is the right length. Reference the review section (`Review §2.2.`) as the
last line so the reasoning is findable.

Say plainly what was and was not verified. "Compiles for xiao_c5_linear. Not
bench-tested" is a complete and honest statement; "fixed" on its own is not.

## 4. Verify per fix, not at the end

Use the `verify` skill to pick checks. Roughly:

| Changed | Run |
|---|---|
| `shared/device-model/` | `cd tools && npm run model:test` |
| `firmware/control/`, `firmware/utils/` | `cd tools && npm run firmware:test` |
| a JS↔C++ pair (CLAUDE.md's table) | **both** sides, always |
| `dustgate-ui/src` | `cd dustgate-ui && npm test` |
| firmware that has to build | `PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e <env>` |

A red suite means stop and fix it, not carry on and mention it.

## 5. Tick it off

When a finding lands, **strike it from the review file in the same commit as the
fix** — replace the section body with a one-line `**FIXED <date>** — <commit
subject>` and keep the heading, so the numbering the other sections cross-
reference stays stable. Do not delete the heading and renumber; other findings
point at these numbers.

If a whole zone empties, say so at the top of the zone rather than deleting it.

## 6. Finish the session honestly

End with a short summary that says:

- what landed, one line each, with the commit subject
- what was deliberately **not** taken, and why (needs the bench, needs a
  decision, superseded)
- whether the suites are green

Do not push or open a PR unless asked. Do not flash a board — that is always
Jeff's call.
