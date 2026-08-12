// shop.js — DustGate SHOP contract: N airflow systems under one brain.
//
// This is the thin layer above topology.js / routing.js / sequencer.js that
// docs/shop-schema-rfc.md §4.3 calls for. Everything below it still operates on
// ONE airflow graph and keeps its current signature; this file owns the
// container, the machine indirection, and the merge.
//
// The shape (RFC §4.1), five lists each meaning exactly one thing:
//
//   controllers[]  ESP32 boards. SHOP-level, not per-system: a board is mounted
//                  where the cable is convenient, and may drive selectors in any
//                  number of systems (RFC §14).
//   systems[]      airflow graphs. Each is a topology in its own right —
//                  exactly one collector, one tree, no duct shared with a
//                  sibling.
//   machines[]     the things you switch on. A machine owns the smart outlet,
//                  the trip point and the display name (RFC §6.3).
//   devices[]      shop-scoped, non-airflow (air quality, power). Container
//                  only; behaviour is deliberately unspecified (RFC §5).
//
// WHY A CONTAINER RATHER THAN N COLLECTORS IN ONE LIST (RFC §4.2): every
// airflow invariant in topology.js is a statement about ONE blower — "reach the
// collector", "what bleeds through with nothing actuated", "can the blower get
// sealed". Relaxing the one-collector rule would quietly turn each of those into
// a question with two answers, and would accept a graph where a 4" tool routes
// through the 2.5" manifold. Lifting the container ABOVE them leaves all three
// correct exactly as written.
//
// WHY MACHINES ARE A LIST AND PORTS ARE ELEMENTS (RFC §6.3): a table saw with a
// cabinet port and an overarm pickup is one machine behind two gates. Letting a
// tool element have two parents would make the graph a DAG and take the cycle
// check, the reach check and the sequencer with it. So each port stays a proper
// one-parent leaf, and the machine lifts above the graph. The machine spans the
// systems; the air does not.
//
// PURE. No state, no I/O.

'use strict';

const T   = require('./topology');
const RTG = require('./routing');
const SEQ = require('./sequencer');

const SHOP_SCHEMA_VERSION = 2;

// ---------------------------------------------------------------------------
// System views
// ---------------------------------------------------------------------------

/**
 * Present one system as a plain topology, the shape everything below this layer
 * already understands.
 *
 * Controllers are spliced back in from the shop because selectors reference
 * `controllerId` and the per-system validator has to be able to resolve it. This
 * is what RFC §4.2 means by "the per-system body of each loop is today's
 * function, unchanged" — we reshape the input rather than fork the function.
 *
 * @param {Shop} shop
 * @param {System} system
 * @returns {import('./topology').Topology}
 */
function systemView(shop, system) {
  return {
    schemaVersion: 1,
    name:          system.name,
    controllers:   shop.controllers || [],
    elements:      system.elements  || [],
    ducts:         system.ducts     || [],
  };
}

const systemsOf = (shop) => (shop && Array.isArray(shop.systems)) ? shop.systems : [];
const machinesOf = (shop) => (shop && Array.isArray(shop.machines)) ? shop.machines : [];

/** machineId → machine */
function machineIndex(shop) {
  const m = new Map();
  for (const mach of machinesOf(shop)) if (mach && mach.id) m.set(mach.id, mach);
  return m;
}

/**
 * machineId → [{ systemId, port }] for every port of that machine, in document
 * order. Includes DISABLED ports; callers that route must filter (see
 * portsToOpen) — the UI needs the disabled ones to draw them dimmed.
 */
function portsByMachine(shop) {
  const out = new Map();
  for (const sys of systemsOf(shop)) {
    for (const el of (sys.elements || [])) {
      if (el.type !== 'tool' || !el.machineId) continue;
      if (!out.has(el.machineId)) out.set(el.machineId, []);
      out.get(el.machineId).push({ systemId: sys.id, port: el });
    }
  }
  return out;
}

/**
 * A port counts for routing unless it is explicitly disabled.
 *
 * `enabled` is absent on the overwhelming majority of ports and means true —
 * making the field opt-OUT rather than opt-in keeps a migrated v1 doc, where no
 * port carries the field at all, routing exactly as it did before.
 */
const portEnabled = (port) => port.enabled !== false;

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

/**
 * Validate a whole shop: shape, controllers, unique system ids, machines, then
 * every system as a topology in its own right.
 *
 * Errors from a system are prefixed with its id so a message like "exactly one
 * collector required" is attributable — with N systems that message alone no
 * longer says where to look.
 *
 * @param {Shop} shop
 * @returns {{ok:boolean, errors:{code:string,message:string,ref?:string}[]}}
 */
function validateShop(shop) {
  /** @type {{code:string,message:string,ref?:string}[]} */
  const errors = [];
  const err = (code, message, ref) => errors.push(ref ? { code, message, ref } : { code, message });

  if (!shop || typeof shop !== 'object') {
    err('shape', 'shop must be an object');
    return { ok: false, errors };
  }
  if (typeof shop.schemaVersion !== 'number') err('shape', 'schemaVersion must be a number');
  for (const k of ['controllers', 'systems', 'machines']) {
    if (!Array.isArray(shop[k])) err('shape', `${k} must be an array`);
  }
  if (shop.devices !== undefined && !Array.isArray(shop.devices)) {
    err('shape', 'devices must be an array when present');
  }
  if (errors.length) return { ok: false, errors };

  if (shop.systems.length === 0) err('system', 'a shop needs at least one system');

  // ── systems: unique ids, no shared elements ──
  const sysIds = new Set();
  for (const sys of shop.systems) {
    if (!sys || typeof sys !== 'object') { err('system', 'system must be an object'); continue; }
    if (typeof sys.id !== 'string' || !sys.id) { err('system', 'system missing id'); continue; }
    if (sysIds.has(sys.id)) err('system', `duplicate system id: ${sys.id}`, sys.id);
    sysIds.add(sys.id);
    for (const k of ['elements', 'ducts']) {
      if (!Array.isArray(sys[k])) err('system', `system "${sys.id}": ${k} must be an array`, sys.id);
    }
  }
  if (errors.length) return { ok: false, errors };

  // Element ids must be unique across the WHOLE shop, not just within a system.
  // They are referenced from shop-level places (a machine's ports, ui.layout,
  // firmware status keyed by selector id), so a collision between systems would
  // be ambiguous exactly where it is hardest to notice.
  const elemOwner = new Map();
  for (const sys of shop.systems) {
    for (const el of sys.elements) {
      if (!el || typeof el.id !== 'string' || !el.id) continue;
      if (elemOwner.has(el.id)) {
        err('element', `element id "${el.id}" used in both system "${elemOwner.get(el.id)}" and "${sys.id}"`, el.id);
      } else {
        elemOwner.set(el.id, sys.id);
      }
    }
  }

  // ── machines ──
  const machIds = new Set();
  for (const m of shop.machines) {
    if (!m || typeof m !== 'object') { err('machine', 'machine must be an object'); continue; }
    if (typeof m.id !== 'string' || !m.id) { err('machine', 'machine missing id'); continue; }
    if (machIds.has(m.id)) err('machine', `duplicate machine id: ${m.id}`, m.id);
    machIds.add(m.id);
    if (m.name !== undefined && typeof m.name !== 'string') err('machine', 'name must be a string', m.id);
    if (m.sensor !== undefined) {
      if (typeof m.sensor !== 'object' || m.sensor === null) err('machine', 'sensor must be an object', m.id);
      else if (m.sensor.outlet !== undefined && (typeof m.sensor.outlet !== 'object' || m.sensor.outlet === null)) {
        err('machine', 'sensor.outlet must be an object', m.id);
      }
    }
  }

  // ── ports: every tool element resolves to a machine ──
  const ports = portsByMachine(shop);
  for (const sys of shop.systems) {
    for (const el of sys.elements) {
      if (el.type !== 'tool') continue;
      if (typeof el.machineId !== 'string' || !el.machineId) {
        err('port', `tool "${el.name || el.id}" has no machineId`, el.id);
      } else if (!machIds.has(el.machineId)) {
        err('port', `tool "${el.name || el.id}" references unknown machine "${el.machineId}"`, el.id);
      }
      if (el.enabled !== undefined && typeof el.enabled !== 'boolean') {
        err('port', `tool "${el.name || el.id}": enabled must be a boolean`, el.id);
      }
    }
  }

  // A duct's child and parent must be in the SAME system. This is the rule that
  // makes "systems share no duct" structural rather than conventional — without
  // it a layout could plumb a 4" tool into the 2.5" manifold and every
  // per-system invariant below would still pass.
  for (const sys of shop.systems) {
    const here = new Set(sys.elements.map((e) => e && e.id).filter(Boolean));
    for (const d of sys.ducts) {
      if (!d) continue;
      for (const end of ['child', 'parent']) {
        const id = d[end];
        if (id && !here.has(id) && elemOwner.has(id)) {
          err('duct', `duct ${end} "${id}" is in system "${elemOwner.get(id)}", not "${sys.id}"`, id);
        }
      }
    }
  }

  // Machine ids share a namespace with element ids: both address things by bare
  // id in logs, in ui.layout and over the wire, and migrateToShop deliberately
  // reuses a v1 tool's id for its machine — so a COLLISION with a different
  // element is what has to be caught, not the reuse itself.
  for (const m of shop.machines) {
    if (!m || !m.id) continue;
    const owner = elemOwner.get(m.id);
    if (!owner) continue;
    const port = ports.get(m.id) || [];
    const isOwnPort = port.some(({ port: p }) => p.id === m.id);
    if (!isOwnPort) {
      err('machine', `machine id "${m.id}" collides with an element in system "${owner}"`, m.id);
    }
  }

  // elementId → the selector immediately above it and the state that opens the
  // branch it hangs on. Only the first hop: that is the one a machine's own two
  // ports can contend for, which is what the check below is about.
  const feedOf = new Map();
  for (const sys of shop.systems) {
    const byId = new Map(sys.elements.map((e) => [e && e.id, e]));
    for (const d of sys.ducts) {
      if (!d || !d.child) continue;
      const parent = byId.get(d.parent);
      if (!parent || parent.type !== 'selector') continue;
      const branch = (parent.branches || []).find((b) => b.id === d.parentBranch);
      if (branch) feedOf.set(d.child, { selectorId: parent.id, stateId: branch.opensState });
    }
  }

  // ── primary vs supplemental (RFC §11.3) ──
  //
  // `supplemental: true` marks a port whose loss degrades capture but is not a
  // problem in itself. ABSENT MEANS PRIMARY, because a port nobody has thought
  // about is a port whose air you actually need.
  const isSupplemental = (port) => port.supplemental === true;
  for (const m of shop.machines) {
    if (!m || !m.id) continue;
    const p = ports.get(m.id) || [];
    if (p.length === 0) continue;   // already reported below

    const primary = p.filter(({ port }) => !isSupplemental(port));
    // All-supplemental is a machine nothing is obliged to collect from, which
    // makes every verdict about it meaningless — it can never be `stripped`.
    if (primary.length === 0) {
      err('machine', `machine "${m.name || m.id}" has no primary port — mark at least one as required`, m.id);
      continue;
    }

    // THE LOAD-BEARING RULE. A machine's home system is the one holding its
    // primary ports; every port outside it must be supplemental. That is what
    // guarantees cross-system contention can never cost anyone a primary port:
    // someone else's tool, in a system you did not choose to share, can only
    // ever take your bonus pickup.
    //
    // The RFC states it per port ("every port outside the home system is
    // supplemental"); stated that way it is exactly equivalent to "all primary
    // ports share one system", since a primary elsewhere IS a second home. One
    // check, because two would mean one of them could never fire — and the
    // message has to name both systems anyway for the error to be actionable.
    const homes = [...new Set(primary.map(({ systemId }) => systemId))];
    if (homes.length > 1) {
      err('machine',
        `machine "${m.name || m.id}" has primary ports in systems ${homes.map((h) => `"${h}"`).join(' and ')} — only one can be its home; ports outside it must be supplemental`,
        m.id);
    }

    // Two PRIMARY ports of one machine that need the same selector in DIFFERENT
    // states can never both be open, so the machine can never be fully routed —
    // a build error, not a runtime surprise. Judged on the states themselves
    // rather than on the selector's kind: two ports hanging off branches that
    // share an `opensState` do both flow, and that is a legitimate build.
    // The same shape with one port supplemental is merely permanently degraded,
    // and the user may have meant it, so only primaries are checked.
    const held = new Map();   // selectorId → { stateId, portId }
    for (const { port } of primary) {
      const hop = feedOf.get(port.id);
      if (!hop) continue;
      const prev = held.get(hop.selectorId);
      if (prev && prev.stateId !== hop.stateId) {
        err('port', `ports "${prev.portId}" and "${port.id}" are both primary on selector "${hop.selectorId}", which cannot open both at once`, port.id);
      } else if (!prev) {
        held.set(hop.selectorId, { stateId: hop.stateId, portId: port.id });
      }
    }
  }

  // A machine with no port is switched on and routes nowhere. That is not a
  // draft state the UI can leave behind either: deleting the last port IS the
  // "remove this machine" action (RFC §6.4), so reaching here means something
  // dropped a port without dropping the machine.
  for (const m of shop.machines) {
    if (!m || !m.id) continue;
    const p = ports.get(m.id) || [];
    if (p.length === 0) {
      err('machine', `machine "${m.name || m.id}" has no ports — delete the machine or give it one`, m.id);
    } else if (p.every(({ port }) => !portEnabled(port))) {
      // RFC §6.6: all-ports-disabled is an ERROR, not a silent no-op. A machine
      // that can't route anywhere would otherwise run with every gate shut.
      err('machine', `machine "${m.name || m.id}" has every port disabled — it would run with no gate open`, m.id);
    }
  }

  // Plug uniqueness moves up here with the outlet itself. topology.js still
  // enforces it per system (collector switches, and any v1 tool element), but a
  // machine's sensor now lives at shop level, so two machines sharing a plug is
  // only visible from here.
  const plugOwner = new Map();
  for (const m of shop.machines) {
    const ip = m && m.sensor && m.sensor.outlet && m.sensor.outlet.ip;
    if (!ip) continue;
    if (plugOwner.has(ip)) {
      err('machine', `two machines share smart outlet ${ip}: "${plugOwner.get(ip)}" and "${m.id}"`, m.id);
    } else {
      plugOwner.set(ip, m.id);
    }
  }

  // ── each system, as a topology ──
  for (const sys of shop.systems) {
    const res = T.validateTopology(systemView(shop, sys));
    for (const e of (res.errors || [])) {
      errors.push({ ...e, message: `system "${sys.id}": ${e.message}`, ref: e.ref });
    }
  }

  return { ok: errors.length === 0, errors };
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

/**
 * Route the whole shop.
 *
 * Systems share no duct, so routing genuinely is per-system and the merge is a
 * plain union — no cross-system arbitration exists to get wrong. What the shop
 * layer adds is the machine indirection: an active MACHINE opens the path to
 * every ENABLED port carrying its id, in whatever system that port lives.
 *
 * Priority order is preserved into each system (highest first), so the
 * "most-recent-tool-wins" policy the caller expresses by ordering the list keeps
 * working unchanged within a system.
 *
 * @param {Shop} shop
 * @param {string[]} activeMachineIds  highest priority first
 * @returns {ShopRouting}
 */
function routeShop(shop, activeMachineIds) {
  const active = Array.isArray(activeMachineIds) ? activeMachineIds : [];
  const ports  = portsByMachine(shop);

  /** @type {Object.<string,string|null>} */
  const states = {};
  /** @type {{systemId:string,selectorId:string,winner:string,winnerState:string,losers:string[]}[]} */
  const conflicts = [];
  /** @type {Object.<string,boolean>} */
  const portReachable = {};
  /** @type {Object.<string,MachineRouting>} */
  const machines = {};

  for (const sys of systemsOf(shop)) {
    const view = systemView(shop, sys);

    // Machine ids → this system's enabled ports, priority order preserved.
    // One machine can hold several ports in ONE system (floor gate + overarm on
    // the same collector), so this is a flatMap, not a lookup.
    /** @type {{id:string, supplemental:boolean}[]} */
    const wanted = [];
    /** @type {Map<string,string[]>} */
    const portsOfMachineHere = new Map();
    for (const mid of active) {
      const here = (ports.get(mid) || [])
        .filter(({ systemId, port }) => systemId === sys.id && portEnabled(port))
        .map(({ port }) => port);
      if (here.length) portsOfMachineHere.set(mid, here.map((p) => p.id));
      for (const p of here) wanted.push({ id: p.id, supplemental: p.supplemental === true });
    }

    // ARBITRATION ORDER (RFC §11.3). computeRouting is greedy in list order, so
    // the order of this list IS the policy:
    //
    //   1. primary beats supplemental, whatever started more recently
    //   2. among primaries    — most-recent-wins, unchanged
    //   3. among supplementals — most-recent-wins
    //
    // Step 1 matters more than it looks. Without it, starting the table saw
    // AFTER someone's bandsaw hands the manifold to the saw's overarm and leaves
    // the bandsaw cutting into a closed gate: recency beating need. `active` is
    // already in recency order and Array.sort is stable, so partitioning on
    // `supplemental` alone preserves rules 2 and 3 for free.
    const activePorts = wanted
      .slice()
      .sort((a, b) => (a.supplemental === b.supplemental ? 0 : (a.supplemental ? 1 : -1)))
      .map((p) => p.id);

    const r = RTG.computeRouting(view, activePorts);

    Object.assign(states, r.states);
    Object.assign(portReachable, r.reachable);
    for (const c of (r.conflicts || [])) conflicts.push({ systemId: sys.id, ...c });

    // Roll port results up to the machine. A machine is only as routed as its
    // WORST port: RFC §10.3 wants three answers, not two, because a saw whose
    // cabinet gate is shut while its overarm is open is a very different
    // situation from one that is simply running.
    for (const [mid, portIds] of portsOfMachineHere) {
      const got  = portIds.filter((id) => r.reachable[id]);
      const lost = portIds.filter((id) => !r.reachable[id]);
      const prev = machines[mid] || { routed: [], blocked: [], status: 'routed' };
      machines[mid] = {
        routed:  prev.routed.concat(got),
        blocked: prev.blocked.concat(lost),
        status:  'routed',   // recomputed below, once every system has reported
      };
    }
  }

  // Final per-machine verdict, across all its systems.
  //
  //   routed    every enabled port got a clear path
  //   partial   some ports lost, and every one that lost is SUPPLEMENTAL
  //   stripped  a PRIMARY port lost — the alarm case (RFC §10.3)
  //
  // `supplemental: true` marks a port whose loss degrades capture but is not a
  // problem in itself: the overarm. Absent means primary, because a port nobody
  // has thought about is a port whose air you actually need.
  const portIndex = new Map();
  for (const sys of systemsOf(shop)) {
    for (const el of (sys.elements || [])) if (el.type === 'tool') portIndex.set(el.id, el);
  }
  for (const mid of Object.keys(machines)) {
    const m = machines[mid];
    if (m.blocked.length === 0) m.status = 'routed';
    else if (m.blocked.every((id) => (portIndex.get(id) || {}).supplemental === true)) m.status = 'partial';
    else m.status = 'stripped';
  }
  for (const mid of active) {
    // An active machine whose every port is disabled reaches nothing at all.
    // validateShop calls that an error; routing still has to answer for it
    // rather than omit the machine and look like it was never asked.
    if (!machines[mid]) machines[mid] = { routed: [], blocked: [], status: 'stripped' };
  }

  return { states, conflicts, reachable: portReachable, machines };
}

// ---------------------------------------------------------------------------
// Transition planning
// ---------------------------------------------------------------------------

/**
 * Plan the moves for a whole shop.
 *
 * Plans are computed per system — make-before-break is a statement about one
 * blower's air — but they are returned as ONE ordered list, deliberately.
 * Execution is shop-wide and serial: the one-servo-at-a-time current budget
 * (architecture-rfc.md §7) is a property of the power supply, not of a duct run,
 * so two systems transitioning at once would break it even though their air
 * never mixes (RFC §10.2).
 *
 * Systems are planned in document order and their plans are CONCATENATED, never
 * interleaved. Interleaving would let system B's break land between system A's
 * make and A's break — which is exactly the dead-head the sequencer exists to
 * prevent.
 *
 * @param {Shop} shop
 * @param {Object.<string,string|null>} currentStates  selectorId → stateId (shop-wide)
 * @param {Object.<string,string|null>} desiredStates  selectorId → stateId (shop-wide)
 * `deadHeadRisk` stays PER SYSTEM rather than being or-ed into one flag: it asks
 * whether a particular blower ends up sealed, and with two blowers running there
 * are genuinely two answers. Collapsing them would hide which one is at risk,
 * which is the only thing the caller can act on.
 *
 * @param {Object} [opts] forwarded to planTransition unchanged. `collectorRunning`
 *        may be a boolean (applies to every system) or a map of systemId → boolean.
 * @returns {{systemId:string, moves:any[], deadHeadRisk:boolean}[]}
 */
function planShopTransition(shop, currentStates, desiredStates, opts = {}) {
  const out = [];
  const running = opts.collectorRunning;
  for (const sys of systemsOf(shop)) {
    const view = systemView(shop, sys);
    // Narrow the state maps to this system's selectors. planTransition reasons
    // about "every selector in the topology", so handing it the shop-wide map
    // would have it plan moves for selectors that are not in this graph.
    const mine = new Set(T.selectorsOf(view).map((s) => s.id));
    const pick = (m) => {
      const o = {};
      for (const k of Object.keys(m || {})) if (mine.has(k)) o[k] = m[k];
      return o;
    };
    const sysOpts = {
      ...opts,
      collectorRunning: (running && typeof running === 'object')
        ? !!running[sys.id]
        : !!running,
    };
    const plan = SEQ.planTransition(view, pick(currentStates), pick(desiredStates), sysOpts);
    const moves = plan.moves || [];
    // Reported even with no moves when the blower is at risk — an empty plan is
    // exactly how a system arrives at "everything shut while the fan runs".
    if (moves.length || plan.deadHeadRisk) {
      out.push({ systemId: sys.id, moves, deadHeadRisk: !!plan.deadHeadRisk });
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Migration
// ---------------------------------------------------------------------------

/**
 * Lift a schemaVersion-1 topology into a schemaVersion-2 shop.
 *
 * Every existing install is one system with one collector, so the container is
 * pure gain and nobody loses a layout. The only real work is machines: a v1
 * `tool` element carried its own name and plug, and v2 moves both onto a machine
 * (RFC §6.3). One machine is auto-created per tool, keeping the tool's id as the
 * machine id so anything holding an id — `ui.layout`, a firmware status blob, a
 * bug report — still resolves (RFC §12).
 *
 * The port keeps the display name too. That is not redundancy: on a multi-port
 * machine the port name becomes "Cabinet · 4\"" while the machine stays "Table
 * Saw", and starting them equal is the honest single-port case.
 *
 * @param {import('./topology').Topology} topology
 * @param {{systemId?:string, systemName?:string}} [opts]
 * @returns {Shop}
 */
function migrateToShop(topology, opts = {}) {
  const t = topology || {};
  const systemId   = opts.systemId   || 'system-1';
  const systemName = opts.systemName || t.name || 'Dust collection';

  const machines = [];
  const elements = (t.elements || []).map((el) => {
    if (el.type !== 'tool') return { ...el };
    const machine = { id: el.id, name: el.name || el.id };
    if (el.sensor) machine.sensor = el.sensor;
    machines.push(machine);
    // sensor moves to the machine; everything else about the port stays put.
    const { sensor, ...port } = el;
    return { ...port, machineId: el.id };
  });

  const shop = {
    schemaVersion: SHOP_SCHEMA_VERSION,
    name:          t.name || 'My Shop',
    controllers:   (t.controllers || []).map((c) => ({ ...c })),
    systems: [{
      id:       systemId,
      name:     systemName,
      elements,
      ducts:    (t.ducts || []).map((d) => ({ ...d })),
    }],
    machines,
    devices: [],
  };
  if (t.ui) shop.ui = t.ui;
  return shop;
}

/**
 * True for a document already in shop shape. Used at the seams (store, adopt,
 * PUT) so a v1 doc can be migrated on read rather than rejected.
 */
const isShop = (doc) =>
  !!doc && typeof doc === 'object' && Array.isArray(doc.systems);

/** Accept either shape, return a shop. */
const asShop = (doc, opts) => (isShop(doc) ? doc : migrateToShop(doc, opts));

module.exports = {
  SHOP_SCHEMA_VERSION,
  systemView, systemsOf, machinesOf, machineIndex, portsByMachine, portEnabled,
  validateShop, routeShop, planShopTransition,
  migrateToShop, isShop, asShop,
};
