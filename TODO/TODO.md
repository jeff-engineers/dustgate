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

- **Error message in ui** "Work in progress — saved here, but the controller 
  won’t take it yet: system "s2": element "p8" must have exactly one parent 
  duct (has 0)." - the user has no idea what s2 and p8 mean here.  
  In fact neither does Jeff.  Need to use friendier names, and/or a graphical
  to highlight issues

- **A duct that crosses the seam to a second pickup — mockup Option A.**
  [docs/mockups/secondary-ports.html](../docs/mockups/secondary-ports.html),
  "What a second port looks like" → **A · One machine, two spigots**, which the
  mockup already marks as the pick. Read it before building; the decisions below
  are all recorded there and this is the third time the question has come up.

  **Option A:** the machine keeps ONE box, one name and one plug, and grows a
  second inlet of a different SHAPE — square spigot for the primary port, tapered
  hood for a pickup, which is what an overarm guard physically is. The role is
  captioned beside it. The run below it is solid, real ductwork, not a dashed
  relationship: dashes are already spoken for by an unfinished run.

  **It needs no schema change.** The duct crosses the seam in the DRAWING only.
  Structurally both its ends stay in one system — the pickup element and the gate
  feeding it both live in the second system; only the box it is anchored to is
  drawn in the first. So "no duct is shared between two systems, no element belongs
  to two of them" stays true and `validateShop` is untouched. (An earlier version
  of this entry called for relaxing that check. Wrong — nothing structural crosses.)

  **Control is decided, and already implemented.** The plug stays on the primary
  port, in the system the machine calls home; switching the machine on opens gates
  on BOTH collectors, because the saw really is connected to both. shop.js already
  works this way — see "Final per-machine verdict, across all its systems", which
  reports `routed` / `partial` / `stripped` per machine over every system its ports
  live in, with a lost SUPPLEMENTAL port degrading to `partial` and a lost PRIMARY
  being the alarm case.

  **Most of the drawing already exists**, from the same-system pickup: glyph
  `pickup`, `pickupSeat()` anchoring a hood to the machine's box with no cell of its
  own, and the router terminating at that anchor. The mockup names the no-cell part
  as A's real cost and notes it is confined to where a duct's endpoint is resolved —
  which is where it is confined today. `pickupSeat()` already looks the machine's
  primary port up shop-wide, so it works across systems as written.

  **What's actually missing:**
  - `addPickup()` hardcodes `const sys = this.sys()`, so a pickup always lands in
    the machine's own system. It needs to be able to target another.
  - The gesture: dragging a run end in the second system onto a machine drawn in
    the first, creating the pickup there. `bandBlockedBy()` refuses moving a PIECE
    across bands and should keep doing so — a duct is not a piece.
  - The open end's cell in `addPickup()` is clamped by `bandCeiling()` to stay in
    its own band. For a pickup fed from another system that clamp is the wrong
    band.
- **Show free ports in the board dropdown**, and sort boards by the one already
  driving this system. `boards/board-setup.component.ts` already computes
  `gatesOn()`; the picker in `gates/selector-config.component.ts` labels free
  *channels* but says nothing about a board before you select it.
- **Hide the left port on dust collectors** when the collector is in the
  leftmost column.
- **Secondary system right click**
  Once a second system is added, the right click context menu goes away because of
  the dark grey box behind the systems.  Also, please try to fix the grid pattern to
  show on the grey background as well, as long as it's aligned with the original grid
- **Wires crossing ducts needs to be less penalized** wires crossing other wires should be more penalized

- **Moving the whole shop to a new WiFi is a per-board errand nobody is told
  about.** Settings → Forget WiFi resets the PRIMARY only. Each node holds its own
  credentials and has no way to be re-pointed from the app, so a router swap means
  visiting every board in the shop.

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

## Deploy
- **Cant save layout** the 'saving the shop layout' step of the deploy doesnt work, presumably because it's trying to hit the current hostname not the previous - or it just doesn't work at all

## Carried debt

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

## Testing

Nearly all of DustGate compiles and passes host tests without ever having run on
a board. This is the list of what a bench session should actually prove, roughly
cheapest-and-most-unblocking first. Flashing goes through `dev.sh` — see the
`flash` skill for the traps, especially that a filesystem flash erases the saved
shop unless `dev.sh` does the backup for you.

Delete an item once it has genuinely run. "It compiled" is not a pass.

### Bench Testing

**1. A node drives a real servo — no primary needed.** Neither the QT Py S3 nor
the XIAO C5 has ever moved one; the whole secondary path is compile-only. A node
has **no serial console** — it is a dumb bank that only acts on HELLO/PING/SET
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
its own servo. Then repeat for `c5` — its pins are cleared (see
`firmware/wiring/xiao-c5.md` §5), and it needs
`PLATFORMIO_CORE_DIR=~/.platformio-pioarduino` if you build it by hand rather
than through `dev.sh`.

**2. NodeLink end to end — the primary commands a node.** Only after 1 passes;
if 1 fails, this can only tell you the same thing more expensively. Flash a
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