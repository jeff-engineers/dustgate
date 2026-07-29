// topology-device.js — stateful topology-native device simulator.
//
// Wraps the pure core (topology.js + routing.js + sequencer.js) into a device
// that behaves like the real thing: tools draw power → the active set (most-recent
// first) drives routing → selectors move → the collector follows. This is the
// model the topology-native API / mock / demo will all expose (the v2 successor to
// device-model.js's flat stops/outlets state).
//
// Policy baked in here (see v2 memory):
//   - most-recent-tool-wins  → active tools are ordered most-recently-activated
//     first, and routing.js resolves contests in that order.
//   - idle-hold              → with NO active tool, HOLD every selector where it
//     is (never auto-close) and switch the collector off. Keeps a path open so a
//     manual collector start can't dead-head, and avoids wear on a brief tool-off.
//
// PURE + synchronous: reconcile() jumps selectors to their target state. The
// ORDERED make-before-break moves (for a real actuator's timing) are returned via
// planTransition so a consumer (firmware/mock) can animate them; the sim itself
// doesn't need the delay.

'use strict';

const T = require('./topology');
const { computeRouting } = require('./routing');
const { planTransition } = require('./sequencer');

const DEFAULT_THRESHOLD_W = 5;

/** Watts above which a tool counts as "on" (from its sensor outlet, or default). */
function toolThreshold(topology, toolId) {
  const el = T.elementIndex(topology).get(toolId);
  const w = el && el.sensor && el.sensor.outlet && el.sensor.outlet.thresholdW;
  return typeof w === 'number' ? w : DEFAULT_THRESHOLD_W;
}

/** Create a device from a topology: every selector at its closed state, idle. */
function createTopologyDevice(topology) {
  const actuatorStates = {};
  for (const sel of T.selectorsOf(topology)) {
    const cs = T.closedState(sel);
    actuatorStates[sel.id] = cs ? cs.id : null;
  }
  return {
    topology,
    actuatorStates,           // selectorId → current stateId
    toolWatts: {},            // toolId → last power reading
    activationSeq: {},        // toolId → recency seq (absent = inactive)
    seqCounter: 0,
    collectorOn: false,
    lastRouting: { states: {}, conflicts: [], reachable: {} },
  };
}

/** Tools currently active (watts ≥ threshold), most-recently-activated FIRST. */
function activeTools(d) {
  const active = [];
  for (const tool of T.toolsOf(d.topology)) {
    const w = d.toolWatts[tool.id] || 0;
    if (w >= toolThreshold(d.topology, tool.id)) active.push(tool.id);
  }
  // Higher activationSeq = more recent → wins contests (most-recent-wins).
  active.sort((a, b) => (d.activationSeq[b] || 0) - (d.activationSeq[a] || 0));
  return active;
}

/**
 * Recompute desired actuator states from the active tools and apply them.
 * Idle (no active tool): HOLD all selectors, collector off. Active: route to the
 * winners and close the rest (focus suction), collector on.
 * @returns {{routing: object, plan: object}}
 */
function reconcile(d) {
  const active = activeTools(d);
  const routing = computeRouting(d.topology, active);
  const plan = planTransition(d.topology, d.actuatorStates, routing.states, { collectorRunning: d.collectorOn });

  if (active.length > 0) {
    d.actuatorStates = { ...d.actuatorStates, ...routing.states };
    d.collectorOn = true;
  } else {
    d.collectorOn = false;   // idle: hold positions (actuatorStates unchanged)
  }
  d.lastRouting = routing;
  return { routing, plan };
}

/** Set a tool's live power reading; tracks the OFF→ON edge (recency) and reconciles. */
function setToolPower(d, toolId, watts) {
  const th = toolThreshold(d.topology, toolId);
  const wasActive = (d.toolWatts[toolId] || 0) >= th;
  d.toolWatts[toolId] = watts;
  const nowActive = watts >= th;
  if (nowActive && !wasActive) d.activationSeq[toolId] = ++d.seqCounter; // rising edge → newest
  if (!nowActive) delete d.activationSeq[toolId];
  return reconcile(d);
}

/** Projected status for consumers (mock / demo / UI). */
function statusView(d) {
  const tools = {};
  for (const tool of T.toolsOf(d.topology)) {
    const w = d.toolWatts[tool.id] || 0;
    tools[tool.id] = { watts: w, active: w >= toolThreshold(d.topology, tool.id) };
  }
  return {
    actuators: { ...d.actuatorStates },
    tools,
    collectorOn: d.collectorOn,
    conflicts: d.lastRouting.conflicts,
    reachable: d.lastRouting.reachable,
  };
}

module.exports = {
  DEFAULT_THRESHOLD_W, toolThreshold,
  createTopologyDevice, activeTools, reconcile, setToolPower, statusView,
};
