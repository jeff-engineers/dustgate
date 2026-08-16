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

Read `shared/device-model/README.md` before touching anything in that directory.

## Commands

Tests live in two package.json files. UI suites run under plain node (no browser).

```
cd dustgate-ui && npm test        # spec-runner + routing + wiring geometry
cd tools && npm run model:test    # topology, shop, nodelink, plug-claim (JS)
cd tools && npm run firmware:test # the C++ host tests (router, controller, nodebus, shop, faults, plugclaim)
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
- A secondary node gets already-resolved angles/positions on the wire, never
  state names. That's what lets a $5 board be a node and keeps a schema change
  from needing a flash to every board in the shop.
- One brain: the primary owns topology, routing, and Shelly polling.

## UI work

Build an interactive HTML mockup in `docs/mockups/` and publish it for review
**before** writing non-trivial UI code. Hover is never the only trigger for
anything. Focus dims rather than hides.

`dustgate-ui/src/app/build/build.component.ts` is ~4k lines — grep it, don't
read it whole.

## Voice

Comments and docs explain *why*, at the altitude of the surrounding code, and
say plainly when something is unverified or a known compromise. Match that.
