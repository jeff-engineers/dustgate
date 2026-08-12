/** selector-types — the FLATTENING readers.
 *
 *  Plain TypeScript, no Angular, no browser. Run with `npm test`.
 *
 *  elementsOf() and ductsOf() answer shop-wide questions, so they flatten across
 *  systems. The failure mode if they don't is the quiet kind: handed a shop they
 *  find no `elements` at the root, return [], and every caller concludes the shop
 *  is empty — no gates need calibrating, no ids are taken, nothing leaks.
 */

import { suite } from '../../test-harness';
import type { Topology } from '@topology';
import { controllersOf, ductsOf, elementsOf } from './selector-types';

const { check, eq, report } = suite();

const gate = (id: string, ch: number) => ({
  id, type: 'selector', name: id, controllerId: 'primary', kind: 'servoGate',
  states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
  branches: [{ id: 'b1', opensState: 'open', role: 'tool' }],
  servo: { channel: ch, referenceAngle: 90 },
});

const v1 = {
  schemaVersion: 1,
  controllers: [{ id: 'primary', role: 'primary' }],
  elements: [{ id: 'dc', type: 'collector' }, gate('g1', 0), { id: 't1', type: 'tool' }],
  ducts: [{ child: 'g1', parent: 'dc' }, { child: 't1', parent: 'g1', parentBranch: 'b1' }],
} as unknown as Topology;

const shop = {
  schemaVersion: 2,
  controllers: [{ id: 'primary', role: 'primary' }, { id: 'node-1', role: 'secondary' }],
  systems: [
    {
      id: 'big',
      elements: [{ id: 'dc-big', type: 'collector' }, gate('g1', 0), { id: 'p1', type: 'tool', machineId: 'saw' }],
      ducts: [{ child: 'g1', parent: 'dc-big' }, { child: 'p1', parent: 'g1', parentBranch: 'b1' }],
    },
    {
      id: 'small',
      elements: [{ id: 'dc-small', type: 'collector' }, gate('g2', 1), { id: 'p2', type: 'tool', machineId: 'drill' }],
      ducts: [{ child: 'g2', parent: 'dc-small' }, { child: 'p2', parent: 'g2', parentBranch: 'b1' }],
    },
  ],
  machines: [{ id: 'saw' }, { id: 'drill' }],
} as unknown as Topology;

// ── v1 still reads exactly as it did ────────────────────────────────────────
{
  eq('v1 elements read from the root', elementsOf(v1).map(e => e.id), ['dc', 'g1', 't1']);
  eq('v1 ducts read from the root', ductsOf(v1).length, 2);
  eq('v1 controllers', controllersOf(v1).map(c => c.id), ['primary']);
}

// ── a shop flattens ─────────────────────────────────────────────────────────
{
  // The whole point: BOTH systems, or the second one is invisible to every
  // shop-wide question in the app.
  eq('elements flatten across systems', elementsOf(shop).map(e => e.id),
    ['dc-big', 'g1', 'p1', 'dc-small', 'g2', 'p2']);
  eq('ducts flatten across systems', ductsOf(shop).length, 4);
  eq('two collectors are visible at once',
    elementsOf(shop).filter(e => e.type === 'collector').length, 2);
  eq('...and both gates, so neither escapes calibration checks',
    elementsOf(shop).filter(e => e.type === 'selector').map(e => e.id), ['g1', 'g2']);

  // Controllers are shop-level in BOTH shapes — a board is mounted where the
  // cable reaches and may drive selectors in any number of systems, so it is not
  // owned by one.
  eq('controllers are read from the root, not per system',
    controllersOf(shop).map(c => c.id), ['primary', 'node-1']);
}

// ── degenerate input ────────────────────────────────────────────────────────
{
  eq('no elements → []', elementsOf({ schemaVersion: 1 } as unknown as Topology), []);
  eq('empty systems → []', elementsOf({ systems: [] } as unknown as Topology), []);
  eq('a system with no elements → []',
    elementsOf({ systems: [{ id: 'a' }] } as unknown as Topology), []);
  eq('null in → []', elementsOf(null as unknown as Topology), []);
  eq('null in → [] for ducts', ductsOf(null as unknown as Topology), []);
  // `systems` that isn't an array must not be treated as one — a truncated or
  // hand-edited file shouldn't crash the canvas on load.
  check('a non-array `systems` falls back to root reading',
    elementsOf({ systems: 'nope', elements: [{ id: 'x', type: 'tool' }] } as unknown as Topology).length === 1);
}

report();
