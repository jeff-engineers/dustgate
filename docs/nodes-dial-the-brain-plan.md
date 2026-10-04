# Nodes find the brain — scope (2026-10-04, rewritten the same day)

**Status: a plan, nothing built.** Written after the first full-shop test (nine nodes
paired, brain at 22 KB free internal RAM, 14 KB at the low-water mark, the app refusing
to load), and reshaped by jeff's suggestion the same evening: *pair from the brain, keep
the steady-state link node-initiated, and when a link drops let both ends seek each
other, with the brain's seeking limited to one or two attempts at a time.* The first
version of this document had unpaired nodes dialling in; this one does not.

## The problem, measured

`bash dev.sh tasks` on the loaded brain (build `8d58d3c`, 9 nodes paired):

- internal RAM 22 KB free, 14 KB lowest-ever, largest free block 15 KB — below the 28 KB
  floor the page guard needs to serve the app, so every page load got 503;
- **nine `nodelink` tasks**, one per paired node, each with a 5 KB stack (~45 KB; the
  three that were linked had ~1.4 KB of it free, so it cannot be trimmed); each link
  measured at roughly 7–10 KB all-in (stack + WebSocket client + socket);
- the brain DIALS each node, so it also owns every node's address: a cached IP that goes
  stale, an mDNS lookup per node (the mDNS lock showing `busy — skipped`), and the
  "hollow connection" class (TCP opens, the upgrade never completes);
- `rejoinWifiIfNetworkBlocksNodes` exists only because the brain dials (narrowed
  2026-10-04: no rejoin while any node is linked).

Both radios are on 2.4 GHz by design (`WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY)`); the
brain and nodes were on different mesh APs (channels 11 and 1) and linked across them.

## The design

**Pairing stays exactly as it is.** The brain finds or is given a node's address, dials it
once, runs the HELLO/WELCOME claim, and stores it. Nothing about the user's flow, the
claim rule (a node belongs to one brain; takeover is user-confirmed and one-shot), or the
Boards screen changes. At pairing the node also stores who owns it and where that brain
is.

**The steady-state link is node-initiated.** The node dials the brain's `/nodelink`
(a second `AsyncWebSocket` beside the app's `/ws`). On the brain that is an event-driven
socket on the existing `async_tcp` task: **no task, no stack, no per-node address, no
mDNS lookup.** A node holds exactly ONE link; a second arrival is refused (`REFUSE
duplicate`) — whichever connection gets there first wins, and the NODE decides.

**When a link drops, both ends seek:**
- **The node** retries toward the brain, in order, stopping at the first hit:
  1. the address it last connected to (NVS);
  2. `<owner>.local` by mDNS — a fast path, never relied on;
  3. a UDP **broadcast beacon** the brain sends every ~10 s on a fixed port (broadcast is
     not multicast and passes most guest networks);
  4. a **subnet sweep** — probe the /24 for `GET /api/info` on port 80 and match the owner
     name, as the brain already does for Tasmota plugs. Slow, jittered, needs nothing from
     the network at all: the floor under everything.
  Nodes have the RAM for this; backoff is jittered per node so nine do not redial the same
  second after a brain reboot.
- **The brain** runs a **seeker with at most two attempts in flight**, cycling through
  paired nodes that are down (round-robin, per-node backoff 2 → 60 s). A seeker attempt
  does NOT hold a link: it connects to the node, sends a one-shot **`WHERE`** message —
  "the brain `<owner>` is at `<ip>` now" — and hangs up; the node then dials in. So the
  brain's cost for a down node is a transient, never a resident socket, and the memory it
  spends on seeking is fixed no matter how many nodes are down.

**Why both directions:** a path can work one way only — on 2026-09-27 the brain's TCP to a
node timed out while a laptop reached the same node in 10 ms (`RemoteActuatorBus.h`,
`LinkHealth`). With both ends trying, one working direction is enough; with either alone
it is not. It also covers the brain's address changing, with **no router setting asked of
anyone** (a DHCP reservation was considered and dropped: CLAUDE.md — an install step the
owner cannot perform makes it a different product).

**Migration is the same mechanism.** A node that has not been updated is simply one the
seeker still dials the old way (a resident client link, ~10 KB, as today) until it is
updated over that link and starts dialling in. There is no separate "dual mode" to build
and delete: the seeker's old-style dial IS the legacy path, and it stays as a recovery path
afterwards.

What does not change: SET, STATE, CONFIG, SENSE, PING/PONG, OTA/OTASTATE; `ActuatorBus`,
`NodeBus`, `TopologyRuntime`, the topology, the UI.

## Protocol

`NODELINK_VERSION` → **2** (the handshake order changed for the steady-state link — an
existing frame's meaning moved, which is the bump rule in `nodelink.js`).

- Node-initiated link: the node connects and sends **`JOIN`** first (today's WELCOME
  content: identity, board, fw, caps, boot info, plus `claimedBy`). The brain checks the
  registry — if the host is paired it answers **`HELLO`** `{primaryId, nodeId}`, otherwise
  **`REFUSE {reason}`** and closes (`not-paired`, `duplicate`). The node then answers
  **`WELCOME {accepted}`** as today, and the claim is still enforced on the NODE.
- Brain-initiated seek: **`WHERE {primaryId, ip, port}`**, sent on a short-lived connection
  to a node's existing `/nodelink`, answered `ACK` and followed by the node dialling in.
  A node acts on it ONLY if `primaryId` is its owner; the claim check still decides every
  command, so a forged `WHERE` can at worst make a node knock on a door that refuses it.
- New: `JOIN`, `REFUSE`, `WHERE`. Bounds on any of them go in the CLAUDE.md constants table;
  `nodelink.test.js` ↔ `test_nodebus.cpp`, same cases, same order.

## Memory and headroom — measured where it says measured, estimated where it does not

**Measured (`ws-spike.js`, 2026-10-04, quiet brain `c8e151f`):** a brain with no layout and
no nodes has **85–94 KB** internal RAM free; one more inbound WebSocket costs **2.4–2.8 KB**
(2,786 / 2,080 / 2,432 B for the first three; the fourth was refused by a bug in the page
guard counting WebSocket upgrades as static files, since fixed). A node link today costs
roughly 7–10 KB, from the 9-node run: 85 KB → 22 KB.

**Estimated, to be re-measured on the first build:**

| item | cost |
|---|---|
| one node, inbound (socket + runtime state) | ~3.5 KB (2.6 measured + ~1 KB state) |
| layout loaded, runtime maps | ~5 KB |
| the app: 2–3 browser sockets | ~8 KB |
| the seeker (one small task + 2 attempts) | ~8 KB |
| 16 outlet objects | ~5 KB |
| **fixed total** | **~26 KB** → about **59 KB** left to spend |
| to keep: the page-load floor (the guard refuses the app below it) | 28 KB |
| **left for nodes** | **~31 KB** |

### What that buys

| | brain serves the app | app hosted off-board (`tools/ui-host.js` / a Pi) |
|---|---|---|
| floor to keep | 28 KB | 12 KB (the API floor) |
| app sockets | ~8 KB | ~3 KB |
| left for nodes | ~31 KB | ~52 KB |
| **nodes** | **~9** | **~14–15** |

- **Your shop (8 nodes)** fits either way, but with the app on the brain it sits just above
  the floor (~31 KB free), so a layout save, an OTA upload or three browsers at once will
  push it into 503s now and then. Off-board, it is comfortable.
- Levers, none of them counted above: `dev.sh ota --extmem N` (send small allocations to
  PSRAM — an unmeasured 10–20 KB, maybe 3–5 more nodes); lowering the guard to 2 slots /
  20 KB (+8 KB); a bigger board remains the answer past ~15 nodes.
- **Hard caps today:** `MAX_SECONDARY_NODES` = 10 (firmware.ino), and **`SMART_OUTLET_COUNT`
  = 7** (config.h). The second matters more than RAM: you have 12–15 outlets, and a plug
  occupies a brain outlet slot even when a node polls it (the brain keeps the object for
  identity and claiming). It must go to ~16 (≈ +0.3 KB each, plus NVS keys) before the
  shop can be paired at all.

### Outlets

- **Tasmota, brain-polled:** ~0.3 KB for the object, plus ~4 KB for the poll in flight — shared,
  one at a time in `outletPoll` — so 15 of them cost ≈ 5 KB, not 60.
- **Tasmota or Shelly, polled by the node that controls the tool:** the same ~0.3 KB on the
  brain, and nothing for the polling. Preferred for any tool whose gate is on a node.
- **Shelly with push** (the plug dials the brain's `/shelly-rpc`): ~2.6 KB each, resident, like a
  node link. A handful is fine; fifteen would be ≈ 39 KB, which the budget does not have.
  Polling a Shelly (or node-polling it) costs a fraction of that, so beyond a few, **poll**
  Shellys rather than letting them push.

## Work

**Brain**
- `control/RemoteActuatorBus.{h,cpp}` splits. The policy half (move state, SENSE storage,
  CONFIG, OTA state, link health) stays behind `ActuatorBus`; the transport half is replaced
  by an inbound binding for node-initiated links, and the old per-node dial shrinks to the
  seeker's one-shot `WHERE` (and the legacy dial for un-updated nodes).
- the `/nodelink` server and JOIN/HELLO/REFUSE handshake; the seeker; the beacon;
  app-level PING from the loop (it is how a half-dead link is noticed).
- delete: `NodeLinkPlan.h`, the per-node task, the staggered start, `kNodeLinkTaskStack`,
  `rejoinWifiIfNetworkBlocksNodes`, per-node mDNS resolve. `ensureRemotePool()` becomes a
  pool of bus objects with no tasks.
- raise `SMART_OUTLET_COUNT` to ~16.

**Node** (`node/dustgate_node.cpp`)
- keeps its `/nodelink` listener (for pairing, `WHERE` and legacy dials) and gains a
  `WebSocketsClient` on a small task of its own (its `connect()` blocks up to the dial
  timeout, which must not stall servos or the clamp). One-link rule; frame handling reused.
- brain discovery (the four steps), NVS for the owner's address, jittered backoff.
- `OtaGuard` health becomes "WiFi up and a JOIN accepted" — it already means that.

**Tools/tests**
- `shared/device-model/nodelink.js` ↔ `NodeLink.h`: `JOIN`, `REFUSE`, `WHERE`, version 2,
  the handshake order; `tools/mock-node.js` and the nodelink conformance suite learn to dial.
- `dev.sh provision` carries the brain's name for a node flashed with no owner yet.

## What it removes, what it adds

Removes: nine tasks, ~55–60 KB of internal RAM at nine nodes, per-node mDNS, the
"hollow connection" class, the WiFi-rejoin heuristic, the staggered boot.

Adds: node-side brain discovery (four steps); a cross-dial race and its one-link rule; a
broadcast beacon; more states to test, which is where the care goes. The listener on
`/nodelink` is unauthenticated, as today's node listener is — refused unless the host is
paired — which is no stronger than the existing "whoever is on the network" trust model and
should be said plainly, not fixed here.

## Open questions for Jeff

1. ~~Brain address: DHCP reservation?~~ **Settled: no.** Cached address → mDNS → broadcast
   beacon → subnet sweep. Still open: the beacon's port and interval, and how patient the
   sweep should be.
2. ~~Fresh nodes: which brain to dial?~~ **Settled by the new design:** a node never dials a
   brain it is not paired with. Pairing is brain-initiated, as now.
3. ~~Dual mode vs a cable pass.~~ **Settled:** the seeker's old-style dial covers legacy
   nodes; migration is by OTA over the existing link, no cable.
4. **Where does the app live?** On the brain it fits eight nodes tightly; off-board it fits
   ~14. This is the biggest headroom decision and it is a product one, not a firmware one.
5. **The seeker's pace:** two attempts in flight, backoff 2 → 60 s per node — is recovery of
   10–30 s after a long outage acceptable?

## Order of work

0. ~~spike~~ **done** — `ws-spike.js`, 2.4–2.8 KB per inbound client; rerun for the full nine
   after the guard fix is on the brain;
1. protocol: JS + C++ + tests (`JOIN`, `REFUSE`, `WHERE`, version 2);
2. brain: the inbound listener, the bus split, the seeker, the beacon, `SMART_OUTLET_COUNT`;
3. node: the dialling client, brain discovery, one-link rule;
4. migrate one node over OTA, then the shop; re-run `dev.sh tasks` on a fully loaded brain and
   compare it with the table above;
5. delete the per-node task and the rejoin heuristic.
