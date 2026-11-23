// layout-gate.test.js — what a brain refuses (shop.js layoutGate) and which gates it never moves (sharedChannelGates).
// PAIRED with firmware/test/test_layoutgate.cpp: same cases, same order, the refusal words asserted literally (D-88).
//
// Run: `node layout-gate.test.js`.
'use strict';

const { layoutGate, sharedChannelGates } = require('./shop');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });

const good = { controllers: [{ id: 'primary', role: 'primary' }], machines: [],
  systems: [{ id: 's1', elements: [{ id: 'dc', type: 'collector' }] }] };
const sys1 = (elements) => [{ id: 's1', elements }];
const P = [{ id: 'p', role: 'primary' }];

check('a shop with one primary, one system, one collector passes', layoutGate(good) === null);
check('no systems: refused', layoutGate({ controllers: P, machines: [], systems: [] }) === 'a shop needs at least one system');
check('systems missing: refused', layoutGate({ controllers: P, machines: [] }) === 'systems not an array');
check('two primaries: refused', layoutGate({ controllers: [{ id: 'a', role: 'primary' }, { id: 'b', role: 'primary' }], machines: [],
  systems: sys1([{ id: 'dc', type: 'collector' }]) }) === 'exactly one primary controller required');
check('no controllers: refused', layoutGate({ machines: [], systems: sys1([{ id: 'dc', type: 'collector' }]) }) === 'controllers not an array');
check('no machines array: refused', layoutGate({ controllers: P, systems: sys1([{ id: 'dc', type: 'collector' }]) }) === 'machines not an array');
check('a system with no id: refused', layoutGate({ controllers: P, machines: [], systems: [{ elements: [{ id: 'dc', type: 'collector' }] }] }) === 'system missing id');
check('a system with two collectors: refused', layoutGate({ controllers: P, machines: [],
  systems: sys1([{ id: 'a', type: 'collector' }, { id: 'b', type: 'collector' }]) }) === 'system "s1": exactly one collector required');
check('a system with none: refused', layoutGate({ controllers: P, machines: [], systems: sys1([]) }) !== null);
// Two gates on one board's channel are a problem the shop runs around (D-88), not a reason to refuse the save.
const clash = { controllers: P, machines: [], systems: sys1([{ id: 'dc', type: 'collector' },
  { id: 'g1', type: 'selector', controllerId: 'n1', servo: { channel: 0 } },
  { id: 'g2', type: 'selector', controllerId: 'n1', servo: { channel: 0 } },
  { id: 'g3', type: 'selector', controllerId: 'n1', servo: { channel: 1 } },
  { id: 'g4', type: 'selector', controllerId: 'n2', servo: { channel: 0 } }]) };
check("two gates on one board's channel 0: kept", layoutGate(clash) === null);

// S — gates that share a servo channel
const s = sharedChannelGates(clash);
check("both gates on n1's channel 0 share it", s.size === 2 && s.has('g1') && s.has('g2'));
check('...the next channel on that board does not', !s.has('g3'));
check('...the same channel on another board does not', !s.has('g4'));
check('a shop with no gates shares nothing', sharedChannelGates(good).size === 0);

let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
