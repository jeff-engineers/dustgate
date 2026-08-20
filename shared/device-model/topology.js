// topology.js — DustGate topology contract: types, helpers, and validation.
//
// This is the single source of truth for the SHAPE of an install (the schema
// in docs/topology-schema.md, as code). Firmware, mock, demo, and the
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

// Per-host (per-controller) actuator budget — one ESP32 drives at most the servo
// PWM bank (g_servos[4]) plus a single stepper driver. More than this on one host
// is a hardware impossibility; spread selectors across secondary controllers.
const MAX_SERVOS_PER_HOST = 4;
const MAX_LINEAR_PER_HOST = 1;

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
 * @property {number} [offsetDeg]    realization for servo kinds — degrees from the
 *                                   calibrated servo.referenceAngle (see servoCommandAngle)
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
 * @property {Object} [linear]            (selector kind:linear) { channel?, calibration }
 *                                        channel = which stepper driver (default 0);
 *                                        parallels servo.channel so >1 linear selector
 *                                        is a wiring question, not a model limit
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

/**
 * stateId → commanded angle, for every state on a servo selector.
 * The inverse direction of applyAbsoluteAngles: what the configurator seeds its
 * jog control with when re-opening an already-calibrated gate.
 * @returns {Object<string, number>|null}
 */
function absoluteAngles(sel) {
  if (!sel || (sel.kind !== 'servoGate' && sel.kind !== 'servoManifold')) return null;
  const out = {};
  for (const s of sel.states || []) {
    const a = servoCommandAngle(sel, s.id);
    if (a !== null) out[s.id] = a;
  }
  return out;
}

/**
 * Fold captured ABSOLUTE angles back into the schema's reference+offset form.
 *
 * Calibration is empirical — you jog the valve until it's physically right and
 * capture where it landed — so the configurator thinks in absolute angles. The
 * schema stores one calibrated referenceAngle plus fixed offsets, so the reference
 * state (a gate's OPEN, a manifold's LEFT) anchors the set and everything else
 * becomes a signed offset from it.
 *
 * Rejects a set that lands outside the servo's travel: that means the horn is
 * clocked wrong on its spline, and no amount of arithmetic fixes it (see the
 * mechanical notes in docs/topology-schema.md).
 *
 * @param {Object} sel                       servo selector
 * @param {Object<string, number>} captured  stateId → absolute angle
 * @returns {{ ok: boolean, selector?: Object, error?: string }}
 */
function applyAbsoluteAngles(sel, captured) {
  if (!sel || (sel.kind !== 'servoGate' && sel.kind !== 'servoManifold'))
    return { ok: false, error: 'not a servo selector' };

  const ref = (sel.states || []).find((s) => !s.isClosed);
  if (!ref) return { ok: false, error: 'selector has no open position to calibrate against' };

  const refAngle = captured[ref.id];
  if (typeof refAngle !== 'number')
    return { ok: false, error: `capture "${ref.id}" first — the other positions are measured from it` };

  const sv = sel.servo || {};
  const lo = typeof sv.minAngle === 'number' ? sv.minAngle : 0;
  const hi = typeof sv.maxAngle === 'number' ? sv.maxAngle : 180;

  const states = [];
  for (const s of sel.states) {
    const abs = captured[s.id];
    if (typeof abs !== 'number') return { ok: false, error: `position "${s.id}" hasn't been captured yet` };
    if (abs < lo || abs > hi)
      return { ok: false, error: `"${s.id}" lands outside the servo's travel — re-seat the horn and start over` };
    states.push(Object.assign({}, s, { offsetDeg: Math.round(abs - refAngle) }));
  }

  return {
    ok: true,
    selector: Object.assign({}, sel, {
      states,
      servo: Object.assign({}, sv, { referenceAngle: Math.round(refAngle) }),
    }),
  };
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
    // A SECONDARY is only reachable through its link. Without a host the primary
    // has no address to dial, so every gate on that board would be silently
    // un-driveable at run time — the firmware leaves it unregistered and reports
    // its moves as failed (see NodeBus). Catch it here, at config time, where the
    // user can still fix it. The primary needs no link: it IS the local board.
    if (c.role === 'secondary') {
      if (!c.link || typeof c.link !== 'object') {
        err('controller', `secondary board "${c.name || c.id}" has no link — the primary can't reach it`, c.id);
      } else if (typeof c.link.host !== 'string' || !c.link.host.trim()) {
        err('controller', `secondary board "${c.name || c.id}" has no link.host — the primary can't reach it`, c.id);
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

  // ── plugs: one physical outlet, one machine ──
  // A tool's plug is a SENSOR (sensor.outlet — we watch its draw) and the
  // collector's is a SWITCH (control.outlet — we command it), but both name the
  // same scarce thing: a Shelly on the network. Sharing one makes the router
  // believe two machines started at once, and sharing the collector's would have
  // the blower's own draw hold itself on. Firmware's toolForOutlet() maps by
  // ip/host and can only answer with one id, so this has to be unique here.
  const plugOwner = new Map();     // ip → element id
  for (const e of t.elements) {
    const outlet = e.type === 'collector' ? (e.control || {}).outlet : (e.sensor || {}).outlet;
    const ip = outlet && outlet.ip;
    if (!ip) continue;
    if (plugOwner.has(ip))
      err('element', `smart outlet ${ip} is on two elements ("${plugOwner.get(ip)}" and "${e.id}")`, e.id);
    else plugOwner.set(ip, e.id);
  }

  // ── selectors: kind, states, branches, refs ──
  // Per-host actuator tally — enforced against the hardware budget after the loop.
  const hostServos = new Map();   // controllerId → servo-selector count
  const hostLinear = new Map();   // controllerId → linear-selector count
  const servoChannelOwner = new Map(); // "controllerId channel" → selector id holding it
  for (const sel of selectorsOf(t)) {
    const ref = sel.id;
    if (!SELECTOR_KINDS.includes(sel.kind)) err('selector', `bad kind "${sel.kind}"`, ref);
    if (sel.controllerId && !ctrlIds.has(sel.controllerId))
      err('selector', `controllerId "${sel.controllerId}" does not resolve`, ref);
    else if (!sel.controllerId) err('selector', 'selector missing controllerId', ref);

    if (sel.controllerId) {
      if (sel.kind === 'linear') hostLinear.set(sel.controllerId, (hostLinear.get(sel.controllerId) || 0) + 1);
      else if (sel.kind === 'servoGate' || sel.kind === 'servoManifold')
        hostServos.set(sel.controllerId, (hostServos.get(sel.controllerId) || 0) + 1);
    }

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
      // reversed: a UI-only hint captured during calibration — the servo sits behind
      // some gates, so the jog arrows have to point the way the HANDLE moves. Firmware
      // never reads it; the captured angles are already correct either way.
      if (sv.reversed !== undefined && typeof sv.reversed !== 'boolean')
        err('selector', 'servo.reversed must be a boolean', ref);
      // Two servo selectors on one board sharing a PWM channel would move together.
      if (sel.controllerId && typeof sv.channel === 'number') {
        const key = `${sel.controllerId}\u0000${sv.channel}`;
        if (servoChannelOwner.has(key))
          err('selector', `servo channel ${sv.channel} on host "${sel.controllerId}" `
            + `is already used by "${servoChannelOwner.get(key)}"`, ref);
        else servoChannelOwner.set(key, sel.id);
      }
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

  // ── per-host actuator budget: ≤4 servos + ≤1 stepper on any one controller ──
  for (const [cid, n] of hostServos)
    if (n > MAX_SERVOS_PER_HOST)
      err('controller', `host "${cid}" has ${n} servo selectors (max ${MAX_SERVOS_PER_HOST} per host)`, cid);
  for (const [cid, n] of hostLinear)
    if (n > MAX_LINEAR_PER_HOST)
      err('controller', `host "${cid}" has ${n} linear selectors (max ${MAX_LINEAR_PER_HOST} per host)`, cid);

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
// A structurally valid topology can still be a BAD airflow design. Two ways a
// tool ends up leaking, both of which mean "you can't actually select this tool":
//
//   always-open — its path to the collector crosses NO actuated selector, so the
//                 collector bleeds through it no matter what is running.
//   co-open     — routing TO this tool unavoidably leaves another tool open too.
//                 The common shape: two tools teed off ONE outlet of a sliding
//                 gate or manifold (or one hung on the run that feeds another).
//                 Every gate on the path is open for both, so running either one
//                 sucks air through the other.
//
// The old check only asked "is there a selector somewhere above me", which the
// co-open case passes — the shared gate is above both tools. So the real test is
// the routed one: set the selectors the way this tool needs them (everything else
// at its closed state, exactly what routing.js commands) and see what else is
// still reachable from the collector.
//
// Advisory (a valid document, a bad config), and a HARD BLOCK on running in the
// Live view: leaky is never a valid state to drive, though it's fine transiently
// while editing. Reused by the configurator, firmware, and conformance so they
// all agree on "leaky".
function airflowIssues(topology) {
  const byId = new Map((topology.elements || []).map((e) => [e.id, e]));
  const parentDuct = parentDuctIndex(topology);
  const collector = collectorOf(topology);
  const tools = toolsOf(topology);

  // parent → its ducts, for walking back DOWN from the collector
  const kids = new Map();
  for (const d of topology.ducts || []) {
    if (!kids.has(d.parent)) kids.set(d.parent, []);
    kids.get(d.parent).push(d);
  }

  const issues = [];
  const alwaysOpen = new Set();

  // ── 1. always-open: nothing actuated between the tool and the collector ──
  // Except when it is the system's ONLY tool. "Always open" is a leak because
  // suction bleeds away from whatever you meant to run; with one tool there is
  // nothing else to run and nowhere else for the air to go, so the ungated shape
  // is the correct one — a sander on a shopvac, which the user wants switched on
  // automatically and never wants a gate in front of. Flagging it blocked the
  // Live view on a shop that routes perfectly (routeShop already reports the tool
  // `routed` with no states to command).
  if (tools.length > 1) for (const tool of tools) {
    let cur = tool.id, gated = false;
    for (;;) {
      const d = parentDuct.get(cur);
      if (!d) break;
      const parent = byId.get(d.parent);
      if (!parent) break;
      if (parent.type === 'selector') { gated = true; break; }
      cur = parent.id;
    }
    if (!gated) {
      alwaysOpen.add(tool.id);
      issues.push({ id: tool.id, name: tool.name || tool.id, kind: 'always-open' });
    }
  }

  /** selectorId → stateId this tool needs, or null if its path is broken/blocked. */
  function requiredStates(toolId) {
    const need = {};
    const seen = new Set();
    let cur = toolId;
    for (;;) {
      if (seen.has(cur)) return null;                 // cycle
      seen.add(cur);
      if (cur === (collector && collector.id)) return need;
      const d = parentDuct.get(cur);
      if (!d) return null;
      const parent = byId.get(d.parent);
      if (!parent) return null;
      if (parent.type === 'selector') {
        const b = (parent.branches || []).find((x) => x.id === d.parentBranch);
        if (!b || b.role === 'blocked') return null;  // can't be opened at all
        need[parent.id] = b.opensState;
      }
      cur = parent.id;
    }
  }

  /** Tools the collector can still see with the selectors set that way. */
  function toolsReachable(need) {
    const found = [];
    const stack = collector ? [collector.id] : [];
    const seen = new Set(stack);
    while (stack.length) {
      const id = stack.pop();
      const el = byId.get(id);
      if (el && el.type === 'tool') found.push(id);
      for (const d of kids.get(id) || []) {
        if (el && el.type === 'selector') {
          // only the branch the commanded state opens lets air through
          const b = (el.branches || []).find((x) => x.id === d.parentBranch);
          if (!b || b.role === 'blocked' || b.opensState !== need[el.id]) continue;
        }
        if (!seen.has(d.child)) { seen.add(d.child); stack.push(d.child); }
      }
    }
    return found;
  }

  // ── 2. co-open: routing to this tool leaves some OTHER tool open as well ──
  // Tools already flagged always-open are skipped on both sides: they're reported
  // once as the root cause instead of again beside every tool they leak past.
  if (collector) {
    for (const tool of tools) {
      if (alwaysOpen.has(tool.id)) continue;
      const need = requiredStates(tool.id);
      if (!need) continue;                             // unreachable — a validation error, not a leak
      const others = toolsReachable(need)
        .filter((id) => id !== tool.id && !alwaysOpen.has(id))
        .map((id) => byId.get(id));
      if (others.length) {
        issues.push({
          id: tool.id,
          name: tool.name || tool.id,
          kind: 'co-open',
          with: others.map((e) => ({ id: e.id, name: e.name || e.id })),
        });
      }
    }
  }

  return issues;
}

// ── redundant gates ───────────────────────────────────────────────────────────
//
// A gate earns its place by isolating something the gate above it doesn't already
// isolate. Two gates sitting inline on the same run, with no branch taking off
// between them, are doing one job twice — the second is a part to buy, wire,
// calibrate and eventually have fail, for nothing.
//
// That inline pair is the ONLY thing flagged. An earlier version asked the broader
// behavioural question ("does the shop behave identically without it?") and that
// caught real cases but also fired on gates that were merely unfinished — a valve
// with nothing but an open end below it isolates nothing YET, and telling someone
// their half-built run is redundant is just noise. Branching is what makes a gate
// earn its keep, so branching is what the test looks for.
//
// Advisory only, and deliberately NOT part of airflowIssues: a redundant gate is a
// valid, working shop, and nothing should refuse to run over it.

/** Ducts hanging off each element, for spotting a branch. */
function childDuctIndex(t) {
  const m = new Map();
  for (const d of t.ducts || []) {
    if (!m.has(d.parent)) m.set(d.parent, []);
    m.get(d.parent).push(d);
  }
  return m;
}

/** Branches that can actually carry air — a capped one isn't a fork in the run. */
function liveBranchCount(sel) {
  return (sel.branches || []).filter((b) => b.role !== 'blocked').length;
}

/**
 * Selectors that duplicate the gate immediately above them.
 *
 * For each gate, walk up the run looking for another gate. Anything passed on the
 * way must be a plain pass-through — a junction with a single child. The moment a
 * fork appears, the two gates are isolating different things and both are earning
 * their keep, so the walk stops.
 *
 * Of a duplicated pair, the one flagged is the newest (element order is insertion
 * order) so the message lands on the piece someone just placed rather than on a
 * gate that's been in the shop from the start. A gate with several live branches is
 * never flagged, whichever end of the pair it sits at: pulling it would take its
 * other legs with it.
 */
function redundantSelectors(topology) {
  const byId = new Map((topology.elements || []).map((e) => [e.id, e]));
  const order = new Map((topology.elements || []).map((e, i) => [e.id, i]));
  const parentDuct = parentDuctIndex(topology);
  const kids = childDuctIndex(topology);
  const flagged = new Map();

  for (const el of topology.elements || []) {
    if (el.type !== 'selector') continue;

    // Walk up to the nearest gate, refusing to pass a fork.
    let cur = el.id, above = null;
    const seen = new Set();
    for (;;) {
      if (seen.has(cur)) break;                       // cycle guard
      seen.add(cur);
      const d = parentDuct.get(cur);
      if (!d) break;
      const parent = byId.get(d.parent);
      if (!parent) break;
      if (parent.type === 'selector') { above = parent; break; }
      if (parent.type === 'collector') break;         // reached the top, no pair
      if ((kids.get(parent.id) || []).length !== 1) break;   // a branch takes off here
      cur = parent.id;
    }
    if (!above) continue;

    // Either end may be the removable one, but only if pulling it wouldn't strand
    // other legs. Between the two, the newer piece is the one worth mentioning.
    const pair = [above, el].filter((g) => liveBranchCount(g) <= 1);
    if (!pair.length) continue;
    const pick = pair.reduce((a, b) => ((order.get(b.id) ?? 0) > (order.get(a.id) ?? 0) ? b : a));
    flagged.set(pick.id, { id: pick.id, name: pick.name || pick.id });
  }

  return [...flagged.values()];
}

module.exports = {
  CONTROLLER_ROLES, ELEMENT_TYPES, SELECTOR_KINDS, BRANCH_ROLES,
  elementIndex, controllerIndex, parentDuctIndex,
  collectorOf, selectorsOf, toolsOf, closedState, servoCommandAngle,
  absoluteAngles, applyAbsoluteAngles,
  validateTopology, airflowIssues, redundantSelectors,
};
