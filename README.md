# DustGate

This project is a work in progress and is not ready for use by anyone but its author. Use at your own risk.

Automated dust collection for a woodworking shop. When a tool starts drawing power, DustGate opens that tool's blast gate, closes the others, and starts the dust collector. When the last tool stops it leaves the gate where it is and lets the collector coast down. You lay the shop out once on a canvas in a phone browser (collector, ducts, gates, tools) and the brain routes from that picture. The brain runs on a Raspberry Pi; every other board is an ESP32-C5 node.

See [what has run on hardware](#what-has-run-on-hardware) before trusting any feature: most of the code compiles and passes host tests well ahead of what has run on real boards.

---

## What has run on hardware

The one list (dated) of what has actually run on real boards. Anything not here is compiled and host-tested only.
Update it in the same session as the bench that proves something.

- **Primary and node roles** with PWM servos and NodeLink between them; a real tool opening its gate.
- **The ST3215 slider**: as a primary (2026-08-28: homing, the reference sweep, gate moves on a 4-gate rockler-2.5 rack);
  as a NODE (2026-09-03); through the Pi brain (2026-10-17: homed on its first SET, then moved; 2026-10-18: homed and swept
  again after an endstop rewire). A failed home reaches the slider page in the node's own words (STATE.err, 2026-10-17), and
  its Try again re-homes it (2026-10-18). Not run: the wake-button homing trigger (the prototype slider board has no button).
- **The bin sensor** (QS18VN6D through a discrete 4N35 — not a PC817 breakout, which put 4 V on a GPIO): on a primary
  2026-09-13; **from a node, end to end, 2026-10-11** — `bin-full`, and every board on that system blinking red.
- **The status lights (D-86)**, 2026-10-22: a board's pixel and its shop light (a 12-LED RGBW ring, Adafruit 2851, chained
  off the cyclone node's pixel) driven as one chain from the node; the ring showed the brain's solid amber ("the shop
  needs a person", collector idle). Not yet seen: green suction, the blinking forms, a full bin on the ring.
- **A CT clamp starts the collector and moves a gate**, end to end (2026-09-18). ⚠️ It takes TWO gates to see it:
  with one there is nothing to route away from, and "idle leaves the gate where it is" means doing nothing is correct.
- **Clamp-jack detection** (2026-10-17): a switched jack (cyclone, miter saw) and a bare D0 (the C5's internal
  pull-up survives the ADC setup) both read "no clamp" when empty.
- **The collector's jobs on a node**: PRESS (2026-10-05), keying a real Rockler receiver (2026-10-06), the bin beam
  (2026-10-11).
- **The native brain**: a whole bench shop on a Mac (2026-10-06: gate moves on five boards, tool → gate → collector, the
  press policy correcting an inverted belief); **the shop on a Raspberry Pi since 2026-10-11**, including node OTA
  updates, optional boards (a board with no gate switched off raises nothing), and the servo pulse range from Settings.
- **The status screen** on a C5 (D4/D5), wake button on D1.
- **Bus servos on a bus board** (`xiao_c5_bus` on Seeed's Bus Servo Driver Board, one ST3215, through the Pi brain),
  2026-11-01: Add a servo found a factory-fresh servo and numbered it (2), set-up moves from the app across its whole turn
  (about 70, each arriving, 19–21% peak load at 12.2 V, no missed reads in 2,300), the positions saved, and the board armed
  its fail-closed plan with the bus gate. The old PWM boards linked to the new brain unchanged (NodeLink stayed at
  version 1). Not run yet: a tool routing the manifold from the Live page, a valve and the slider asked at once, a servo
  unplugged mid-move, Replace a servo, two servos on one chain, and the bench console's fixed `setid`. Seen once and not
  since: the board stopped acknowledging the brain for ~40 s just after a servo was numbered (now logged as `[LOOP] slow
  pass` if it recurs).
- **Two serial servos on one bus board, through the Pi brain** (2026-11-25, dustgate-serial-1: the Manifold plugged
  straight in, the Miter Saw Valve on servo extension cable): **Find its servo** tied the valve's untied servo to it.
  Extension on the valve at 4, 8, 12 and 16 ft with no visible change: 11.9 V at the
  servo against 12.0 V plugged straight in, the same 20-25% peak loads, and 7 missed reads in 1,105 against none in
  2,155 — the first sign of margin, not a fault (which length they happened at is not known). PWM on 4 ft was already
  worse (Jeff). Also seen the same day: serial servos turn the same way as PWM ones after the board's direction flip
  (updated over WiFi from the app), a serial valve reaches its full turn, and positions set on the Gates page stay
  saved. Not run yet: a tool routing either gate.
- **The ball valve's load on an ST3215** (the bus bench console, `xiao_c5_bus_bench`, 12 V): the prototype valve needs
  about 0.7 kg·cm (15.6% peak coupled vs 13.2% uncoupled, 2026-11-01); the new design reads about 20% peak, gross,
  2026-11-12. Not yet measured: a valve on a duct with the collector pulling.

**Not yet proven:** a PRESS never replaying across a link drop (unit-tested only); the collector sensed by a CT clamp
(the Shelly + contactor build); node plug polling under a load; anything under load; 9 V vs 12 V; the 4" manifold.

---

## How it works

```
 tool plug (Shelly / Tasmota)  ─┐
 current clamp on a board      ─┼─►  the BRAIN  ─►  gate boards (a servo on each ball valve, or a slider rack)
 dust-bin beam on a board      ─┘   (routing,       ─►  the collector's RF remote, pressed by a board
                                     plugs, UI)        ─►  the app, in your phone's browser
```

- **Boards.** One part, the [Seeed XIAO ESP32C5](https://www.seeedstudio.com), does every board's job; what it does is chosen by which program it is flashed with and by the layout. Every board in the shop is a **node**: it drives gates (PWM servos, or bus servos on a bus board), and can also read a current clamp or a dust-bin sensor, poll plugs for the brain, or key the collector's remote. Nodes dial the brain themselves and find it again by a UDP beacon if it moves, so no board needs a fixed address. A C5 can still be built as a **primary** (a brain on the board itself), but that brain is frozen (below).
- **The layout** is one document the brain stores: collectors (one per airflow system), ducts, gates, machines and their ports, and which board drives what. The app edits it; every other part reads it. Nothing on a node interprets it: a node is only ever sent resolved angles or positions.
- **Sensing is not switching.** A tool is only ever *sensed*. The collector is the one thing DustGate commands, and it does it by pressing the RF remote already in the shop, never by carrying motor current. The reasoning is in [`CLAUDE.md`](CLAUDE.md) and [`docs/archive/tool-sensing-rfc.md`](docs/archive/tool-sensing-rfc.md).
- **One rule set, one codebase for decisions.** Routing, sequencing and the node protocol are plain C++ (`firmware/control/`), host-tested, and the same files run in the ESP32 and in the native brain below. The behaviour is specified once in JavaScript ([`shared/device-model/`](shared/device-model/README.md)) and both the simulators and the firmware are held to it by paired tests.

### Where the brain runs

| | Status | Notes |
|---|---|---|
| **On an ESP32-C5** | **Frozen** (2026-10-18). Compiles and gets shared-core fixes; no new features, no bench time. | One cheap board doing everything, with RAM as the ceiling (see [how big a shop](#how-big-a-shop-can-a-c5-run)). Kept revivable, not maintained. |
| **On a Mac or Linux box** (`native/`) | Run on the bench for a whole shop, 2026-10-06. | The same brain core with a Boost.Beast shell: links nodes, routes gates, presses the collector, polls and claims plugs, serves the app. `make -C native test`. See [`native/README.md`](native/README.md). |
| **On a Raspberry Pi** (`native/pi/`) | **Runs the shop** (since 2026-10-11). | The same native brain, deployed from the Mac with `native/pi/deploy.sh` (setup, update with rollback, a systemd unit). See [`native/pi/README.md`](native/pi/README.md). |

**The brain is the Pi** (decided 2026-10-18). A $15 Pi runs the same brain core with room to spare, and keeping a second brain on the C5 in step kept costing time. Every board in the shop is a C5 **node**; the Pi has no pins of its own, so the collector's remote and the dust-bin sensor sit on a node. The C5 brain is frozen rather than deleted: it still builds, and the shared core is kept so it could be revived as a thin shell. What a Pi would need before anyone but the author could install it (first-boot WiFi, a read-only root) is not built, and the reasoning for the original split is in [`docs/archive/brain-options.md`](docs/archive/brain-options.md).

---

## How big a shop can a C5 run?

These are approximations from the code's hard caps and from bench measurements, so treat them as planning numbers rather than guarantees. They describe **one C5 as the brain**, which is frozen (above); the Pi brain removes most of them.

| Limit | Number | Where it comes from |
|---|---|---|
| Gate boards (nodes) paired to one brain | **10** hard cap; **about 8** comfortable | `kMaxPairedNodes`; each linked node costs the brain roughly 3.5 KB of RAM |
| Gates per ball-valve board | **1** (a board has 2 PWM channels: one gate, one to press a remote fob) | `SERVO_COUNT` |
| Gates on a slider rack | **up to 8**, always an even number | `NUM_STOPS`, manifolds ship in pairs |
| Smart plugs the brain polls itself | **7** (every 500 ms) | `SMART_OUTLET_COUNT` |
| Sensors one node can carry for the brain | **4**, across plugs, a clamp and a bin sensor | `kMaxSensorsPerNode`. A node that polls plugs takes that load off the brain |
| Size of the saved layout | **24 KB**, which is about 60 to 70 pieces (a tool, a gate, a fitting each count) or roughly **20 to 25 tools with their gates** | `kMaxTopologyBytes`; a measured 16-piece shop is 5.6 KB |
| Airflow systems (collectors) | as many as fit the layout, one collector each | |
| Gate moves at once | **1, shop-wide** | so a switchover is never two servos drawing current together |
| Browsers using the app at once | **a few**; the app refuses a page load when memory is low | the brain keeps a ~28 KB guard for serving a page |

A realistic **small shop** on a C5 brain: 8 to 12 tools, 6 to 8 gate boards, 1 or 2 collectors, 7 or so plugs on the brain and the rest on nodes. Measured on the bench: a brain with **five nodes and four plugs idles at about 40 KB of free RAM**, a network scan briefly took it to 1.5 KB, and a layout save in the same few seconds can fail an allocation, so the margin is thin by 8 nodes. A shop with **12 to 15 plugs and 8 or more nodes** is where to move the brain to a Pi.

---

## Hardware

| Part | Notes |
|------|-------|
| Raspberry Pi (a Zero 2 W runs the shop) | The brain. Set up from [`native/pi/`](native/pi/README.md) |
| Seeed XIAO ESP32C5 | Every other board, flashed as a node. Pin map in [`firmware/boards/xiao_c5.h`](firmware/boards/xiao_c5.h) |
| PWM servo on each ball valve (or a printed gate) | A PWM board drives one gate; its second channel can press the collector's remote fob |
| ST3215 bus servos on a **bus board** (a C5 on Seeed's Bus Servo Driver Board) | Chained serial servos, numbered from the app (**Add a servo**): ball valves, or a slider on a rack and pinion with two endstops (up to 8 gates over a Rockler manifold). PWM servos and the serial bus never share a board |
| Shelly Plug (US) per tool | Senses a tool, ~$21. A Gen 2 device (the `/rpc/` API) |
| Tasmota plug with power metering | Sense-only, with no relay, so it can never switch a tool on. Found by address or a network sweep, because stock Tasmota does not announce itself |
| 30 A current clamp on a board | The other way to sense a tool, and the right one for 240 V tools, which a plug-in outlet cannot meter. A board reports whether a clamp is plugged into its jack |
| Dust-bin beam sensor | Optional; reports a full bin |
| A switch for the collector | A 315 MHz transmitter that presses the collector's existing RF remote, or a smart relay switching a contactor. Either way a clamp or a metering plug confirms the blower really started |
| 12 V supply | For a bus board's servos; PWM boards run from USB or their own supply |

Bill of materials, with the passives whose values matter: [`docs/BOM.md`](docs/BOM.md). Wiring: [`docs/carrier-wiring.html`](docs/carrier-wiring.html). The 2.5" Rockler manifold is measured (gates about 82.9 mm apart, `MANIFOLD_PROFILES` in the device model). The 4" variant is not, and is disabled in the UI until it is.

---

## Smart plug setup

Do this before first boot.

**Shelly.** Add each plug to your WiFi with the Shelly app. It needs no fixed address: the brain knows a paired plug by its MAC and finds it again if the router gives it a new address. Local control must be on (it is by default; the cloud is not needed). Check it answers at `http://<plug-ip>/rpc/Switch.GetStatus?id=0`, which should include `"apower": 0.0`. When asked for the generation, answer **2**.

**Tasmota.** Flash or buy a metering plug and find it on the **Plugs** page: stock Tasmota does not announce itself, so the brain sweeps the subnet for it, or you can type its address. It is polled every half second.

**Ownership.** DustGate asks a plug who owns it before writing anything to it. A plug that belongs to another controller is only polled, and is taken over only when you approve it on the Plugs page. Releasing a plug hands back whatever push address it had before.

**240 V tools** cannot use a plug-in outlet (they are 120 V / 15 A). Use a current clamp on a node.

---

## Software prerequisites

- [PlatformIO](https://platformio.org/) (CLI or the VS Code extension). The C5 builds use the pioarduino platform, which `dev.sh` points at for you
- [Node.js](https://nodejs.org/) 18 or newer and npm, for the web app and the tests
- For the native brain: a C++17 compiler and Boost (`brew install boost`, or `apt install libboost-dev`)

---

## Build and flash

Everything on the bench goes through [`dev.sh`](dev.sh); its header comment is the reference. It handles the board identification and the layout backup that raw `pio` commands do not, so prefer it.

```bash
./dev.sh ports                     # which boards are attached, and which is pinned as primary / node
./dev.sh flash-node dustgate-node-1  # a PWM node: node firmware + WiFi, by cable
./dev.sh flash-node --bus          # a bus board (bus servos, or a slider)
./dev.sh ota                       # update nodes over WiFi
./dev.sh flash                     # the frozen ESP32 primary: app + firmware + filesystem + WiFi/hostname
```

WiFi and the hostname come from `tools/.env`; override them for one flash with `--host`, `--ssid` (it prompts for the password, hidden) or `--ask`, and write them back with `--save`. **Confirm which board you are flashing**: primary and node are the same hardware, and flashing a primary's filesystem erases its saved layout (`dev.sh` backs it up first).

A node needs a hostname unique among your nodes, because that is what the brain finds it by. After the first cable flash, nodes are updated over WiFi from the Boards screen (**Update**, or **Update all**, one board at a time) or with `./dev.sh ota`.

Then in the app: **Boards → Scan for boards → Add**.

### First boot

1. **Set up the brain** on a Raspberry Pi: [`native/pi/README.md`](native/pi/README.md) (Raspberry Pi Imager, `setup.sh` once, then `deploy.sh` from the Mac). The app is then at `http://dustgate.local`, or the Pi's address from your router's client list.
2. **Power each node.** One flashed with `dev.sh` joins the WiFi from `tools/.env`. A board with no WiFi stored makes a hotspot, `DustGate-Setup`, no password: join it, open **http://192.168.4.1**, enter your WiFi and save. Once a board has joined your network it keeps retrying it rather than falling back to the hotspot.
3. In the app, `/` sends you to the layout tool if the shop is unfinished and to the Shop list if it is.

---

## Setting up your shop

| Page | For |
|---|---|
| **Shop** (`/shop`) | The daily screen: your tools, which one is collecting, and the collector's state |
| **Build** (`/build`) | The canvas: place the collector, run duct, add gates and tools, and tap a piece for its setup |
| **Boards** (`/boards`) | Find and pair boards, rename, update, unpair, switch a board's current clamp on, add a bus servo, and the brain's log |
| **Plugs** (`/plugs`) | Find, pair, rename, release and take over plugs, with each one's live draw and owner |
| **Tools** (`/tools`) | Per tool: its plug or clamp, and the wattage at which it counts as running |
| **Gates** (`/gates`) | Recalibrate a valve that got knocked |
| **Settings** (`/settings`) | Developer mode, the PWM servo pulse range, resets and Forget WiFi |

1. **Draw the plumbing** on Build, the same shape as the pipe overhead. Add a second collector for a second airflow system; delete one beyond the first (its whole system goes with it), or use **⋯ → Clear shop** to return to the first collector alone.
2. **Pair your boards** and assign gates to them. A board with a current clamp has its switch on **Boards**; it defaults to off.
3. **Calibrate each gate** from its badge. A bus servo is first added from its board's menu (**Add a servo…**). A ball valve is nudged to its open and closed positions; a slider homes, sweeps its two endstops and lets you place each gate.
4. **Pair a plug or a clamp to each tool.** On **Plugs**, switch the tool on and watch for the plug that jumps, then **Pair to…**. A tool with neither is manual-only.
5. **Set up the collector**: the remote that switches it, the plug or clamp that tells DustGate whether it is really running, and the dust-bin sensor if there is one.

The layout is saved to the brain as you go, so you can stop and come back.

---

## Daily use

- **Automatic.** Turn a tool on. Within about a second DustGate opens its gate, closes the others (open the new gate before closing the old one, so the collector is never dead-headed) and starts the collector if it is not running. Most recent tool wins.
- **When the last tool stops** the gate stays where it is and the collector coasts for a few seconds before it is switched off.
- **By hand.** Tap a tool on **Shop** to route to it without running it, and tap the collector card to run it by hand. A hand-switched tool is released when another tool starts.
- **When something is asked for and not happening** (a gate that did not move, a collector that did not start, a board or plug that stopped answering) it is listed on the Shop screen, worded by the device, and does not go away until it is fixed.

---

## Settings

- **Developer mode**: shows each tool's bus gate and the load of its last move on Shop, and a bus board's servos.
- **PWM servo pulse range**: the pulse at 0° and at 180°, for servos that differ from the default.
- **Reset gate calibration**: clears trained positions.
- **Reset shop layout**: erases ducts, gates, tools and plugs; boards stay paired.
- **Reset everything**: also forgets every paired board and plug. Keeps WiFi, the app key and calibration.
- **Forget WiFi**: erases the credentials and reboots into the setup hotspot.

---

## Running the brain on a Mac (bench)

The Pi runs the same program as `native/`; on the bench it also runs on a Mac or a Linux box:

```bash
make -C native                    # builds native/build/dustgate-brain
make -C native test               # end to end, against fake nodes and fake plugs
native/build/dustgate-brain --port 8080 --state "$(mktemp -d)" --www dustgate-ui/dist/dustgate-ui/browser --pair dustgate-planer,dustgate-tablesaw
```

Give a bench brain an empty state directory and fixture layouts: one started on a copy of the shop's layout claims and repoints the shop's plugs. A node dials the brain it is paired to by id, so while the Pi's brain (`--id dustgate`) is running, give this one a different `--id`. See [`native/README.md`](native/README.md).

---

## Development

Work on the app against a live brain, or with no hardware at all:

```bash
./dev.sh demo      # the app in a browser with a simulated shop, no device
./dev.sh mock      # the local Node stand-in for the device API
./dev.sh live      # hot-reload against the real device (default host dustgate.local)
```

or by hand: point `dustgate-ui/proxy.conf.json` at the device and run `npm start` in `dustgate-ui/`.

## Project structure

```
firmware/                ESP32 C++ (Arduino / PlatformIO)
  firmware.ino           the ESP32 primary's sketch (frozen)
  node/                  the node program (a separate small program, not a flavour of the sketch)
  control/               the brain: router, sequencer, controller, runtime, node protocol and sessions.
                         Pure C++, host-tested, and shared with native/
  outlets/               Shelly and Tasmota drivers, plug claiming and provisioning (shared with native/)
  api/                   the HTTP and WebSocket server; api/ApiCore.h holds the routes both brains share
  sensing/               the current-clamp trip logic
  boards/                pin maps — the source of truth for which pad does what
  utils/                 WiFi joining and the setup hotspot, shared by every board
  test/                  host (g++) tests and fixtures

native/                  the brain as a Linux / macOS program, and native/pi/ for the Raspberry Pi
dustgate-ui/             the Angular app, served from the brain (see its README)
shared/device-model/     the canonical device model, constants manifest and conformance suites
tools/                   mock-api.js, mock-node.js and the checks run in CI
docs/                    the bill of materials, the wiring page, and mockups (canvas.html and decisions.html are canonical)
docs/archive/            superseded design notes, RFCs and plans: history, never a current reference
TODO/                    TODO.md (open items) and DONE.md (finished work worth keeping)
.github/workflows/       CI
```

## Testing and CI

Device behaviour is defined once in [`shared/device-model/`](shared/device-model/README.md), which drives both the local mock and the in-browser demo, so they cannot drift. C++ cannot share that JavaScript, so it is held to the same behaviour by executable contracts that run against any target, plus host tests for the pure C++ layer. Numbers that two builds both need are listed in `shared/device-model/constant-pairs.json` and checked.

```bash
cd tools
npm run model:test                 # the JS model, the paired constants, NodeLink frames
npm run firmware:test              # host C++: router, controller, NodeBus, sessions, sensing, API core
npm run conformance:ci             # the slider device API against the mock
npm run topology:conformance:ci    # the layout API against the mock
npm run nodelink:conformance:ci    # primary to node protocol against mock-node.js
npm run ui:fresh                   # the built app carries the current model
cd ../dustgate-ui && npm test      # every UI suite (plain TypeScript under node, no browser)
make -C native test                # the native brain end to end
```

[CI](.github/workflows/ci.yml) runs these on every push in four jobs: conformance, UI build and tests, firmware compile with the C++ host tests (the C5 primary and the PWM node; the bus builds are not compiled in CI), and the native brain built with GCC on Linux. To certify real hardware against the same contracts (the slider suite is destructive: it homes, moves and wipes calibration):

```bash
node shared/device-model/conformance.js http://<device-ip> <api-key> --force
node shared/device-model/topology-conformance.js http://<device-ip>
node shared/device-model/nodelink-conformance.js ws://<node>.local/nodelink
```

---

## Limitations and known issues

- Validation on hardware is partial (see the top of this file). The open bench checks are in [`TODO/TODO.md`](TODO/TODO.md).
- The Raspberry Pi brain runs the author's shop, but it is not a product: it needs first-boot WiFi and a read-only root before anyone else could install it.
- One servo moves at a time across the whole shop, and an interrupted move is re-sent once per fault, not retried indefinitely.
- A collector started for a few seconds of tool use can be slow to arrive; where the time goes is still to be measured on a bench.
- 240 V tools need a current clamp; plug-in outlets are 120 V only.
- A brain on a C5 is bounded by RAM, as above.
