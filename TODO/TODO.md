# TODO

Jeff's parking lot. Add anything here rather than derailing whatever is in
flight — a line is enough, context can come later.

Anything with a plan behind it lives in `docs/` and is linked from here rather
than restated. When an item lands, either delete it — the git history is the
record — or move it to [`DONE.md`](DONE.md), which is for the ones whose
reasoning was contested, or that a still-open item above leans on.

**Finished items go to `DONE.md`, not to the bottom of this file.** They lived
here until 2026-09-17 and had grown to 144 lines — longer than most of the
active sections above them, which is how a parked item stops being read.

## Bugs

- **"How does DustGate know it's running?" is not a yes/no question, but the
  control under it is Yes/No.** element-outlet-config.component.ts. The label
  used to read "Smart outlet on this tool?", which a Yes/No answers honestly; it
  was reworded on 2026-09-16 to cover clamps without touching the control
  beneath it, so the sheet now asks an open question and offers two answers that
  do not fit it. The tool sheet (tool-setup.component.ts) already asks this
  properly as a three-way — Metering plug / Current clamp / Nothing — so the fix
  is probably to make these one component rather than to reword this one back.

- **The outlet sheet shows "Scanning..." twice while a scan runs.**
  outlet-picker.component.ts says it in the empty-state line (:87) and again on
  the rescan button (:93), and both render together. One of them should go —
  probably the empty-state line, since the button is where the action is.

- **UI AUDIT 2026-09-17 — the same question is asked by two components, in two
  vocabularies.** Scanned every component for this; the findings are below,
  worst first. The pattern to copy is `selector-config`, which the canvas AND
  gate-list both open — one sheet, two entry points, no second copy.

  1. **A TOOL'S SENSING IS CONFIGURED IN TWO SHEETS, and only one of them knows
     clamps exist.** `tool-setup.component.ts` (Tools screen) asks it properly as
     a three-way — Metering plug / Current clamp / Nothing — with a board picker.
     `element-outlet-config.component.ts` (from the build canvas) asks Yes/No and
     can only ever write a plug. Both write the same `machine.sensor`. That is
     the root of the unanswerable-question bug above: the canvas sheet was
     reworded to cover clamps it structurally cannot offer. Merging these two is
     the single highest-value cleanup in the UI.

  2. **HALF OF `element-outlet-config` IS UNREACHABLE.** Its `mode` input takes
     'sensor' | 'switch', but build.component.html routes `outletMode ===
     'switch'` to `app-collector-setup` — so `isSwitch` is permanently false and
     every collector branch in that component is dead, the broken label
     included. Deleting the switch role would shrink it by roughly half before
     anyone tries to merge it with anything.

  3. **`tool-setup` REIMPLEMENTS the outlet picker** instead of using
     `app-outlet-picker`, which `element-outlet-config` and `collector-setup`
     both share. Its own scan button, empty state and "Scanning…" strings
     (:401, :406) are a hand-rolled second copy — and it owns a second set of
     scan flags to drive them.

  4. **`thresholdW` is edited in two sheets** (`element-outlet-config`,
     `tool-setup`) — the same split as (1), and it disappears with it.

  5. **A clamp can be paired from three surfaces**: dragged on the canvas
     (build.component), the Tools sheet, and the collector sheet. The canvas
     drag is the one the boards-dock rework is expected to retire; worth
     deciding that before adding a fourth.

  Not duplicated, and worth keeping that way: `paired-outlet-row` (shared by
  four), `outlet-picker` (two), `selector-config` (two), `servo-calibration` /
  `linear-calibration` (both owned solely by selector-config).

- **Wake on button push isn't working on nodes**

- **Brains seem to be keeping the screen alive full time.**

- **Dust collector - deadheaded - running with nothing open, stop it** 
  this state shouldn't exist, when the last tool is turned off it's gate should remain open
  there should be no way outside of manual user invervention to dead-head a system                      

- **loop() is still one enormous function — LANDED 2026-09-14, keep watching.**
  A stack protection fault on a collector board at boot, 2026-09-12:
  `SP 0x4085d060` against bounds `0x4085d068`, canary `0xabba1234` on the
  pointer, a half-built status JSON in the stack dump.

  The mechanism is worth keeping even though this is fixed, because it will
  recur: **loop() is a single function, so every `StaticJsonDocument` declared
  anywhere inside it reserves space in the SAME frame whether or not that branch
  runs.**

  Done: `SET_LOOP_TASK_STACK_SIZE(16 * 1024)` (was 8 KB), `sweepProbeOne()`, and
  then ping, rename and release pulled out into their own functions — a `<512>`
  and two `<256>`s that had been resident on every pass to serve requests that
  arrive a handful of times in a shop's life. **loop() now declares no
  StaticJsonDocument at all**; the four remaining documents in it are
  `DynamicJsonDocument`, which are heap.

  Also done, and the part that matters from here: the boot banner prints
  `uxTaskGetStackHighWaterMark()`. **Baseline measured on hardware 2026-09-14:
  13644 bytes free of 16384**, on a primary with a screen. A number that shrinks
  release over release is the warning nobody used to get — that is the whole
  reason it is printed, so compare it rather than glancing at it.

  What is NOT done: loop() is still ~1800 lines and will keep growing, and
  nothing enforces any of this. The next thing to extract when it bites is
  whatever has grown a document since.

- **Can the collector node run on ONE brick? (2026-09-11, decides a purchase.)**

  The collector node is being built as power topology **A** first —
  `firmware/WIRING.md#9-bin-sensor` §6 — two supplies, grounds genuinely
  separate. Not because A is better (it is two bricks at one machine, the worse
  install) but because it is the **baseline**: that board already carries three
  unvalidated changes in its CT section, and a shared ground underneath them
  would give any failure four candidate causes and nothing to compare against.

  **The real question is whether B works**, since B is the install anyone would
  want: one 12 V supply, a plain buck to 5 V, grounds common through the
  regulator. Once A gives a CT reading that cleanly separates a running blower
  from a quiet one, try B — same board, same clamp, same firmware, one thing
  different. If it matches, the install gets simpler for free.

  **If B is worse**, that is a real answer about shared grounds and the fix is
  topology C, which keeps one brick: an isolated DC-DC. Traco TMR 6-1211 (6 W,
  5 V/1.2 A) for a build with a fob servo, TMR 3-1211 (3 W, 600 mA) for RF-only;
  Mornsun URB1205S is the cheaper equivalent. Buy REGULATED — the 1–2 W
  unregulated parts sag under load, which is the failure mode a lumpy load
  produces. Acceptance test is one second with a meter: continuity between input
  and output GND reads open on an isolated module, ~0 Ω on a plain buck.

  **Buy nothing until B has actually been tried and failed.** A first, B second,
  C only with evidence.


- **DEFERRED 2026-09-16 (jeff): "we'll deal with CNC stuff when it comes down to
  it. I want to deal with getting my shop online, and my CNC isn't part of that."**
  Not a disagreement with anything below — the analysis stands and the design is
  written. It is a priority call: no tool in THIS shop idles loudly, so nothing
  here blocks a working shop, and it is the kind of feature that is better built
  against a real machine than imagined. Leave §5.4d as the record and come back
  when a CNC exists to test with.

- **A tool that idles LOUDLY needs a threshold, and the schema has nowhere to put
  one (jeff's friend, via jeff, 2026-09-16).** §5.4b concluded there is nothing
  to threshold, because every tool measured has a standby under the noise floor.
  **A CNC router breaks that**: servos at stall current plus a PC, drawing real
  continuous current while cutting nothing. The gate must not open for that, and
  the signal of interest is the step UP when it starts cutting.

  The shape, from §5.4d: **optional** `sensor.ct.thresholdA`, absent keeping
  today's floor-relative trip, **in amps and never watts** (§5.4c measured a PF
  of 0.66 on the collector — a watt threshold would be 34% out). It rides CONFIG
  to the node as a resolved number, which does not breach `nodelink.js`'s
  invariant: the node compares, it does not interpret.

  Two open bits beyond the field itself. **Where the number comes from** — asking
  a woodworker for amps is a poor screen, so a "learn while idling" button that
  records the draw and trips above it is likelier. And **hysteresis**, since a cut
  is not continuous; the collector's coast-down may already cover it.

  The work this displaces: `collector-doc.spec.ts` currently ASSERTS a CT-sensed
  element carries no threshold anywhere, and `test_nodebus.cpp` asserts the CONFIG
  sensor spec is exactly id+kind+channel. Both were right for §5.4b and both have
  to change.

- **~~The GUI sweep did not find a Tasmota~~ DIAGNOSED + HALF FIXED 2026-09-16.**
  **The device was never asked.** jeff confirmed the same sweep from the CLI
  found the plug, which exonerates the whole firmware side — prefix, the
  two-phase timings, `TasmotaOutlet::probe()`. Curling the plug directly returns
  a clean 200 with a scalar `StatusSNS.ENERGY.Power`, so it would have parsed.

  The canvas started a sweep only when something was unpaired
  (`if (this.unpairedTargets().length) void this.scanOutlets()`), and `showTray`
  hid the tray under the SAME condition — with the "Look for new" button inside
  it. So a fully-wired shop had no way to sweep at all, which is precisely the
  shop where you have just bought another plug.

  **Fixed:** a `Find plugs` item in the ⋯ menu, beside `Find boards`, which pins
  the tray open and runs the sweep regardless of the layout. And the empty-tray
  text now separates "nothing answered" from "all N already on machines" —
  those send you to opposite places, and reading the second as the first is how
  a sweep that WORKED looks like one that failed.

  **STILL OPEN:** which of the two actually bit him is unknown, because a plug
  already claimed in the layout is filtered out of the tray by design
  (`freeOutlets()`), and that also reads as "not found". Worth confirming
  against his real layout once the collector is back on WiFi — and worth asking
  whether a claimed plug should be shown greyed with its machine's name rather
  than hidden outright.

  The plug answered fine at **192.168.87.44** — found from bash, confirmed by
  `curl /cm?cmnd=Status%208` returning a full ENERGY block — and
  `POST /api/outlets/sweep` did not turn it up.

  **Not a range problem:** `OutletSweep.h` knocks on `<prefix>.1 .. .254`, so .44
  is covered. That leaves the timeout, the knock/ask split, or the prefix — and
  the prefix is worth checking first, since the collector was running
  DISCONNECTED from the shop system at the time (see below) and a board on a
  different /24 would sweep the wrong subnet entirely and report a clean miss.

  **This is the first real test of the two-phase sweep** ("Knock first, then ask:
  the sweep's one timeout was two jobs", 00d4676), which landed unverified
  because the plug was off the network that day. It has now been exercised once
  and failed once. A Tasmota has no mDNS to fall back on — the sweep is the ONLY
  way to find one — so a sweep that misses is a plug the UI cannot reach at all.

- **Bench context: the collector runs STANDALONE while the shop is replumbed
  (jeff, 2026-09-16).** Disconnected from the shop system, which is the most
  stable arrangement mid-replumb. Worth recording because it colours every
  measurement taken now: no NodeLink traffic, no nodes, one board being its own
  brain. The 0.195 A noise floor was measured in that state — so whatever is
  making it, it is NOT node chatter, and the quieter radio makes the number a
  floor-of-floors rather than a worst case.

- **~~Re-measure the CT on the rebuilt divider~~ DONE 2026-09-16.** Scale held
  (+1.50% vs the Tasmota, against +0.94% before), the screen's contribution fell
  from ~80% of the floor to 11%, and the floor underneath is the C5 ADC's own
  noise — shorting the CT out does not move it. **§5.5 is closed** (§5.5b), and
  the `Hz` column was found to report a fraction of the sample rate when fed
  noise, which wasted an hour. `ct-bench.md`'s parts table is no longer stale.

  What is left is optional and deliberately unbuilt: a 60 Hz demodulator would
  take the floor down 10-20x, and nothing needs it while a running collector
  sits 63x above it.

- **Pick a CHANNEL on a multi-channel Tasmota meter (2026-09-10; half done
  2026-09-14).** `TasmotaOutlet::doPoll()` now DETECTS `Power` as an array and
  refuses the plug loudly — unreachable, with a log line naming the EM2/EM6 and
  saying it will not guess a channel — instead of `as<float>()`ing an array to
  0.0 and reporting a working meter as a tool that is never on, forever.

  What is left is the actual feature: `sensor.outlet.channel` (absent = scalar),
  a UI to choose it, and the model/firmware pair to carry it. §6.0 of
  `docs/tool-sensing-rfc.md` lists what else it drags in.

  Still true, and the reason this is not a nice-to-have: **it bites on the FIRST
  EM2, not on the first ganged pair.** The device has two channels in hardware
  whether or not both are clamped, so it answers with an array either way. Until
  the channel work lands, an EM2 is a plug DustGate can see and cannot use — which
  is now at least a visible refusal rather than a silent zero.

- **Calibrate isn't reachable from the /gates page.** Opening a gate there
  (`http://dustgate.local/#/  gates`) offers no calibrate option, so the only way
  in is whatever other path still has one. Find where the entry point went and
  put it back on that page.


## UI

- **Replace drag-to-branch on a duct with "move this run here" (2026-09-07,
  jeff).** Today, dragging a branch dot tees in a passive leg. The more useful
  gesture is moving the RUN — put it where I want it and keep it there, the way
  moving a tee already works. That is also the honest answer to a run the router
  has drawn somewhere ugly: let the person say where it goes, rather than adding
  another rule to argue with. Needs a story for what "keep it there" means when
  something later moves under it.

- **No way to move a system, so the shop cannot be given breathing room.** Found
  2026-09-07 while rearranging the demo layout by hand: a system owns a contiguous
  row band, and there is no gesture for "push this system down" or for reordering
  two systems. Everything inside the band would have to travel with it. Related to
  the delete-a-system item below — both are missing verbs on a system rather than
  on the pieces in it.

  **Half the machinery now exists** (2026-09-08): `openRows()` in
  build.component.ts does the push, and D-71 drives it from a piece being dropped
  on the seam. What is missing is a way to ASK for it with no piece involved —
  dragging the seam itself, or a menu on the grey ground. Jeff called this
  not critical. It is also the only route to the UPWARD case D-71 deliberately
  left out: a piece dragged up into the seam is itself what closes the gap, so
  reopening one means moving the system above.

- **Move all the setup buttons on the bottom of /shop to a dropdown menu** on the top
  right of the page

- **Moving the whole shop to a new WiFi is a per-board errand nobody is told about**
  aSettings → Forget WiFi resets the PRIMARY only. Each node holds its own
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

- **No way to delete a system, or a collector.** You can add both and never
  remove either. A collector delete has one obvious guard — at least one must
  remain, since a shop with no collector is not a shop — and deleting a SYSTEM is
  the harder half: it owns a contiguous row band, and everything standing in that
  band has to go somewhere or go away. Decide what happens to the machines and
  gates inside it (delete with the system? move to the surviving system's band?
  refuse while it is non-empty?) before writing any of it. Related to the 'Clear
  shop' button below, which is the blunt version of the same need.

- **Add a 'Clear shop' button** Add this to the shop dropdown menu, go back to a single
  dust collector with no connections.

- **Finish the collector barrel (2026-08-25).** The glyph itself LANDED — the
  canvas draws a 76x76 violet barrel carrying its own name and its own plug row,
  the impeller is gone, and the router's footprint grew with it
  (`COLLECTOR_HALF`, geometry.ts). Three loose ends:
  - `docs/mockups/canvas.html` was never updated, so the canonical mockup still
    describes the circle. Update it IN PLACE and add a decision-log row, then
    move `docs/mockups/collector-glyph.html` (the exploration) into
    `archived/` with the banner that page's neighbours all wear.
  - The collector's live plug row is UNVERIFIED. `systemWatts`/`systemIdOf()` in
    build.component.ts feed it from `status.systems[].plug`, and nothing has ever
    had a plug paired to a collector while the blower drew current — so the green
    "412 W" state has been reasoned about, not seen.
  - The Live view's collector card still draws the old impeller spiral as its
    icon (live.component.ts, `.cyc`). The canvas and that card no longer agree,
    and the card is the other place a collector is drawn. -- I'm actually OK with this -Jeffs

- **Hover tooltips on the canvas glyphs.** Primary vs secondary port, the 1–4
  numbered outlet icons, probably others. Low priority — and hover can't be the
  only way in (see the mockup rules), so whatever this becomes needs a tap path too.

## Carried debt

- **OTA for nodes, so the shop is flashed once — and the numbers say it is nearly
  free. (jeff's goal, 2026-09-17.)** "I'd love to get to a point where the nodes
  can all be flashed, and I only have to maintain the primary once this is in the
  real shop." Measured today rather than estimated:

  | | image | slot it has | what OTA needs |
  |---|---|---|---|
  | node (`huge_app.csv`) | 1.35 MB | ONE 3 MB `app0`, plus 896 KB of spiffs it never mounts | two 1.6 MB slots — **fits the same 4 MB with room to spare** |
  | primary (`partitions-xiao-c5-primary.csv`) | 1.88 MB | one 2.62 MB `app0` + 1.44 MB `ffat` | 2×2.62 + 1.44 = 6.7 MB — needs the flash size raised to **8 MB, which the chip already has** |

  So a NODE can have dual-slot OTA today for the cost of a partition CSV. The
  table it has now spends its whole budget on one oversized slot and a filesystem
  nothing mounts. The primary needs `board_upload.flash_size = 8MB` as well —
  also free: `partitions-xiao-c5-primary.csv` fills 4 MB exactly and the board
  reports 8.

  `otadata` is already in BOTH tables, which is the rollback half and the reason
  a bad push does not brick a board.

  Delivery, in the order that costs least:
  1. New partition tables. Nothing else changes; boards keep working.
  2. The node PULLS over HTTP (`HTTPUpdate`) from the primary, rather than the
     primary pushing over NodeLink. A pull is a plain GET the ESP32 core already
     implements and can verify; a push means chunking a 1.35 MB image through a
     WebSocket we would have to write and get right.
  3. The primary serves it from its filesystem partition — the node image ships
     alongside the Angular bundle, so `dev.sh flash` updates both.
  4. Trigger over NodeLink, and only when it is needed: a node already reports
     its `fw` in WELCOME, so the primary compares and stays quiet when they
     match. That is what makes it "flashed once" rather than "reflashed on every
     boot".

  ⚠️ The node image must be built by the SAME commit as the primary serving it,
  or a shop drifts into two firmwares that disagree about NodeLink. Whatever
  ships the image should stamp it with the git sha the UI already reports.


- **Delete the three bench envs? (jeff, 2026-09-17 — deferred, not rejected.)**
  `xiao_c5_bus_bench`, `xiao_c5_ht12e_bench`, `xiao_c5_ct_bench`. Jeff's point,
  and it is the decisive one: **they run the same physical hardware as the
  normal builds.** They are not a different rig — they are a different *program*
  on the same board, and the thing that justified them was that the ordinary
  build could not reach the pad. That stopped being true on 2026-09-16: one pin
  map means every build has `PIN_CT` and `PIN_RF_TX`, and the serial console
  already carries `ct`, `press`, `rfscan`, `stroke` and `servo`.

  So the remaining argument for keeping them is narrow but real: `ct_bench` runs
  with **no WiFi**, which is how the noise floor was measured on 2026-09-15
  (5.80 counts against 6.03 with the radio up — see WIRING.md §8). A bench build
  that removes a known noise source is a measuring instrument, not a duplicate.
  `st3215_bench` sets servo IDs on a new ST3215, which is a real setup task with
  no other home.

  Deferred rather than done because the shop is half-commissioned and the node
  CT path is still unproven; deleting the instrument you would use to debug it
  is the wrong order. Revisit once a clamp has been trusted on hardware. Three
  envs, three `bench/*.cpp` files, and nothing else depends on them — the wiring
  docs were already merged (2026-09-17) without touching the code.


- **Six builds and counting — is the primary/node split worth it? (jeff,
  2026-09-11.)** There are now three primaries and three nodes (servo, slider,
  collector), and the pairs differ only by `-DDUSTGATE_SECONDARY` plus a
  `build_src_filter` that compiles a different sketch. Jeff's question: at what
  point is it simpler to make **everything a primary** and let a build flag say
  "this one is really a node"?

  It is a good question and the answer is not obviously no. The user-facing cost
  is real — six things to pick between, and the difference between them is a
  thing about OUR code rather than about their shop.

  What is actually load-bearing, so a future decision does not rediscover it:

  - **Partitions differ.** A primary carries the Angular bundle in `ffat`
    (`partitions-xiao-c5-primary.csv`); a node uses `huge_app.csv` and has no
    filesystem. Merging means every node ships a web UI it never serves, or the
    partition table stops being a property of the build.
  - **They are different sketches**, not different flags: `firmware.ino` vs
    `node/dustgate_node.cpp`. That was deliberate — see the rationale at the top
    of the node sketch — and a merged build would compile the routing brain,
    topology, plug polling and HTTP server onto boards that use none of it.
  - **8 MB of flash makes the size argument weaker than it was** (see the
    partition item above): the old "a node cannot afford the primary's code" may
    simply not be true any more, and nobody has re-measured it.

  A middle path nobody has costed: keep one BUILD per carrier (servo, slider,
  collector) and make primary-vs-node a **runtime** decision — NVS, or the ID
  resistor below — which would take six envs to three and let a board change
  role without a reflash.

- **Auto-detect the carrier with an ID resistor (jeff, 2026-09-11).** A resistor
  of known value to ground on an otherwise-unused pin, read by the ADC at boot,
  tells the firmware which carrier it is sitting on. One firmware, no
  `-D` flags, no picking the wrong env and chasing a dead servo.

  It is how plenty of hardware does exactly this, and it composes with the item
  above: carrier from the resistor, role from NVS, and the six envs collapse.

  **The wrinkle, which is specific to us:** D0 is the only analog pad on the
  edge, and on a collector build it is spoken for by the CT. Reading an ID
  there at boot does not work either — the CT's divider holds that pin at
  ~1.65 V, which swamps any ID value. So either the collector carrier is the one
  that has no ID resistor (detected by exclusion, which is fragile), or the ID
  moves to something that is not an ADC read — a pin strapped high/low, a
  one-wire EEPROM, an I²C part on the screen's bus. **The I²C option is probably
  the right one**: the bus already exists on D4/D5, a 24C02 costs cents, and it
  can carry more than an identity — a serial number, a calibration constant, the
  carrier revision.

  **DECIDED 2026-09-15 (jeff): buy nothing yet, and do this in two stages.**

  The framing that settled it is his: separate **what a board HAS** (servo count,
  bus servo, CT, RF, bin) from **what it IS** (primary or node). Those are
  different questions with different lifetimes and they were tangled together in
  nine `-D` flags.

  - **Stage 1 — capabilities become a RUNTIME STRUCT**, populated from the board
    header exactly as today. Nothing changes behaviourally; every `#if HAS_*`
    consumer stops asking the preprocessor. This is the stage that does the
    collapsing, it is pure software, and **it does not care where the data comes
    from** — which is precisely why no part needs buying to start it.
  - **Stage 2 — change the SOURCE**: NVS first, a chip later. One function.

  **What collapses and what does not.** `ENABLE_SERVO`, `DUSTGATE_SERVO_BUS` and
  `DUSTGATE_COLLECTOR` are pins and drivers, so they all go runtime.
  `DUSTGATE_SECONDARY` does **not**, and should not: it inverts
  `build_src_filter` into a different program with a different partition table,
  and keeping role at flash time means a board cannot accidentally become a
  second brain. One-brain is an ARBITRATION constraint (CLAUDE.md), not a size
  one. Endgame is **2 shipping builds plus the bench consoles**, from 6 — not the
  1 the "one firmware" phrasing above implies.

  **NVS over a chip while this is perfboard.** Its one real weakness is that it
  follows the MCU rather than the carrier, so a dead XIAO or a wipe means
  re-tagging — a thirty-second serial command, on a bench holding one of each
  board. The EEPROM's genuine advantage is that a carrier-mounted tag survives a
  reflash AND an MCU swap, and what varies IS the carrier; that advantage is
  worth paying for on a PCB and not on a crowded perfboard.

  **If a physical tag does happen: DS2431 in TO-92, parasite-powered** — 2 holes
  and a 4.7 kΩ pullup, one GPIO (D10 on the collector map, D0 on a node with no
  CT). The 24C02 recommended above is right on pin budget and wrong on SIZE: the
  DIP-8 does not fit the perfboards as they are built. Revisit at PCB layout,
  where a SOIC-8 on the existing I²C bus costs nothing and the pin stays free.

  **Do it AFTER the CT path works end to end**, so there is a running system to
  regression-test the refactor against.

- **A single-servo + CT board for 240 V tools (jeff, 2026-09-11).** Jeff has
  already built several single-servo nodes for testing, to save wiring time and
  cost. That is a fourth carrier: one gate, one CT, and the pin budget is easy
  because three PWM channels come back.

  The variant worth thinking about alongside it is **CT-only, no servo** — for a
  240 V tool where the gate is elsewhere. The argument is not cost: it is that a
  CT lead running from the tool to a board that also drives a gate is **a cable
  across the floor**, and a shop floor with a trip hazard on it is a worse
  product than one without. A CT-only board sits at the tool and talks over
  WiFi, so the only wire is its own supply.

  **The CT-only variant is now the live one (2026-09-14).** It has a name and a
  build: §5.6, the planer node — RS-25-5 off the unswitched side of the tool's own
  switch, clamp on the switched side, no actuators. What unblocked it was not the
  noise floor being solved but the question getting smaller: §5.4b closes the
  standby problem by finding there is nothing to measure, so a CT-sensed tool
  carries no threshold field at all and the baseline is the board's own floor.

  What it is waiting on is no longer measurement, it is PROTOCOL — NodeLink has
  no inbound sensor direction and a node is handed no config to describe a CT
  with. §5.6b has the three gaps.

  The single-servo + CT carrier above is unaffected and still undecided.



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

- **Every C5 partition table assumes 4 MB. The chip is 8 MB (2026-09-09).**
  `esptool flash_id` on the bench primary: `Detected flash size: 8MB`, on an
  ESP32-C5 rev v1.0. Both tables we ship stop at `0x400000` —
  `partitions-xiao-c5-primary.csv` says so in its closing line ("Fills 4 MB
  exactly"), and the nodes' `huge_app.csv` lays out the same 4 MB. **Half the
  flash on every board is unaddressed.**

  Worth more than the free space. That file carries a long comment weighing
  whether the slack should go to the app or to the LittleFS bundle, and reasons
  carefully to "the slack goes to the app" — against a total that was wrong by
  4 MB. The decision is not wrong, but it was never as tight as it reads, and
  the next person to size a partition will trust that comment. Fix the comment
  with the table.

  The bootloader already knows: the board JSON declares `flash_size: 8MB`, so
  only our CSVs are holding the line at 4.

- **OTA updates for our own boards — tabled 2026-09-09, jeff.** Feasible and
  roomy once the 8 MB above is claimed: dual OTA slots at the *current* app0
  size (2.62 MB each, 64% used) plus the 1.44 MB bundle is ~6.7 MB of 8, with no
  shrinking and no trimming the Angular bundle.

  Shape, if it gets built: `Update.h` behind a POST on our own `HttpApiServer`
  with an upload control in the UI — the same shape as Tasmota's firmware
  button. **Not `ArduinoOTA`/espota**, which advertises over mDNS and walks
  straight back into the constraint in CLAUDE.md ("never require anything a
  network is allowed to block"); our own HTTP endpoint works anywhere the UI is
  reachable, which is by definition the network it is on.

  Three things that are not incidental:

  - **A partition change cannot be delivered over the air.** Every board needs
    one wired flash to receive the new table. Free today — bench boards, shop
    not in service — and genuinely expensive once a node is mounted in a
    ceiling. That is the argument for doing it *before* the shop goes live, not
    when we happen to want it.
  - **Rollback is not free.** `Update.h` does not validate-and-revert by
    default, and without it a bad image on a node is a board that must come down
    and go on USB. That recovery cost is exactly what argued *against* writing
    custom Tasmota firmware (`docs/tool-sensing-rfc.md` §12.4), so it would be
    dishonest to accept it here without the rollback flag on from the start.
  - **The primary must refuse an update while anything is moving** or the
    collector is running.

- **The C5 has an 802.15.4 radio (2026-09-09).** `esptool` reports `Wi-Fi 6
  (dual-band), BT 5 (LE), IEEE802.15.4` on the board we already ship. Noted
  because the Zigbee/Thread half of the "other smarthome protocols" question in
  `docs/tool-sensing-rfc.md` §11 is not a hardware question — the radio is
  present. Still parked; the near-term job is one shop on one guest network.

- **No right-click menu on a duct.** Every other thing on the canvas has one now.
  A duct would want "add a fitting here" — which the branch dots already do, at the
  same point, so it may be redundant — and "delete this run", which has no
  primitive behind it: removing a duct means deciding what happens to everything
  downstream of it. Left out deliberately until that question has an answer.

- **The outlet picker exists TWICE, and they had already drifted (2026-09-09).**
  `tools/outlet-picker.component.ts` is used by the build canvas (through
  `element-outlet-config`); `tool-setup.component.ts` carries its own inline
  copy for the /tools list. Same interaction — identify-by-power, pick a plug —
  in two implementations, and they don't even agree on the words: "Scan again"
  vs "Rescan", "Which outlet is this one's?" vs "Turn X on — the outlet that
  jumps to green is the one."

  Found the honest way: adding the add-by-address field to the shared component
  changed nothing on /tools, because /tools does not use it. **It had to be
  written twice**, which is the whole argument for collapsing them. The next
  change to either one will hit the same wall, and there is no reason left for
  two — the inline version predates the extracted component.

  Fold `tool-setup`'s inline picker onto `OutletPickerComponent`. The extracted
  one is the keeper (it already has the `excludeIps`/`excludeReason` inputs);
  what needs porting into it is the plug-row styling and the "already assigned
  to another tool" wording that /tools uses.

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

- **Nothing stops two tools or plugs from sharing names.** A tool and a plug may
  share one; two tools or two plugs should not. In general names need to be
  distinct, at least across systems.

  **Wants a brainstorm before code (2026-09-07, jeff).** The questions that have
  to be answered first: is uniqueness per-shop or per-system; which KINDS collide
  (tool vs tool, plug vs plug, gate vs gate, board vs board — and do a tool and a
  gate collide?); what happens to shops already saved with duplicates; and whether
  a duplicate blocks the rename, warns, or auto-suffixes. Names are typed in
  several places (the canvas rename, the tool sheet, the Boards screen), so
  wherever the rule lives it has to be one rule, not four.

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

**2. NodeLink — the happy path passes, THE FAIL-SAFE HAS NEVER BEEN TRIED.**

The link itself: ✅ **passed 2026-08-23** on a C5 primary driving a C5 node —
paired, gate assigned to a node channel, and the node moves when the primary
resolves the angle. **Routing from a real tool passes too**: a tool drawing
power opens its gate, the first time the whole chain has run end to end.

**Still open, and it is the half that matters with a tool actually running:**
kill the primary mid-move. Pass = every servo **HOLDS** where it is. No timeout
closing gates, no homing on reconnect, no autonomous behaviour at all. A node
that tidies up after a lost primary can shut a gate under a running tool, which
is the dead-head the whole system exists to prevent.

Two minutes with both boards already on the bench, so there is no good reason
for this to keep waiting behind bigger items.

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
(pitch 127 mm) is derived by the same method that validated 2.5" and has never
been measured. Also still open: pitch uniformity past 2 gates.

**It is NOT disabled in the UI, and this item said it was until 2026-09-08.** Two
different controls got conflated. What IS disabled is the **port size** dropdown
on /settings — `<option value="4in" disabled>4" (soon)</option>`. What is not is
the **manifold profile** picker in the linear calibrator, where `Rockler 4"
manifold — 127 mm between outlets` is a live, selectable option
(`MANIFOLDS` in gates/linear-calibration.component.ts) that will run a reference
sweep against an unmeasured pitch and place every gate from it. Nothing in the
code gates it. Decide which way that goes — grey it out to match /settings, or
leave it selectable and accept that the sweep is the measurement — but do not
re-read this item as a promise that the UI already refuses.

**8. The wake button — a press on D1 lights the panel.** What that leaves:
- **The toggle's off half.** Press-again-to-blank was written after that test and
  has never been pressed. `statusscreen::toggle()`; the pass is that a second
  press puts the panel out and the next real event still lights it.
- **A held button through reset.** D1 uses the internal pull-up and a plain
  momentary to GND. The pad is safe at reset (the C5's straps are 26/27/28),
  which is why D1 was chosen — worth confirming a held button doesn't stop it
  booting anyway.
- Pass: pressing it lights a blanked screen within a beat, and a button held down
  through reset does **not** light the screen at boot (`begin()` seeds from the
  pin for exactly that).

**9. What the screen work has and hasn't touched hardware.** Proven: the SSD1306
on D4/D5 at 0x3C, and the wake button on D1. Not proven: the toggle's off half,
and the pads nothing has been attached to — D0/D3, and D6 (ST3215 bus).
Everything the build actually drives is confirmed by a working signal.

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

**NodeLink out to a secondary passes** (2026-08-23), which was the last untested
part of the primary role. See bench item 2.

Still open on this board:
- **The collector refusal on the self-test has not been exercised.** All four
  channels sweep on both a primary and a node (2026-08-22), which also confirms
  the `_deenergize()` fix — repeated moves on one channel is exactly what the
  sweep does. What nobody has tried is holding the button *while the collector
  runs* and confirming it refuses rather than moving anything.
- The API still advertises the linear vocabulary — position, homing, stops — to a
  UI on a board that has none. Now user-visible, since the UI loads.
- Routing a real gate end to end from a topology, rather than servo bring-up.

**11. The collector takes 5-10 seconds to notice a tool (2026-08-23).** Real
complaint from a real bench run, and it is a latency problem, not a correctness
one — the gate opens and the collector starts, just late enough to be annoying.

Our own budget only explains ~1.5s of it: one 500ms reconcile
(`OUTLET_POLL_INTERVAL_MS`) plus the 1000ms `OUTLET_ON_DEBOUNCE_MS`. The wake-on-
push already exists, so a reading is acted on the tick it lands.

The structural suspect is in `SmartOutletControl::doPoll()`:

```cpp
if (!o->isPushConnected()) o->poll();
```

A push-connected plug is **never** HTTP-polled, so once the WebSocket is up the
500ms poll stops and detection latency becomes whatever cadence the plug chooses
to report `apower` at. If a Gen4 plug reports power periodically rather than on
change, push is not a slower path than polling — it is the only path, and the
poll that would have caught it is switched off.

**Measure before touching either number.** `plugtrace` on the primary's console
timestamps every frame a plug sends; the gaps between `[PUSH]` lines are the
plug's cadence, and `[DEBOUNCE] committed after Xms` is our half. Then:
- *Plug slow to report* → poll a plug whose last push is stale, so push
  accelerates detection but never gates it. This is the likely fix.
- *Plug reports promptly* → the time is between `[PUSH]` and `[DEBOUNCE]`, and
  the debounce is the knob. Don't shrink it blind: it is there to stop motor
  inrush from false-triggering a gate.

Board-independent — nothing here is specific to the C5 or blocks it.

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
