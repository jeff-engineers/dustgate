# What the nodes do and what the brain does (2026-10-04)

**Status: describes the code as it is on `brain-core-shared`, and what stays true after the
shared-core rewrite.** The rewrite moves the brain's *process* (ESP32, Pi, Mac); it does not move
any of these decisions. Items marked ⚠ are built but not yet verified on hardware, or are a known
duplicate. The governing rule is the header of `shared/device-model/nodelink.js`: **a node may own
any control loop faster than a WiFi round trip, and no interpretation of the document.**

## 1. Who decides what

| Decision | Made by | Detail |
|---|---|---|
| **A CT clamp's "tool is running" bit** | **node** | The node samples its ADC over a ~60 ms window (about 3.5 cycles at 60 Hz) and computes RMS in raw ADC counts, learns its own noise **floor** at rest, then trips when the RMS exceeds `tripRatio × floor` (with a `minCounts` guard so a tiny floor cannot make it hypersensitive) and releases below `clearRatio × trip` (hysteresis). Amps are converted with the board's own amps-per-count, for display only. The brain never sees counts or volts; it receives a boolean. `sensing/CtTrip.h`, `CtSensor.h` |
| The CT tuning numbers (`tripRatio`, `minCounts`, `clearRatio`) | **brain** chooses, **node** applies | Sent in CONFIG so retuning a shop is a brain change, not a ladder at every node. ⚠ Until 2026-10-04 `RemoteActuatorBus` copied only the clamp's channel, so these never reached the wire (a known bug); `NodeSession::configureSensors` now carries them. Bounds are a JS/C++ pair |
| **A smart plug's wattage** (a tool whose gate is on a node) | **node** reads | The node polls the Shelly or Tasmota over HTTP on its own task, and sends `watts` (plus an unreachable flag) in SENSE |
| ⚠ **That plug's on/off threshold** | **brain, and also node** | The node compares watts to the `thresholdW` it was sent and sets SENSE `on`; **the brain ignores that bit and compares the watts itself** (`TopologyRuntime::pollSensors` feeds `setMachinePower(id, watts)`, which applies the layout's threshold). Two comparisons of the same number — harmless today, a candidate to simplify |
| A smart plug's wattage and threshold (a tool whose gate is on the brain's own board, or on no node) | **brain** | Polled (or pushed, for a Shelly) and compared on the brain in `SmartOutletControl` / `TopologyController` |
| **Dust bin level** | **node** reads and debounces; **brain** consumes the bit | The node reads its pad, applies `invert`, holds a reading for 2 s (`BinSensor.h`), reports `on` = full. The brain turns it into `systems[].bin.full` |
| **Which tools are active; which gates open** | **brain** | Active set ordered most-recent-first, routing per system, `idle-hold` when nothing runs. `TopologyRouter.h`, `Shop.h`, `routing.js` |
| **Make-before-break ordering; never dead-head** | **brain** | Opens before closes, a move at a time (the shop-wide servo mutex), re-assert on a machine's rising edge, move timeouts. `TopologySequencer.h`, `TopologyRuntime.h` |
| **Where a gate goes** (the resolved angle or mm) | **brain** | `referenceAngle + offsetDeg`, clamped, from the layout's calibration. The node receives a **number**, never a state name |
| **Servo motion itself** (PWM, move-then-detach, hold at rest) | **node** | Plus "ACK means accepted, STATE means arrived" |
| **The slider's homing sweep, datum, steps** | **node** | A closed loop between an endstop and a servo that cannot round-trip per step. The brain sends a position in mm; the node homes itself first if it must |
| **Collector start/stop policy** (when to command, retry, give up) | **brain** | `CollectorPress.h`: press once, wait out the spin-up grace, read the draw, press again if it disagrees, give up after 3. The cooldown is a safety property (a second press during spin-up turns the blower off) |
| **Keying the RF transmitter** | **node** ⚠ | On a PRESS the node keys its 315 MHz pad once with the address, data, timing and repeat count from the frame. Not yet run on a node |
| **"Is the blower actually running?"** | **brain** | `CollectorPlugState.h`: draw ≥ 50 W after a 4 s spin-up grace, from the collector's plug, or from a clamp (as a synthetic wattage). Needs a board that reports the draw |
| **Collector coast-down, manual run** | **brain** | Per system, default 5 s |
| **Plug identity, discovery, ownership (claim), provisioning** | **brain** | mDNS and subnet sweep, `plugclaim` rules, `Ws.SetConfig` / Tasmota Mem1, takeover (user-confirmed only), MAC-based relocation |
| **Which board polls which plug** | **brain** | `plugOwners()` ↔ `plugOwnerOf()`: the board that controls the tool's gate polls its plug, else the brain |
| **Who owns a node** (the claim) | **node** enforces, **brain** asks | A node persists its owner and refuses SET, CONFIG, OTA and PRESS from anyone else; only a user-confirmed takeover moves it |
| **Pairing, the node registry, link health, problems list** | **brain** | |
| **Firmware update** | **node** executes, **brain** orders and serves | The node pulls the image over HTTP, checks MD5, runs the new slot on probation, and rolls back by itself. The brain stages the image and sends an OTA order |
| **Finding the brain** | **node** | Last address, `<owner>.local`, the brain's UDP beacon, a slow subnet sweep, or a `WHERE`; the brain accepts a JOIN. Pairing is still brain-initiated |
| **The UI, the API, the layout, persistence** | **brain** | Nodes serve nothing but `/nodelink` and `/api/brainlink` |

## 2. What crosses NodeLink

Every frame is one JSON message on one WebSocket, `v: 1`. The node protocol is additive: an old end
ignores a frame it does not know.

| Direction | Frame | Carries | Why it exists |
|---|---|---|---|
| node → brain | `JOIN` | `nodeId` | A node dialling its brain |
| brain → node | `HELLO` | `primaryId` (also the claim), `nodeId`, `takeover?` | Start of the handshake |
| node → brain | `WELCOME` | identity (`nodeId`, `board`, `fw`), `caps` {`servos`, `linear`, `ct`, `plug`, `join`, `rf`, `bin`}, `accepted?` + `claimedBy`, boot info (`upS`, `rst`) | Says what the board **can** do. Capabilities are reported from the pin map, never chosen |
| brain → node | `REFUSE` | `reason` (`not-paired`, `duplicate`, `busy`) | A JOIN the brain declines |
| brain → node | `SET` | `channel`, `drive`, `angle` or `positionMm`, `holdAtRest`, ids | A **resolved** command. No topology, no state names |
| node → brain | `ACK` | `seq`, `ok`, `err?` | Accepted (not arrived) |
| node → brain | `STATE` | selector, state, `moving` | Arrived |
| brain → node | `CONFIG` | the **whole** sensor list: `ct` {`channel`, tuning}, `plug` {`ip`, kind, `thresholdW`}, `bin` {`invert`} | What the layout says is wired to this board. A list, never a delta; an empty list means "report nothing" |
| node → brain | `SENSE` | `sensorId`, `on`, and telemetry: `level`, `amps`, `floorA`, `tripA`, `fault`, `watts`, `plug` | One bit, decided on the node; the rest is for people. Sent on change and every 5 s |
| brain → node | `PRESS` ⚠ | `address`, `data`, `tickUs`, `repeats` | Key the transmitter once. The edge, not a state |
| brain → node | `OTA` | `path`, `size`, `md5`, `fw` | Pull and install an image |
| node → brain | `OTASTATE` | `state`, `pct`, `err?` | Progress |
| brain → node | `WHERE` | `primaryId`, `ip`, `port` | "I am here now" (the seeker, phase 2) |
| both | WebSocket ping/pong | — | Liveness: the node declares the brain dead after two missed pongs; the brain calls a node offline after 6 s of silence |

**What does not cross:** the layout or its schema, clamp thresholds or raw samples, calibration
data (only the resolved number), plug credentials, the UI. A node never reads the document; that is
what lets a $5 board be a node and keeps a schema change from needing a flash to every board.

## 3. A tool turning on, end to end

1. **A tool starts.** Its clamp sees RMS cross `tripRatio × floor` (node), or its plug's watts rise (node or brain).
2. **The node sends SENSE** `{sensorId: "<machine>", on: true, …}`. For a plug it sends watts; the brain applies the layout's threshold.
3. **The brain** marks the machine active, routes the system, and plans the transition: open the new path, then close the old one.
4. **The brain sends SET** frames, one move at a time, each with a resolved angle. The node ACKs, moves, and sends STATE `arrived`.
5. **The brain starts the collector**: a plug switch, or a PRESS to the collector node, then judges the result from the draw.
6. **Tool stops.** SENSE `on: false`; the brain holds the gates (idle-hold) and coasts the blower down.

**Failure behaviour:** if the link drops mid-move the node **holds every gate** where it is (it never
slams one shut), and the brain marks that board un-commandable so a failed *make* holds the blower
off. A queued PRESS is dropped on a link drop — it is an edge against a toggle and must never replay.

## 4. After the rewrite

Nothing in the tables above moves. The brain becomes a platform-neutral core with two shells
(ESP32 and native), and a hosted brain simply has no hardware of its own: every gate, clamp, bin and
transmitter is a node, which is why the collector-node frames came first. The decisions in §1 stay
where they are because each is either faster than a WiFi round trip (clamp RMS, servo timing, homing)
or needs a view of the whole shop (routing, sequencing, collector policy, claims).
