# DustGate v2 topology schema

**Status:** draft (2026-07-28) — P0 contract artifact
**Companion:** [`v2-architecture-rfc.md`](v2-architecture-rfc.md)

This is the **single source of truth** for a DustGate v2 install. Firmware, the
shared device-model (`shared/device-model/`), the demo, and the configurator UI
all consume this one document. It replaces v1's flat `stops[] + outlets[]` model
(migration decided 2026-07-28 — no back-compat, the one existing device re-runs
setup).

## Vocabulary (resolve the "node" overload)

- **controller** — an ESP32 board. One is `primary` (owns GUI + routing + Shelly
  polling); others are `secondary` (dumb actuator banks). ≈ the RFC's "nodes."
- **element** — a vertex in the airflow graph: a `tool`, a `selector` (an
  actuated branch-selector), a `junction` (passive merge), or the `collector`.
- **branch** — one selectable outlet of a selector. Each branch is opened by
  exactly one selector *state* and carries a *role*.
- **duct** — a directed edge, `child → parent`, pointing toward the collector.

## Shape

Three top-level lists + metadata:

```jsonc
{
  "schemaVersion": 1,
  "name": "Jeff's Shop",

  "controllers": [ /* ESP32 boards */ ],
  "elements":    [ /* airflow graph vertices */ ],
  "ducts":       [ /* edges, child → parent, toward the collector */ ]
}
```

The **branch-selector HAL** lives on each `selector` element (`kind`, `states[]`)
— that's all a controller needs to drive it. The **graph** (elements + ducts +
branch roles) is what the routing engine on the primary consumes to turn "these
tools are on" into "each selector should be in this state."

## Annotated example

A one-controller shop: a 4-branch **linear actuator** (two tools, one `feed` to a
downstream manifold, one capped) plus a **servo manifold** pairing two tools, all
pulling to one collector.

```jsonc
{
  "schemaVersion": 1,
  "name": "Jeff's Shop",

  "controllers": [
    {
      "id": "primary",
      "role": "primary",                 // primary | secondary
      "name": "Shop Brain",
      "board": "devkitc",                // maps to boards/*.h build flag
      "link": { "transport": "wifi-ws", "host": "dustgate.local" }
    }
  ],

  "elements": [
    // ── the sink ──────────────────────────────────────────────────────────
    {
      "id": "dc",
      "type": "collector",
      "name": "Dust Collector",
      "control": {                       // the switchable DC plug (we turn it on/off)
        "outlet": { "gen": 2, "ip": "192.168.87.20", "host": "shellyplugusg4-aabbcc" },
        "offDelayMs": 4000               // coast-down before auto-off (v1's DC delay)
      }
    },

    // ── an actuated selector: the linear actuator (N-branch) ─────────────────
    {
      "id": "sel-main",
      "type": "selector",
      "name": "Main Actuator",
      "controllerId": "primary",
      "kind": "linear",                  // linear | servoGate | servoManifold

      // HAL states: mutually-exclusive commands the firmware can issue.
      // Exactly ONE has isClosed:true (the all-closed rest state).
      "states": [
        { "id": "home", "isClosed": true,  "positionMm": 0.0  },
        { "id": "s1",   "isClosed": false, "positionMm": 12.5 },
        { "id": "s2",   "isClosed": false, "positionMm": 95.4 },
        { "id": "s3",   "isClosed": false, "positionMm": 178.3 },
        { "id": "s4",   "isClosed": false, "positionMm": 261.0 }
      ],

      // branches: each is opened by one state and has a role.
      "branches": [
        { "id": "b1", "opensState": "s1", "role": "tool"       },
        { "id": "b2", "opensState": "s2", "role": "tool"       },
        { "id": "b3", "opensState": "s3", "role": "feed"       },  // → downstream selector
        { "id": "b4", "opensState": "s4", "role": "blocked"    }   // physically capped
      ],

      // kind-specific hardware + calibration
      "linear": {
        "calibration": {
          "stepsPerMm": 51.47,
          "measuredSpanSteps": 4387,
          "homeIsMaxEndstop": false,
          "manifoldModel": "rockler-2.5"
        }
      }
    },

    // ── an actuated selector: a servo manifold (2 branches + closed) ─────────
    {
      "id": "sel-manifold-a",
      "type": "selector",
      "name": "Manifold A",
      "controllerId": "primary",
      "kind": "servoManifold",
      // Per-state realization is JUST the angle — the alignment-compensation knob
      // (e.g. 85 not 90 for closed). Reference designs tune these per gate.
      "states": [
        { "id": "closed", "isClosed": true,  "angleDeg": 85  },
        { "id": "left",   "isClosed": false, "angleDeg": 5   },
        { "id": "right",  "isClosed": false, "angleDeg": 166 }
      ],
      "branches": [
        { "id": "mL", "opensState": "left",  "role": "tool" },
        { "id": "mR", "opensState": "right", "role": "tool" }
      ],
      // Timing/hold/clamp live on the actuator, not per-state.
      "servo": {
        "channel": 0,                    // LEDC channel (or PCA9685 index)
        "moveMs": 600,                   // one sweep time for the whole actuator
        "holdAtRest": true,              // keep energized holding position (SAFE default).
                                         // set false to detach after moving IF this build
                                         // holds by hard-stop friction / detent magnets —
                                         // saves power + stops buzz, but risks airflow back-drive
        "minAngle": 0, "maxAngle": 180   // optional safety clamp on any commanded angle
      }
    },

    // ── tools (sources): the plug on each senses when it's running ───────────
    { "id": "tool-bandsaw",  "type": "tool", "name": "Bandsaw",
      "sensor": { "outlet": { "gen": 2, "ip": "192.168.87.27", "host": "…", "thresholdW": 6.0 } } },
    { "id": "tool-tablesaw", "type": "tool", "name": "Table Saw",
      "sensor": { "outlet": { "gen": 2, "ip": "192.168.87.30", "host": "…", "thresholdW": 5.0 } } },
    { "id": "tool-router",   "type": "tool", "name": "Router Table",
      "sensor": { "outlet": { "gen": 2, "ip": "192.168.87.31", "host": "…", "thresholdW": 8.0 } } },
    { "id": "tool-sander",   "type": "tool", "name": "Drum Sander",
      "sensor": { "outlet": { "gen": 2, "ip": "192.168.87.32", "host": "…", "thresholdW": 7.0 } } }
  ],

  // edges: child connects toward the collector via parent (+ branch if parent is a selector)
  "ducts": [
    { "child": "sel-main",       "parent": "dc" },
    { "child": "tool-bandsaw",   "parent": "sel-main",       "parentBranch": "b1" },
    { "child": "tool-tablesaw",  "parent": "sel-main",       "parentBranch": "b2" },
    { "child": "sel-manifold-a", "parent": "sel-main",       "parentBranch": "b3" },  // feed
    { "child": "tool-router",    "parent": "sel-manifold-a", "parentBranch": "mL" },
    { "child": "tool-sander",    "parent": "sel-manifold-a", "parentBranch": "mR" }
  ]
}
```

## Field reference

### controller
| field | notes |
|---|---|
| `id` | stable string, UI-assigned |
| `role` | `primary` \| `secondary` |
| `board` | build-flag target (`devkitc`, `feather_s2`, …) |
| `link` | transport config — `{ transport: "wifi-ws", host }` for now; `NodeLink`/ESP-NOW later |

### element (common)
| field | notes |
|---|---|
| `id`, `type`, `name` | `type` ∈ `collector` \| `selector` \| `tool` \| `junction` |

### element: selector
| field | notes |
|---|---|
| `controllerId` | which board drives it |
| `kind` | `linear` \| `servoGate` \| `servoManifold` |
| `states[]` | HAL states: `{ id, isClosed, …realization }`. Exactly one `isClosed:true`. Realization is kind-specific: `positionMm` (linear) or `angleDeg` (servo — also the alignment-tuning knob). |
| `branches[]` | `{ id, opensState, role }`. `opensState` references a non-closed state id. `role` ∈ `tool` \| `unassigned` \| `blocked` \| `feed`. |
| `linear` | `{ calibration: { stepsPerMm, measuredSpanSteps, homeIsMaxEndstop, manifoldModel } }` |
| `servo` | `{ channel, moveMs, holdAtRest, minAngle?, maxAngle? }`. `holdAtRest` **defaults true** (keep holding — safe); set false to detach after moving only if the build holds by hard-stop friction / optional detent. |

### element: tool
| field | notes |
|---|---|
| `sensor.outlet` | the Shelly plug that detects the tool running, + `thresholdW`. Attaches to the **tool**, not a stop (the semantic fix migration buys us). |

### element: collector
| field | notes |
|---|---|
| `control.outlet` | the switchable DC plug (we command on/off) |
| `control.offDelayMs` | coast-down before auto-off |

### duct
| field | notes |
|---|---|
| `child`, `parent` | element ids; airflow flows child → parent → … → collector |
| `parentBranch` | required iff `parent` is a selector — which branch this child hangs off |

## Servo actuator — mechanical build notes

From the DIY Blast Gate reference (parts + sketch angles; the assembly video isn't
transcribed, so the *hold* mechanism is inferred from "magnets are optional"):

- **Positions are set by physical hard stops** — printed parts named by position
  (`Open/Closed/Left/Right.stl`). The servo drives the ball *into* a hard stop; it
  doesn't define the position itself.
- **What holds a gate at rest is hard-stop + seal/gear friction.** Detent magnets
  (`N52`, 4mm×2mm) are **optional** and gate-level — so friction must be the baseline
  hold. Magnets add back-drive resistance and a tactile snap; they matter most on the
  **manual** handle version, not the servo build. (Earlier I wrongly implied the servo
  version depends on them — it doesn't.)
- **Indexing the servo to the ball:** command the servo to its *closed* reference angle
  first (standard gate ≈ `0°`, manifold ≈ `85°` center), rotate the ball to its physical
  closed hard stop, then attach the coupler/arm so the two are clocked together. Trim the
  open/side `angleDeg` in software to drive firmly into each stop (hence 91/92/94°). A
  **clutch servo** forgives overshoot into the stop.
- **`holdAtRest` is per-build for exactly this reason:** whether a detached servo holds
  depends on that gate's friction fit and whether detent magnets were added — which
  varies gate to gate. Default `true` (hold); opt into detach only once a build is
  confirmed to stay put.

## How routing consumes it (sketch)

1. Active tools = tools whose `sensor.outlet` crosses `thresholdW`.
2. For each active tool, walk ducts `child → parent` to the collector; for every
   `selector` parent on the path, set its actuator to `branch.opensState` of the
   `parentBranch` used.
3. Every selector **not** on any active path → its `isClosed` state.
4. **Conflict** = two active tools requiring different states of the same selector
   (e.g. both branches of one manifold). Surfaced, never last-writer-wins.

`moveMs` per state lets the primary sequence moves under the one-servo-at-a-time
current mutex (RFC §7).

## What a v1 linear-only setup looks like now (the migration target)

The phase 1 wizard's *flow* is unchanged; its *output* is now this schema: one
`primary` controller, one `selector` (`kind:"linear"`) with N branches + `home`,
one `tool` element per assigned branch (carrying its plug), a `collector`, and a
star of ducts. A lone linear actuator is the depth-1 special case — no separate
"v1 model" exists anymore.

## Validation rules (for the shared model + conformance)

- Exactly one `collector`; it is the root (no outgoing duct).
- Every non-collector element has exactly **one** parent duct (tree rooted at the
  collector). Junctions/selectors may have many children.
- Each selector: exactly one `isClosed:true` state; every non-closed state is the
  `opensState` of exactly one branch; every branch's `opensState` exists.
- `parentBranch` present ⟺ parent is a selector; the branch exists on that parent.
- A branch with role `feed` must have a `selector` child; `blocked` must have no
  child; `tool` a `tool` child; `unassigned` no child.
- Every `controllerId` / `actuator` reference resolves.

## Open modeling questions (surfaced by writing this)

1. **Junctions** — kept as an element type for a passive merge that isn't the
   collector or a selector, but the star/tree works without them. Ship the type
   but leave it unused until a real need? (lean: define, don't build UI yet)
2. **Two tools sharing one branch** (a passive Y before a selector branch) — the
   junction case. Disallow for v1 (one tool per `tool` branch)?
3. **Servo realization detail** — RESOLVED (2026-07-28, from the DIY Blast Gate
   reference). Per-state `angleDeg` is the only realization needed AND is itself the
   alignment-compensation knob (reference tunes 91/92/94° per gate by hand — no
   pulse-width calibration). `moveMs`, `holdAtRest`, and an optional `minAngle`/
   `maxAngle` clamp live on the `servo` block, not per state. `holdAtRest` **defaults
   true** (keep the servo holding — safe against airflow back-drive); detach-after-move
   (`false`) is a per-build opt-in for gates that hold position by hard-stop friction
   (or optional detent magnets). Dropped the earlier `minUs/maxUs` idea. See the
   mechanical notes below for what actually holds a gate at rest.
4. **Where the current-mutex scope lives** — per controller (each board sequences
   its own servos) vs global. Per-controller is simpler and matches the power
   rail being per-node. Confirm.
5. **Manifold pairing constraint** in the model — two tools on one manifold can't
   run together. Is that just a *routing conflict* at runtime, or also a *setup*
   warning (pair tools never used together)? (lean: both — detect at routing,
   hint at setup.)
6. **Duct geometry** (lengths/diameters for airflow hints) — deferred; graph stays
   purely topological for now (RFC §11).
7. **ID generation & stability** across re-runs of setup (so status/history line
   up) — UUIDs vs slugs. (lean: short stable slugs from names + collision suffix.)
