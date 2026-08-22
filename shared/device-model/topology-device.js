// topology-device.js — stateful topology-native device simulator.
//
// Wraps the pure core (topology.js + routing.js + sequencer.js) into a device
// that behaves like the real thing: tools draw power → the active set (most-recent
// first) drives routing → selectors move → the collector follows. This is the
// model the topology-native API / mock / demo will all expose (the successor to
// device-model.js's flat stops/outlets state).
//
// Policy baked in here (see the design notes):
//   - most-recent-tool-wins  → active tools are ordered most-recently-activated
//     first, and routing.js resolves contests in that order.
//   - idle-hold              → with NO active tool, HOLD every selector where it
//     is (never auto-close) and coast the collector down. Keeps a path open so a
//     manual collector start can't dead-head, and avoids wear on a brief tool-off.
//   - collector coast-down   → the blower stays on for control.offDelayMs after
//     the last tool stops. Expired lazily on read (tickCollector), since this
//     model owns no clock.
//
// SHOP-AWARE since schemaVersion 2 (shop.js). A shop holds N airflow systems, so
// everything that used to be one answer is now one answer PER SYSTEM — the
// collector, the idle-hold decision, the coast timer. Actuator states stay one
// flat map because selector ids are unique shop-wide. What you switch on is a
// MACHINE; a `tool` element is a port. A v1 document is normalised on the way in
// (asShop), so a v1 tool simply is its own machine and nothing here branches on
// the version. Mirrors TopologyController.h / TopologyRuntime.h field-for-field.
//
// PURE + synchronous: reconcile() jumps selectors to their target state. The
// ORDERED make-before-break moves (for a real actuator's timing) are returned via
// planTransition so a consumer (firmware/mock) can animate them; the sim itself
// doesn't need the delay.

'use strict';

const T = require('./topology');
const S = require('./shop');

const DEFAULT_THRESHOLD_W = 5;
const DEFAULT_COLLECTOR_OFF_DELAY_MS = 5000;  // ↔ kDefaultCollectorOffDelayMs (TopologyRuntime.h)

const machineIndex = (shop) => S.machineIndex(shop);

/** Watts above which a MACHINE counts as "on" (from its sensor outlet, or default). */
function machineThreshold(shop, machineId) {
  const m = machineIndex(shop).get(machineId);
  const w = m && m.sensor && m.sensor.outlet && m.sensor.outlet.thresholdW;
  return typeof w === 'number' ? w : DEFAULT_THRESHOLD_W;
}
/** v1 spelling — a tool WAS the machine before ports existed. */
const toolThreshold = (shop, id) => machineThreshold(S.asShop(shop), id);

/** Create a device from a topology or shop: every selector closed, every blower idle. */
function createTopologyDevice(doc) {
  const shop = S.asShop(doc);
  const actuatorStates = {};
  const collectors = {};
  for (const sys of S.systemsOf(shop)) {
    collectors[sys.id] = { on: false, coasting: false, coastUntilMs: 0, manual: false };
    for (const sel of T.selectorsOf(S.systemView(shop, sys))) {
      const cs = T.closedState(sel);
      actuatorStates[sel.id] = cs ? cs.id : null;
    }
  }
  return {
    // Kept under `topology` so every existing consumer (mock GET, demo service,
    // the conformance suites) keeps reading the same field. It is a shop now.
    topology: shop,
    actuatorStates,           // selectorId → current stateId (shop-wide)
    toolWatts: {},            // machineId → last power reading
    activationSeq: {},        // machineId → recency seq (absent = inactive)
    seqCounter: 0,
    collectors,               // systemId → { on, coasting, coastUntilMs }
    lastRouting: { states: {}, conflicts: [], reachable: {}, machines: {} },
  };
}

/**
 * Coast-down for this layout, in ms. Absent means "the shop didn't say" and gets
 * the default, not zero — a blower that cuts the instant a bandsaw drops below
 * threshold leaves the duct full and short-cycles between cuts. An explicit 0
 * does disable it. Mirrors kDefaultCollectorOffDelayMs in TopologyRuntime.h.
 */
function collectorOffDelayMs(doc, systemId) {
  const shop = S.asShop(doc);
  const sys = S.systemsOf(shop).find((x) => x.id === systemId) || S.systemsOf(shop)[0];
  const c = sys && (sys.elements || []).find((e) => e.type === 'collector');
  const v = c && c.control && c.control.offDelayMs;
  return typeof v === 'number' ? v : DEFAULT_COLLECTOR_OFF_DELAY_MS;
}

/**
 * Expire a finished coast. Called on every read rather than from a timer: the
 * model owns no clock (mock-api owns setTimeout, the UI owns await delay), and
 * lazy expiry gives the same answer to anyone who asks with a real `nowMs`.
 */
function tickCollector(d, nowMs) {
  for (const c of Object.values(d.collectors)) {
    if (c.coasting && nowMs >= c.coastUntilMs) {
      c.coasting = false;
      c.on = false;
    }
  }
}

/** Is ANY blower running / coasting — the single-collector shorthand. */
const anyCollectorOn = (d) => Object.values(d.collectors).some((c) => c.on);
const anyCollectorCoasting = (d) => Object.values(d.collectors).some((c) => c.coasting);

/** Machines currently active (watts ≥ threshold), most-recently-activated FIRST. */
function activeMachines(d) {
  const active = [];
  for (const m of S.machinesOf(d.topology)) {
    const w = d.toolWatts[m.id] || 0;
    if (w >= machineThreshold(d.topology, m.id)) active.push(m.id);
  }
  // Higher activationSeq = more recent → wins contests (most-recent-wins).
  // Within a system, routeShop then puts primaries ahead of supplementals — the
  // one place recency does NOT decide (RFC §11.3).
  active.sort((a, b) => (d.activationSeq[b] || 0) - (d.activationSeq[a] || 0));
  return active;
}
const activeTools = activeMachines;

/**
 * Recompute desired actuator states from the active tools and apply them.
 * Idle (no active tool): HOLD all selectors, collector off. Active: route to the
 * winners and close the rest (focus suction), collector on.
 * @returns {{routing: object, plan: object}}
 */
function reconcile(d, nowMs) {
  const shop = d.topology;
  const active = activeMachines(d);
  const routing = S.routeShop(shop, active);

  // Which systems are being asked for. A machine counts here even if its port
  // lost a contest: the blower still has to run for whatever DID win, and a
  // stripped machine is something to report, not a reason to stop the shop.
  const ports = S.portsByMachine(shop);
  const wanted = new Set();
  for (const mid of active) {
    for (const { systemId, port } of (ports.get(mid) || [])) {
      if (S.portEnabled(port)) wanted.add(systemId);
    }
  }

  // Planned against the CURRENT blower states, before they're updated below — the
  // dead-head question is "would this destination seal a blower turning now".
  const running = {};
  for (const [sysId, c] of Object.entries(d.collectors)) running[sysId] = c.on;
  const plans = S.planShopTransition(shop, d.actuatorStates, routing.states,
                                     { collectorRunning: running });

  for (const sys of S.systemsOf(shop)) {
    const c = d.collectors[sys.id];
    if (!c) continue;
    if (wanted.has(sys.id)) {
      // Route this system to the winners and close the rest of ITS selectors.
      for (const sel of T.selectorsOf(S.systemView(shop, sys))) {
        if (sel.id in routing.states) d.actuatorStates[sel.id] = routing.states[sel.id];
      }
      c.on = true;
      c.coasting = false;      // a machine is running here again — coast is moot
    } else if (c.manual) {
      // Running by hand: no tool is asking for this blower, the user is. Hold the
      // positions like any idle system — except that idle-hold is only safe while
      // something is open, and here the blower is about to run. See openAPath().
      openAPath(d, sys);
      c.on = true;
      c.coasting = false;      // not coasting down: it was not left running, it was started
    } else {
      // Idle: hold this system's positions and COAST its blower down. Safe
      // because idle moves nothing: no path can close under a running blower.
      // A busy system no longer drags an idle one along, which is the whole
      // point of the container.
      const delay = collectorOffDelayMs(shop, sys.id);
      if (c.on && !c.coasting && delay > 0) {
        c.coasting = true;
        c.coastUntilMs = nowMs + delay;
      } else if (!c.coasting) {
        c.on = false;
      }
    }
  }
  d.lastRouting = routing;
  // `plan` (singular) kept for callers that predate systems: with one system it
  // is that system's plan, which is what they have always been handed.
  return { routing, plans, plan: plans[0] || { moves: [], deadHeadRisk: false } };
}

/**
 * Make sure ONE system has somewhere for its air to go.
 *
 * Idle-hold leaves every selector where it was, which is safe precisely because
 * nothing is running — and it is the reason a manual start is normally safe too:
 * the shop rests open. But "normally" is not "always". A layout adopted a moment
 * ago starts with every selector CLOSED (createTopologyDevice), and a system whose
 * last act was closing down can be sealed. Starting a blower into that is the one
 * thing this project has a hard rule about, so a manual run opens a path first —
 * make-before-break, the same order a tool transition uses.
 *
 * WHICH path: the system's first machine in document order. Deliberately not
 * "the last one this blower served" — that sounds better and is almost never
 * different, because the only way to reach a SEALED system is a layout just
 * adopted (nothing has run yet) or a dead-head stop. Remembering a winner to use
 * in the one case where there is no winner to remember buys nothing, and it would
 * be a second thing to keep identical between this file and TopologyRuntime.h.
 *
 * It is not a guess about what the user wants, either. A manual run is about
 * moving air, not about a tool; the alternative — refusing to start until someone
 * opens a gate — would make the one job this button exists for (clear a clog)
 * impossible on a freshly-adopted layout.
 */
function openAPath(d, sys) {
  const shop = d.topology;
  const view = S.systemView(shop, sys);
  const sels = T.selectorsOf(view);

  // Already open? A selector that isn't sitting in its closed state is a path.
  // Same test planTransition uses for deadHeadRisk, so the two agree about what
  // "sealed" means. A system with no selectors at all is open by construction.
  const sealed = sels.length > 0 && sels.every((sel) => {
    const closed = T.closedState(sel);
    return !!closed && d.actuatorStates[sel.id] === closed.id;
  });
  if (!sealed) return;

  const ports = S.portsByMachine(shop);
  const inThisSystem = (mid) => (ports.get(mid) || [])
    .some((pp) => pp.systemId === sys.id && S.portEnabled(pp.port));
  const target = (shop.machines || []).map((m) => m.id).find(inThisSystem);
  if (!target) return;   // a system with no machines has nothing to open toward

  const routing = S.routeShop(shop, [target]);
  for (const sel of sels) {
    if (sel.id in routing.states) d.actuatorStates[sel.id] = routing.states[sel.id];
  }
}

/**
 * Run ONE system's blower by hand, or stop it.
 *
 * It HOLDS. There is no automation to hand back to — a blower with no tool asking
 * for it is running because a person said so — and the errand this exists for
 * (clear a clog, sweep the floor, test a run) is one where a blower that switched
 * itself off partway would be worse than one left running. A tool starting on the
 * same system does not clear it either: routing proceeds normally over the top,
 * and when that tool stops the blower keeps running instead of coasting, which is
 * what "I switched this on" should mean. Switching it off is the only thing that
 * ends it.
 *
 * Mirrors TopologyRuntime::setCollectorManual.
 */
function setCollectorManual(d, systemId, on, nowMs = Date.now()) {
  tickCollector(d, nowMs);
  const c = d.collectors[systemId];
  if (!c) return reconcile(d, nowMs);
  c.manual = !!on;
  // Switching OFF ends the run outright rather than starting a coast-down: the
  // coast exists to catch the dust still in the pipe after a CUT, and nothing was
  // being cut. Leaves the gates where they are, like every other idle.
  if (!c.manual && !activeMachines(d).length) { c.on = false; c.coasting = false; }
  return reconcile(d, nowMs);
}

/** Is this system's blower running because a person switched it on? */
const collectorIsManual = (d, systemId) => !!(d.collectors[systemId] || {}).manual;

/** Set a MACHINE's live power reading; tracks the OFF→ON edge and reconciles. */
function setMachinePower(d, machineId, watts, nowMs = Date.now()) {
  tickCollector(d, nowMs);
  const th = machineThreshold(d.topology, machineId);
  const wasActive = (d.toolWatts[machineId] || 0) >= th;
  d.toolWatts[machineId] = watts;
  const nowActive = watts >= th;
  if (nowActive && !wasActive) d.activationSeq[machineId] = ++d.seqCounter; // rising edge → newest
  if (!nowActive) delete d.activationSeq[machineId];
  return reconcile(d, nowMs);
}
const setToolPower = setMachinePower;

/** Projected status for consumers (mock / demo / UI). */
function statusView(d, nowMs = Date.now()) {
  tickCollector(d, nowMs);
  // Keyed by MACHINE, not by port: this answers "what is running", and what runs
  // is a machine. A two-port saw appears once, as it should. Matches
  // TopologyRuntime::writeStatus.
  const tools = {};
  for (const m of S.machinesOf(d.topology)) {
    const w = d.toolWatts[m.id] || 0;
    tools[m.id] = { watts: w, active: w >= machineThreshold(d.topology, m.id) };
  }
  const systems = {};
  for (const [sysId, c] of Object.entries(d.collectors)) {
    systems[sysId] = { collectorOn: c.on, coasting: c.coasting, manual: !!c.manual };
  }
  return {
    actuators: { ...d.actuatorStates },
    tools,
    // "Is ANY blower running" — what a one-system shop has always read, and what
    // it still means there. Per-blower truth is in `systems`, which is where a
    // caller that knows about N systems should look: collapsing two blowers into
    // one boolean is fine for a summary and wrong for a decision.
    collectorOn: anyCollectorOn(d),
    // Only present while true, matching TopologyRuntime::writeStatus — additive,
    // so a consumer that predates coasting still sees the contract it expects.
    ...(anyCollectorCoasting(d) ? { collectorCoasting: true } : {}),
    conflicts: d.lastRouting.conflicts,
    // Keyed by PORT id — the thing that either got air or didn't.
    reachable: d.lastRouting.reachable,
    // The rolled-up verdict per machine: routed / partial / stripped.
    machines: d.lastRouting.machines || {},
    systems,
  };
}

module.exports = {
  DEFAULT_THRESHOLD_W, DEFAULT_COLLECTOR_OFF_DELAY_MS,
  machineThreshold, collectorOffDelayMs,
  createTopologyDevice, activeMachines, reconcile, setMachinePower, statusView,
  tickCollector, anyCollectorOn, anyCollectorCoasting,
  setCollectorManual, collectorIsManual,
  // v1 spellings — a tool WAS the machine before ports existed.
  toolThreshold, activeTools, setToolPower,
};
