# DustGate

Automated dust collection for a woodworking shop. Tools plug into Shelly smart
outlets; when one draws power, the controller opens that tool's blast gate and
starts the collector. The shop is laid out once on a canvas in a phone browser,
and the firmware routes from that topology.

**Status: mostly hardware-UNTESTED, with real exceptions.** Everything compiles
and passes host tests. What has actually run on hardware: primary and node roles
with PWM servos and NodeLink between them; since 2026-08-28, the **ST3215 slider
as a primary** — homing, the reference sweep, and gate moves on a 4-gate
rockler-2.5 rack; and since 2026-09-02, the **slider NODE (`xiao_c5_linear`)
booting, joining NodeLink, and reporting itself correctly to the UI**.

**The slider node moves a gate, confirmed 2026-09-03** — it homes, finds its
datum, takes a SET and drives the rack. That was the last unknown on the node
path.

What is NOT yet exercised is how homing is now TRIGGERED. The node used to sweep
at boot; as of 2026-09-03 it sweeps **on demand** — on the first SET that needs a
datum, or on a one-second hold of the wake button — because an unasked movement
at power-up is the wrong default near a rack. The sweep itself is the same proven
code; only its trigger is new, and neither trigger has run on hardware. Anything
under load, 9V vs 12V, and the 4" manifold remain unverified too. Do not describe
behaviour as verified unless it is on that list.

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
  | `MANIFOLD_PROFILES` — `gatePitchMm` / `firstGateOffsetMm` / `endMarginMm` (device-model.js) | `MANIFOLD_2_5_GATE_PITCH_MM`, `MANIFOLD_4_GATE_PITCH_MM` and friends (config.h) | Rockler manifold geometry. **Found unregistered on 2026-08-28** — it had been a pair since the profiles were written, with nothing pointing either way, which is exactly the situation this table exists to prevent. `gatePitchMm` is the number the reference sweep trusts and centres the gate array on, so a change on one side alone mis-places every gate on real hardware while every test still passes. |
  | `NODELINK_VERSION`, `PING_INTERVAL_MS`, `PONG_TIMEOUT_MS`, `RECONNECT_MIN_MS`, `RECONNECT_MAX_MS` (nodelink.js) | `kVersion`, `kPingIntervalMs`, `kPongTimeoutMs`, `kReconnectMinMs`, `kReconnectMaxMs` (control/NodeLink.h) | NodeLink protocol timing |

  The reference pair has company now: `manual-blower.test.js` ↔
  `firmware/test/test_manual_blower.cpp` covers running a blower by hand, and the
  two assert the same cases in the same order for the same reason.

  **Not everything shared is a pair, and saying so is part of the job.**
  `collector-plug.test.js` has NO C++ partner on purpose: the firmware reports
  what a collector's plug says (`systems[].plug` — watts, reachable, onForMs) and
  never judges it, so `COLLECTOR_RUNNING_W` and `COLLECTOR_SPINUP_GRACE_MS` exist
  once, in `topology-device.js`, with nothing to drift against. If the OLED ever
  needs to say "blower not starting" too, that is the moment those become a pair
  and earn a row above — not before.

  `kMoveTimeoutMs` (control/NodeLink.h) is the same shape and catches people out
  harder, because it sits in the one file that otherwise mirrors `nodelink.js`
  frame for frame. It is the primary's own bookkeeping — how long to wait for a
  STATE before calling a move lost — and it never goes on the wire, so there is
  no `MOVE_TIMEOUT_MS` on the JS side to keep it in step with. Change it alone.
  (It went 12s → 90s on 2026-08-28: it had been sized for the stepper, and both
  a slider traverse and a node's boot-time homing sweep outrun 12s.)

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
cd tools && npm run model:test    # topology, shop, nodelink, plug-claim, adopt-outlets, manual-blower, collector-plug (JS)
cd tools && npm run firmware:test # the C++ host tests (router, controller, nodebus, shop, faults, plugclaim, screen, blower)
cd tools && npm run conformance:ci topology:conformance:ci nodelink:conformance:ci  # run separately
```

Firmware compiles — `pio run -e <env>`:

| Env | Board | Role |
|---|---|---|
| `xiao_c5_primary` | XIAO ESP32C5 | **primary** — the routing brain, 4 PWM valves |
| `xiao_c5` | XIAO ESP32C5 | secondary node, 4 PWM valves |
| `xiao_c5_linear_primary` | XIAO ESP32C5 | **primary** on the slider board (ST3215 rack) |
| `xiao_c5_linear` | XIAO ESP32C5 | secondary node on the slider board |
| `xiao_c5_bus_bench` | XIAO ESP32C5 | not a role — the bus-servo console |

**One board, two roles.** Same board, same carrier, same pin map; the difference
is `build_src_filter` and `-DDUSTGATE_SECONDARY`. Both roles are proven on
hardware, including NodeLink between them and a real tool opening its gate.

**PWM servos and a serial bus never share a board.** The slider gets dedicated
hardware that rides along with it. `config.h` `#error`s if a pin map claims both,
and `HAS_LINEAR` derives from the bus pins — 1 on the two `_linear` envs and the
bench, 0 on the PWM pair. `-DDUSTGATE_SERVO_BUS` is what flips a board between
the two personalities: the header then presents the bus pins (D6/D7) and the
endstops (D8/D9) instead of the four PWM pads.

**The slider is BOTH a primary and a node, and the node is the interesting one.**
A one-slider shop is a whole shop, so `xiao_c5_linear_primary` is a complete
brain that happens to drive a rack. `xiao_c5_linear` is the same actuator at the
far end of a NodeLink socket — and it is **the first node in this design with a
brain**, because a homing sweep is a closed loop between an endstop and a servo
that cannot round-trip per step over WiFi. It owns a sweep state machine, ticked
from `loop()` (a blocking sweep outruns the 10s watchdog), homes itself at boot,
and holds any move it is sent until the datum lands. Moves are still
already-resolved numbers off the wire — only *calibration* is local. Read the
CALIBRATION note at the top of `firmware/node/dustgate_node.cpp` before changing
any of it.

The **stepper is gone entirely** as of 2026-08-28 — driver, TMC2209 params,
DevKitC pin map and all. It spent a week in `firmware/attic/linear/` waiting to
be repurposed and the ST3215 driver ended up owing it nothing but the
`MotorDriver` contract. `LimitSwitchDistance` is the one part that came back:
the endstops outlived the stepper, because a step-counting bus servo has no
datum either. `git log` has the rest.

**Every env assumes a screen.** A board header that names `PIN_OLED_*` gets the
driver, and an I²C ACK at 0x3C at boot decides whether a panel is really there.
Verified on a C5 (D4/D5), wake button on D1.

Both envs ride the pioarduino platform (official `espressif32` has no ESP32-C5)
and build against `~/.platformio-pioarduino`, which `dev.sh`/`deploy.sh` set for
you. By hand, and they can share one `pio run`:

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_primary -e xiao_c5
```

Bench work goes through `dev.sh` (`demo`, `mock`, `live`, `flash`, `flash-node`,
`monitor [node]`, `ports [--pin primary|node]`, `provision`, `erase`) — its header
comment is the reference. Prefer it over raw `pio`/`esptool`. See the `flash`
skill for the gotchas. **Pin the boards**: primary and node are the same part with
the same USB VID, so nothing but a pinned serial can tell them apart.

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
- **Sensing is not switching.** A tool is only ever *sensed*; the collector is
  the one thing DustGate commands. They were fused only because a smart plug
  happened to do both, and a 1HP collector tripping a Shelly Plus Plug US on
  2026-09-03 broke that. Large tools get a no-relay metering plug (or our own
  CT); the collector is switched by the RF dust-collector remote already in the
  shop, so **nothing in the control path carries motor current**.
  `sensor.outlet` vs `control.outlet` in the model already said this. See
  [`docs/tool-sensing-rfc.md`](docs/tool-sensing-rfc.md) — decided, nothing
  bench-tested.
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
