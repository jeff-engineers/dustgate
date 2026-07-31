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
      // Per-state realization is an OFFSET from the calibrated referenceAngle — a
      // valve-DESIGN constant, not a per-build tune. LEFT is the reference (offset
      // 0); closed/right are the ball's known port offsets from it.
      "states": [
        { "id": "left",   "isClosed": false, "offsetDeg": 0   },
        { "id": "closed", "isClosed": true,  "offsetDeg": 80  },
        { "id": "right",  "isClosed": false, "offsetDeg": 161 }
      ],
      "branches": [
        { "id": "mL", "opensState": "left",  "role": "tool" },
        { "id": "mR", "opensState": "right", "role": "tool" }
      ],
      // referenceAngle is captured in setup (jog until LEFT is exact); everything
      // else derives from it. Timing/hold/clamp live on the actuator, not per-state.
      "servo": {
        "channel": 0,                    // LEDC channel (or PCA9685 index)
        "referenceAngle": 5,             // CALIBRATED: servo angle where LEFT is exact
        "moveMs": 600,                   // one sweep time for the whole actuator
        "holdAtRest": false,             // move then detach — valve holds by friction/detent;
                                         // analog servos groan while holding. Set true only for
                                         // a build that would back-drive de-energized.
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
| `states[]` | HAL states: `{ id, isClosed, …realization }`. Exactly one `isClosed:true`. Realization is kind-specific: `positionMm` (linear) or `offsetDeg` (servo — angular offset from the calibrated `referenceAngle`; a valve-DESIGN constant, e.g. a gate's closed = open ±90°). |
| `branches[]` | `{ id, opensState, role }`. `opensState` references a non-closed state id. `role` ∈ `tool` \| `unassigned` \| `blocked` \| `feed`. |
| `linear` | `{ calibration: { stepsPerMm, measuredSpanSteps, homeIsMaxEndstop, manifoldModel } }` |
| `servo` | `{ channel, referenceAngle, moveMs, holdAtRest, minAngle?, maxAngle? }`. `referenceAngle` = per-build calibrated angle of the reference state (a gate's OPEN / a manifold's LEFT, viewed from the servo side), captured in setup; commanded angle = `referenceAngle + offsetDeg`. `holdAtRest` **defaults false** (move then detach — analog servos groan while holding and the valve holds by friction/detent); set true only for a build that would back-drive de-energized. |

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

**DustGate departs from the DIY Blast Gate reference here.** The reference positions
the ball by driving the servo *into* physical hard stops (`Open/Closed/Left/Right.stl`)
and hand-tuning each angle to overshoot into a stop. We DON'T — that only works well
with a clutch servo, and it stalls/groans/wears a clutchless analog servo (e.g. the
Power HD 3001HB). Instead:

- **Calibrate to ONE reference position, derive the rest.** During setup, jog the
  servo until the valve sits *exactly* at its reference — a gate's OPEN, a manifold's
  LEFT (viewed from the servo side) — and store that as the servo's `referenceAngle`.
  Every other state is `referenceAngle + offsetDeg`, where `offsetDeg` is a fixed
  valve-DESIGN constant (a quarter-turn gate: closed = ±90°; a manifold: the ball's
  known port offsets). One calibration point defines everything, and re-clocking the
  horn just means re-capturing the one reference.
- **No stalling into stops.** The servo goes to a computed angle and — with
  `holdAtRest:false` (the default) — detaches once seated (`SERVO_MOVE_MS`), so it
  never fights a hard stop or hunts. The valve holds by friction/detent. Hard stops,
  if present, are a mechanical backstop, not the positioning mechanism.
- **`holdAtRest` stays per-build:** default false (move-then-detach — right for the
  analog servos here and confirmed on the bench); set true only for a build that would
  back-drive when de-energized.
- **Orientation & sign convention (jeff's build, 2026-07-28):** viewed from the SERVO
  side, servo `0°` = fully clockwise (right); increasing angle = counterclockwise. Mount
  the horn HORIZONTAL at assembly, and calibrate the reference (gate OPEN / manifold
  LEFT) as a small POSITIVE angle — set at, or slightly clockwise of, true open — so
  every `offsetDeg` is POSITIVE (CCW) and stays inside 0–180°. Thus a gate's
  `closed.offsetDeg ≈ +90`; a manifold's closed/right are positive offsets from left.
- **Coupling slop / backlash → use magnetic DETENTS (preferred fix).** The horn↔stem
  joint has play, and a beefier servo doesn't fix it. The clean solution is a magnetic
  detent at each valve position (the reference's optional N52 magnets): drive the servo
  *near* the target — a hair *into* the detent — then **detach**, and the magnet snaps
  the ball to the exact detent and holds it. This defeats slop (the magnet seats the
  ball regardless of backlash, as long as the servo lands within capture range — a few
  degrees of slop is well inside it), provides the de-energized hold (no drift under
  suction), AND takes up backlash from a consistent side for free. This is a DETENT
  (servo detaches into it), NOT a hard stop the servo stalls against — so it keeps the
  clutchless-servo-friendly, no-groan behavior. **The detent magnet placement now
  defines the true positions**, so that's the thing to get right mechanically.
  - Servo-angle precision therefore RELAXES: `referenceAngle + offsetDeg` just needs to
    land within capture range and bias slightly into the detent. Firmware
    backlash-compensation (same-direction approach) becomes optional — keep only a small
    deliberate overshoot-toward-detent.
  - Also tighten the coupler where cheap (set screw / D-profile), but the detent is what
    makes precision robust.
  - Possible schema flag: `servo.detented: true` — documents that landing near + a hair
    over is fine (magnet seats it) and that `holdAtRest:false` is safe (magnet holds).
- **Digital servo (jeff leaning this way):** more torque, tighter deadband, faster, less
  hunting — all good, and **no firmware change** (ESP32Servo drives digital positional
  servos identically; move-then-detach still applies and matters *more*, since digitals
  hold hard). Watch current: a beefy digital pulls a bigger stall — size the 5V rail +
  bulk cap, though one-move-at-a-time keeps the average low. May want per-servo pulse
  (min/max µs) tuning.
- **Still open:** the manifold's real port offsets (measure the ball; the reference's
  5/85/166° implies ~±80° from center).

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
- A branch with role `feed` must have a `selector` **or `junction`** child (a
  junction is the passive tee you get when branching a gated tube — routing passes
  through it, so the gate above governs the whole group); `blocked` must have no
  child; `tool` a `tool` child; `unassigned` no child.
- Every `controllerId` / `actuator` reference resolves.

## Open modeling questions (surfaced by writing this)

1. **Junctions** — kept as an element type for a passive merge that isn't the
   collector or a selector, but the star/tree works without them. Ship the type
   but leave it unused until a real need? (lean: define, don't build UI yet)
2. **Two tools sharing one branch** (a passive Y before a selector branch) — the
   junction case. Disallow for v1 (one tool per `tool` branch)?
3. **Servo realization** — RESOLVED (2026-07-28, updated after bench-testing Power HD
   3001HB analog servos). Positions are **calibrated, not hard-stop-driven**: each
   servo stores a per-build `referenceAngle` (jog to a gate's OPEN / manifold's LEFT in
   setup), and each state is `referenceAngle + offsetDeg` where `offsetDeg` is a fixed
   valve-design constant. `moveMs`, `holdAtRest`, `referenceAngle`, and an optional
   `minAngle`/`maxAngle` clamp live on the `servo` block. `holdAtRest` **defaults false**
   (move-then-detach — analog servos groan holding and the valve holds by
   friction/detent). Dropped the earlier `angleDeg`-absolute + `minUs/maxUs` ideas.
   Sign convention RESOLVED: servo 0° = clockwise (servo side), `offsetDeg` positive
   (CCW), reference set as a small positive angle. STILL OPEN: the manifold's real port
   offsets (measure the ball), and backlash compensation in the servo HAL (approach
   each target from one direction — coupling has slop). See the mechanical notes.
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
