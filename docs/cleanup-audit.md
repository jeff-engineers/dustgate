# Cleanup audit before the shared-core work (2026-10-04)

**Status: a ranked list, nothing changed.** Read-only pass over the firmware, the node
program, the shared model and the UI, looking for **duplicated paths and duplicated rules**
that the shared-core plan (`docs/brain-options.md`, "One codebase") would otherwise have to
move, port or fix twice. Line numbers are as of branch `brain-core-shared` and will drift.

The test applied to every item: *does leaving it make the platform split bigger or riskier?*
Cosmetic duplication, and anything the dial-out removal deletes anyway, is not on this list.

## Do these first (they shrink the core or the split)

### 1. One answer to "is this controller me, and which bus is it?" — HIGH value, small, medium risk

Five places decide whether a `controllerId` is this board or a given node, and they are
**not the same rule**:

| Where | Rule |
|---|---|
| `control/NodeBus.h:44-110` `bareHost`, `busForController`, alias map | host-normalised (`.local`, case, trailing dot); empty or own id = local |
| `control/TopologyRuntime.h:1119` `sameBoard` | `bareHost` equality, empty = this board |
| `utils/BinSensor.h` `localBinSystemId` | **exact** compare — its own comment says it mirrors NodeBus "deliberately" while saying it does not normalise |
| `control/Shop.h:235` `plugOwnerOf` | role-based (`isSecondary`), a third notion |
| `firmware.ino` (new this week): the RF board test `rfBoard != ownControllerId()` and `busForNode` | **exact** compare, and a normalised one — I added both |
| `firmware.ino:3814` jog handler | goes through `busForController`, correct |

A layout whose `controllerId` is spelled differently from the paired host already caused one
"routes gates perfectly and drops every jog" bug (the comment at `NodeBus.h` and `ActuatorBus.h`
tell the story). **Fix:** one small pure header (`control/BoardId.h`: `normalize`, `isSelf`,
`same`) that every site above calls; `bareHost` moves into it. The JS side has two more copies
of `bareHost` (`tools/mock-api.js`, `dustgate-ui/.../board-setup.component.ts:358`); export it
from `shared/device-model` and add it to the CLAUDE.md pair table as a pair with the C++.
Why first: the platform split decides board identity in the shared core, so it must be one rule
before it is moved.

### 2. Read a plug's claim in one place — HIGH value, small, low risk

"Ask the plug who owns it, then decide" is written **three times**, each branching Tasmota
(`readOwner` / `decideMarker`) against Shelly (`readPushConfig` / `decide`):
`control/SmartOutletControl.cpp:538-600` (provisioning), `firmware.ino:2152-2170` (discovery
probe), `firmware.ino:2524-2550` (rename). The Tasmota branch of each differs only in what it
also reads (name, MAC). **Fix:** a virtual `SmartOutlet::readClaim(ourHost, ourName) →
{known, Claim}` (the two implementations already exist; this just stops the callers branching on
protocol). It also removes the `static_cast<TasmotaOutlet*>` at `SmartOutletControl.cpp:~538`.
Why first: plug handling moves behind a platform interface, and a branch on protocol at three
call sites is three places to keep in step with it. This is also the area the Gen4 timeout bug
lived in.

### 3. A SensorPlan built once, used to push and to poll — HIGH value, medium, medium risk

`TopologyRuntime::pushSensorConfig` (~1150-1260) and `pollSensors` (~1290-1410) each walk
systems and elements to resolve the same three sensor kinds (clamp, plug, and since this week
the bin) by the same ids (`clampOf`, `sensedIdOf`, `binSensorFor`, `plugOwnerOf`). Every new
sensor kind had to be added **twice** and read back by the same string key (`"bin:" + sysId`
exists in both). **Fix:** one function that resolves the layout into a list of
`{id, kind, owner board, params}`; the push serialises it per board, the poll looks readings up
by `id`. It also removes the `_nodePlugs` side table. Why first: it is the core's contract with
the node protocol, and it is the exact code a second shell must not re-derive.

### 4. Split `RemoteActuatorBus` into a session and a transport — HIGH value, LARGE, high risk

`control/RemoteActuatorBus.{h,cpp}` (~1,100 lines) holds the portable part (WELCOME, ACK,
SENSE, STATE, OTASTATE, move timeout, cached CONFIG, link health, OTA state, the new PRESS) and
**two transports** with duplicated bookkeeping: the dial-out socket on its own task, and the
node-initiated socket. The send drains are written twice (`taskLoop` ~440-460 vs `pumpInbound`
~560-578) and `markDown`, `attachInbound`, `end`, `begin` each reset overlapping state, which is
how the stale-socket bug of today's soak got in. **Fix:** a pure `NodeSession`
(`onFrame`, `takeOutgoing`, `online(now)`, `health`, clock passed in; no sockets, no mutex, no
task) with thin transports on each side. This is the single biggest piece of the core that is not
shared yet, and it is where a Linux shell needs to plug in. **Re-run the hardware soak after.**

### 5. Primary-as-a-node: delete it — MEDIUM value, small, low risk (needs one decision)

`api/HttpApiServer.cpp` (~300-415, 620-640) and `firmware.ino` (~3975-4015) implement a
**second node**: a primary can answer HELLO/PING/SET on its own `/nodelink` as if it were a
secondary (`_nodeWs`, `_nodeSetCmd`, `consumeNodeSet`, `reportNodeState`, `nodeLinkConnected`).
It duplicates the real node program's frame handler (HELLO and WELCOME construction, SET
parsing), is the reason my JOIN hook has to intercept events on that listener, and is what the
status "self" node and the OLED's "primary linked" read. The slider-as-node is now the
`xiao_c5_linear` node program. **Decision needed:** is a primary adopted as someone else's node
a supported shape? If not, remove it and the listener becomes the node-initiated listener only.

## Do these as part of the shared-core work (not before)

### 6. API handlers out of `HttpApiServer.cpp` — the plan's step 2

2,500 lines, 50 routes, 49 `checkAuth(req)` calls, 80 `sendError(req, …)`, 30 `StaticJsonDocument
doc;` and 31 `sendOk(req)`: the same auth, parse, validate, respond shape every time, with the
logic inline (pairing, plug claiming, topology adopt, resets, discovery results). A small route
helper (auth, parse into a document, run a function returning `{status, body}`) removes most of
the repetition and is exactly the seam that makes the handlers shareable. Doing it as the
platform interface lands avoids doing it twice.

### 7. One HTTP seam for plugs

`outlets/ShellyGen2Outlet.cpp` (4 `HTTPClient` blocks) and `TasmotaOutlet.cpp` (6) each build a
URL, set timeouts, GET or POST, check the code and parse; the timeout bug lived in the gap
between them. A `plughttp::get/post(url, timeoutMs) → {code, body}` function removes the
duplication and **is** the platform interface for step 3 (Arduino `HTTPClient` on the ESP32,
an HTTP client on Linux). `SerialDebugControl.cpp` and the node's OTA download use `HTTPClient`
too and can follow.

## Low priority — real, but not blocking

- **UI constants.** The servo-bank budget is still a five-sided constant (`SERVO_PORTS`,
  `SERVO_CHANNELS_PER_BOARD`, `servoPortsPerBoard`, topology.js, config.h) and the wiring
  fixtures have twice encoded it as channel indices. Quick and safe; independent of the rewrite.
- **`build.component.ts` is 6,219 lines.** Leave it; split it when a feature touches it.
- **`firmware.ino` is 4,917 lines** of setup, loop and glue. It shrinks as items 4-7 land; do
  not reorganise it separately.
- **Three call sites of `syncPairedNodes(WiFiProvisioner::getHostname().c_str())`** at
  `firmware.ino` — trivial; fold into a helper if the signature changes.

## Strategic duplicates — leave alone, and know why

- **Routing, sequencing, shop and nodelink exist in C++ and in `shared/device-model`**, kept in
  step by paired tests and shared fixtures. That is the anti-drift rule working, not a defect.
  Once the C++ core also builds natively (and, optionally, to WASM for the in-browser demo), the
  JS copies of the *decision* logic become redundant; that is a later, deliberate decision.
- **Validation is authoritative in JS** (`validateTopology`/`validateShop`); the firmware
  validates minimally. Fine while the UI always validates; revisit if a non-UI client ever PUTs a
  layout.
- **`tools/mock-api.js` mirrors the device API** by design.

## Recommended order, and how to land it

1. Items 1, 2, 5 first: small, independent, each leaves the tests green; 5 needs your call.
2. Item 3, then item 4 (largest; **hardware soak afterwards**, the same eight reboots and four
   pause/resume cycles).
3. Merge those to `main` as small PRs **before** the native build starts, so the Linux shell
   begins from the cleaned core.
4. Items 6 and 7 land with the platform interface.

Nothing here needs hardware to be written; items 3-4 need it to be trusted.
