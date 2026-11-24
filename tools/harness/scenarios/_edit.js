// _edit.js — the layout edits the scenarios make, by hand for now. The point of the restructure (TODO/audit) is that
// these become the shared model's own operations, the same ones the app calls; until then each is written out here,
// which is exactly the duplication the audit names. Each returns a fresh copy.
'use strict';
const clone = (d) => JSON.parse(JSON.stringify(d));
const elems = (d) => d.systems.flatMap((s) => s.elements);
const el = (d, id) => elems(d).find((e) => e.id === id);
const board = (d, id) => d.controllers.find((c) => c.id === id);

/** Put a gate on another board: a PWM channel, or a bus servo number. Off every chain first. */
function moveGate(doc, gateId, boardId, where) {
  const d = clone(doc);
  const g = el(d, gateId);
  for (const c of d.controllers) if (c.chains) { c.chains = c.chains.map((x) => x.filter((i) => i !== gateId)).filter((x) => x.length); if (!c.chains.length) delete c.chains; }
  g.controllerId = boardId;
  const { referenceAngle, detented, holdAtRest } = g.servo || {};
  g.servo = { ...where, referenceAngle, detented, ...(holdAtRest !== undefined ? { holdAtRest } : {}) };
  if (where.bus !== undefined) { const b = board(d, boardId); b.chains = b.chains || [[]]; b.chains[0].push(gateId); }
  return d;
}

/** Say a board is now flashed as a bus board (or back to PWM). */
function boardDrives(doc, boardId, drives) {
  const d = clone(doc);
  const b = board(d, boardId);
  if (drives === 'bus') b.drives = 'bus'; else { delete b.drives; delete b.chains; }
  return d;
}

/** Take a tool and the gate that feeds it out of the shop. */
function removeToolAndGate(doc, toolId, gateId) {
  const d = clone(doc);
  for (const s of d.systems) {
    s.elements = s.elements.filter((e) => e.id !== toolId && e.id !== gateId);
    s.ducts = s.ducts.filter((x) => ![toolId, gateId].includes(x.child) && ![toolId, gateId].includes(x.parent));
  }
  d.machines = d.machines.filter((m) => m.id !== toolId);
  for (const c of d.controllers) if (c.chains) { c.chains = c.chains.map((x) => x.filter((i) => i !== gateId)).filter((x) => x.length); if (!c.chains.length) delete c.chains; }
  return d;
}

/** Take a board out of the layout (its gates must have moved off it first). */
function removeBoard(doc, boardId) {
  const d = clone(doc);
  d.controllers = d.controllers.filter((c) => c.id !== boardId);
  return d;
}

module.exports = { moveGate, boardDrives, removeToolAndGate, removeBoard, el, board, clone };
