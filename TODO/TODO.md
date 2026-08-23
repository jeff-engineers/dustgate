# TODO

Jeff's parking lot. Add anything here rather than derailing whatever is in
flight — a line is enough, context can come later.

Anything with a plan behind it lives in `docs/` and is linked from here rather
than restated. Delete an item when it lands; the git history is the record.

## Bugs
- **wifi pairing between nodes and masters sucks** not very reliable. Bench
  observation 2026-08-19: the **XIAO C5 nodes hold a link fine**; the **Adafruit
  QT Py S3 does not**. Two boards, one NodeLink implementation, so the first
  question is whether this is the radio/antenna on that part rather than
  anything in `RemoteActuatorBus`/`dustgate_node` — which would make it the
  same decision as bench test 7 (Feather S2), not a code fix. Worth capturing a
  monitor log from both sides of an S3 drop before changing code.
- **A tool is overlapping a gate** — see `Screenshot 2026-08-16 at 7.06.02 AM.png`
  next to this file.

## UI

- **Highlight a validation problem ON THE CANVAS.** The message half landed
  2026-08-20 (`services/wip-message.ts`): the guide bar now names the piece and
  the system instead of `s2`/`p8`. What is still missing is the graphical half —
  every issue carries a `ref` (the element id), so the piece it is about is
  already known and could be marked on the board. Wants a mockup first; it is a
  new marking on the canvas, and there is no vocabulary for "this piece is the
  problem" yet.

- **Dragging a duct** to a gate is still triggering the 'Toolname is already in
  that cell' error message, this shouldn't happen ( we can put a warning
  that this will trigger an auxilliary port)
  
- **Highlight ducts and wires when hovering over them** might also trigger this on 
  hover of tools/gates/etc - aka "show the airflow/electron path"

- **The /tools and /gates pages should split by system, with anything unassigned 
  listed in an "Unassigned" section after the systems

- **We should probably expose /boards page** that behaves similarly to the tools and 
  gates page

- **Move all the setup buttons on the bottom of /shop to a dropdown menu** on the top
  right of the page

- **Moving the whole shop to a new WiFi is a per-board errand nobody is told about**
  aSettings → Forget WiFi resets the PRIMARY only. Each node holds its own
  credentials and has no way to be re-pointed from the app, so a router swap means
  visiting every board in the shop.

- **Add a 'Clear shop' button** Add this to the shop dropdown menu, go back to a single
  dust collector with no connections

  It is not as bad as it looks — a node runs the same `WiFiProvisioner` as the
  primary, so a board that can't join within 12 s **at boot** raises its own
  captive portal and can be re-pointed from a phone. No re-flash needed. Two sharp
  edges make that unusable as-is:

  - **Only at boot.** `maintain()` nudges `WiFi.reconnect()` forever while down and
    never falls back to the portal, so a node that was already running when the AP
    changed sits there retrying a network that no longer exists. You have to know
    to power-cycle it.
  - **Nothing tells you.** The primary can't see a node on a different network, so
    the shop just shows boards "not answering" — the same symptom as a dead board,
    a bad flash, or a hostname collision. Nothing says "these three are on the old
    SSID, go press reset."

  So the cheap version is probably not a fleet-wide push at all: it is `/boards`
  knowing an SSID change just happened and saying which boards haven't reappeared
  and what to do about each. A real push (primary stages new credentials over
  NodeLink before anything moves) is the ambitious version, and has an ordering
  problem — the primary can only reach the nodes on the OLD network, so anything
  that misses the message needs a defined fallback.

- **Hover tooltips on the canvas glyphs.** Primary vs secondary port, the 1–4
  numbered outlet icons, probably others. Low priority — and hover can't be the
  only way in (see the mockup rules), so whatever this becomes needs a tap path too.


## Carried debt

- **`POST /api/dustcollector/switch` is dead under a shop.** It drives collector
  slot 0 directly (`SmartOutletControl::setDcManual`), and with a topology loaded
  the main loop re-asserts every slot from `g_topoRuntime.collectorOn(systemId)`
  on every pass — so the switch is undone microseconds after it lands. It reports
  success the whole time, which is the worst way for an endpoint to be broken.
  `POST /api/collector {systemId?, on}` replaced it (D-59) and goes through the
  routing runtime, where the decision survives. Deleting the old route is
  phase-1 cleanup: the route, `_dcSwitchPending`/`consumeDustCollectorSwitchRequest`,
  its consumer in `firmware.ino`, and `ApiService.setDustCollector()` +
  `DemoApiService`'s override. `setDcManual`/`setCollectorManual` themselves STAY —
  the runtime is what calls them.

- **No right-click menu on a duct.** Every other thing on the canvas has one now.
  A duct would want "add a fitting here" — which the branch dots already do, at the
  same point, so it may be redundant — and "delete this run", which has no
  primitive behind it: removing a duct means deciding what happens to everything
  downstream of it. Left out deliberately until that question has an answer.

- **Hostname collision is guarded in one direction only.** `run_flash_node`
  refuses a node hostname that matches the primary's (`dev.sh`), but a primary
  flash will happily take a name a node is already using, and then the two fight
  over the same mDNS record. The primary flash now confirms the hostname on every
  firmware flash, so this is a prompt away from being catchable.

- **Nothing stops two NODES sharing a hostname either**, which is the harder and
  more valuable half. `next_node_hostname` suggests the next free-looking name,
  but nothing verifies it: flash two boards accepting the default and both answer
  to the same `.local`, the primary reaches exactly one of them, and the symptom
  is a node that "works" while its twin is silently dead. Worth thinking about
  whether an mDNS probe before flashing, a check against the saved topology's
  `link.host` values, or a hostname derived from the chip's MAC is the right
  answer — a MAC-derived default would make collisions structurally impossible,
  at the cost of names nobody can read.

- **Nothing stops two tools or plugs from sharing names** - a tool and a plug
  can have the same name, but not 2 tools or 2 plugs.  Really in general
  we need to make sure names are distinct, at least acros systems

-**clicking calibrate from the /gates page doesn't give a cailbrate option**
  routes to http://dustgate.local/#/gates
## Testing

Nearly all of DustGate compiles and passes host tests without ever having run on
a board. This is the list of what a bench session should actually prove, roughly
cheapest-and-most-unblocking first. Flashing goes through `dev.sh` — see the
`flash` skill for the traps, especially that a filesystem flash erases the saved
shop unless `dev.sh` does the backup for you.

Delete an item once it has genuinely run. "It compiled" is not a pass.

**This list is the critical path now.** Phase 2 merged to `main` on 2026-08-22 —
222 files, and the only things in it proven on hardware are the C5 driving four
servos (bench 1) and the SSD1306 screen. Everything else compiles and passes host
tests, which is not the same claim. The newest arrival is the sharpest example:
running a blower BY HAND (D-59) opens a path through the ordinary move queue
before it starts the collector, so it exercises exactly the code a bench session
would reach first — and no gate has ever moved for it.

### Bench Testing

**1. A node drives a real servo — no primary needed.** ✅ **XIAO C5: passed
2026-08-21** — all four PWM channels drive real servos, so the C5's pin map is
confirmed by movement rather than by Seeed's drawing. **The QT Py S3 has still
never moved one**, and it is the *default* node env (`dustgate_node`), so the
board most likely to be flashed is the one least proven. What remains below is
that board. A node has **no serial console** — it is a dumb bank that only acts on HELLO/PING/SET
over its `/nodelink` WebSocket — so `servo 1 90` on the primary's console moves
the PRIMARY's own pins, not the node's. The cheap isolated test is to be the
primary yourself, by pointing the NodeLink conformance runner at the real node:

```bash
bash dev.sh flash-node s3 dustgate-node
bash dev.sh monitor node          # watch the other side while it runs
node shared/device-model/nodelink-conformance.js ws://dustgate-node.local/nodelink http://dustgate-node.local
```
Pass: the suite is green AND servos physically move. Green with nothing moving
means the link works and the actuator doesn't — which is exactly the split this
test exists to make visible. Then walk all four channels and confirm each moves
its own servo.

No need to repeat this for the C5 — that is the part already done. Its pin map is
proven end to end (`firmware/wiring/xiao-c5.md` §6); if you do rebuild it by hand
it still needs `PLATFORMIO_CORE_DIR=~/.platformio-pioarduino`.

**2. NodeLink end to end — the primary commands a node.** Only after 1 passes;
if 1 fails, this can only tell you the same thing more expensively. On the C5, 1
now passes — but that does not settle this item, whose point is the *link*: a
primary resolving an angle and a node acting on it, then holding through a
primary that dies mid-move. Flash a
primary and a node, pair them, assign a gate to a node channel, and drive it from
the UI. Pass: the primary resolves the angle and the node moves. Worth watching
the wire: a node must receive resolved angles/positions and never state names —
that's the constraint that lets a $5 board be a node and keeps a schema change
from needing a flash to every board in the shop.

Also check the fail-safe deliberately, since it is the one that matters with a
tool running: kill the primary mid-move. Pass: every servo **holds**. No timeout
closing gates, no homing on reconnect, no autonomous behaviour at all.

**3. Certify real firmware against the conformance suite.** This is the one that
tells you whether firmware has drifted from `shared/device-model/` — the whole
point of the suite. DESTRUCTIVE (it homes, moves and wipes), so it refuses a
non-localhost target without `--force`:
```bash
node shared/device-model/conformance.js http://dustgate.local <api-key> --force
```
Should be green. If it isn't, it has found real drift, which is a result, not a
failure of the test.

**4. The three resilience fixes, all compile-only since 2026-07-28.** Each has one
specific thing to try:
- *WiFi auto-recovery* — pull the AP, wait, bring it back. Pass: it rejoins with
  no power cycle (`WiFiProvisioner::maintain()` nudges `WiFi.reconnect()` every
  10 s while down).
- *Own-IP-change recovery* — force a DHCP lease change. Pass: Shelly push
  recovers instead of dialing a dead URL forever
  (`SmartOutletControl::checkLocalIpChange()`).
- *Main-loop watchdog* — induce a hang. Pass: `esp_task_wdt` reboots it inside
  ~10 s.

**4b. The layout backup survives a RENAME.** The backup read the hostname being
flashed TO rather than the one the board answers to, so renaming a board could
never back itself up — it aborted instead (correctly) and the deploy stopped.
Fixed 2026-08-20 by probing the old name first (`backup_candidates()` in
`deploy.sh`), and verified only against a fake HTTP board. Pass: flash a board
under a NEW hostname with a shop saved on it, and confirm the layout comes back
after the reboot.

**5. The multi-system shop on hardware.** Model, firmware and UI all shipped
without a hardware pass. Draw a two-collector shop, save it to a real device,
power-cycle, and confirm it comes back intact and routes per system.

**6. The 4" Rockler profile.** BLOCKED — needs a built 4" slider. `rockler-4`
(pitch 127 mm) is derived by the same method that validated 2.5", never measured,
and stays disabled in the UI until one `calibrate rockler-4 <gates>` sweep
confirms it. Also still open: pitch uniformity past 2 gates.

**7. Feather S2 — a decision, not a test.** Unvalidated since Gen1 removal, push,
and the board abstraction. Either run it through steps 1–3 or mark it
experimental in `platformio.ini` and CLAUDE.md. Leaving it ambiguous is the worst
of the three.

**8. The wake button — proven on the C5, not on the other two.** A press on a
XIAO C5's D1 lights the panel (2026-08-22). What that leaves:
- **The toggle's off half.** Press-again-to-blank was written after that test and
  has never been pressed. `statusscreen::toggle()`; the pass is that a second
  press puts the panel out and the next real event still lights it.
- **The other two boards' pin arrangements**, below. The driver is proven; what
  isn't is each board's electrical choice.
- **DevKitC is the one to watch.** GPIO34 is input-only and has **no internal
  pull-up** — `INPUT_PULLUP` is accepted and does nothing — so that board carries
  an external 10kΩ to 3V3 and `WAKE_BTN_INPUT_MODE INPUT` instead
  (`wiring/devkitc.md` §5). Forget the resistor and the pin floats: the screen
  wakes at random, which reads as a firmware bug rather than a missing part.
- **QT Py S3 (GPIO37/MISO) and XIAO C5 (GPIO0/D1)** both use the internal pull-up
  and want a plain momentary to GND. The C5 pad is safe at reset (its straps are
  26/27/28), which is the whole reason D1 was chosen — worth confirming a held
  button doesn't stop it booting anyway.
- Pass: pressing it lights a blanked screen within a beat, and a button held down
  through reset does **not** light the screen at boot (`begin()` seeds from the
  pin for exactly that).

**9. What the screen work has and hasn't touched hardware.** Proven: the SSD1306
on a DevKitC 2026-08-21 (GPIO16 SDA / GPIO4 SCL, 0x3C) and on a XIAO C5 node
2026-08-22 (D4/D5), and the C5's wake button the same day. Not proven: the S3's
STEMMA-QT path, the other two boards' button wiring (item 8), the toggle's off
half, and the wiring docs' non-servo pin positions, which were written from
datasheets and pin maps rather than from a board on a bench. Treat the unverified
rows of `wiring/*.md` as a proposal until someone has built one.

**10. A XIAO C5 as a PRIMARY — BOOTED on hardware 2026-08-22.** First run of the
primary sketch with `HAS_LINEAR == 0`. What the boot log proved: it comes up,
`NullMotorDriver`/`NullFeedback` report clean (`linear=refused gates=ok
plugs=ok`), LittleFS mounts, the API server listens on port 80, servo bring-up is
ready. Two bugs it found, both fixed the same day — the FAULT indicator latching
on a board with no rack, and the pre-topology outlet→stop machinery running a
full move/arrive round trip against a carriage that doesn't exist.

**It serves the UI, and well** (2026-08-22) — the single-core worry did not
materialise: async web server, Shelly poll task and loop() together, and the page
is responsive. Servos move. That was the gate on retiring the other variants, and
it is passed; see [[c5-everywhere-architecture]] for the env cleanup that unlocks.

**Both scans work** (2026-08-23) — the setup wizard lists real Shelly plugs and
/boards lists the real node. Two bugs stood in the way and both are fixed: the
deferred-reply pattern 501s on the ESP32Async fork the C5 pulls in, and the mDNS
window (400ms plugs / 800ms nodes) was far too short for this network — 3000ms
finds everything first try. Details in the commit messages and MdnsQuery.h.

Still open on this board:
- **The collector refusal on the self-test has not been exercised.** All four
  channels sweep on both a primary and a node (2026-08-22), which also confirms
  the `_deenergize()` fix — repeated moves on one channel is exactly what the
  sweep does. What nobody has tried is holding the button *while the collector
  runs* and confirming it refuses rather than moving anything.
- **NodeLink from a C5 primary to a secondary.** The only part of the primary
  role untested. `[NODE] Paired nodes dialling: 0` is as far as it has been
  exercised.
- The API still advertises the linear vocabulary — position, homing, stops — to a
  UI on a board that has none. Now user-visible, since the UI loads.
- Routing a real gate end to end from a topology, rather than servo bring-up.

### GUI Testing

Everything here has run in the demo (`bash dev.sh demo`) and nowhere else. What
these need is the UI served off the device's LittleFS, against a real saved shop
— the demo can't catch a persistence or round-trip fault.

**1. Boards on the canvas round-trip.** New this session, and it changed the saved
shape: `ui.wiring.boards` went from `Record<id, number>` (a dead rail slot) to
`Record<id, Cell>`. Drag a board to a deliberate cell, Save, power-cycle the
device, reload. Pass: the board is where you put it. Also load a shop saved
BEFORE today — its boards carry numbers, which are dropped on purpose, so they
should land top-right of their system rather than stacking or vanishing.

**2. A board's cell is exclusive.** Try to drop a gate on a board, and a board on
a gate, and a board on a cell a duct runs through. Pass: all three refused, with
the thing in the way named in the guide bar. Then check a duct actually routes
around a board rather than through it.

**3. Gate setup saves each pane on its own.** The bug this fixed was `saved.emit()`
firing only from the calibration widget. Open a gate, change ONLY the board (or
only the name), Save that pane, close. Pass: the change survives — and confirm it
survives a device round-trip, not just the sheet closing.

**4. `pickBoard()` with a second board actually paired.** The defect was the fifth
gate drawn colliding on channel 0 while a second board sat idle. Pair two boards,
draw five gates, and confirm the fifth lands on the second board and the layout
saves. This one genuinely needs two boards paired on the device — the demo's
board list is simulated.

**5. Live view against real hardware.** `bash dev.sh live` (hot reload proxied to
the real device) and confirm the tool list, manual override, and gate state track
what the hardware is doing.