// sequencer.js — DustGate transition sequencer (make-before-break).
//
// Routing (routing.js) says WHERE to end up. This says in WHAT ORDER to get
// there without ever sealing the system while the blower runs — never dead-head.
//
// Rule: open new paths BEFORE closing old ones ("make before break"). A servo
// move to a non-closed state is a "make"; to its closed state, a "break". A
// LINEAR selector maintains airflow through any move (it's a sliding aperture),
// so it's never a break — it can move anytime. The one-servo-at-a-time current
// mutex is honored implicitly: moves are a serial list.
//
// Idle-hold is a CALLER policy, not ours: when no tool is active the caller just
// doesn't ask for a transition (leave everything where it is). We only sequence a
// transition the caller actually wants.
//
// PURE. Consumes the topology contract (topology.js); no state, no I/O.

'use strict';

const T = require('./topology');

/**
 * @typedef {Object} TransitionPlan
 * @property {{selectorId:string,toState:string,kind:string,phase:'make'|'break'}[]} moves
 *           ordered: all makes (opens) first, then breaks (closes)
 * @property {boolean} deadHeadRisk  the DESTINATION seals everything while the
 *           blower runs — collector-control should switch the blower off instead
 */

/**
 * @param {import('./topology').Topology} topology
 * @param {Object.<string,string>} currentStates  selectorId → current stateId
 * @param {Object.<string,string>} desiredStates  selectorId → target stateId (from routing)
 * @param {{collectorRunning?: boolean}} [opts]
 * @returns {TransitionPlan}
 */
function planTransition(topology, currentStates, desiredStates, opts = {}) {
  const collectorRunning = !!opts.collectorRunning;
  const current = currentStates || {};

  const makes = [];
  const breaks = [];
  let anyOpen = false;

  for (const sel of T.selectorsOf(topology)) {
    const desired = desiredStates[sel.id];
    if (desired === undefined) continue;      // not addressed → leave as-is

    const closed = T.closedState(sel);
    const desiredIsClosed = !!closed && desired === closed.id;
    if (!desiredIsClosed) anyOpen = true;      // something ends up routing air

    const cur = current[sel.id];
    if (cur === desired) continue;             // already there → no move

    // Linear maintains flow through any move → never a break. A servo settling to
    // its closed state is the only "break" (it seals that path).
    const isBreak = desiredIsClosed && sel.kind !== 'linear';
    (isBreak ? breaks : makes).push({
      selectorId: sel.id, toState: desired, kind: sel.kind, phase: isBreak ? 'break' : 'make',
    });
  }

  // Dead-head risk: the destination leaves nothing open while the blower runs.
  const deadHeadRisk = collectorRunning && !anyOpen;

  return { moves: [...makes, ...breaks], deadHeadRisk };
}

module.exports = { planTransition };
