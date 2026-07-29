// routing.js — DustGate v2 routing engine.
//
// The pure core of P0: given a topology and which tools are running (in PRIORITY
// order), compute the state every selector should be in to route airflow from
// the winning tools to the collector, and report any conflicts (two tools that
// need incompatible states of a shared selector).
//
// Conflict policy lives in the CALLER, expressed purely as input order: the
// active-tool list is highest-priority-first, and when two tools contend for a
// selector the earlier one wins. Pass most-recently-powered-on first and you get
// "most recent tool wins"; the engine itself is policy-agnostic (order-driven),
// so a different policy is just a different sort — no engine change.
//
// PURE. Consumes the topology contract (topology.js); no state, no I/O.

'use strict';

const T = require('./topology');

/**
 * @typedef {Object} RoutingResult
 * @property {Object.<string,string|null>} states     selectorId → stateId to command
 *           (winner's state on a contested selector; closed state otherwise)
 * @property {{selectorId:string,winner:string,winnerState:string,losers:string[]}[]} conflicts
 *           selectors a lower-priority tool lost (it yields to the earlier tool)
 * @property {Object.<string,boolean>} reachable       toolId → won a clear path right now
 */

/**
 * @param {import('./topology').Topology} topology
 * @param {string[]} activeToolIds  ids of tools currently drawing power
 * @returns {RoutingResult}
 */
function computeRouting(topology, activeToolIds) {
  const byId = T.elementIndex(topology);
  const parentDuct = T.parentDuctIndex(topology);
  const collector = T.collectorOf(topology);
  const active = [...new Set(activeToolIds || [])].filter((id) => {
    const e = byId.get(id);
    return e && e.type === 'tool';
  });

  // Walk a tool up to the collector, collecting the selector state each hop needs.
  // Returns null if the path is broken, hits a blocked branch, or cycles.
  function pathToCollector(toolId) {
    const path = [];
    const seen = new Set();
    let cur = toolId;
    while (true) {
      if (seen.has(cur)) return null;          // cycle guard
      seen.add(cur);
      const el = byId.get(cur);
      if (!el) return null;
      if (el.type === 'collector') return path;
      const duct = parentDuct.get(cur);
      if (!duct) return null;                  // dead end
      const parent = byId.get(duct.parent);
      if (!parent) return null;
      if (parent.type === 'selector') {
        const branch = (parent.branches || []).find((b) => b.id === duct.parentBranch);
        if (!branch || branch.role === 'blocked') return null;
        path.push({ selectorId: parent.id, stateId: branch.opensState });
      }
      cur = parent.id;
    }
  }

  // 1. Each active tool's required (selector → state) path, in priority order.
  const toolPaths = new Map();
  for (const toolId of active) toolPaths.set(toolId, pathToCollector(toolId));

  // 2. Greedy priority-ordered assignment. Process tools in the given order (the
  //    caller sorts by policy — most-recent-first for most-recent-wins). A tool
  //    wins its whole path only if no selector on it is already committed by an
  //    earlier (higher-priority) tool to a DIFFERENT state; otherwise it loses
  //    and is reported. Sharing a selector at the SAME state is fine (both flow).
  const committed = {}; // selectorId → stateId (first commit wins)
  const winnerOf  = {}; // selectorId → toolId that committed it
  const reachable = {};
  const conflictBySel = new Map(); // selectorId → { winner, winnerState, losers:Set }

  for (const toolId of active) {
    const path = toolPaths.get(toolId);
    if (!path) { reachable[toolId] = false; continue; }

    let blockedSel = null;
    for (const { selectorId, stateId } of path) {
      const c = committed[selectorId];
      if (c !== undefined && c !== stateId) { blockedSel = selectorId; break; }
    }
    if (blockedSel !== null) {
      reachable[toolId] = false;
      let e = conflictBySel.get(blockedSel);
      if (!e) { e = { winner: winnerOf[blockedSel], winnerState: committed[blockedSel], losers: new Set() }; conflictBySel.set(blockedSel, e); }
      e.losers.add(toolId);
      continue;
    }
    for (const { selectorId, stateId } of path) {
      if (committed[selectorId] === undefined) { committed[selectorId] = stateId; winnerOf[selectorId] = toolId; }
    }
    reachable[toolId] = true;
  }

  // 3. States: every selector defaults to closed; winners' committed states win.
  const states = {};
  for (const sel of T.selectorsOf(topology)) {
    const cs = T.closedState(sel);
    states[sel.id] = cs ? cs.id : null;
  }
  for (const [selectorId, stateId] of Object.entries(committed)) states[selectorId] = stateId;

  // 4. Conflicts: selectors a lower-priority tool yielded to an earlier one.
  const conflicts = [...conflictBySel.entries()].map(([selectorId, e]) => ({
    selectorId, winner: e.winner, winnerState: e.winnerState, losers: [...e.losers],
  }));

  return { states, conflicts, reachable };
}

module.exports = { computeRouting };
