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
  | `NUM_STOPS` (device-model.js) | `NUM_STOPS` (config.h) | max stops on one sliding gate. **8 since 2026-09-05**, lowered from 16 so it matches `MAX_SLIDE_BRANCHES` (topology.js) and `SLIDE_MAX_OUTLETS` (dustgate-ui) — three numbers that all claim to be the same limit, and were not. Must stay EVEN (`static_assert` in config.h): Rockler ships gates in pairs, so an odd request rounds up. Changing it changes the persisted `CalibrationData` layout — bump `CALIB_VERSION` with it |
  | `MIN_STOP_SEPARATION_MM` (device-model.js) | `MIN_STOP_SEPARATION_MM` (config.h) | overlap backstop between stops |
  | `IDLE_TIMEOUT_SEC_DEFAULT` (device-model.js) | `IDLE_TIMEOUT_SEC_DEFAULT` (config.h) | idle power-off default |
  | `MANIFOLD_PROFILES` — `gatePitchMm` / `firstGateOffsetMm` / `endMarginMm` (device-model.js) | `MANIFOLD_2_5_GATE_PITCH_MM`, `MANIFOLD_4_GATE_PITCH_MM` and friends (config.h) | Rockler manifold geometry. **Found unregistered on 2026-08-28** — it had been a pair since the profiles were written, with nothing pointing either way, which is exactly the situation this table exists to prevent. `gatePitchMm` is the number the reference sweep trusts and centres the gate array on, so a change on one side alone mis-places every gate on real hardware while every test still passes. |
  | the **default** for an absent `kind` on `sensor.outlet` / `control.outlet` (topology.js) | the default in `outletKindFromName()` and `OutletConfig`'s `o<N>_kind` (outlets/OutletFactory.h, OutletConfig.h) | which protocol a plug speaks — `shelly` or `tasmota`. The VALUE rides the document and so isn't a pair; the **default when the document is silent** is, and it must be Shelly on both sides or every layout written before 2026-09-09 starts polling the wrong endpoint. An unknown string defaults the same way, so a document from a newer UI degrades to the old behaviour rather than to a plug that reads nothing |
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

  `kBinDebounceMs` (utils/BinSensor.h) is another: how long the dust-bin beam
  must hold a reading before the firmware believes it. No JS model simulates a
  flickering beam — the mock and demo STAGE bin state directly, the way they
  stage a plug fault — so it exists once and has nothing to drift against. What
  *is* shared is the reported shape, and `bin-sensor.test.js` ↔
  `firmware/test/test_binsensor.cpp` are a collector-plug-style pair rather than
  a nodelink-style one: same rule (`systems[].bin` is omitted when nothing
  watches that bin), two engines, deliberately NOT the same numbers twice.

  `kMoveTimeoutMs` (control/NodeLink.h) is the same shape and catches people out
  harder, because it sits in the one file that otherwise mirrors `nodelink.js`
  frame for frame. It is the primary's own bookkeeping — how long to wait for a
  STATE before calling a move lost — and it never goes on the wire, so there is
  no `MOVE_TIMEOUT_MS` on the JS side to keep it in step with. Change it alone.
  (12s → 90s on 2026-08-28: it had been sized for the stepper, and both a slider
  traverse and a node's homing sweep outrun 12s. **90s → 210s on 2026-09-05**,
  because homing is now triggered BY a move, so the primary's clock runs for the
  whole sweep — and a sweep sized for an 8-gate 4" rack is ~162s. It must exceed
  `HOMING_TIMEOUT_MS`, which `firmware/node/dustgate_node.cpp` static_asserts.)

  `HOMING_TIMEOUT_MS` (config.h) is firmware-only too, and **derived** — from
  `HOMING_MAX_TRAVEL_MM`, `HOMING_SPEED_STEPS_PER_SEC` and a bench tracking
  factor — so it moves when the rail or the speed does. There is a real
  cross-language invariant near it that is NOT in the table above because it is a
  relationship rather than a shared value: `HOMING_MAX_TRAVEL_MM` must exceed
  `manifoldProfile('rockler-4', gates).spanMm` from device-model.js for the
  largest rack anyone builds. It did not, until 2026-09-05 — 700mm against an
  891mm 8-gate 4" rack — and the symptom would have been a healthy home failing
  on the biggest rack in the shop.

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
cd dustgate-ui && npm run bench:routing   # sample layouts, for tuning the routing costs
cd dustgate-ui && ./routing-sweep.sh TURN 48 64   # ...the same, sweeping one constant
cd tools && npm run model:test    # topology, shop, nodelink, plug-claim, adopt-outlets, manual-blower, collector-plug, bin-sensor (JS)
cd tools && npm run firmware:test # the C++ host tests (router, controller, nodebus, shop, faults, plugclaim, screen, blower, binsensor)
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
- **Sensing is not switching, and for a TOOL that is a safety rule.** A tool is
  only ever *sensed*; the collector is the one thing DustGate commands.

  This was recorded as a convenience — *a tool has its own switch, so we never
  needed to* — until 2026-09-09. The real reason is worse than that, and a rule
  with a weak reason gets relitigated the first time someone wants remote tool
  control: **a tool switched off at the OUTLET with its own switch left ON is
  armed.** Energise that outlet — remotely, on a schedule, by tapping the wrong
  row in an app — and a table saw spins up with nobody's hand on it, possibly
  mid blade-change. That is the exact hazard no-volt-release switches and
  magnetic starters exist to prevent, and a switchable smart outlet reintroduces
  it.

  So: **never add tool switching**, and prefer a plug that structurally cannot
  do it. A no-relay sense-only plug is not "will not" — it *cannot*.

  The 1HP collector tripping a Shelly Plus Plug US on 2026-09-03 is what first
  broke the sensing/switching fusion, but it is the smaller reason. Large tools get a no-relay metering plug (or our own
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

**Two canonical pages, and they are not interchangeable.** This used to say
canvas.html was "the only place decisions are recorded", which stopped being
true and sent people to add rows to a page that has no decision log.

- **[`docs/mockups/canvas.html`](docs/mockups/canvas.html) is the canonical
  canvas DESIGN.** The line and glyph vocabulary, the port and system rules, the
  drag rules, and dated `Built` pills on the sections that have shipped. Read it
  before changing anything on the build canvas.
- **[`docs/mockups/decisions.html`](docs/mockups/decisions.html) is the
  canonical DECISION REGISTER** — the dated list of what was decided and why,
  rendered from one array at the top of the file. Read it before re-deriving a
  UI decision from scratch, and append to it when one is settled.

**Update them IN PLACE.** A settled question does not get a new page: change the
canvas section it affects, and append an entry to the register. Seven mockups
that disagreed with each other is what made cross-system runs and the meaning of
a dashed line each get re-litigated three times (2026-08-20).

Three screen-specific pages — `gates-list.html`, `oled-status.html`,
`shop-status-chips.html` — carry their own decision logs as well. That is
allowed and useful: a log next to the drawing it explains beats a cross-
reference. The rule that keeps it from becoming the 2026-08-20 problem again is
that **a decision affecting more than one screen belongs in the register**, and
the per-page log links to it rather than restating it.

Still build a throwaway mockup when *exploring* something genuinely new, and
publish it for review before writing non-trivial UI code — then fold the outcome
into `canvas.html` and archive the exploration. Hover is never the only trigger for
anything. Focus dims rather than hides.

`docs/mockups/archived/` holds everything superseded, decided against, or built and
later removed. Nothing in there is current; each page opens with a banner saying
what replaced it. Add that banner when you move something in — a stale mockup that
still looks current is worse than no mockup at all.

`dustgate-ui/src/app/build/build.component.ts` is ~5.7k lines — grep it, don't
read it whole.

## Voice

Comments and docs explain *why*, at the altitude of the surrounding code, and
say plainly when something is unverified or a known compromise. Match that.
