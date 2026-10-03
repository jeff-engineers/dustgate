// plug-owner.test.js — which board polls a machine's plug (shop.js plugOwners).
// PAIRED with firmware/test/test_plugowner.cpp: same fixture, same cases, same order.
'use strict';
const fs = require('fs');
const path = require('path');
const S = require('./shop');

const shop = JSON.parse(fs.readFileSync(path.join(__dirname, '..', '..', 'firmware', 'test', 'fixtures', 'plugOwners.json'), 'utf8'));
const owners = S.plugOwners(shop);

const results = [];
const eq = (name, got, want) => results.push({ name, ok: got === want, detail: `got ${JSON.stringify(got)} want ${JSON.stringify(want)}` });

eq('a tool behind a node\'s gate is polled by that node', owners.get('mA'), 'nodeA');
eq('every tool behind a manifold node is polled by it (1)', owners.get('mB1'), 'nodeB');
eq('every tool behind a manifold node is polled by it (2)', owners.get('mB2'), 'nodeB');
eq('a tool behind the primary\'s own gate stays with the brain', owners.get('mP'), '');
eq('a tool plumbed straight to a junction has no board of its own', owners.get('mD'), '');
eq('a two-port machine takes the first port that has a node', owners.get('mTwo'), 'nodeA');
eq('every machine has an answer', owners.size, 6);

let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok ? '' : '  — ' + r.detail}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
