# Where does the brain live? — options and a recommendation (2026-10-04)

**Status: a decision document, nothing decided.** Written after a day of bench testing the
`node-links-stay-up` branch against a brain and five nodes, and after jeff asked whether the
project is "trying to do too much". Numbers marked *measured* are from that bench; the rest are
estimates and say so.

## What the bench measured

- A C5 brain with **five nodes linked and four plugs paired idles at ~40 KB internal RAM
  free** (65–85 KB with no layout and no nodes). *Measured.*
- A **board scan took the low-water mark to 1.5 KB** (twice to 7 KB), a layout save in the same
  few seconds is enough to fail an allocation. Free memory came back to ~35 KB each time: no
  leak, just no margin. *Measured.*
- **Node updates pulled from the brain's own HTTP server failed twice in a row** ("download
  stalled") for three of five nodes while the UI was in use. *Measured.*
- With node-initiated links in place (the PR), **8 of 8 brain reboots and 4 of 4 pause/resume
  cycles recovered all five nodes in 5–15 s with no flaps.** The link layer is no longer the
  weak part; memory is. *Measured.*
- The page-load guard needs ~28 KB; a node costs ~3.5 KB inbound and ~10 KB with a dial-out
  task; an S3 adds 40–90 KB of headroom (estimate); a large shop (12–15 outlets, 8+ nodes) is
  close to the line even there.

## The options

### A. Keep the brain on an ESP32 (C5 now, S3 later)

The current design. The brain serves the app, routes, polls plugs, holds every node link and
drives its own hardware.

- **For:** one cheap board, no extra device, the install story is the one already built, the
  slider-primary shop is a complete product.
- **Against:** RAM is the ceiling, and every feature competes for it (UI hosting, discovery,
  sweeps, plug push sockets, OTA staging). An S3 buys ~2x headroom, not an order of magnitude.
  Every new feature has to be measured against the page guard.
- **Verdict:** fine for a small shop, and worth keeping working as that. Not the answer for a
  large one.

### B. A Raspberry Pi Zero 2 W as the brain and the UI host (recommended direction)

The Pi runs a **native build of the same C++ brain core the ESP32 runs** (see "One codebase"
below) and serves the built app; nodes dial it exactly as they dial the C5 today.

- **For:** 512 MB against 240 KB, so the RAM problem and the node-count cap disappear. One
  implementation of the brain logic — the C++ already on the ESP32 — instead of a second brain
  in another language. Serving
  node images and the app is trivial, which removes the failed-download problem. The work
  already done carries over unchanged: JOIN, the beacon, caps.join, OTA pull, one-link rule.
- **Against:** an extra device ($15 + SD + supply). **An install step a woodworker cannot do is
  a different product** (CLAUDE.md): needs a pre-imaged SD card or a first-boot WiFi page, and
  the hosted brain has no pins, so the RF press and the bin sensor must move to a node
  (the existing TODO "a collector cannot run as a node yet"). SD cards and shop power cuts need a
  read-only root filesystem, a state partition and a hardware watchdog. ~30 s boot against 3 s.
  It must be the **Zero 2 W**: Node.js no longer supports the original Zero's ARMv6.
- **Cost to get there:** moderate to large. The decision logic is portable already; the work is
  carving the API handlers out of `HttpApiServer.cpp` / `firmware.ino` behind a small platform
  interface, a Linux shell (node listener, HTTP/WebSocket server, file storage, plug HTTP,
  mDNS), and the appliance packaging.

### C. No brain: nodes run a baked routing table, the editor is a static page

Each gate node holds the layout as a small table. Tool-sensing nodes broadcast "tool X on/off"
(WiFi UDP, which the beacon already proves works on the shop's network); every node applies the
same deterministic rule. A static editor page exports the table and pushes it to each node.

- **For:** no central point of failure, no UI host, no RAM contest, works with the brain gone.
- **Against:** "most recent tool wins" and "never dead-head" are shop-wide invariants that a
  central brain enforces easily and a distributed system must reach by agreement over lossy
  broadcasts. A lost packet can leave one gate closed while the blower runs for up to a
  broadcast interval. Fixable (state gossiped every second, ordering by sequence number), but it
  is a **rewrite of the control model and of its safety argument**, not a refactor. The
  features that need somewhere to live — the live tool list, manual override, plug claiming,
  problems, bin warnings — still need a viewer.
- **Verdict:** a good long-term direction as a *degraded mode* (nodes keep routing when the
  brain is down), not as a replacement now.

### D. Build on ESPHome

ESPHome compiles per-device firmware from YAML, with ESP-NOW and UDP transports and a browser
flasher.

- **For:** mature OTA and a browser flashing story (ESP Web Tools); a large component library.
- **Against:** it compiles on the user's side (a toolchain, or a Home Assistant add-on), which is
  an install step the owner cannot perform. **ESP-NOW needs every board on one radio channel**,
  and the bench boards sat on different mesh APs (channels 1 and 11) — the exact "it must work
  whichever AP a board lands on" requirement. Our slider (ST3215 bus servo, homing), the 60 Hz
  clamp loop and OTA rollback would all be custom C++ components anyway. ESP32-C5 support is
  newer than the S3's (from memory — check before relying on it).
- **Verdict:** not as the platform. Borrow the browser flasher (ESP Web Tools) for our own
  firmware, which gives the "flash from a web page" install without ESPHome.

## Comparison

| | A. ESP32 brain | B. Pi Zero 2 W brain | C. Distributed | D. ESPHome |
|---|---|---|---|---|
| RAM headroom | ~40 KB, a hard ceiling | effectively none needed | per node, small | per node |
| Nodes supported | ~9 (C5), maybe ~15 (S3) | dozens | any | any |
| Single point of failure | yes | yes (but a restart, not a reflash) | no | no |
| Keeps UI, manual override, claiming | yes | yes | needs a viewer | needs a viewer |
| Extra hardware | none | Pi + SD | none | none |
| Install a woodworker can do | yes (browser flash) | needs a pre-made image | needs the editor to push | **no** (compile) |
| Work already done carries over | all of it | nearly all | the node link layer | little |
| New work | none | transports + packaging | the control model | a platform port |
| Biggest risk | memory, always | SD / power-cut reliability, packaging | distributed safety | install story, ESP-NOW channels |

## Recommendation

1. **Pursue B, as one codebase** (next section). Build the native brain on a Mac against the
   five real nodes first (no hardware to buy): nodes linking, a gate move, plug polling, the app
   against it, brain restarts. Then move it to a Zero 2 W and make the appliance part work
   (image, first-boot WiFi, read-only root, watchdog).
2. **Keep A as the small-shop product.** Stop adding features to it; keep it stable.
3. **Move the app to a static page** hosted anywhere. It works with B, with A (via a proxy) and
   with any later brain.
4. **Take the browser flasher from D** (ESP Web Tools) for our own firmware.
5. **Treat C as a later degraded mode**, designed on top of B once the invariants are written
   down as a spec a node can run.
6. **Move the collector's jobs onto a node** (RF press, bin sensor, frames for each). B, C and an
   S3 brain without hardware all need it; it is the one prerequisite shared by every future.

## One codebase (decided 2026-10-04: the ESP32 brain and the hosted brain must be the same code)

The decision-making core is **already** platform-neutral C++: `TopologyRuntime.h` (1,407 lines),
`TopologyController.h`, `TopologyRouter.h`, `Shop.h`, `NodeBus.h`, `NodeLink.h`,
`CollectorPlugState.h`, `CollectorPress.h`. They include only ArduinoJson and the standard
library and are compiled by plain `g++` for the host tests today. A native build links the same
source.

What differs per platform is the world outside the core, and goes behind an interface:

| Behind an interface | ESP32 shell (today) | Linux / macOS shell (new) |
|---|---|---|
| HTTP + WebSocket server | ESPAsyncWebServer | a small C++ HTTP/WebSocket library |
| Node transport (inbound JOIN, one-shot pairing dial) | AsyncWebSocket + WebSocketsClient | the same library's WebSocket |
| Plug HTTP (Shelly / Tasmota) | Arduino HTTPClient | an HTTP client |
| Discovery and advertising | ESPmDNS | avahi / mDNSResponder |
| Storage | NVS + LittleFS | files |
| Clock, log ring, link log | millis(), SerialLog, LittleFS | std::chrono, memory, files |

The real work is `firmware.ino` (~4,700 lines) and `HttpApiServer.cpp`, which interleave
Arduino calls with API logic; that logic moves into shared handlers that take an abstract request
and response. The JS model in `shared/device-model/` stays as the executable spec, the mock and
the demo, and the paired JS/C++ tests keep guarding against drift; it is not a brain.

Order: (1) a native `dustgate-brain` target that runs the core on a Mac against the real nodes;
(2) the API handlers made shared; (3) plug HTTP, discovery and storage behind interfaces;
(4) the Zero 2 W and the appliance packaging. The ESP32 build stays working at every step.

## What to decide, and what would change the answer

- **Is a Pi acceptable in the product?** If the product must be ESP32-only, the answer is A
  plus an S3 and a smaller feature set, and C becomes the only way to a large shop.
- **How large is "large"?** Under ~8 nodes and ~10 outlets, A is fine and B is optional.
- **Packaging effort.** If a pre-imaged SD card is not practical, B is a developer product and
  the hosted brain should be a self-install guide, not an appliance.
- **What to measure on the prototype:** memory and CPU of the Node.js brain on a Zero 2 W, boot
  to first node link, reliability under 50 power cycles of the SD card with the read-only root,
  and time-to-recovery when the brain restarts mid-cut.

## What not to do

Do not keep shaving internal RAM off the C5 brain to make the same architecture hold a larger
shop. Two days of that bought ~25 KB; one board scan spends it.
