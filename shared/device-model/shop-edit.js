// shop-edit.js — the edits a person makes to a shop, written ONCE (root CLAUDE.md, "Architecture rules", 2026-11-23).
//
// The canvas, the Boards page and the scenario harness call these instead of assigning ids and filtering arrays
// themselves. That is the whole point: a gate's id can sit in ducts, a bus board's chains and the canvas cells, and a
// board's id in gates, clamps, the collector and the wiring view. Each edit path used to clean up those places by
// hand, and the paths disagreed (a deleted gate left in a chain refused the next save, 2026-11-23).
//
// REFS below declares every place each kind of id can appear. A new kind of reference is added THERE, in the same
// change — `danglingRefs` (and the tests that run every edit through it) fails if an edit leaves one behind.
//
// Every edit takes the shop document and changes it IN PLACE, like the canvas edits its document, and returns it.
// Pure JS, no I/O: the UI (via the @shop-edit alias), the harness and the mock all require this file.
'use strict';

const { systemsOf, machinesOf } = require('./shop.js');

const elementsOf = (doc) => systemsOf(doc).flatMap((s) => s.elements || []);
const elementById = (doc, id) => elementsOf(doc).find((e) => e.id === id) || null;
const boardById = (doc, id) => (doc.controllers || []).find((c) => c.id === id) || null;
const isGate = (e) => !!e && e.type === 'selector';

// ── where each kind of id can appear ────────────────────────────────────────
// Each entry: a name for messages, every holder of the id (`find` yields [holder, key] pairs whose holder[key] is the
// id, or arrays that contain it), and what to do when the thing goes (`onRemove`).
const REFS = {
  gate: [
    { where: 'a duct (its parent or child)', each: (doc, fn) => { for (const s of systemsOf(doc)) for (const d of s.ducts || []) { fn(d, 'parent'); fn(d, 'child'); } } },
    { where: "a bus board's chain", each: (doc, fn) => { for (const c of doc.controllers || []) for (const ch of c.chains || []) for (let i = 0; i < ch.length; i++) fn(ch, i); } },
    { where: 'its canvas cell', each: (doc, fn) => { const l = doc.ui && doc.ui.layout; if (l) for (const k of Object.keys(l)) fn(l, k, true); } },
  ],
  board: [
    { where: "a gate's board", each: (doc, fn) => { for (const e of elementsOf(doc)) if (isGate(e)) fn(e, 'controllerId'); } },
    { where: "a collector's board", each: (doc, fn) => { for (const e of elementsOf(doc)) if (e.type === 'collector') fn(e, 'controllerId'); } },
    { where: "a tool's clamp", each: (doc, fn) => { for (const m of machinesOf(doc)) { const ct = m.sensor && m.sensor.ct; if (ct) fn(ct, 'controllerId'); } } },
    { where: 'its canvas cell', each: (doc, fn) => { const b = doc.ui && doc.ui.wiring && doc.ui.wiring.boards; if (b) for (const k of Object.keys(b)) fn(b, k, true); } },
  ],
};

/** Every place `id` (a `kind` of thing) appears: [{where, holder, key, isKey}]. */
function refsTo(doc, kind, id) {
  const out = [];
  for (const r of REFS[kind]) r.each(doc, (holder, key, isKey) => {
    if (isKey ? key === id : holder[key] === id) out.push({ where: r.where, holder, key, isKey: !!isKey });
  });
  return out;
}

/** Point every reference to `from` at `to`. A key (a canvas cell) is moved, keeping its value. */
function renameRefs(doc, kind, from, to) {
  for (const r of refsTo(doc, kind, from)) {
    if (r.isKey) { r.holder[to] = r.holder[from]; delete r.holder[from]; }
    else r.holder[r.key] = to;
  }
}

/** References to things the shop no longer has: [{kind, id, where}]. Empty is the invariant every edit keeps. */
function danglingRefs(doc) {
  const have = {
    gate: new Set(elementsOf(doc).map((e) => e.id)),   // a duct may join any element, so any element id resolves
    board: new Set((doc.controllers || []).map((c) => c.id)),
  };
  const out = [];
  for (const kind of Object.keys(REFS)) for (const r of REFS[kind]) r.each(doc, (holder, key, isKey) => {
    const id = isKey ? key : holder[key];
    if (id === undefined || id === null || id === '') return;
    if (!have[kind].has(id)) out.push({ kind, id, where: r.where });
  });
  return out;
}

// ── gates ────────────────────────────────────────────────────────────────────

/** Take a gate off every bus board's chain; empty chains go, and so does an empty `chains`. */
function unchain(doc, gateId) {
  for (const c of doc.controllers || []) {
    if (!Array.isArray(c.chains)) continue;
    const kept = c.chains.map((x) => x.filter((id) => id !== gateId));
    while (kept.length && !kept[kept.length - 1].length) kept.pop();
    if (kept.length) c.chains = kept; else delete c.chains;
  }
}

/**
 * Unplug a gate from its board: it stays in the shop, drawn and routed as closed, until it is wired again. This is
 * what a person does when the board under it changed (reflashed from PWM to serial, or gone) and they are not done
 * deciding where it goes. Its calibration (referenceAngle, states) is kept; only the socket goes.
 */
function unwireGate(doc, gateId) {
  const g = elementById(doc, gateId);
  if (!isGate(g)) throw new Error(`no gate "${gateId}"`);
  unchain(doc, gateId);
  delete g.controllerId;
  const block = g.kind === 'linear' ? g.linear : g.servo;
  if (block) { delete block.channel; delete block.bus; }
  return doc;
}

/**
 * Wire a gate to a board: `where` is `{channel}` for a PWM servo or `{bus}` for a serial one. A serial servo goes on the
 * end of the board's first chain. Whatever the gate was wired to before is let go first.
 */
function wireGate(doc, gateId, boardId, where) {
  const g = elementById(doc, gateId);
  if (!isGate(g)) throw new Error(`no gate "${gateId}"`);
  const b = boardById(doc, boardId);
  if (!b) throw new Error(`no board "${boardId}"`);
  unwireGate(doc, gateId);
  g.controllerId = boardId;
  const key = g.kind === 'linear' ? 'linear' : 'servo';
  g[key] = g[key] || {};
  if (where.bus !== undefined) {
    g[key].bus = where.bus;
    b.chains = Array.isArray(b.chains) && b.chains.length ? b.chains : [[]];
    b.chains[0].push(gateId);
  } else {
    g[key].channel = where.channel;
  }
  return doc;
}

/** Is this gate wired to a board? */
const isWired = (gate) => !!(gate && gate.controllerId);

// ── boards ───────────────────────────────────────────────────────────────────

/**
 * Replace one board with another: every gate, clamp, collector job and canvas cell on `oldId` moves to `newId`, and
 * `oldId` leaves the layout. For a dead board swapped for a new one, or a board renamed by a reflash. `newBoard` is the
 * new board's layout entry when the layout does not have it yet (a board just paired); its name defaults to the old one's.
 */
function replaceBoard(doc, oldId, newId, newBoard) {
  const old = boardById(doc, oldId);
  if (!old) throw new Error(`no board "${oldId}"`);
  if (oldId === newId) return doc;
  let nb = boardById(doc, newId);
  if (!nb) {
    // A copy of the old board under the new name: its kind (`drives`), its chains and its name come along.
    nb = { ...old, ...(newBoard || {}), id: newId };
    if (old.chains) nb.chains = old.chains.map((c) => [...c]);
    if (!(newBoard && newBoard.link)) nb.link = { ...(old.link || {}), host: newId };
    doc.controllers.push(nb);
  } else if (old.chains) {
    nb.chains = [...(nb.chains || []), ...old.chains];   // a board already in the layout keeps its own chains first
  }
  renameRefs(doc, 'board', oldId, newId);
  doc.controllers = doc.controllers.filter((c) => c.id !== oldId);
  return doc;
}

module.exports = { REFS, refsTo, renameRefs, danglingRefs, unwireGate, wireGate, isWired, replaceBoard, unchain };
