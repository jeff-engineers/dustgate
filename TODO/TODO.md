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

- **A SECONDARY duct type, allowed to cross systems.** Decided; not built. You
  should be able to drag a hose from one system across the boundary and land it on
  a port in another, and that action should be able to create the secondary port
  it lands on (low priority — landing on an existing one is the main case).

  Today `validateShop` (shop.js §ducts) rejects any duct whose child and parent
  sit in different systems. Read the comment there before touching it: that rule
  is what makes "systems share no duct" structural rather than conventional, and
  it is load-bearing for every per-system invariant below it. The job is to carve
  out an exemption for ducts explicitly marked secondary — **not** to delete the
  check, which would also let a 4" tool be plumbed into the 2.5" manifold by
  accident and leave nothing to catch it.

  A duct is `{ child, parent, parentBranch? }` today, so the mark itself is cheap.
  What isn't cheap is everything that assumes a system is a closed graph. Open
  questions, roughly in the order they bite:

  - **Where does a crossing duct live?** `sys.ducts` is per-system, and a duct with
    one end elsewhere is exactly what the current check catches. Either it lives in
    the system owning its CHILD and is marked, or cross-system ducts hoist to a
    shop-level array. The first is a smaller change; the second is more honest
    about what it is.
  - **Which system's view sees it?** `systemViews()`, `airflowIssues()` and
    `redundantSelectors()` all walk one system. A secondary duct appearing in both
    views double-counts; appearing in neither means it is never checked at all.
  - **Control semantics, and this is the real one.** If a tool can be fed from two
    collectors, which one starts when it draws power? Never both — that is a
    two-blower answer to a one-blower question and nothing in the router says which
    is right. Most likely the secondary run is a "this branch is served by the other
    collector" declaration and the router follows it to that system's blower. Needs
    deciding before any firmware moves. Note it interacts with never-dead-head:
    make-before-break is currently reasoned about one system at a time.
  - **The UI drag.** `bandBlockedBy()` refuses moving a PIECE across bands, which
    stays right. A duct is not a piece, so drawing one across the seam is a
    separate path that doesn't exist yet.

  Nothing here is blocked on the machines-with-ports model — a machine holding
  ports in several systems is a different feature that solves a different problem,
  and it is not what was asked for.
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

- **No right-click menu on a duct.** Every other thing on the canvas has one now.
  A duct would want "add a fitting here" — which the branch dots already do, at the
  same point, so it may be redundant — and "delete this run", which has no
  primitive behind it: removing a duct means deciding what happens to everything
  downstream of it. Left out deliberately until that question has an answer.

## Testing

Nearly all of DustGate compiles and passes host tests without ever having run on
a board. This is the list of what a bench session should actually prove, roughly
cheapest-and-most-unblocking first. Flashing goes through `dev.sh` — see the
`flash` skill for the traps, especially that a filesystem flash erases the saved
shop unless `dev.sh` does the backup for you.

Delete an item once it has genuinely run. "It compiled" is not a pass.

### Bench Testing

**1. XIAO C5 strapping pins — do this before wiring a servo to one.**
`boards/xiao_c5.h` takes its pin map from Seeed's published pinout, not from a
datasheet or a multimeter. Servo channels 2 and 3 are **GPIO8 and GPIO9**, and
nobody has confirmed those aren't strapping pins on the C5. A servo signal idling
on a strapping pin can stop the board booting — and the symptom is a board that
flashes fine and then looks dead, which reads as a bad flash rather than a bad
pin choice. That's the trap the QT Py C3's map had to dodge (GPIO2/8/9 there).

- Open the ESP32-C5 datasheet, find the strapping-pin table, and check GPIO8 and
  GPIO9 against it. Write the answer into `firmware/wiring/xiao-c5.md` §5 either
  way — the point is to stop re-deriving it.
- Then prove it empirically, one channel at a time, servo unplugged first:
  ```bash
  bash dev.sh flash-node c5 dustgate-node-c5
  ```
  Boot it bare and confirm it comes up. Wire ONE servo to D8 (GPIO8), power-cycle,
  confirm it still boots. Repeat for D9. A board that boots bare but not with the
  servo attached is the strapping failure, not a flash problem.
- While the datasheet is open: confirm four ADC pads are genuinely free. Same
  reason — the published pinout is the only source so far.
- If either pin is strapping, move the channel and update `boards/xiao_c5.h`,
  `firmware/wiring/xiao-c5.md`, and the pin table in both.

Note the C5 needs its own core dir and can't share a `pio run` with any other
env — `dev.sh` handles it, but by hand it's
`PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5`.

**2. A node drives a real servo — no primary needed.** Neither the QT Py S3 nor
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
its own servo. Repeat for `c5` once step 1 clears its pins.

**3. NodeLink end to end — the primary commands a node.** Only after 2 passes;
if 2 fails, this can only tell you the same thing more expensively. Flash a
primary and a node, pair them, assign a gate to a node channel, and drive it from
the UI. Pass: the primary resolves the angle and the node moves. Worth watching
the wire: a node must receive resolved angles/positions and never state names —
that's the constraint that lets a $5 board be a node and keeps a schema change
from needing a flash to every board in the shop.

Also check the fail-safe deliberately, since it is the one that matters with a
tool running: kill the primary mid-move. Pass: every servo **holds**. No timeout
closing gates, no homing on reconnect, no autonomous behaviour at all.

**4. Certify real firmware against the conformance suite.** This is the one that
tells you whether firmware has drifted from `shared/device-model/` — the whole
point of the suite. DESTRUCTIVE (it homes, moves and wipes), so it refuses a
non-localhost target without `--force`:
```bash
node shared/device-model/conformance.js http://dustgate.local <api-key> --force
```
Should be green. If it isn't, it has found real drift, which is a result, not a
failure of the test.

**5. The three resilience fixes, all compile-only since 2026-07-28.** Each has one
specific thing to try:
- *WiFi auto-recovery* — pull the AP, wait, bring it back. Pass: it rejoins with
  no power cycle (`WiFiProvisioner::maintain()` nudges `WiFi.reconnect()` every
  10 s while down).
- *Own-IP-change recovery* — force a DHCP lease change. Pass: Shelly push
  recovers instead of dialing a dead URL forever
  (`SmartOutletControl::checkLocalIpChange()`).
- *Main-loop watchdog* — induce a hang. Pass: `esp_task_wdt` reboots it inside
  ~10 s.

**6. The multi-system shop on hardware.** Model, firmware and UI all shipped
without a hardware pass. Draw a two-collector shop, save it to a real device,
power-cycle, and confirm it comes back intact and routes per system.

**7. The 4" Rockler profile.** BLOCKED — needs a built 4" slider. `rockler-4`
(pitch 127 mm) is derived by the same method that validated 2.5", never measured,
and stays disabled in the UI until one `calibrate rockler-4 <gates>` sweep
confirms it. Also still open: pitch uniformity past 2 gates.

**8. Feather S2 — a decision, not a test.** Unvalidated since Gen1 removal, push,
and the board abstraction. Either run it through steps 2–4 or mark it
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