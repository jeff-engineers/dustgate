// _edit.js — the layout edits the scenarios make. Where the shared model has the operation (shop-edit.js), this calls
// it, so a scenario exercises the same code the app does; the rest are still written out here until shop-edit.js grows
// them (TODO.md, "One owner per kind of thing"). Each returns a fresh copy.
'use strict';
const E = require('../../../shared/device-model/shop-edit.js');
const clone = (d) => JSON.parse(JSON.stringify(d));
const elems = (d) => d.systems.flatMap((s) => s.elements);
const el = (d, id) => elems(d).find((e) => e.id === id);
const board = (d, id) => d.controllers.find((c) => c.id === id);

/** Put a gate on another board: a PWM channel, or a bus servo number. */
function moveGate(doc, gateId, boardId, where) { return E.wireGate(clone(doc), gateId, boardId, where); }

/** Unplug a gate from its board, leaving it in the shop. */
function unwireGate(doc, gateId) { return E.unwireGate(clone(doc), gateId); }

/** Swap one board for another (a dead board replaced by a new one). */
function replaceBoard(doc, oldId, newId) { return E.replaceBoard(clone(doc), oldId, newId); }

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

module.exports = { moveGate, unwireGate, replaceBoard, boardDrives, removeToolAndGate, removeBoard, el, board, clone };
