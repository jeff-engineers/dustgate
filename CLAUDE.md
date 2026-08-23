# DustGate

Automated dust collection for a woodworking shop. Tools plug into Shelly smart
outlets; when one draws power, the controller opens that tool's blast gate and
starts the collector. The shop is laid out once on a canvas in a phone browser,
and the firmware routes from that topology.

**Status: hardware-UNTESTED.** Everything compiles and passes host tests; almost
none of it has run on real hardware. Do not describe behaviour as verified.

## Layout

| Path | What |
|---|---|
| `shared/device-model/` | **Canonical device model** — pure JS, single source of truth |
| `firmware/` | ESP32 C++ (Arduino/PlatformIO). Primary owns everything; nodes are dumb actuator banks |
| `dustgate-ui/` | Angular app, served off the device's LittleFS |
| `tools/` | `mock-api.js` (simulated device), `mock-node.js` (simulated secondary), conformance runners |
| `dev.sh` | Thin bash wrapper over PlatformIO/esptool for every bench workflow |

## The anti-drift rule

Device behaviour used to be implemented three times (firmware, mock, demo) and
drifted constantly. Now `shared/device-model/` is the spec:

- `tools/mock-api.js` and `dustgate-ui/.../demo-api.service.ts` both `require`
  the same model, so they cannot drift from each other.
- Firmware **can't** import JS, so it's certified against the same contract by
  the executable conformance suites instead.
- Where a JS unit test asserts a specific number, the matching C++ test asserts
  the same number — `nodelink.test.js` ↔ `firmware/test/test_nodebus.cpp` is the
  reference pair. **Change one, change both.**
- Below that, individual constants have the same problem on a smaller scale: a
  bare number in `shared/device-model/` that firmware also hardcodes, with
  nothing but a comment holding the two together. The known pairs, so a change
  doesn't have to be re-discovered by grep every time:

  | JS (`shared/device-model/`) | C++ (`firmware/`) | What it is |
  |---|---|---|
  | `DEFAULT_COLLECTOR_OFF_DELAY_MS` (topology-device.js) | `kDefaultCollectorOffDelayMs` (control/TopologyRuntime.h) | collector coast-down default |
  | `DEFAULT_THRESHOLD_W` (topology-device.js) | `kDefaultThresholdW` (control/TopologyController.h) | machine-on wattage default |
  | the `* 3` in `setToolManual()` (dustgate-ui demo-api.service.ts) | the `* 3.0f` in `manualWattsFor()` (control/TopologyRuntime.h) | synthetic wattage for a manual switch-on |
  | `MAX_SERVOS_PER_HOST` / `MAX_LINEAR_PER_HOST` (topology.js) | `SERVO_COUNT` (config.h) | servo bank size a controller can actually drive |
  | `NUM_STOPS` (device-model.js) | `NUM_STOPS` (config.h) | compile-time max stops |
  | `MIN_STOP_SEPARATION_MM` (device-model.js) | `MIN_STOP_SEPARATION_MM` (config.h) | overlap backstop between stops |
  | `IDLE_TIMEOUT_SEC_DEFAULT` (device-model.js) | `IDLE_TIMEOUT_SEC_DEFAULT` (config.h) | idle power-off default |
  | `NODELINK_VERSION`, `PING_INTERVAL_MS`, `PONG_TIMEOUT_MS`, `RECONNECT_MIN_MS`, `RECONNECT_MAX_MS` (nodelink.js) | `kVersion`, `kPingIntervalMs`, `kPongTimeoutMs`, `kReconnectMinMs`, `kReconnectMaxMs` (control/NodeLink.h) | NodeLink protocol timing |

  The reference pair has company now: `manual-blower.test.js` ↔
  `firmware/test/test_manual_blower.cpp` covers running a blower by hand, and the
  two assert the same cases in the same order for the same reason.

  **This table is a cache, not the source of truth — keep it honest or delete
  rows rather than let them go stale.** Touching either side of a pair: update
  the other side's value AND this table's "What it is" cell if the meaning
  moved. Landing a NEW pair (a JS default with a C++ mirror, or vice versa,
  that didn't exist before): add a row here in the same change — that's what
  keeps the next person (or model) from re-deriving it from scratch the way
  the coast-down default (row 1) had to be on 2026-08-19, with no comment
  pointing either way until then. A constant that's read straight out of the
  saved document at runtime (like `offDelayMs` itself) isn't a pair — only the
  **default applied when the document doesn't say** is, since that's the value
  that can silently disagree.

Read `shared/device-model/README.md` before touching anything in that directory.

## Commands

Tests live in two package.json files. UI suites run under plain node (no browser).

```
cd dustgate-ui && npm test        # spec-runner + routing + wiring geometry
cd tools && npm run model:test    # topology, shop, nodelink, plug-claim, adopt-outlets, manual-blower (JS)
cd tools && npm run firmware:test # the C++ host tests (router, controller, nodebus, shop, faults, plugclaim, screen, blower)
cd tools && npm run conformance:ci topology:conformance:ci nodelink:conformance:ci  # run separately
```

Firmware compiles — `pio run -e <env>`:

| Env | Board | Role |
|---|---|---|
| `esp32dev_wroom32` | ESP32-DevKitC | **primary target** |
| `esp32dev_servo` | ESP32-DevKitC | primary, servo valves |
| `adafruit_feather_esp32s2` | Feather S2 | original prototype |
| `dustgate_node` | QT Py ESP32-S3 | secondary node |
| `xiao_c5` | XIAO ESP32C5 | secondary node |
| `xiao_c5_primary` | XIAO ESP32C5 | **primary** on the node's board — servo-only, no rack |

**One env per board, and every one of them assumes a screen** (2026-08-22). The
`-DHAS_STATUS_SCREEN` flag and the two `*_screen` envs are gone: a board header
that names `PIN_OLED_*` gets the driver, and an I²C ACK at 0x3C at boot decides
whether a panel is really there. The panel has run on a DevKitC (2026-08-21,
GPIO16/4) and a C5 node (2026-08-22, D4/D5), and its **wake button works on the
C5** (D1, 2026-08-22) — the only board where one has been pressed. The reasoning and the measured cost are at the top of
`platformio.ini`.

**A C5 can be either role, and the hardware is identical** (2026-08-22). Same
board, same carrier, same pin map; `xiao_c5_primary` vs `xiao_c5` is only which
program gets compiled (`build_src_filter`) and whether `-DDUSTGATE_SECONDARY` is
set. **The primary role is proven on hardware** — WiFi, the Angular UI served off
LittleFS, and servo movement, 2026-08-22. NodeLink to a secondary from a C5
primary is the one part of that role still untested. What made that possible is `HAS_LINEAR` finally becoming load-bearing:
config.h derives it from the pin map, and since the C5 header wires no
`PIN_TMC_*`, the stepper and endstop code now compiles out entirely and the
sketch takes `NullMotorDriver` / `NullFeedback` instead. Before that a rackless
board could only be a node, because the primary sketch could not be built without
a rack. `NO_LINEAR_FITTED` is a different thing and stays: it means "this pin map
has a rack, but no rack is attached".

Bench: `bash dev.sh flash --c5`. It needs its own partition table
(`partitions-xiao-c5-primary.csv`) — core 3.x builds ~350 KB larger, which
overflows the DevKitC's split.

`xiao_c5` rides the pioarduino platform, not espressif32. The two collide over
package names in a shared core directory, so the fork gets **its own**
`PLATFORMIO_CORE_DIR` (`~/.platformio-pioarduino`, 7.6 GB, downloaded once) —
see the essay at the top of `tools/boardinfo.sh` for why isolation beats fixing
collisions one at a time. `dev.sh` and `deploy.sh` call `use_core_for_env` for
you. By hand:

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5
```

Because the core dir is one env var per process, **`xiao_c5` can't share a
`pio run` with any other env.** Nothing in `~/.platformio` is touched by a C5
build. (The warning `dev.sh` prints about "swapping the core in and out" is
stale — that was the earlier approach.)

Bench work goes through `dev.sh` (`demo`, `mock`, `flash`, `flash-node`,
`monitor`, `ports`, `live`) — its header comment is the reference. Prefer it
over raw `pio`/`esptool`. See the `flash` skill for the gotchas.

## Design constraints

These are decided; don't relitigate them in code review or suggestions.

- **Never dead-head the collector.** Transitions are make-before-break: open the
  new gate before closing the old one. Most-recent-tool-wins. Idle leaves the
  gate where it is (the system rests open).
- **Gate count must be even.** Rockler manifolds ship in pairs; odd counts
  misplace gates.
- **No duct size anywhere in the UI.** Sizing is the woodworker's job, out of
  scope.
- **The plug belongs to the tool**, and draws under the tool's name — never on a
  port.
- **New pieces default into the system you're working in.** Adding a gate places
  it inside the active system's row band (`activeSystemId`, which follows whatever
  you last touched), not at some shop-wide origin. Systems own contiguous,
  non-interleaving row bands — that is what makes the grey ground drawable and what
  `bandBlockedBy()` enforces on a drag — so a default that ignores the band can put
  a piece somewhere a drag would refuse to move it. A **board** follows the same
  rule by a different route: it belongs to no system, so it is placed at the top-
  right of the active system's extent (`defaultBoardCell()`) but never band-checked.
- **A board stands on the canvas and owns its cell**, exclusively — nothing else
  may stand there and ducts route around it. It lived on a pinned rail above the
  grid until 2026-08-16; `docs/boards-on-canvas-plan.md` records why that came out
  and which alternatives were rejected. Nothing stops you dragging a board low,
  where its cables route badly: the fix is the default, not a rule.
- **A machine is ONE box, however many ports it has.** A second pickup — an
  overarm guard, a hood — is a differently-shaped inlet on that same box (square =
  the primary port, tapered = the secondary port), not a second body. It owns
  no cell; it rides its machine's box, and each duct lands on its own glyph.
- **A tool lives in ONE system; a SECONDARY port's run may cross the seam.** That
  run is the only thing allowed to, and it is drawn grey, dashed and thinner so a
  shared machine reads as shared. It can be disconnected and re-routed to a primary
  on the second system. (Grey dashed is free: an unfinished run's stub is *accent
  orange*, `.open-stub`. Getting that backwards cost a round trip on 2026-08-20.)
- A secondary node gets already-resolved angles/positions on the wire, never
  state names. That's what lets a $5 board be a node and keeps a schema change
  from needing a flash to every board in the shop.
- One brain: the primary owns topology, routing, and Shelly polling.

## UI work

**[`docs/mockups/canvas.html`](docs/mockups/canvas.html) is the canonical canvas
design and the only place decisions are recorded.** Read it before changing
anything on the build canvas, and before re-deriving a UI decision from scratch.
It carries the line/glyph vocabulary, the port and system rules, the drag rules,
and a dated decision log.

**Update it IN PLACE.** A settled question does not get a new page: change the
section it affects and add a row to the decision log. Seven mockups that disagreed
with each other is what made cross-system runs and the meaning of a dashed line
each get re-litigated three times (2026-08-20).

Still build a throwaway mockup when *exploring* something genuinely new, and
publish it for review before writing non-trivial UI code — then fold the outcome
into `canvas.html` and archive the exploration. Hover is never the only trigger for
anything. Focus dims rather than hides.

`docs/mockups/archived/` holds everything superseded, decided against, or built and
later removed. Nothing in there is current; each page opens with a banner saying
what replaced it. Add that banner when you move something in — a stale mockup that
still looks current is worse than no mockup at all.

`dustgate-ui/src/app/build/build.component.ts` is ~4k lines — grep it, don't
read it whole.

## Voice

Comments and docs explain *why*, at the altitude of the surrounding code, and
say plainly when something is unverified or a known compromise. Match that.
