# Optional boards — a plan (2026-10-07)

**Status: BUILT 2026-10-08 (branch `servo-range`), host-tested, not yet seen in the shop.** Asked for by Jeff after the first day
in the shop on the Pi: the planer's sensor board is powered only while the planer is plugged in, and every time it goes, the shop
says something is wrong. Built: the rule in both engines (`isOptionalBoard`, `controllerIdForHost`), no `board-offline` for an
optional board, the beacon hurried only by required boards, the Boards screen's grey "Off · optional" row (no mockup — Jeff:
"we can tweak it later"), and `intermittent` deleted. Step 4 needed no code: the stale-is-off test already pins it.

## The rule

> **A board is optional when nothing the shop does depends on it being on.** Losing it costs a reading, never a gate or the blower.

Jeff's wording was "any node without a gate/slider connected should be optional". One addition makes it safe: the collector's
**transmitter** board (`control.rf.controllerId`) is required too, gate or no gate — without it the blower never starts. So, from the
layout alone:

| The board carries… | Optional? | What losing it costs |
|---|---|---|
| any gate (servo or slider) — a selector whose `controllerId` is this board | **no** | a tool that cannot get air |
| the collector's RF transmitter | **no** | a blower that never starts |
| only sensing: a clamp, plugs it polls, the dust-bin beam | **yes** | that tool reads as off; the bin goes unwatched |
| nothing in the layout at all (paired, unused) | **yes** | nothing |

**Derived, not chosen.** The layout already holds every fact the rule needs, and a derived rule cannot go stale when a gate is moved
to another board. That retires `controllers[].intermittent` (topology.js) — validated since 2026-09-15, read by nothing, and a
setting a person would have had to know to turn on. Delete it (no backwards compatibility: one shop). If an override is ever wanted
("tell me when this sensor board is off"), add it then, as the exception rather than the switch.

## What changes

1. **The rule, once, in both engines** — `isOptionalBoard(shop, controllerId)` in `shop.js` ↔ the same in `control/Shop.h`, paired
   tests (`optional-board.test.js` ↔ `test_shop.cpp` block), a row in CLAUDE.md's pair table. Every consumer below asks it; nothing
   re-derives it.

2. **No alarm for an optional board that is off** (`control/DeviceProblems.h`). Today `board-offline` ("Not linked for Ns…") fires
   for every board after 20 s. For an optional board, raise nothing. Still raised for an optional board: `board-fault` (a board that
   links and then reports a fault is broken, not off) and the too-old-to-dial-in warning.

3. **"Off" is a state the Boards screen can show calmly** (`/boards`). An optional board that is down reads grey — "Off · powered with
   its tool" — not red, and does not count toward "Needs attention". A required board keeps today's red. **Mockup first**
   (docs/mockups, then the decision register): the row, its words, and how the Boards header counts it.

4. **Its readings, when it is off.** Already right, and the plan is to keep it so with a test: a clamp on a board that has gone
   away reads stale, and stale is OFF (`TopologyRuntime::pollSensors`, RFC §5.6a "absent means off"). The tool's gate is not
   opened for it and the blower is not started for it — the safe direction, since the opposite default runs the collector forever.
   Add the end-to-end case to `native/test/e2e.sh`: a fake node with a clamp disconnects mid-"on", the tool goes off within
   `SENSE_STALE_MS`, no problem is raised.

5. **Plugs it polls** (`plugOwners()` ↔ `plugOwnerOf()`): a plug a sensor-only board polls on the brain's behalf falls back to the
   brain while that board is off. Check the native and ESP32 paths both do this today (`syncPlugs()` keys on `nodePlug()`); if one
   does not, that is the bug to fix, not a new feature.

6. **The beacon**: `NodeHub::anyDown()` (native) and its ESP32 twin beacon every 5 s while ANY node is down. An optional board that is
   off for days would keep it fast forever. Count only required boards. (A planer board that powers up still finds the brain at once
   — it dials its cached address first; the beacon is for a brain that moved.)

## Jeff's answers (2026-10-08)

- A sensor board powered from a tool's SWITCH (rather than its plug): not expected. The ~3 s boot-to-SENSE lag stays unaddressed.
- Noticing a dead optional board ("not seen in 7 days"): probably not needed.
- Boards tied to a COLLECTOR — its clamp, its bin beam, its transmitter — are never optional. (The rule above already said so.)

## Order

1 → 4 (tests that pin today's behaviour) → 2 → 6 → mockup → 3. Steps 1, 2, 4 and 6 need no hardware and no node reflash: the brain
alone decides what is optional. Bench check afterwards: unplug the planer with the shop running and confirm no problem, a grey row,
and the planer's gate never opening by itself.
