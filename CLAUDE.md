# DustGate

Automated dust collection for a woodworking shop. When a tool draws power (sensed by a smart plug or a current clamp),
the brain opens that tool's blast gate and starts the collector. The shop is laid out once on a canvas in a phone
browser, and the brain routes from that layout.

This file holds **rules and current facts**; the dated stories behind them are in `TODO/DONE.md` and git history.
See "Keeping it current" at the end before adding to it.

## Hardware status

**Never call anything verified on hardware unless the "What has run on hardware" section of
[`README.md`](README.md#what-has-run-on-hardware) says so** — read it before making that claim. Everything compiles and
passes host tests; that is not the same thing. When a bench verifies something, add it there, dated, in the same session
(see "Keeping it current").

## Working here

- **Flashing and bench tests are Jeff's call.** Confirm before anything that writes to a board (`dev.sh flash`,
  `flash-node`, `ota`, `erase`, `provision`), and run flashes in his terminal where he can answer the prompts — never
  non-interactively (a flash that hit EOF on a prompt once left the primary with no layout). The Pi deploy needs his sudo
  password, so he runs it. The `flash` skill has the traps.
- **Commit only when Jeff asks for that commit**; branch off `main` first. No "Generated with Claude Code" line in
  commits or PR descriptions — the commit log is where he reviews. Turn on Auto-fix for every PR you open; merge only
  when he asks.
- **Never read `.env`** (`tools/.env`: the WiFi password and the API key) — not even one line. Ask, or read the
  equivalent off the device (`/api/info`, the boot banner).
- **A test brain gets an empty scratch state** (`mktemp -d`) and fixture layouts with no real IPs. A brain started on a
  copy of the real layout claims and repoints the real plugs.
- **No backwards compatibility.** One shop, one owner, cheap reflashes over WiFi: change the protocol or schema and
  update the boards, rather than adding fallbacks for old firmware or old layouts.
- **When it's unclear where something fails, add logging** before reasoning toward a guess.
- Procedures live in skills, not here: `verify` (which checks for what changed), `flash`, `burndown` (a review file),
  `wiring-diagram` (`docs/carrier-wiring.html`).

## Glossary — one meaning each

| Word | Means |
|---|---|
| **brain** / primary | the one decider: layout, routing, plug polling. The native brain on a Pi. The ESP32 primary is FROZEN (below) |
| **node** | any other board; owns fast local loops (a homing sweep, a clamp's RMS), interprets no layout |
| **board** | a XIAO C5 on its carrier, brain or node; named by `controllerId` in the layout |
| **system** | one collector and the ducts it pulls through; a shop has one or more |
| **machine** | the thing you switch on (a table saw); it has one or more **ports** |
| **port** | where air leaves a gate or enters a machine. Never "outlet" |
| **outlet** / plug | a smart plug (Shelly, Tasmota) only |
| **gate** / selector | a ball valve, manifold or sliding gate the brain moves |
| **clamp** | a CT current clamp on a board's jack |

## Layout

| Path | What |
|---|---|
| `shared/device-model/` | **The spec** — pure JS. `topology.js`/`shop.js` validate the layout and ARE its schema; routing, sequencing, the NodeLink protocol and the brain-as-simulation live here. Read its README before touching it |
| `firmware/` | ESP32 C++ (PlatformIO). `control/` is the brain core, shared with `native/`; `boards/` holds the pin maps (the truth for which pad does what) |
| `native/` | **The brain as a Linux/macOS program** around the same `firmware/control` + `firmware/outlets` code. `make -C native test`. `native/pi/` deploys it to the Raspberry Pi that runs the shop |
| `dustgate-ui/` | Angular app, served by the brain |
| `tools/` | `mock-api.js` (simulated brain), `mock-node.js` (simulated node), conformance runners, checks |
| `docs/` | `BOM.md` (parts), `carrier-wiring.html` (the PWM board's wiring), `mockups/` (two canonical pages). `docs/archive/` is history only |
| `TODO/` | `TODO.md` (open work) and `DONE.md` (finished work worth keeping, newest first) |
| `dev.sh` | Thin bash wrapper over PlatformIO/esptool for every bench workflow; its header is the reference |

## Anti-drift

`shared/device-model/` is the spec. The mock and the app's demo `require` it, so they cannot drift; the firmware can't
import JS, so it is held to the same contract by the conformance suites and by **matched tests** (a number asserted in
a JS test is asserted in its C++ twin, same cases, same order). **Change one, change both.** The matched pairs and the
constants shared across the boundary, and why each is a pair: `shared/device-model/CLAUDE.md`.

**The ESP32 brain is FROZEN (jeff, 2026-10-18): the Pi is the brain.** A $15 Pi does the job, DustGate is past the point
of being a product for anyone without reasonable tech skills, and keeping two brains in step kept costing shop time. The
ESP32 primary envs still compile (CI) and pick up fixes to the shared core for free; nothing new is built into its shell
(`firmware.ino`'s brain half, `HttpApiServer.cpp`), and nothing is bench-tested on it. Nodes are unaffected — every board
in the shop is still a XIAO C5 node. A rule that decides anything still goes in the shared code (`firmware/control/`,
`firmware/outlets/`), never in `native/src/`, so the frozen brain can be revived as a thin shell rather than a rewrite.
`tools/check-brain-parity.js` (`npm run brain:parity`, run by `firmware:test`) still fails if the native brain stops using
something the ESP32 does (that is the Pi missing a feature); what the Pi has and the ESP32 lacks is listed, not failed. The
topology contract suite runs against the mock and the native brain alike (`native/test/e2e_conformance.sh`). Firmware specifics:
`firmware/CLAUDE.md`. UI specifics: `dustgate-ui/CLAUDE.md`.

## Commands

```
cd dustgate-ui && npm test           # UI specs (plain node), routing, wiring geometry
cd tools && npm run model:test       # every JS model suite + the constant-pairs check
cd tools && npm run firmware:test    # every C++ host test
cd tools && npm run docs:check       # links in current docs, archive banners, CLAUDE.md size
make -C native test                  # the native brain, end to end against fakes
cd tools && npm run conformance:ci   # also topology:conformance:ci, nodelink:conformance:ci — run separately
```

Firmware builds and environments: `firmware/CLAUDE.md`. Deploying the Pi: `bash native/pi/deploy.sh <user>@dustgate.local`.

## Design constraints

Decided; don't relitigate them in review or suggestions.

- **Never dead-head the collector.** Make-before-break: open the new gate before closing the old. Most-recent-tool-wins.
  Idle leaves the gate where it is (the system rests open).
- **Gate count must be even.** Rockler manifolds ship in pairs.
- **Sensing is not switching; for a TOOL that is a safety rule.** A tool switched off at the outlet with its own switch
  left on is ARMED, so energising that outlet can start a saw with nobody at it. **Never add tool switching**; prefer a
  plug that structurally cannot switch (a no-relay metering plug, or our CT). The collector is the one thing DustGate
  commands, and nothing in its control path carries motor current: the RF remote, or a smart relay switching a
  contactor's coil. Every way of commanding it is a stateless toggle or blind, so **feedback (a clamp or a metering
  plug) is mandatory**.
- **An install step the owner cannot perform is a different product**, ranked above cost and elegance. Woodworkers are
  not likely to solder or call an electrician: the shipping answer to pressing the remote is a servo arm on a printed
  fixture. RF injection (HT12E) stays an option — proven on the bench, and a factory-modified fob satisfies the rule.
- **Never require anything a network is allowed to block.** mDNS may be a fast path, never a requirement; every
  discovery path degrades to a typed IP. A Tasmota has no mDNS (a set SetOption bit is not a feature): it is found by
  the subnet sweep. An unreachable name is not evidence the network blocks anything — check the board is powered.
- **The collector gets its own board** — its transmitter, clamp and bin beam all on ONE board, named by the collector's
  `controllerId` (absent = the primary's own; a Pi brain has no pads and raises `collector-no-board`). The press retry
  policy stays on the brain (`control/CollectorPress.h`): only the brain can read whether the blower agreed.
- **A node may own any control loop faster than a WiFi round trip, and no interpretation of the document.** It takes
  resolved angles and positions, never state names. The full statement is the header of `shared/device-model/nodelink.js`.
- **One brain** owns the layout, routing and plug polling. An arbitration rule: sticky enabled, never-steal-a-plug, the
  shop-wide servo mutex and most-recent-tool-wins all assume a single decider.
- **Canvas rules** (a board owns its cell, a machine is one box, ports vs outlets, no duct size): `dustgate-ui/CLAUDE.md`.

When a constraint changes, the decision register (`docs/mockups/decisions.html`) gets the dated reason; this list only
changes its line.

## Definition of done

1. The right checks ran and pass (the `verify` skill); firmware changes also build.
2. Docs and comments the change touches are current ("Keeping it current" below), and `npm run docs:check` passes.
3. The TODO entry, if any, moved to `TODO/DONE.md`.
4. The PR description says what is NOT tested on hardware.
5. Nothing is called verified on hardware unless README's list says so.

## Keeping it current

Spending a few tokens during the work is cheaper than an audit afterwards. These are part of finishing any change.

1. **One home per fact.** Values live in code; rules here; work in `TODO/`; parts and wiring in `docs/BOM.md` and
   `docs/carrier-wiring.html`; UI in the two canonical mockups. Prose never restates a value that lives in code — it
   names the symbol. If you find the same fact in two places, delete one.
2. **A bench result updates everything in the same session:** README's hardware list (dated), the TODO entry (moved to
   `DONE.md`), and every comment or doc that still calls it unverified —
   `git grep -n -i -E "unverified|not yet run|never run|untested|host-tested only|not seen in the shop"` for the
   feature's name.
3. **Finishing a feature tidies its paperwork in the same commit:** its TODO entry moves to `DONE.md`, its plan to
   `docs/archive/` and its mockup to `docs/mockups/archived/` (each with a banner), and a register entry if a decision
   was made.
4. **No plan documents in `docs/`.** A plan lives in its TODO entry or the PR description. A design doc that must exist
   opens with a dated Status line and is archived when built.
5. **Renaming or deleting anything includes `git grep` for the old name** — code, comments, this file's tables, the
   READMEs, `TODO/` — in the same change.
6. **This file holds rules and current facts, not history.** "Found on <date> when…" belongs in the commit message or
   `DONE.md`; here it is the rule plus one line of why. New code comments say why the code is this way, not what was
   tried first.
7. **Hardware results go in the repo** (README's hardware list), not only in an assistant's memory, so every session and
   every person sees the same truth.
8. **Size budgets:** this file stays under 200 lines and each directory's `CLAUDE.md` under 100 (`docs:check` enforces
   it). Detail that only matters in one directory goes in that directory's file.
9. **`docs/archive/` and `docs/mockups/archived/` are history.** Never update them, never cite them as a spec; a fact
   found only there must be re-checked before it is relied on.

## Voice

Comments and docs explain *why*, at the altitude of the surrounding code, and say plainly when something is unverified
or a known compromise. Keep replies and docs short: the result first, then what the reader needs to act.
