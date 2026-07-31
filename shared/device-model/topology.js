// topology.js — DustGate v2 topology contract: types, helpers, and validation.
//
// This is the single source of truth for the SHAPE of a v2 install (the schema
// in docs/v2-topology-schema.md, as code). Firmware, mock, demo, and the
// configurator all consume it. PURE — no HTTP/timers/state; just data + checks.
//
// Vocabulary (resolve the "node" overload):
//   controller — an ESP32 board (primary owns GUI + routing; secondaries are
//                dumb actuator banks).
//   element    — a vertex in the airflow graph: collector | selector | tool | junction.
//   branch     — one selectable outlet of a selector, opened by exactly one state,
//                carrying a role (tool | unassigned | blocked | feed).
//   duct       — a directed edge child → parent, pointing toward the collector.
//
// See routing.js for the engine that turns "these tools are on" into
// "each selector should be in this state," and topology.test.js for fixtures.

'use strict';

// ── Enums (mirror the schema doc) ───────────────────────────────────────────
const CONTROLLER_ROLES = ['primary', 'secondary'];
const ELEMENT_TYPES    = ['collector', 'selector', 'tool', 'junction'];
const SELECTOR_KINDS   = ['linear', 'servoGate', 'servoManifold'];
const BRANCH_ROLES     = ['tool', 'unassigned', 'blocked', 'feed'];
const LINK_TRANSPORTS  = ['wifi-ws', 'esp-now'];

// ── Typedefs (JSDoc — gives TS consumers types without a build step) ─────────
/**
 * @typedef {Object} Outlet
 * @property {2} gen                 Shelly generation (always 2; Gen1 dropped)
 * @property {string} ip
 * @property {string} [host]         mDNS hostname (device id), for DHCP re-resolve
 * @property {string} [name]         Shelly-app name
 * @property {number} [thresholdW]   tool sensor: watts above which the tool is "on"
 *
 * @typedef {Object} SelectorState
 * @property {string} id
 * @property {boolean} isClosed      exactly one state per selector is the all-closed rest
 * @property {number} [positionMm]   realization for kind:"linear"
 * @property {number} [angleDeg]     realization for servo kinds (also the alignment knob)
 *
 * @typedef {Object} Branch
 * @property {string} id
 * @property {string} opensState     id of the (non-closed) state that opens this branch
 * @property {'tool'|'unassigned'|'blocked'|'feed'} role
 *
 * @typedef {Object} Link            optional — how the primary reaches a controller
 *                                   board over the network. Absent for a purely
 *                                   local board; present for a secondary "dumb"
 *                                   ESP32 that hosts remote gates.
 * @property {'wifi-ws'|'esp-now'} transport
 * @property {string} [host]         mDNS hostname (device id) — the STABLE key
 * @property {string} [ip]           last-known IP cache (re-resolved via host)
 * @property {string} [name]
 *
 * @typedef {Object} Controller
 * @property {string} id
 * @property {'primary'|'secondary'} role
 * @property {string} [name]
 * @property {string} [board]
 * @property {Link} [link]
 *
 * @typedef {Object} Duct
 * @property {string} child
 * @property {string} parent
 * @property {string} [parentBranch] required iff parent is a selector
 *
 * @typedef {Object} Element        (discriminated by .type — see the schema doc)
 * @property {string} id
 * @property {'collector'|'selector'|'tool'|'junction'} type
 * @property {string} [name]
 * @property {string} [controllerId]      (selector)
 * @property {'linear'|'servoGate'|'servoManifold'} [kind]  (selector)
 * @property {SelectorState[]} [states]   (selector)
 * @property {Branch[]} [branches]        (selector)
 * @property {Object} [linear]            (selector kind:linear) { calibration }
 * @property {Object} [servo]             (selector servo kinds) { channel, moveMs, holdAtRest, ... }
 * @property {Object} [sensor]            (tool) { outlet }
 * @property {Object} [control]           (collector) { outlet, offDelayMs }
 *
 * @typedef {Object} Topology
 * @property {number} schemaVersion
 * @property {string} [name]
 * @property {Controller[]} controllers
 * @property {Element[]} elements
 * @property {Duct[]} ducts
 */

// ── Helpers (shared by routing + consumers) ─────────────────────────────────

/** id → element */
function elementIndex(t) {
  const m = new Map();
  for (const e of t.elements || []) m.set(e.id, e);
  return m;
}
/** id → controller */
function controllerIndex(t) {
  const m = new Map();
  for (const c of t.controllers || []) m.set(c.id, c);
  return m;
}
/** childId → its (single) parent duct */
function parentDuctIndex(t) {
  const m = new Map();
  for (const d of t.ducts || []) if (!m.has(d.child)) m.set(d.child, d);
  return m;
}
const collectorOf  = (t) => (t.elements || []).find((e) => e.type === 'collector') || null;
const selectorsOf  = (t) => (t.elements || []).filter((e) => e.type === 'selector');
const toolsOf      = (t) => (t.elements || []).filter((e) => e.type === 'tool');
/** the all-closed rest state of a selector (or null) */
const closedState  = (sel) => (sel.states || []).find((s) => s.isClosed) || null;

/**
 * Commanded servo angle for a state on a servo selector:
 *   angle = referenceAngle + state.offsetDeg, clamped to [minAngle, maxAngle].
 * referenceAngle is the per-build calibrated angle of the reference position
 * (gate OPEN / manifold LEFT); offsetDeg is a fixed valve-design constant. The
 * firmware servo state-driver mirrors this (like manifoldProfile is mirrored).
 * Returns null for a non-servo selector or an unknown state.
 */
function servoCommandAngle(sel, stateId) {
  if (!sel || (sel.kind !== 'servoGate' && sel.kind !== 'servoManifold')) return null;
  const st = (sel.states || []).find((s) => s.id === stateId);
  if (!st || typeof st.offsetDeg !== 'number') return null;
  const sv = sel.servo || {};
  const ref = typeof sv.referenceAngle === 'number' ? sv.referenceAngle : 0;
  const lo  = typeof sv.minAngle === 'number' ? sv.minAngle : 0;
  const hi  = typeof sv.maxAngle === 'number' ? sv.maxAngle : 180;
  return Math.min(hi, Math.max(lo, ref + st.offsetDeg));
}

// ── Validation ──────────────────────────────────────────────────────────────

/**
 * Validate a topology against the schema's structural rules.
 * @param {Topology} t
 * @returns {{ ok: boolean, errors: {code:string,message:string,ref?:string}[] }}
 */
function validateTopology(t) {
  /** @type {{code:string,message:string,ref?:string}[]} */
  const errors = [];
  const err = (code, message, ref) => errors.push(ref ? { code, message, ref } : { code, message });

  // ── top-level shape ──
  if (!t || typeof t !== 'object') { err('shape', 'topology must be an object'); return { ok: false, errors }; }
  if (typeof t.schemaVersion !== 'number') err('shape', 'schemaVersion must be a number');
  for (const k of ['controllers', 'elements', 'ducts']) {
    if (!Array.isArray(t[k])) err('shape', `${k} must be an array`);
  }
  if (errors.length) return { ok: false, errors };

  // ── controllers ──
  const ctrlIds = new Set();
  let primaries = 0;
  for (const c of t.controllers) {
    if (typeof c.id !== 'string' || !c.id) { err('controller', 'controller missing id'); continue; }
    if (ctrlIds.has(c.id)) err('controller', `duplicate controller id: ${c.id}`, c.id);
    ctrlIds.add(c.id);
    if (!CONTROLLER_ROLES.includes(c.role)) err('controller', `bad role "${c.role}"`, c.id);
    if (c.role === 'primary') primaries++;
    if (c.link !== undefined) {
      if (typeof c.link !== 'object' || c.link === null) err('controller', 'link must be an object', c.id);
      else {
        if (!LINK_TRANSPORTS.includes(c.link.transport))
          err('controller', `link bad transport "${c.link.transport}"`, c.id);
        if (c.link.host !== undefined && typeof c.link.host !== 'string')
          err('controller', 'link.host must be a string', c.id);
        if (c.link.ip !== undefined && typeof c.link.ip !== 'string')
          err('controller', 'link.ip must be a string', c.id);
      }
    }
  }
  if (primaries !== 1) err('controller', `exactly one primary controller required (found ${primaries})`);

  // ── elements: ids, types, one collector ──
  const byId = elementIndex(t);
  const elemIds = new Set();
  let collectors = 0;
  for (const e of t.elements) {
    if (typeof e.id !== 'string' || !e.id) { err('element', 'element missing id'); continue; }
    if (elemIds.has(e.id)) err('element', `duplicate element id: ${e.id}`, e.id);
    elemIds.add(e.id);
    if (!ELEMENT_TYPES.includes(e.type)) err('element', `bad type "${e.type}"`, e.id);
    if (e.type === 'collector') collectors++;
  }
  if (collectors !== 1) err('element', `exactly one collector required (found ${collectors})`);

  // ── selectors: kind, states, branches, refs ──
  for (const sel of selectorsOf(t)) {
    const ref = sel.id;
    if (!SELECTOR_KINDS.includes(sel.kind)) err('selector', `bad kind "${sel.kind}"`, ref);
    if (sel.controllerId && !ctrlIds.has(sel.controllerId))
      err('selector', `controllerId "${sel.controllerId}" does not resolve`, ref);
    else if (!sel.controllerId) err('selector', 'selector missing controllerId', ref);

    const states = sel.states || [];
    const stateIds = new Set();
    let closed = 0;
    for (const s of states) {
      if (typeof s.id !== 'string' || !s.id) { err('selector', 'state missing id', ref); continue; }
      if (stateIds.has(s.id)) err('selector', `duplicate state id "${s.id}"`, ref);
      stateIds.add(s.id);
      if (s.isClosed) closed++;
      // Realization per kind. Linear: non-home states carry positionMm. Servo:
      // every state carries offsetDeg (angular offset from the calibrated
      // servo.referenceAngle; commanded angle = referenceAngle + offsetDeg).
      if (sel.kind === 'linear') {
        if (!s.isClosed && typeof s.positionMm !== 'number')
          err('selector', `linear state "${s.id}" needs positionMm`, ref);
      } else { // servoGate | servoManifold
        if (typeof s.offsetDeg !== 'number')
          err('selector', `servo state "${s.id}" needs offsetDeg`, ref);
      }
    }
    if (closed !== 1) err('selector', `exactly one isClosed state required (found ${closed})`, ref);

    // Servo block: light checks (referenceAngle is populated at calibration, so
    // optional; validate the shape of what's present).
    if (sel.kind === 'servoGate' || sel.kind === 'servoManifold') {
      const sv = sel.servo || {};
      if (sv.referenceAngle !== undefined && typeof sv.referenceAngle !== 'number')
        err('selector', 'servo.referenceAngle must be a number', ref);
      if (sv.detented !== undefined && typeof sv.detented !== 'boolean')
        err('selector', 'servo.detented must be a boolean', ref);
    }

    // branches ↔ non-closed states must be a bijection
    const branches = sel.branches || [];
    const branchIds = new Set();
    const openedStates = new Set();
    for (const b of branches) {
      if (typeof b.id !== 'string' || !b.id) { err('selector', 'branch missing id', ref); continue; }
      if (branchIds.has(b.id)) err('selector', `duplicate branch id "${b.id}"`, ref);
      branchIds.add(b.id);
      if (!BRANCH_ROLES.includes(b.role)) err('selector', `branch "${b.id}" bad role "${b.role}"`, ref);
      if (!stateIds.has(b.opensState)) err('selector', `branch "${b.id}" opensState "${b.opensState}" missing`, ref);
      else if (openedStates.has(b.opensState)) err('selector', `state "${b.opensState}" opened by >1 branch`, ref);
      else openedStates.add(b.opensState);
    }
    const nonClosed = states.filter((s) => !s.isClosed).length;
    if (branches.length !== nonClosed)
      err('selector', `branches (${branches.length}) must equal non-closed states (${nonClosed})`, ref);
  }

  // ── ducts: refs, collector-is-root, parentBranch rules ──
  const parentCount = new Map(); // childId → number of parent ducts
  for (const d of t.ducts) {
    const c = byId.get(d.child), p = byId.get(d.parent);
    if (!c) err('duct', `duct child "${d.child}" does not resolve`);
    if (!p) err('duct', `duct parent "${d.parent}" does not resolve`);
    if (c && c.type === 'collector') err('duct', 'collector cannot be a child (it is the root)', d.child);
    parentCount.set(d.child, (parentCount.get(d.child) || 0) + 1);

    if (p && p.type === 'selector') {
      if (!d.parentBranch) err('duct', `duct to selector "${d.parent}" needs parentBranch`, d.child);
      else if (!(p.branches || []).some((b) => b.id === d.parentBranch))
        err('duct', `parentBranch "${d.parentBranch}" not on selector "${d.parent}"`, d.child);
    } else if (p && d.parentBranch) {
      err('duct', `parentBranch set but parent "${d.parent}" is not a selector`, d.child);
    }
  }

  // ── tree: every non-collector has exactly one parent; collector has none ──
  for (const e of t.elements) {
    const n = parentCount.get(e.id) || 0;
    if (e.type === 'collector') { if (n !== 0) err('tree', 'collector must have no parent duct', e.id); }
    else if (n !== 1) err('tree', `element "${e.id}" must have exactly one parent duct (has ${n})`, e.id);
  }

  // ── branch role ↔ child consistency ──
  // Map (selectorId, branchId) → child element via ducts.
  const childOfBranch = new Map();
  for (const d of t.ducts) {
    const p = byId.get(d.parent);
    if (p && p.type === 'selector' && d.parentBranch) childOfBranch.set(`${p.id}:${d.parentBranch}`, byId.get(d.child));
  }
  for (const sel of selectorsOf(t)) {
    for (const b of sel.branches || []) {
      const child = childOfBranch.get(`${sel.id}:${b.id}`);
      if (b.role === 'tool') {
        if (!child) err('role', `branch "${b.id}" role tool but nothing wired to it`, sel.id);
        else if (child.type !== 'tool') err('role', `branch "${b.id}" role tool but child is ${child.type}`, sel.id);
      } else if (b.role === 'feed') {
        // A feed branch feeds a downstream sub-network: another selector, or a
        // junction (a passive tee where the pipe branches). Routing passes through
        // junctions transparently, so the gate above still governs the whole group.
        if (!child) err('role', `branch "${b.id}" role feed but nothing wired to it`, sel.id);
        else if (child.type !== 'selector' && child.type !== 'junction') err('role', `branch "${b.id}" role feed but child is ${child.type}`, sel.id);
      } else { // unassigned | blocked
        if (child) err('role', `branch "${b.id}" role ${b.role} must have no child (has "${child.id}")`, sel.id);
      }
    }
  }

  // ── no cycles: every element must reach the collector via parent ducts ──
  const parentDuct = parentDuctIndex(t);
  const collector = collectorOf(t);
  if (collector) {
    for (const e of t.elements) {
      if (e.type === 'collector') continue;
      let cur = e.id;
      const seen = new Set();
      let reached = false;
      while (true) {
        if (seen.has(cur)) break;         // cycle
        seen.add(cur);
        const d = parentDuct.get(cur);
        if (!d) break;                    // dead end (already flagged by tree/ref checks)
        if (d.parent === collector.id) { reached = true; break; }
        cur = d.parent;
      }
      if (!reached && (parentCount.get(e.id) || 0) === 1) {
        // only complain about cycles for otherwise-well-formed single-parent elements
        err('tree', `element "${e.id}" does not reach the collector (cycle or broken chain)`, e.id);
      }
    }
  }

  return { ok: errors.length === 0, errors };
}

// ── Airflow integrity ─────────────────────────────────────────────────────────
// A structurally valid topology can still be a BAD airflow design: any tool whose
// path to the collector crosses NO actuated selector is "always open" — the
// collector bleeds suction through it no matter which tool is running, so the
// whole system runs weak. This is advisory (a valid document, a bad config), and
// is a HARD BLOCK on save in the configurator: an always-open gate is never a
// valid saved state, though it's fine transiently while editing. Reused by the
// configurator, firmware, and conformance so they all agree on "leaky".
function airflowIssues(topology) {
  const byId = new Map((topology.elements || []).map((e) => [e.id, e]));
  const parentDuct = parentDuctIndex(topology);
  const issues = [];
  for (const tool of toolsOf(topology)) {
    let cur = tool.id, gated = false;
    // walk up toward the collector; a selector anywhere on the path gates it
    for (;;) {
      const d = parentDuct.get(cur);
      if (!d) break;
      const parent = byId.get(d.parent);
      if (!parent) break;
      if (parent.type === 'selector') { gated = true; break; }
      cur = parent.id;
    }
    if (!gated) issues.push({ id: tool.id, name: tool.name || tool.id, kind: 'always-open' });
  }
  return issues;
}

module.exports = {
  CONTROLLER_ROLES, ELEMENT_TYPES, SELECTOR_KINDS, BRANCH_ROLES,
  elementIndex, controllerIndex, parentDuctIndex,
  collectorOf, selectorsOf, toolsOf, closedState, servoCommandAngle,
  validateTopology, airflowIssues,
};
