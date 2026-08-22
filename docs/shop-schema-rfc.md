# DustGate shop schema RFC — multiple collection systems, one brain

**Status:** draft (2026-08-12) — proposed `schemaVersion: 2`
**Companions:** [`topology-schema.md`](topology-schema.md) ·
[`architecture-rfc.md`](architecture-rfc.md) ·
[`outlet-discovery-provisioning.md`](outlet-discovery-provisioning.md)

## 1. Motivation

The v2 contract was written against a stated constraint — one collector per shop
— that turned out not to be true of the shop it was written for. A typical
hobby shop runs **two independent collection systems**:

| System | Duct | Selector hardware | Tools |
|---|---|---|---|
| Big | 4" | ball valves | planer, jointer, table saw |
| Small | 2.5" | Rockler manifold | drill press, bandsaw, sanders |

They share a room, a WiFi network and an operator. They share **no duct** — air
never crosses between them.

Two further pressures land on the same seam:

- **Shop-scoped devices are coming** that belong to no duct run at all — ambient
  air monitoring driving automatic filtration, whole-shop power monitoring.
- **The LAN is not ours alone.** Smart plugs are shared infrastructure; Home
  Assistant, the Shelly app, a bench unit being flashed, or a replacement board
  mid-provision can all lay claim to the same plug we do.

## 2. The central decision

**One brain owns the shop. The shop owns N airflow graphs.**

Not N brains each owning one graph. **N is not two** — the shop that prompted
this has two, but nothing in the contract counts them, and four is an ordinary
shop (cyclone, small-tool manifold, downdraft table, lathe vac).

Every roadmap item that motivated this
change is *room*-scoped, not *system*-scoped: air quality is a property of the
room, shop power draw is one number across every system, and filtration responds
to the room after any tool ran. Two brains would each hold half of every one of
those answers and would need to gossip to reconstruct them — a distributed
system built to solve a problem we don't have.

The usual objection is single point of failure, and it is **explicitly
accepted**: a dead brain strands its own gates regardless of any fallback, so
redundancy buys nothing for the system that failed. The only case it bites is a
second system made *dependent* on the first, which this design forbids —
systems are independent by construction (§4). A dead brain means manual gates
for an afternoon.

Port budget is not an argument against it either: NodeLink secondaries already
carry ports while the primary keeps the picture ([`architecture-rfc.md`
§6](architecture-rfc.md)).

## 3. Two axes, not one choice

These read as competing fixes and are not. They are independent, and **both are
needed**:

| Axis | Question | Answer |
|---|---|---|
| Topology | How many airflow graphs does one controller own? | `systems[]` (§4) |
| Ownership | How do devices avoid claiming each other's plugs? | claims (§8) |

Ownership work is required **even with exactly one brain**, because DustGate is
not the only consumer of those plugs. Pairing repoints a plug's push target via
`Ws.SetConfig`, which silently steals it from whatever had it before. See §8.

## 4. Shape change

### 4.1 What moves

Today's three top-level lists assume one graph:

```jsonc
{ "schemaVersion": 1, "controllers": [], "elements": [], "ducts": [] }
```

`elements` + `ducts` become **per-system**. `controllers` stay shop-level — a
board may drive selectors in **any number** of systems, because a node is mounted
where the cable is convenient, not where the ductwork goes (§14).

```jsonc
{
  "schemaVersion": 2,
  "name": "Jeff's Shop",

  "controllers": [ /* unchanged — ESP32 boards, shop-wide */ ],

  "systems": [
    {
      "id": "big",
      "name": "4\" system",
      "elements": [ /* exactly one collector, + selectors/tools/junctions */ ],
      "ducts":    [ /* edges, child → parent, toward THIS collector */ ]
    },
    {
      "id": "small",
      "name": "2.5\" system",
      "elements": [ /* … */ ],
      "ducts":    [ /* … */ ]
    }
  ],

  "machines": [ /* the things you switch on — see §6 */ ],
  "devices":  [ /* shop-scoped, non-airflow — see §5 */ ]
}
```

Five top-level lists, each meaning exactly one thing: **controllers** are boards,
**systems** are airflow graphs, **machines** are the things you switch on,
**devices** are everything else in the room. A `tool` element inside a system is
a *port* — one machine's connection to one duct.

### 4.2 Why not N collectors in one element list

Relaxing [`topology.js:276`](../shared/device-model/topology.js) from
`collectors !== 1` to `collectors < 1` looks like a one-line change and is a
trap. Every remaining invariant is a statement about **one blower's airflow**:

- *reach the collector* ([`topology.js:452`](../shared/device-model/topology.js))
  would become "reach **a** collector" — which would silently accept a graph
  where a tool on the 4" trunk routes through the 2.5" manifold.
- *always-open / bleed-through* ([`topology.js:498`](../shared/device-model/topology.js))
  asks what the collector pulls through with nothing actuated. Meaningless
  across two blowers.
- *dead-head risk* ([`sequencer.js`](../shared/device-model/sequencer.js)) asks
  whether **the** blower gets sealed. Two blowers, two answers.

Keeping one collector per graph and lifting the container above it leaves all
three correct as written. The per-system body of each loop is today's function,
unchanged.

### 4.3 Model API

`validateTopology`, `airflowIssues`, `redundantSelectors`, `collectorOf`,
`selectorsOf`, `toolsOf`, and the whole of `routing.js` / `sequencer.js` keep
their current signatures and operate on **a system**, not a shop. Add a thin
shop layer above them:

```js
validateShop(shop)      // shape + controllers + unique system ids, then
                        //   validateTopology(system) for each
routeShop(shop, active) // routing per system, merged selector states
planShopTransition(...) // sequencer per system; plans never interleave
```

Naming note: what these functions receive is still a topology. Prefer renaming
the *parameter* over renaming the functions — the contract discipline in
[`architecture-rfc.md` §10](architecture-rfc.md) makes churn in exported
names expensive across firmware + UI + conformance.

## 5. Shop-scoped devices

Ambient sensors and power monitors have **no duct and no parent**. Putting them
in `elements` would break tree validation for something that is not in the tree.
They go in `shop.devices[]`, outside any airflow graph:

```jsonc
{ "id": "air-1", "type": "airQuality", "name": "Bench corner",
  "link": { "kind": "wifi", "host": "…" } }
```

This RFC does **not** specify their behavior — only that the container exists so
the airflow contract stays clean when they arrive.

## 6. Machines: one tool, many ports

### 6.1 The case

A table saw with a 4" cabinet port near the floor **and** a 2.5" stepped-down
overarm/blade-guard pickup is one machine behind two gates. It is normal
practice, not an exotic build: SawStop sells the
[overarm accessory](https://www.sawstop.com/product/overarm-dust-collection-tsa-odc/)
and [advises a separate vac on the guard port](https://www.sawstop.com/learn-explore/in-depth-articles-and-information-about-sawstops-line-of-table-saws-router-tables-and-accessories/sawstop-dust-collection/)
"to maximize airflow at each port," and shop reports describe exactly that split
(base → dust collector, overarm → shop vac).

There is a physical reason it splits: the cabinet port wants **high CFM** (500+
for a cabinet saw), which is what a big impeller delivers; the overarm is small
and restrictive and wants **high static pressure**, which is what a vac
delivers. The single-collector alternative — a Y merging both — works, but
divides airflow and costs capture at the main port.

Two arrangements, one shape:

| Arrangement | Ports | Systems |
|---|---|---|
| Floor gate + stepped-down ball valve, both to the 4" collector | 2 | 1 |
| Cabinet on the 4" collector, overarm on the 2.5" system or a vac | 2 | 2 |

### 6.2 The problem it creates

A tool fed by two gates has **two parent ducts**, which violates the tree:

```
shared/device-model/topology.js:401
// every non-collector has exactly one parent
```

Allowing multi-parent tools turns the graph into a DAG and takes the cycle
check, the reach check, and the sequencer with it — the same class of mistake as
§4.2.

### 6.3 Proposal: a machine is a list, not a flag

Keep every `tool` element a proper leaf with exactly one parent duct — those are
**ports**. Lift the machine above the graph into its own shop-level list:

```jsonc
"machines": [
  { "id": "table-saw", "name": "Table Saw",
    "sensor": { "outlet": { "gen": 2, "ip": "…", "thresholdW": 100 } } }
],

"systems": [{ "id": "big", "elements": [
  { "id": "ts-cabinet", "type": "tool", "machineId": "table-saw",
    "name": "Cabinet · 4\"" },
  { "id": "ts-overarm", "type": "tool", "machineId": "table-saw",
    "name": "Overarm · 2.5\"", "enabled": false }
]}]
```

**Exactly one primary port, and 0–2 supplemental ones** (decided 2026-08-13).
Three connections is the ceiling, and both halves of that are physical rather
than arithmetic:

- **One primary.** "Which connection is this machine's real one" has exactly one
  honest answer — the cabinet port, the under-table port — and everything else
  is a pickup. Making it one rather than "at least one" collapses two other
  rules into structure: the home system *is* the primary port's system (§11.3
  rule 2 becomes unrepresentable rather than validated), and one machine's two
  required ports can no longer contend for one single-open selector.
- **Two supplementals.** A machine has room for a main port and a pickup or two
  before you run out of tool to bolt them to. A port list long enough to need
  scrolling is a second machine being modelled as one.

Routing reads *machine active* → open the path to **every enabled port**
carrying that `machineId`. Two simultaneous opens are two `make` moves, which
[`sequencer.js`](../shared/device-model/sequencer.js) already orders correctly
ahead of any `break`. Nothing new is needed there.

The same mechanism covers both rows of the §6.1 table. Ports in one system are
your floor + overarm gates; ports in two systems are the cabinet/vac split. The
graph stays a tree either way, and "systems share no duct" (§13) never has to
bend — **the machine spans the systems, the air does not.**

### 6.4 Why a list, and not a "primary port"

An earlier draft hung `sensor.outlet` on one port flagged primary. That flag
bought nothing but problems: state to keep valid, an ambiguous delete, and a UI
obliged to explain which of two identical ports is special.

To be precise about what this rejects, since §6.3 and §11.3 do have a primary
port: the thing rejected is **a port owning the machine's identity** — the plug,
the trip point, the name. `primary` survives as a statement about *airflow
priority* ("this is the connection the machine cannot do without"), which is a
fact about a duct connection and belongs on a port. The plug is not, and does
not.

Look instead at what accumulates on a tool element — the smart outlet, the trip
point, the display name, the identity arbitration matches on. **None of those
are facts about a duct connection.** What a port genuinely owns is its parent
duct and its gate. One list per meaning is the same move §4 made for systems and
§5 made for shop devices.

Consequences:

- **Deleting a supplemental port is a non-event.** The machine survives with one
  fewer pickup and the plug claim never moves. **The primary port is not
  deletable** — a machine cannot exist without its one required connection, so
  the only thing that removes it is *remove this machine*: drop the machine and
  all its ports, release the claim, confirm once. The canvas therefore offers no
  delete on a primary port at all; deleting the tool is that action.
- **Every port draws the same outlet**, identically live, and every one opens
  the same sheet. No mirroring, no read-only follower
  ([`mockups/outlet-dock.html`](mockups/outlet-dock.html)).
- **Arbitration is per machine**, so a machine's own ports can never compete
  with each other.
- **Warn on a same-collector split.** Two ports of one machine on one collector
  is the case the woodworking sources say costs capture at the main port —
  guide-bar advisory, not a validation error. People do it deliberately.

Cost, stated plainly: **every tool gets a machine record**, single-port ones
included. That is bureaucracy for the common case and still the right trade —
one lookup path beats "check the element, unless it's grouped, then check the
group." Migration auto-creates one per existing tool (§12), so nobody types
anything. Firmware follows: `syncTopologyOutlets` walks machines rather than
elements.

### 6.5 Adding ports is a user action

Multi-port machines are **declared, never inferred**. Table saws are only the
obvious case — router tables, bandsaws (over-table and under-table), lathes with
a chip hood, drum sanders. Any attempt to guess from names or duct sizes would
be wrong often enough to be worse than an explicit "add another port to this
machine" on the canvas.

**Duct size stays out of the model.** No `diameter` field: sizing is the user's
job, we have no way to verify a claim about it, and a wrong number would look
authoritative. But ports need names anyway to be told apart, so the port-naming
UI should *suggest* size-and-use — `Cabinet · 4"`, `Overarm · 2.5"`, `Under
table · 4"` — as prefilled text the user can overwrite. The information lands
where it helps (a human reading the canvas) without becoming data we pretend to
understand.

### 6.6 Temporarily disabling a port

An overarm guard is useless and in the way for a non-through cut, and gets
pulled off the saw for the afternoon. When it does, routing must stop opening
that gate — otherwise every cut splits airflow toward an open pickup lying on
the bench.

`enabled: false` on the **port**, toggled from the canvas. Distinct from
capping, which says *no duct was ever run here* and is a build fact; this says
*the duct is there, the hood is off, ignore it for now*. Rules:

- **supplemental ports only. The primary is always enabled** (decided
  2026-08-13). Switching off the connection a machine cannot run without means
  "collect nothing from this tool", which is what deleting the machine — or
  unpairing its plug — is for. The canvas shows **no disable control on a
  primary port at all**; validation is the backstop for a document that arrived
  some other way
- a disabled port is skipped by routing, exactly as if the machine had one fewer
  port — and its gate settles closed like any unrouted branch, which the
  sequencer already treats as a `break`
- a disabled port stays visible on the canvas, dimmed, so the state is
  discoverable and reversible in one tap — never hidden
- **all ports disabled is a guide-bar error, not a silent no-op.** A machine
  that can be sensed but never routed would run the collector and open nothing,
  which is the dead-head case. With the primary always enabled this is now
  unreachable except on a machine that has no primary either — which is its own
  error, and worth reporting twice rather than letting through
- it is per-port, never per-machine — and since a single-port machine's one port
  is its primary, that machine has nothing to disable; unpairing or deleting is
  what you want

**`enabled` is sticky (decided).** It survives reboots, WiFi drops, node
reconnects, and — the case that actually constrains the implementation — a
topology re-adopt from the configurator. You pulled the hood off on Tuesday; the
system has no business deciding on Thursday that you put it back.

That last case rules out treating it as an ordinary topology field. The
configurator pushes a whole document, and it may have been composed from a stale
copy that still says `enabled: true`; a naive push would silently re-arm a gate
whose hood is sitting on the bench. So:

> The port's `enabled` value is **owned by the device**, persisted locally,
> keyed by port id, and **merged over any adopted topology**. A topology push
> may create and delete ports; it may not flip one back on.

`enabled` still appears in the schema (§6.3) — it is what a fresh install starts
from and what the UI reads — but on adopt the device's stored value wins for any
port id it already knows. Deleting a port clears its stored entry, so a
re-created port starts enabled rather than inheriting a ghost.

The configurator therefore has to *read* current port state to render the canvas
honestly, rather than assuming the document it holds is the truth. That is the
same discipline already required for live gate positions.

## 7. Collector health (schema room only)

**Nothing here is specified for implementation.** It exists so the shape doesn't
have to be broken open later. A collector today is a duct-graph root with a
`control.outlet` we command; it is also a physical bin that fills, clogs, and is
frequently out of earshot behind a running tool.

### 7.1 Three signals worth having room for

| Signal | Source | Already have? |
|---|---|---|
| Running | `control.outlet` power | yes — we command it, and can read it |
| Bin level | ultrasonic or IR rangefinder over the bin | no — new sensor |
| Clog / choke | sustained current **spike** on the collector's own plug | no — but the plug already reports it |

The third is the interesting one: a blocked duct or a packed filter makes a
blower's draw move, and the plug we already switch is also a wattmeter. It costs
no new hardware — only a baseline and a rule about what "sustained" means. Both
of those need bench data before anything is written down.

### 7.2 Shape to leave room for

```jsonc
{ "id": "dc-big", "type": "collector", "name": "1.5HP",
  "control": { "outlet": { … }, "offDelayMs": 4000, "onDelayMs": 2000 },

  "bin": {                       // optional
    "sensor": { "kind": "ultrasonic", "controllerId": "node-dc" },
    "emptyMm": 900, "fullMm": 250, "warnPct": 80
  },
  "health": {                    // RESERVED — deferred, §14. Nothing reads this.
    "baselineW": null, "clogFactor": null
  }
}
```

`bin.sensor.controllerId` points at a controller because a rangefinder needs a
board, which suggests a **collector node** profile down the line: rangefinder in,
indicator out, one board per collector. That is a hardware decision, not a schema
one, and this shape doesn't force it.

### 7.3 Indicators, and why they are shop-level

The existing manual rig — green when powered, blinking red when near full — is
right about the problem and limited by being *at the collector*. The whole point
is that you are at the saw, wearing hearing protection, and the collector is
behind you.

Every node is already planned to carry a NeoPixel, which makes an indicator a
**shop-level output with many surfaces**, not a property of the collector:

```jsonc
"indicators": [
  { "controllerId": "node-dc",  "kind": "neopixel", "count": 1 },
  { "controllerId": "node-saw", "kind": "neopixel", "count": 1 }
],
"alerts": {
  "collectorRunning": { "pattern": "solid",   "color": "green", "scope": "own" },
  "binNearFull":      { "pattern": "blink",   "color": "red",   "scope": "own" },
  "clog":             { "pattern": "strobe",  "color": "red",   "scope": "shop" }
}
```

`scope` is the load-bearing field. *own* lights the collector's own node — the
current rig. *shop* lights **every** node, so a clog strobes at the gate next to
your hand. That is the thing the manual version can't do, and the reason the
alert policy belongs to the shop rather than to the collector element.

Open before any of this is built: what distinguishes a clog from a full bin from
a bag that needs shaking, whether a false strobe mid-cut is worse than a missed
clog, and whether the rangefinder wants to be IR or ultrasonic in an environment
that is, definitionally, full of airborne dust.

## 8. Claims: sharing a LAN

### 8.1 The failure

Discovery is a broadcast mDNS sweep, so every brain sees every plug. `excludeIps`
([`build.component.ts:1401`](../dustgate-ui/src/app/build/build.component.ts))
only knows the local topology, so nothing prevents two claimants of one physical
plug. The dangerous half is silent: pairing repoints push via `Ws.SetConfig`, so
the previous owner simply stops hearing that the tool started. No error on
either side.

### 8.2 Proposal

**Two signals, different jobs.**

1. **The plug's friendly name carries the owner, for humans.** Firmware already
   writes plug names — `ShellyGen2Outlet::setName()` →`Switch.SetConfig`
   ([`ShellyGen2Outlet.cpp:147`](../firmware/outlets/ShellyGen2Outlet.cpp)),
   today for gates. Extend it to carry the owning brain:

   ```
   Table Saw · dustgate-big
   ```

   Discovery parses the suffix and **strips it for display**. The suffix is the
   brain's mDNS hostname, which is already the identity used for
   `reresolve()`.

2. **The push target is the authority, for the machine.** Names are user-editable
   — [`outlet-discovery-provisioning.md:51`](outlet-discovery-provisioning.md)
   already flags renaming in the Shelly app as a known breakage. Read
   `Ws.GetConfig` and compare the configured server URL against ours. That says
   who owns the plug regardless of what it's called.

### 8.3 Resulting states

| Plug state | Discovery shows | Pickable |
|---|---|---|
| Push target is us | its name, suffix stripped | yes |
| Push target is another DustGate | "owned by dustgate-small" | no |
| Push target is set, not DustGate | "shared with home-assistant.local" | yes, **polled** (§14) |
| No push target | **unknown outlet** | yes |

"Not pickable, with a stated reason" is the existing `excludeReason` channel —
no new UI mechanism. Row 3 is the interesting one and is **not** a takeover
prompt: a plug shared with Home Assistant is paired read-only by polling, and
its `Ws` config is left alone. See §14.

### 8.4 Auto-naming makes drag-and-drop the default

With the claim written on drop, attaching an outlet to a tool needs no dialog:

> **drop = claim = rename the plug = repoint push**

The config sheet becomes the override path (rename by hand, change threshold),
not the primary one.

## 9. UI: drawing a machine once

A machine is drawn **once**, in its home system, with a **port tab** where each
of its ducts lands. The auxiliary duct is drawn crossing the room, because that
hose physically crosses the room.

This needs **no model change**. Ports already live in systems and machines
already do not, so both ends of the overarm's duct are still inside the 2.5"
system and §13's same-system rule holds untouched. Only the *machine* spans
systems, and it was never an airflow vertex to begin with. It also retires the
mirroring problem in §6.4 outright: one machine, one glyph, one outlet chip,
nothing to mirror.

### 9.1 Primary reads heavier than supplemental

The port strip carries a rank now (§6.3), so it has to *look* like one. The
primary is drawn at full weight — solid duct, solid glyph, the machine's name.
Supplemental ports are **ghosted**: lighter stroke, the duct drawn thinner, the
port label at reduced emphasis. One glance should answer "which of these is the
one that matters," because that is exactly what the arbitration will decide when
two people are in the shop.

Two constraints on the ghosting, both learned the hard way:

- **Ghosted is not disabled.** `enabled: false` already means dimmed (§6.6), so
  supplemental cannot borrow the same treatment. Rank is *weight* (stroke,
  label emphasis); disabled is *dimming* the whole element. A ghosted-and-dimmed
  supplemental has to still read as two separate facts.
- **Never hover-only.** Rank is visible at rest, on the canvas, without pointing
  at anything — the same rule the rest of the canvas follows.

The primary also has no delete affordance (§6.4). Removing it is *delete this
tool*, which lives on the machine, not on the port.

### 9.2 What focus lights

Focus is a *ducts-layer* idea, and dimming whole boxes stops working once a
machine can belong to two of them.

| Thing | Lit when |
|---|---|
| plumbing (collector, selectors, trunk) | its system is focused |
| a duct | its port's system is focused |
| **a machine** | **any of its ports is in the focused system** |
| **a machine with an unassigned port** | **always** |

A machine is therefore never cut in half — the saw is bright under either
system, with only the duct belonging to the other one dimmed. That reads as
"this machine participates here, through this port," which is the true statement.

The unassigned rule matters more than it looks: a port that is drawn but not yet
plumbed is unfinished work, and unfinished work is the last thing that should
vanish when you narrow the view. It keeps the machine bright until you have
actually run the duct.

The **wiring layer ignores focus entirely** (§14) — a node is mounted where the
cable was convenient, and dimming half the shop would hide the cable that proves
it.

## 10. UI: "WiFi devices"

The wiring layer's **boards** tray becomes **WiFi devices**, holding both
secondary controllers and unclaimed outlets.

The organizing principle is **how the thing reaches us**, not what it switches.
A secondary board is a WiFi peer of the primary; so is a smart plug. That a plug
happens to switch mains is irrelevant to the picture the tray is drawing. Cable
runs are a separate concern and already have their own layer.

Unclaimed plugs appear as *unknown outlet* and can be dragged onto a tool.
Foreign-owned plugs appear locked with their owner named, so it is visible *why*
a plug isn't on offer rather than it silently missing.

Prior art for the interaction: [`mockups/outlet-dock.html`](mockups/outlet-dock.html).

## 11. Behavior changes worth deciding deliberately

1. **Arbitration becomes per-system.** "Most recent tool wins, one tool at a
   time" now holds *within* a system. Planer on the big system while a sander
   runs on the small one is a legal simultaneous state, and correct for airflow.
2. **Both collectors can run at once**, and start staggering is now a general
   rule rather than a two-collector special case — see §11.1.
3. **Idle-hold stays per-system.** Leaving gates where they are at rest is a
   property of one graph and needs no shop-level coordination.

### 11.1 Start stagger (decided)

**Never switch a collector on at the same instant something else is starting.**
Two motors sharing a circuit is the common shop wiring, not the exception, and
the peak that trips a breaker is inrush, which lasts well under a second.

Two delays, both configurable:

| Delay | Between | Default | Where |
|---|---|---|---|
| `onDelayMs` | a tool being sensed and **its** collector switching on | 2 s | collector `control` |
| `collectorStaggerMs` | one collector switching on and the **next** | 2 s | shop |

`onDelayMs` is the mirror of the `offDelayMs` that already exists on a
collector's `control` ([`topology-schema.md` §element: collector](topology-schema.md)),
and it costs the first moment of a cut going uncollected. That is an acceptable
trade at ~2 s: the saw is still spinning up, and the tool's own inrush is over
before the blower adds its own.

Ordering note: the delay is measured from **sensing**, not from the switch being
thrown, and a poll cycle has already elapsed by then. The real gap is therefore
larger than the configured number — worth measuring on the bench before tuning
the default.

With more than two systems the stagger is **cumulative** — the third collector
waits 2× `collectorStaggerMs` behind the first, not alongside the second. A
shop that starts four blowers at once is exactly the shop that most needs them
separated, and the delay only applies to simultaneous starts, which are rare.

Gate movement is not staggered against collectors — servos are a trivial load
next to a blower. But see §11.2: they must still be serialised against *each
other*.

### 11.2 The servo mutex is shop-wide, not per-system

[`sequencer.js`](../shared/device-model/sequencer.js) says it plainly:

> The one-servo-at-a-time current mutex is honored implicitly: moves are a
> serial list.

That holds because today there is exactly one plan. Once `planShopTransition`
produces **one plan per system**, two systems can transition in the same instant
— you switch tools on the 4" while the 2.5" is still settling — and two servos
move at once. The implicit mutex is gone, silently, and the failure mode is a
brownout on a shared supply rather than a wrong gate position.

So: **plans are per-system; execution is not.** The primary holds one move queue
for the whole shop and drains it serially, regardless of which system a move
came from. Within a system the make-before-break ordering is unchanged; across
systems, moves interleave in time but never overlap.

This is the one place where "systems are independent" stops being true, and it
is worth being explicit that the reason is electrical, not aerodynamic.

### 11.3 Cross-system contention (two people in the shop)

A machine that spans systems **holds a port in each while it runs**. If the
second system routes through a single-open-outlet selector — a Rockler manifold
— the table saw's overarm commits that whole system for as long as the saw is
on, and someone else starting the bandsaw either steals it or is refused.

**First, the non-software answer.** The contention is created by *sharing a
selector*, not by spanning systems. An overarm on its own dedicated shop vac
with no gate in front of it costs nothing and conflicts with nobody — the
arrangement the woodworking sources describe (§6.1). Worth saying out loud in
the configurator when someone puts a second port on a manifold.

For the case where the gate exists anyway, two rules, and the second is what
makes the first safe:

#### Rule 1 — a port is primary or supplemental

```jsonc
{ "id": "ts-overarm", "type": "tool", "machineId": "table-saw",
  "name": "Overarm · 2.5\"", "supplemental": true }
```

- **primary** (default, and there is exactly one — §6.3) — losing it means the
  machine runs with no collection.
- **supplemental** (0–2 of them) — a bonus. May be preempted without argument.

#### Rule 2 — only supplemental ports may leave the home system

Every machine has a **home system**: the one holding its primary port. With one
primary that is a definition rather than a constraint — any port in a *different*
system is by construction not the primary, so it is supplemental, and the rule
holds without a check.

This is the load-bearing rule, and it buys a guarantee:

> **Cross-system contention can never cost anyone a primary port.**

Someone else's tool, in a system you did not choose to share, can only ever take
your *bonus* pickup. Your only collection is decided inside your own home
system, where you can see the other machines on the same selector.

It also fits every real multi-port machine: cabinet 4" primary + overarm 2.5"
supplemental, router table under-table primary + fence pickup supplemental,
bandsaw under-table primary + upper-guide supplemental. In each, the port that
crosses is the one you would sacrifice.

#### Arbitration order

Within a system, when a selector can't satisfy everyone:

1. **Primary beats supplemental**, whatever started more recently. Never trade
   someone's only collection for someone else's bonus.
2. Among primaries — **most-recent-wins**, unchanged.
3. Among supplementals — most-recent-wins.

Step 1 matters more than it looks. Without it, starting the table saw *after*
your buddy's bandsaw would hand the manifold to the saw's overarm and leave the
bandsaw cutting into a closed gate — recency would have beaten need.

#### Routing becomes partial

Today a tool is routed or it is not. A machine with two ports can now be *partly*
routed, and the model has to say so:

| Result | Meaning | Surfaced as |
|---|---|---|
| routed | every enabled port open | normal |
| partial | primary open, a supplemental port yielded | say who took it |
| **stripped** | a **primary** port lost | **alarm** |

Rule 2 confines *stripped* to within-system contention — two machines on one
manifold, which is physical and unavoidable — so it is now rare and always
local. It stays the case worth building for: a saw drawing 1.8 kW with its
cabinet gate shut is cutting with no collection, and the operator is at the saw
in hearing protection, not looking at a phone. This is what §7.3's
`scope: "shop"` indicator exists for.

*partial* is never silent either. A port that goes dark without saying why is
indistinguishable from one that broke.

**What this deliberately does not do:** schedule two humans. DustGate cannot
know whose cut matters more, and a system that refuses to route the bandsaw
because someone else's saw is on would be worse than one that degrades loudly
and predictably. The goal is that the shop never *quietly* stops collecting.

## 12. Migration

`schemaVersion: 1 → 2` is mechanical and lossless:

```js
{ schemaVersion: 2, name, controllers,
  systems:  [{ id: 'main', name, elements: elements.map(withMachineId), ducts }],
  machines: toolsOf(old).map((t) => ({          // one per existing tool
    id: t.id + '-m', name: t.name, sensor: t.sensor })),
  devices:  [] }
```

Every v1 tool becomes a one-port machine: the element keeps its id and gains a
`machineId`, and `sensor` moves off the element onto the new machine record
(§6.4). Ports are `enabled: true` by default, so nothing changes behaviorally.

Precedent is set: the v1 → v2 migration took no back-compat and re-ran setup on
the one existing device. This one doesn't need even that — every v1 shop is a
one-system, one-port-per-machine shop, so the transform runs on adopt.

Touch list:
- [`topology.js`](../shared/device-model/topology.js) — container + move the
  collector-count rule per-system
- [`topology.fixtures.js`](../shared/device-model/topology.fixtures.js),
  [`topology.test.js`](../shared/device-model/topology.test.js),
  [`topology-conformance.js`](../shared/device-model/topology-conformance.js) —
  wrap fixtures, add two-system cases
- `TopologyStore.h` / `TopologyRouter.h` + `/api/topology|status` — per-system
  iteration, status reports per system
- [`demo-topology.ts`](../dustgate-ui/src/app/services/demo-topology.ts) and the
  build canvas — render N graphs, add-a-port, disable-a-port
- `syncTopologyOutlets` in [`firmware.ino`](../firmware/firmware.ino)
  — walk `machines[]` for sensor plugs rather than elements (§6.4). The riskiest
  item on this list: it is firmware, and Phase 1 is still hardware-untested.

## 13. Validation rules

**Moves to per-system:** exactly one collector; collector is root with no parent
duct; every element reaches its collector; no cycles.

**New at shop level:**
- ≥1 system; system ids unique. **No upper bound** — two is what this shop has,
  not what the model allows. A cyclone, a 2.5" manifold, a downdraft table and a
  dedicated lathe vac is four, and nothing in the contract counts.
- element ids unique **across the whole shop** (they address hardware and appear
  in logs; per-system uniqueness would make a bare id ambiguous)
- a duct's `child` and `parent` must be in the **same** system — the rule that
  makes "systems share no duct" structural rather than conventional
- every `controller` referenced by a selector exists shop-level
- one plug claimed by at most one **machine**, across all systems — a machine's
  ports share one plug by design (§6.4), so the rule is per machine, not per
  element
- `machineId` may only group elements of `type: 'tool'`, must resolve to a
  `machines[]` entry, and a machine with ports in more than one system is legal
  (§6.3)
- a machine has **exactly one primary port** — no primary is a machine nothing
  is obliged to collect from; two makes "the home system" ambiguous and lets one
  machine's two required ports fight over one single-open selector (§6.3). With
  one, *every port outside the home system is supplemental* holds structurally
  and needs no check of its own — which is what guarantees cross-system
  contention can never cost a primary port (§11.3)
- a machine has **at most two supplemental ports** — three connections total
- a primary port sharing a single-open selector with one of its own machine's
  supplementals is permanently degraded, not wrong: a warning, since the user
  may have meant it
- **the primary port is never `enabled: false`** — only supplementals can be
  switched off (§6.6)
- every machine has **≥1 port**, and **≥1 enabled port** — all-disabled is a
  guide-bar error, since a machine that can be sensed but never routed runs the
  collector into a sealed system (§6.6)
- machine ids unique shop-wide, and disjoint from element ids

## 14. Open questions

1. **How does the guide bar phrase a *stripped* machine** (§11.3) so it reads as
   "your saw has no collection right now" rather than as a routing error?
### Deferred — not being implemented now

- **Clog detection (§7.1).** Whether a sustained wattage spike distinguishes a
  blocked duct from a full bin from a bag that needs shaking is unverified and
  needs a collector, a blast gate and a log. **Deliberately not started.** The
  `health` block in §7.2 stays in the schema as reserved space with null
  thresholds; nothing reads it.

### Resolved while drafting

- **Boards are not pinned to a system** → a controller may drive selectors in
  any number of systems. A node is mounted where the *cable* is convenient, not
  where the ductwork goes: the shop vac can sit at the far wall while its gate is
  easiest to reach beside the saw, sharing that node with a 4" ball valve. The
  wiring layer therefore ignores system focus and shows every system at full
  strength — dimming half the shop would hide the cable that proves the point.
- **A system is born with its collector** → "Add a system" draws a collector,
  switches focus to the new system, and leaves an open end to plumb from. That
  is what makes it a system: the collector is the root every graph invariant is
  written against (§4.2).
- **`enabled` stickiness** → decided, §6.6. Device-owned, persisted locally,
  merged over any adopted topology so a configurator push cannot re-arm a port.
- **Start stagger** → decided, §11.1. Two configurable delays, 2 s defaults, one
  between a tool and its own collector and one between collectors.
- **A tool in two systems** → resolved by §6. Not two tools sharing a name and
  not one tool with two parents: a `machineId` grouping over ordinary
  single-parent leaves.
- **Override for a plug owned by someone else** → resolved below.

### Never steal a plug; poll it instead

The takeover question dissolves once you notice **push is an optimization, not a
capability**. `Ws.SetConfig` buys latency and saves polling traffic; everything
DustGate actually needs from a sensor plug is *read this plug's wattage*, and
polling already does that — the per-outlet polling fallback exists today for
when a push connection is down
([`outlet-discovery-provisioning.md`](outlet-discovery-provisioning.md)).

So the rule is:

> A plug already pointed at another controller is **pairable read-only, by
> polling**. We never rewrite its `Ws` config.

That gives a plug shared with Home Assistant to both owners, with no silent
breakage in either direction, and it removes the need for a scary confirm dialog
in the common case. The UI says *shared with home-assistant.local — polled, not
pushed*, which is true and not alarming.

Takeover remains available but becomes a deliberate, rare act rather than a side
effect of pairing:

- offered only from the config sheet, never from a drag-and-drop
- states plainly what stops working (*home-assistant.local will stop receiving
  updates from this plug*)
- records the previous push URL in the topology so unpairing can hand it back

The one case still needing push is a **collector's switch role** under a tight
`offDelayMs`, and even that only commands the plug — commanding is an HTTP call
that does not touch `Ws` config either. There is no role that *requires* the
theft.
