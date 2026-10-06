# DustGate

This project is a work in progress and is not ready for use by anyone but its author. Use at your own risk.

Automated dust collection for a woodworking shop. When a tool starts drawing power, DustGate opens that tool's blast gate, closes the others, and starts the dust collector. When the last tool stops it leaves the gate where it is and lets the collector coast down. You lay the shop out once on a canvas in a phone browser (collector, ducts, gates, tools) and the brain routes from that picture.

**What has run on real hardware, and what has not.** The honest list lives at the top of [`CLAUDE.md`](CLAUDE.md) and is updated after each bench session. In short: a brain and PWM gate boards linked over WiFi and moved real gates; plugs (Shelly and Tasmota) sensed tools; a current clamp on a board started the collector and moved a gate; a board keyed the collector's RF remote and switched a real receiver; and the brain ran on a Mac for a whole bench shop on 2026-10-06. **Not yet run:** the brain on a Raspberry Pi, the 4" manifold, anything under production load, and several bench checks listed in [`TODO/TODO.md`](TODO/TODO.md). Treat everything else as compiled and host-tested only.

---

## How it works

```
 tool plug (Shelly / Tasmota)  ─┐
 current clamp on a board      ─┼─►  the BRAIN  ─►  gate boards (a servo on each ball valve, or a slider rack)
 dust-bin beam on a board      ─┘   (routing,       ─►  the collector's RF remote, pressed by a board
                                     plugs, UI)        ─►  the app, in your phone's browser
```

- **Boards.** One part, the [Seeed XIAO ESP32C5](https://www.seeedstudio.com), does every job; what it does is chosen by which program it is flashed with and by the layout. A **primary** is the brain. A **node** is any other board: it drives gates, and can also read a current clamp, a dust-bin sensor, poll plugs for the brain, or key the collector's remote. Nodes dial the brain themselves and find it again by a UDP beacon if it moves, so no node needs a fixed address.
- **The layout** is one document the brain stores: collectors (one per airflow system), ducts, gates, machines and their ports, and which board drives what. The app edits it; every other part reads it. Nothing on a node interprets it: a node is only ever sent resolved angles or positions.
- **Sensing is not switching.** A tool is only ever *sensed*. The collector is the one thing DustGate commands, and it does it by pressing the RF remote already in the shop, never by carrying motor current. The reasoning is in [`CLAUDE.md`](CLAUDE.md) and [`docs/tool-sensing-rfc.md`](docs/tool-sensing-rfc.md).
- **One rule set, one codebase for decisions.** Routing, sequencing and the node protocol are plain C++ (`firmware/control/`), host-tested, and the same files run in the ESP32 and in the native brain below. The behaviour is specified once in JavaScript ([`shared/device-model/`](shared/device-model/README.md)) and both the simulators and the firmware are held to it by paired tests.

### Where the brain runs

| | Status | Notes |
|---|---|---|
| **On an ESP32-C5** | The shipping design. Compiles, host-tested, and run on the bench. | One cheap board does everything, but RAM is the ceiling (see [how big a shop](#how-big-a-shop-can-a-c5-run)). |
| **On a Mac or Linux box** (`native/`) | Run on the bench for a whole shop, 2026-10-06. | The same brain core with a Boost.Beast shell: links nodes, routes gates, presses the collector, polls and claims plugs, serves the app. `make -C native test`. See [`native/README.md`](native/README.md). |
| **On a Raspberry Pi** (`native/pi/`) | **Planned as an option, never run on a Pi.** | The scripts (setup, deploy from the Mac, update with rollback, a systemd unit) are written, and CI builds the brain with GCC on Linux. The first goal is only "run the shop without the Mac". See [`native/pi/README.md`](native/pi/README.md). |

The plan is that a shop can **optionally move its brain from the C5 to a Raspberry Pi** when it outgrows the C5, without changing anything else: the nodes, the plugs, the layout and the app stay as they are, because the Pi runs the same brain code. The C5 stays a complete brain for a small shop, and the reasoning for the split is in [`docs/brain-options.md`](docs/brain-options.md). A Pi brain has no pins of its own, so the collector's remote and the dust-bin sensor sit on a node (which is already how a node can be set up). What a Pi would need before anyone but the author could install it (first-boot WiFi, a read-only root, a hardware watchdog) is not built.

---

## How big a shop can a C5 run?

These are approximations from the code's hard caps and from bench measurements, so treat them as planning numbers rather than guarantees. They describe **one C5 as the brain**; a Pi brain removes most of them.

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
| Seeed XIAO ESP32C5 | The one board: primary or node, flashed per role. Pin map in [`firmware/WIRING.md`](firmware/WIRING.md#1-the-board-and-its-one-pin-map) |
| Servo on each ball valve (or a printed gate) | A PWM board drives one gate |
| ST3215 bus servo on a rack and pinion with two endstops | The sliding-gate option, up to 8 gates over a Rockler manifold. A **slider board** drives only this; PWM servos and the serial bus never share a board |
| Shelly Plug (US) per tool | Senses a tool, ~$21. A Gen 2 device (the `/rpc/` API) |
| Tasmota plug with power metering | Sense-only, with no relay, so it can never switch a tool on. Found by address or a network sweep, because stock Tasmota does not announce itself |
| 30 A current clamp on a board | The other way to sense a tool, and the right one for 240 V tools, which a plug-in outlet cannot meter. Switched on per board in the app |
| Dust-bin beam sensor | Optional; reports a full bin |
| 315 MHz transmitter, or a servo that presses the fob | Presses the collector's existing RF remote |
| 12 to 24 V supply | For the slider's servo; ball-valve boards run from USB or their own supply |

Bill of materials, with the passives whose values matter: [`docs/BOM.md`](docs/BOM.md). Wiring: [`firmware/WIRING.md`](firmware/WIRING.md). The 2.5" Rockler manifold is measured (gates about 82.9 mm apart; see [`docs/dual-endstop-calibration.md`](docs/dual-endstop-calibration.md)). The 4" variant is not, and is disabled in the UI until it is.

---

## Smart plug setup

Do this before first boot.

**Shelly.** Add each plug to your WiFi with the Shelly app, then give it a fixed address in your router's DHCP reservations (the brain polls or is pushed to by address, and a changed lease breaks the pairing). Local control must be on (it is by default; the cloud is not needed). Check it answers at `http://<plug-ip>/rpc/Switch.GetStatus?id=0`, which should include `"apower": 0.0`. When asked for the generation, answer **2**.

**Tasmota.** Flash or buy a metering plug, give it a fixed address, and add it by that address on the **Plugs** page. It is polled every half second.

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
./dev.sh ports                 # which boards are attached, and which is pinned as primary / node
./dev.sh ports --pin primary   # pin a board by its USB serial (primary and node are the same part)
./dev.sh flash                 # app + firmware + filesystem + WiFi/hostname, for the primary
./dev.sh flash --fw            # firmware only: skips the filesystem, so the saved layout is safe
./dev.sh flash-node dustgate-node-1
```

WiFi and the hostname come from `tools/.env`; override them for one flash with `--host`, `--ssid` (it prompts for the password, hidden) or `--ask`, and write them back with `--save`. **Confirm which board you are flashing**: primary and node are the same hardware, and flashing the filesystem erases the saved layout (`dev.sh` backs it up first).

A node needs a hostname unique among your nodes, because that is what the brain finds it by. After the first cable flash, nodes can be updated over WiFi from the Boards screen (**Update**) or with `./dev.sh ota`.

Then in the app: **Boards → Scan for boards → Add**.

### First boot

1. Power the board. With no WiFi stored it makes a hotspot, `DustGate-Setup`, no password.
2. Join it and open **http://192.168.4.1**, enter your WiFi, and save. It reboots onto your network and prints its address on serial (`./dev.sh monitor`).
3. Open the app at that address (or `http://<hostname>.local`). `/` sends you to the layout tool if the shop is unfinished and to the Live list if it is.

---

## Setting up your shop

| Page | For |
|---|---|
| **Shop** (`/shop`) | The daily screen: your tools, which one is collecting, and the collector's state |
| **Build** (`/build`) | The canvas: place the collector, run duct, add gates and tools, and tap a piece for its setup |
| **Boards** (`/boards`) | Find and pair boards, rename, update, unpair, and switch a board's current clamp on |
| **Plugs** (`/plugs`) | Find, pair, rename, release and take over plugs, with each one's live draw and owner |
| **Tools** (`/tools`) | Per tool: its plug or clamp, and the wattage at which it counts as running |
| **Gates** (`/gates`) | Recalibrate a valve that got knocked |
| **Settings** | Resets and Forget WiFi |

1. **Draw the plumbing** on Build, the same shape as the pipe overhead. Add a second collector for a second airflow system; delete one beyond the first (its whole system goes with it), or use **⋯ → Clear shop** to return to the first collector alone.
2. **Pair your boards** and assign gates to them. A board with a current clamp has its switch on **Boards**; it defaults to off.
3. **Calibrate each gate** from its badge. A ball valve is nudged to its open and closed angles; a slider homes, sweeps its two endstops and lets you place each outlet.
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

- **Reset gate calibration**: clears trained positions.
- **Reset shop layout**: erases ducts, gates, tools and plugs; boards stay paired.
- **Reset everything**: also forgets every paired board and plug. Keeps WiFi, the app key and calibration.
- **Forget WiFi**: erases the credentials and reboots into the setup hotspot.

---

## Running the brain somewhere other than the ESP32 (optional)

```bash
make -C native                    # builds native/build/dustgate-brain
make -C native test               # end to end, against fake nodes and fake plugs
native/build/dustgate-brain --port 8080 --www dustgate-ui/dist/dustgate-ui/browser --pair dustgate-planer,dustgate-tablesaw
```

A node dials the brain it is paired to by name, so run this only while the ESP32 brain with the same id is paused (`POST /api/nodes/pause`) or give it a different `--id`. On a **Raspberry Pi** (a Zero 2 W is the intended board, with Raspberry Pi OS Lite) use the scripts in [`native/pi/`](native/pi/README.md): `setup.sh` once, then `deploy.sh` from the Mac. They have never been run on a Pi.

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
  firmware.ino           the primary's sketch
  node/                  the node program (a separate small program, not a flavour of the sketch)
  control/               the brain: router, sequencer, controller, runtime, node protocol and sessions.
                         Pure C++, host-tested, and shared with native/
  outlets/               Shelly and Tasmota drivers, plug claiming and provisioning (shared with native/)
  api/                   the HTTP and WebSocket server; api/ApiCore.h holds the routes both brains share
  sensing/               the current-clamp trip logic
  boards/                pin maps
  test/                  host (g++) tests and fixtures
  WIRING.md              wiring reference

native/                  the brain as a Linux / macOS program, and native/pi/ for the Raspberry Pi
dustgate-ui/             the Angular app, served from the brain (see its README)
shared/device-model/     the canonical device model, constants manifest and conformance suites
tools/                   mock-api.js, mock-node.js and the checks run in CI
docs/                    design notes, RFCs, mockups and the bill of materials
TODO/                    TODO.md (open items) and DONE.md (decisions worth keeping)
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

[CI](.github/workflows/ci.yml) runs these on every push in four jobs: conformance, UI build and tests, firmware compile (the primary, the slider primary and both nodes), and the native brain built with GCC on Linux. To certify real hardware against the same contracts (the slider suite is destructive: it homes, moves and wipes calibration):

```bash
node shared/device-model/conformance.js http://<device-ip> <api-key> --force
node shared/device-model/topology-conformance.js http://<device-ip>
node shared/device-model/nodelink-conformance.js ws://<node>.local/nodelink
```

---

## Limitations and known issues

- Validation on hardware is partial (see the top of this file). The open bench checks are in [`TODO/TODO.md`](TODO/TODO.md).
- The Raspberry Pi path is untested and is not a product: it needs first-boot WiFi, a read-only root and a watchdog before anyone else could install it.
- One servo moves at a time across the whole shop, and an interrupted move is re-sent once per fault, not retried indefinitely.
- A collector started for a few seconds of tool use can be slow to arrive; where the time goes is still to be measured on a bench.
- 240 V tools need a current clamp; plug-in outlets are 120 V only.
- A brain on a C5 is bounded by RAM, as above.
