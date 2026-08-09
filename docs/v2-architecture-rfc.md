# DustGate v2 architecture RFC

**Status:** Phases 2–4 IMPLEMENTED (2026-08), hardware-unvalidated
**Last reconciled with the code:** 2026-08-07
**Covers:** the evolution from the single linear-actuator product (v1) to a
multi-actuator, multi-node, graph-routed dust-collection system.
**Companion docs:** [`dual-endstop-calibration.md`](dual-endstop-calibration.md)
(a v1/Phase-1 enhancement that this builds on), and the canonical-model +
conformance discipline in [`../shared/device-model/README.md`](../shared/device-model/README.md).

---

## 1. Motivation

The shipped product controls one rack-and-pinion linear actuator that selects
which of N blast gates is open. The real value that emerged, though, isn't the
actuator — it's the **configuration wizard and the smart-outlet automation**.
v2 generalizes the hardware so the same brain + config layer can drive:

- **servo ball-valve gates** (binary open/closed) and **manifolds** (3-state
  diverter: left / right / closed), from the "DIY Blast Gate" designs;
- **many gates across a shop**, spread over **multiple ESP32 nodes** (no long
  cable runs);
- **airflow routing as a graph**, where reaching a tool may pass through several
  gates, not just a single blade.

## 2. Goals / non-goals

**Goals**
- One device-behaviour model that treats the linear actuator, servo gates, and
  servo manifolds as instances of a single abstraction.
- 2–4 nodes, ~15 gates, one node hosting the GUI.
- A visual (drag-and-drop) topology configurator.
- Keep the contract-first discipline: one canonical model + conformance suite
  across mock, demo, and firmware.

**Non-goals (for v2)**
- **No AI/chatbot** in the v2 configurator — manual/visual only.
- Not optimizing for huge shops (5+ nodes, dozens of gates) yet — the model
  should not *preclude* it, but it's not the target.
- Not building distributed autonomy — see §6.

## 3. Locked decisions (this session)

| Decision | Choice | Why |
|---|---|---|
| Scale | 2–4 nodes, ~15 gates | Medium shop; keeps routing + transport simple |
| Control topology | **Centralized brain** | Primary computes routing; secondaries are dumb actuators. Far simpler to reason about |
| Servo type | **Positional (angle)** | HAL is `state → angle`; drop continuous-rotation from scope |
| Transport (near-term) | **WiFi + WebSocket** | Reuses existing stack; ESP-NOW deferred behind a `NodeLink` seam |
| Power (per node) | **USB-PD** (see §7) | Single USB-C cable; 45 W charger; ≤60 W → no e-marked cable |
| Config UX | Visual graph editor, **no chatbot** | |
| Backwards compat | v1 hardware compat **dropped** | Only one v1 unit exists; it gets retrofit |

## 4. Core model: the branch selector

Every actuator is a **branch selector** — it exposes a set of mutually-exclusive
outlet states, plus (usually) all-closed:

| Actuator | Branches | + closed? |
|---|---|---|
| Binary servo gate | 1 (open) | yes |
| Servo manifold (diverter) | 2 (left, right) | yes |
| **Linear actuator** | N (one per gate) | yes (home) |

The linear actuator is **not a special case** — it's the N-branch instance of the
same interface as the servo gates. This unification is the spine of the whole
design: multi-servo, manifolds, and multi-gate paths all fall out as instances.

**HAL (firmware):** `Actuator { states[]; setState(name); currentState() }`.
Positional-servo implementations map `state → angle (+ move time)`; the linear
actuator wraps the existing stepper as an N-state selector.

## 5. Topology & routing

### 5.1 Graph model

The shop is a directed graph:

- **Nodes**: tools (sources), selectors/gates (actuators), junctions, the dust
  collector (sink).
- **Edges**: duct segments.
- Persisted as JSON — the single contract the firmware, mock, and configurator
  all consume.

### 5.2 Branch/port roles

Every branch of a selector has a **role**, which is what lets selectors compose:

- `tool` — a tool is wired here.
- `unassigned` — a real branch exists, no tool yet.
- `blocked` — physically capped; never a selectable destination.
- `feed` — routes to a **downstream selector** (another manifold cluster or
  actuator), not a tool.

The `feed` role is what integrates the two systems: a linear-actuator port can
feed a servo manifold, so an N-port actuator addresses more than N tools and can
reach a distant cluster on another node without a long rack. (Phase 1 implements
`tool | unassigned | blocked`; `feed` is reserved and realized here in v2.)

### 5.3 Manifold pairing (halve the gate count)

Because **only one tool runs at a time**, a 3-state manifold serves a *pair* of
tools with one actuated device instead of two binary gates:

```
trunk ──┬── [manifold] ─┬─ toolA      N tools → N/2 manifolds
        │               └─ toolB
        └── [manifold] ─┬─ toolC
                        └─ toolD
```

To run tool C: set its manifold to C's branch, set **all other manifolds
closed**. Full control, half the servos/wiring/cost. This directly stretches the
Phase-2 power budget — 4 manifold-servos per node cover **8 tools**.

**Constraint:** two tools on the *same* manifold can't be open simultaneously, so
pair tools that are never run together (and sit near each other). Tools on
*different* manifolds running at once is fine (same airflow-splitting as binary
gates today). Odd tool counts: last manifold serves one tool (other port capped)
or use a single binary gate for the odd one.

### 5.4 Routing engine

Given the set of active tools, compute every actuator's required state:

1. Path-find from each active tool to the collector.
2. Set each selector on the path to the branch that continues toward the tool.
3. Set every selector **not** on any active path to closed.
4. **Conflict detection (first-class):** if two active tools need incompatible
   states of a shared selector (e.g. both sides of one manifold), that's a
   detected, surfaced conflict — never last-writer-wins.

At ~15 gates, naive graph search is instant; model it correctly, don't optimize.
Multi-gate paths (a tool behind a `feed` port + a manifold) are the general case;
the flat star is the depth-1 special case.

## 6. Networking (centralized brain) — BUILT

- **Primary** node: WiFi, GUI, Shelly polling, owns the topology + routing.
- **Secondary** nodes: dumb actuator banks. Built as a SEPARATE, ~200-line program
  (`linear_actuator/node/dustgate_node.cpp`), not as `#ifdef`s in the main sketch —
  the stepper, endstops, homing, sweep and Shelly polling are woven through the
  primary `.ino` in ~100 disjoint places, and a preprocessor split would have left
  a sketch nobody could read. That IS the "mostly subtracting" this section
  predicted; it just lives in its own file.
- **Link-loss safe state:** a secondary that loses the primary **holds** every
  servo where it is. No timeout closes gates, no homing on reconnect, no
  autonomous behaviour at all. Asserted end-to-end by the conformance suite.
  (Per-node configurability was never needed — hold is the only sane policy.)
- **Transport:** WiFi + WebSocket, behind the `ActuatorBus` interface
  (`control/ActuatorBus.h`). ESP-NOW can drop in as another implementation
  without touching routing.
- **Protocol:** `shared/device-model/nodelink.js` — versioned JSON frames
  `HELLO / WELCOME / SET / ACK / STATE / PING / PONG`.

**The load-bearing decision, discovered while building:** the primary RESOLVES
every state into a concrete realization before it goes on the wire. A `SET`
carries `angle` (already `referenceAngle + offsetDeg`, clamped) or `positionMm` —
never "put gate3 in the open state". A secondary therefore needs no topology, no
router, no schema version and no calibration data. That asymmetry is what makes
the cheap servo-only node possible at all, and it means a schema change never has
to be flashed to every board in the shop.

Distributed autonomy (secondaries running local logic offline) is explicitly out
of scope until a real need appears.

## 7. Phase 2 hardware — servo node + power

Sizing from the sibling "12 V→5 V converter" analysis:

- **Servos:** 4× **6 kg metal-gear positional** servos per node. With manifold
  pairing that's up to **8 tools per node**.
- **Firmware invariant:** a **hard mutex** — only one servo commanded to move at a
  time (others hold). This is load-bearing: it keeps peak current low and must be
  enforced, not assumed. If a bug moves all four at once, the rail browns out and
  resets the ESP32 mid-actuation.
- **Power budget:** ~1.5–2 A continuous / ~3 A peak on 5 V → a **3 A 5 V rail**;
  ~1000 µF bulk cap (470 µF would do; 1000 µF is cheap insurance).
- **PWM:** ESP32-S2 native LEDC covers 4 channels; a **PCA9685** (16-ch I²C) is
  the clean path if a node ever drives more.

### Power delivery — **selected: USB-PD**

| Option | 5 V stage | Source | All-in | If source owned |
|---|---|---|---|---|
| Brick + XL4015 5 A buck | $3 | 12–15 V ≥3 A brick | ~$14–17 | ~$6–8 |
| Brick + 3 A UBEC | $6 | brick | ~$17–20 | ~$9–11 |
| **✅ USB-PD** | $3 (XL4015) | 45 W USB-C charger + std cable | ~$24–30 | **~$8–12** |

**Chosen: USB-PD** — one USB-C cable, and because manifold pairing + staggering
dropped the load, a **45 W charger is plenty** and **≤60 W means no e-marked
cable** (a standard cable works). Needs a **PD trigger/sink** to negotiate the
high voltage: CH224K bare IC (~$1) / HUSB238 breakout (~$6) / ZY12PDN module
(~$5). Flow: USB-C → PD sink negotiates ~12–15 V → stepper/high-rail runs off it,
**XL4015 bucks to 5 V** for servos. Cheapest if you already own a USB-C charger,
which most people do.

## 8. Configurator (v2)

- **Reuse:** `ApiService`, the WebSocket status stream, Shelly discovery,
  settings, unit prefs — all still valid.
- **New (the big build):** a **topology/graph canvas** — drag-drop tools, gates,
  manifolds, the collector; draw ducts; set branch roles; live status painted on
  the graph. Replaces the single-slider visualizer as the primary surface; render
  a lone linear actuator as the depth-1 special case.
- New surface under new routes so anything shared stays intact. **No chatbot.**

## 9. Phasing

| Phase | Scope | State |
|---|---|---|
| **1** | Linear actuator (v1) + dual-endstop self-calibration + port roles | code complete; **hardware-UNTESTED** |
| **2** | Servo hardware on a node (HAL, one node drives 0+ servos, USB-PD power) | code complete; **hardware-UNTESTED** |
| **3** | Multi-node (NodeLink, WiFi, primary/secondary; ESP-NOW later) | code complete; **hardware-UNTESTED** |
| **4** | Graph configurator + multi-gate routing UX + conflict detection | canvas + `/boards` + `/gates` done; **conflict UX still to surface** (see §11) |

**Nothing below Phase 1 has run on real hardware.** Everything is verified by
compile, host tests and simulation only. The conformance suites are built to be
pointed at a real device, which is the intended first bench step:

```
node shared/device-model/conformance.js http://<primary-ip> <key> --force
node shared/device-model/topology-conformance.js http://<primary-ip>
node shared/device-model/nodelink-conformance.js ws://<node>.local/nodelink
```

Sequence within v2: **model & contract first** (the branch-selector HAL + topology
schema), then single-node multi-servo, then routing, then multi-node, then the
graph UI. Each phase shippable; the single-node model must be rock-solid before
distributing.

## 10. Contract discipline

Everything new here — actuator HAL states, topology/branch-role schema, the
node protocol, calibration — goes through the canonical model
(`shared/device-model/`) and the conformance suite, so the mock, demo, and
firmware never drift. The topology + branch-selector logic is pure and belongs in
the shared model; the node protocol gets its own conformance scenarios.

## 11. Open questions

### Resolved since this RFC was written

- ~~Topology JSON schema specifics~~ — see [`v2-topology-schema.md`](v2-topology-schema.md).
- ~~`NodeLink` protocol versioning + discovery~~ — versioned frames in
  `shared/device-model/nodelink.js` (`NODELINK_VERSION`; a mismatched `HELLO` is
  refused outright rather than half-spoken). Discovery is mDNS `_dustgate._tcp`
  with a `role` TXT record, surfaced by `GET /api/v2/nodes/discover` and the
  `/boards` screen. **Discovery only populates the picker** — the topology binds
  each board by an explicit `link.host`, so a spare board on the bench or a
  neighbouring system never adopts itself into a live layout.
- ~~Current-mutex scope~~ — see schema doc item 4; resolved as **global**.

### Still open

- **Conflict UX** — the firmware now computes and reports `conflicts`
  (`{selectorId, winner, winnerState, losers[]}`, matching `routing.js`), but the
  Live view still only reads `reachable`. So the UI can say a tool isn't pulling
  and not why. Data side done; presentation not.
- **Real Rockler manifold profile numbers** (shared with the Phase 1 dual-endstop
  work) — still needs a physical measurement.
- **Duct geometry** (lengths/diameters for airflow hints) — deferred; the graph
  stays purely topological.
- **A sliding gate on a SECONDARY board** — calibration drives the v1 motion
  endpoints, which was sound while only the primary had a stepper. `/boards` makes
  multi-board layouts real, so this is now user-reachable: either add v2 motion
  endpoints or block the combination in the picker.
