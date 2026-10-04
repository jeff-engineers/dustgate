# Nodes dial the brain — scope (2026-10-04)

**Status: a plan, nothing built.** Written after the first full-shop test (nine
nodes paired, brain at 22 KB free internal RAM, 14 KB at the low-water mark, the app
refusing to load). Read it with `docs/` RFCs on NodeLink and the ESP-NOW notes in
`TODO/TODO.md`.

## The problem, measured

`bash dev.sh tasks` on the loaded brain (build `8d58d3c`, 9 nodes paired):

- internal RAM 22 KB free, 14 KB lowest-ever, largest free block 15 KB — below the
  28 KB floor the page guard needs to serve the app, so every page load got 503;
- **nine `nodelink` tasks**, one per paired node, each with a 5 KB stack (~45 KB), the
  three that were linked using ~3.7 KB of it (stack free-min ~1.4 KB: it cannot be
  trimmed); each link measured at ~10 KB all-in (stack + WebSocket client + socket);
- the brain DIALS each node, so it also owns every node's address: a cached IP that
  goes stale, an mDNS lookup per node (and the mDNS lock showing `busy — skipped`),
  and the "hollow connection" class (TCP opens, the WebSocket upgrade never completes);
- the "network is isolating us" rule (`rejoinWifiIfNetworkBlocksNodes`) exists only
  because the brain dials. (Already narrowed 2026-10-04: no rejoin while any node is
  linked.)

Both radios are on 2.4 GHz by design (`WiFi.setBandMode(WIFI_BAND_MODE_2G_ONLY)`,
`utils/WiFiProvisioner.h`); the brain and nodes are on different mesh APs, channels 11
and 1, and linking across them works.

## The idea

Reverse the connection. The brain stops dialling. It **listens** on a second
`AsyncWebSocket` (`/nodelink`, beside the app's `/ws`), and every node dials in. A node
connection on the brain is then an event-driven socket on the existing `async_tcp`
task — **no task, no stack, no per-node address, no mDNS lookup**.

What does not change: the frames (SET, STATE, CONFIG, SENSE, PING/PONG, OTA/OTASTATE),
`ActuatorBus`, `NodeBus`, `TopologyRuntime`, the topology, the UI. Only the transport
and the handshake move.

## Memory — an estimate, to be measured before building

| | now (per node) | after (per node) |
|---|---|---|
| link task stack | 5.0 KB | 0 |
| WebSocket client object + buffers | ~2 KB | 0 |
| socket | ~2.5 KB (lwIP pcb + WiFiClient) | ~1 KB pcb |
| `AsyncClient` + `AsyncWebSocketClient` + queue | 0 | ~1.5–2 KB |
| **total** | **~10 KB** | **~2.5–3 KB** |

So roughly **−7 KB per node: ~60 KB at nine nodes, ~100 KB at fifteen** — the difference
between "does not fit" and "fits with room for the UI". This is a guess about the
AsyncTCP side. **Step 0 measures it.**

### Step 0 — a spike that costs an hour and decides the plan
Open N WebSocket clients from the Mac to the brain's existing `/ws` and read
`heap.internal.free` from `/api/info` after each: that is the real per-connection cost of
`AsyncWebSocketClient` on this board, before any firmware is written. If it is above
~5 KB per connection, the saving is much smaller and option A (one shared link task) or
a bigger board gets another look.

## Protocol

NodeLink today: the brain sends `HELLO {primaryId, nodeId, takeover?}`, the node answers
`WELCOME {nodeId, board, fw, caps, claimedBy?, accepted?, upS, rst}`. Inverted:

1. node opens `ws://<brain>/nodelink` and sends **`JOIN`** first — the content of
   today's WELCOME (identity, board, fw, caps, boot info) plus `claimedBy`: the brain it
   believes it belongs to (empty if none);
2. the brain looks the node up in the paired registry (`NodeRegistry`, host =
   `nodeId`) and answers **`HELLO`** `{primaryId, nodeId, takeover?}` if it is paired, or
   **`REFUSE {reason}`** and closes if it is not;
3. the node answers **`WELCOME {accepted}`** as today — the claim rule (a node belongs to
   one brain; takeover is user-confirmed, one-shot) is unchanged and still enforced on
   the NODE;
4. from there it is the existing protocol.

`NODELINK_VERSION` goes to **2** (the handshake order changed — an existing frame's
meaning moved, which is the bump rule in `nodelink.js`). New frames: `JOIN`, `REFUSE`.
Bounds on anything new go in the `CLAUDE.md` constants table.

## The two hard problems

### 1. How does a node find the brain, with NO router changes and no mDNS requirement?
Two constraints from CLAUDE.md bind this: nothing may *require* multicast, and **an install
step the owner cannot perform is a different product** — so a DHCP reservation is NOT an
answer (decided 2026-10-04, jeff: "that doesn't seem like something a woodworker is going
to be able to do"). Nodes find the brain themselves, in order, and stop at the first hit:
1. **the address it last connected to** (NVS) — free, works on every network;
2. **`<owner>.local`** by mDNS (the owner name the node already stores) — a fast path
   only, never relied on;
3. **a UDP broadcast beacon** from the brain, every ~10 s on a fixed port — a node that
   has lost the brain listens for it. Broadcast is not multicast and passes most guest
   networks; it is the main recovery path when the brain's address changes;
4. **a subnet sweep** — probe the local /24 for `GET /api/info` on port 80 and match the
   owner name, the way the brain already finds Tasmota plugs (`/api/outlets/sweep`).
   Slow (tens of seconds, jittered so nodes do not all sweep at once) and needs nothing
   from the network at all; it is the floor under everything above.

A brain whose address changes after a reboot therefore costs its nodes a few seconds, not
a visit: they drop, fail on the old address, and find it again by 2, 3 or 4. (Today the
reverse problem — a NODE's address changing — is what leaves a dark link on a stale cached
IP.) A reservation is still good practice for someone who can set one, and the install
notes may say so, but nothing depends on it.

### 2. Unpaired and unclaimed nodes (the "Add another board" screen)
Today the brain finds new boards by an mDNS scan (multicast). Inverted, an unpaired node
simply **dials in and sends JOIN**, and the brain answers `REFUSE not-paired` and keeps a
short-lived `seen` list (host, address, board, fw, claimedBy, last-seen). `GET
/api/nodes/discover` returns that list, so discovery stops needing multicast at all —
a net improvement on the "never require a blockable network" rule. Pairing adds the host
to the registry; the node's next retry (a few seconds) is accepted. Cap the unpaired
connections held at once (2) so a noisy network cannot spend the RAM this is meant to save.

A **fresh** node has no owner and no address to dial: it uses step 3 above, else browses
mDNS for `_dustgate._tcp` brains and dials each in turn — it will appear in the Add-board
list of every brain on the network and the person pairs it from the one they want.

## Work

**Brain**
- `control/RemoteActuatorBus.{h,cpp}` (955 + 323 lines) splits in two. The *policy* half —
  move state, SENSE storage, CONFIG, OTA state, link health — stays and keeps the
  `ActuatorBus` interface. The *transport* half — the task, `links2004` dialling, mDNS
  resolve, backoff, hollow-drop counting — is replaced by an inbound binding: a
  `WS_EVT_DATA` handler that routes a frame to the bus owning that client id.
- the `/nodelink` server, the JOIN/HELLO/REFUSE handshake, the `seen` list, app-level
  PING from the loop (kept: it is how a half-dead link is noticed).
- delete `NodeLinkPlan.h` (incremental dialling), `syncPairedNodes`' dial/stop machinery,
  the staggered start, `rejoinWifiIfNetworkBlocksNodes`, `kNodeLinkTaskStack`, the
  NodeLink-only mDNS lock use. `ensureRemotePool()` becomes a pool of bus objects with no
  tasks.

**Node** (`node/dustgate_node.cpp`)
- the `AsyncWebSocket nodeWs` server and `kMaxLinkClients` go; a `WebSocketsClient` runs on
  a small task of its own (its `connect()` blocks up to the dial timeout, which must not
  stall servos or the clamp — the node has the RAM for it). Frame handling is reused.
- brain discovery (the four steps above) and NVS for the last brain address; jittered
  backoff per node so nine nodes do not redial the same second after a brain reboot.
- `OtaGuard` health becomes "WiFi up and a JOIN accepted" — it already means that.

**Tools/tests**
- `shared/device-model/nodelink.js` ↔ `NodeLink.h`: `JOIN`, `REFUSE`, version 2, the
  handshake order. `nodelink.test.js` ↔ `test_nodebus.cpp`, same cases, same order.
- `tools/mock-node.js` and the nodelink conformance suite become clients.
- `dev.sh provision` carries the brain's name; `dev.sh ota` is unchanged.

## Migration — by OTA, no cable

Every board that has had the cable pass has two app slots, so a node can be updated over
the link it already has. The brain runs **both modes for one release**: it still dials a
paired node that has not migrated (a v1 node: it listens) and accepts inbound v2 nodes.
Update the brain, then update each node from the Boards screen; when a node reboots into
v2 it stops listening and dials in, and the brain's old dial to it fails and is dropped
(the registry remembers it as migrated). The dual mode is deleted in the release after.
Without it, the first inverted brain would strand every node until a cable reached it.

## What this removes, what it adds

Removes: nine tasks, ~60 KB of internal RAM, stale cached addresses, per-node mDNS,
hollow connections, the WiFi-rejoin heuristic, the staggered boot, the unpaired-board
multicast scan.

Adds: the brain's address becomes a thing the nodes must find; an unauthenticated listener
(any device on the LAN can dial `/nodelink` and send a JOIN — it is refused unless the
host name is paired, which is no stronger than today's trust model of "whoever is on the
network" and should be said plainly, not fixed here); a thundering herd after a brain
reboot (jitter); node-side code to be debugged on nine boards at once.

## Open questions for Jeff

1. ~~**Brain address:** DHCP reservation?~~ **Settled 2026-10-04: no.** Nodes find the
   brain by cached address → mDNS → UDP broadcast beacon → subnet sweep (above). Still
   open: the beacon's port and interval, and how patient the sweep should be.
2. **Fresh nodes:** dial every brain found by mDNS (the person pairs from the right one),
   or require the brain's name at flash time and refuse to dial anything else?
3. **Dual mode:** one release of both-mode, as above — or take the risk and cable every
   node once?
4. **The unpaired list:** how long should an unpaired node stay in the discover list
   (60 s?) and how many unpaired connections may the brain hold at once (2?).

## Order of work

0. spike — measure `AsyncWebSocketClient` cost on the board (an hour, decides the rest);
1. protocol: JS + C++ + tests (`JOIN`, `REFUSE`, version 2);
2. brain: the inbound listener and the bus split, behind dual mode;
3. node: the dialling client and brain discovery;
4. migrate one node over OTA, then the shop; re-run `dev.sh tasks` on a fully loaded brain;
5. delete the dial path.
