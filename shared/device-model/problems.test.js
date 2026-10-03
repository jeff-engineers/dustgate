// problems.test.js — the Live view's "needs attention" list (topology-device.js,
// "Problems"). PAIRED with the "problems —" block in firmware/test/test_nodebus.cpp:
// same cases, same order, text asserted literally so a one-sided edit fails here.
//
// Run: `node problems.test.js`.
'use strict';

const TD = require('./topology-device');
const { clone, twoSystemShop } = require('./topology.fixtures');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const GRACE = TD.COLLECTOR_SPINUP_GRACE_MS;
const probs = (d, t) => TD.statusView(d, t).problems;
const plugged = () => {
  const shop = clone(twoSystemShop);
  const c = shop.systems.find((s) => s.id === 'big').elements.find((e) => e.type === 'collector');
  c.control = { outlet: { gen: 2, ip: '10.0.0.50' } };
  return TD.createTopologyDevice(shop);
};

{
  const d = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  check('no plug configured, running → nothing to say', probs(d, 1000 + GRACE).length === 0);
}
{
  const d = plugged();
  check('an idle shop lists none', probs(d, 0).length === 0);
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  check('a healthy blower lists none', probs(d, 1000 + GRACE).length === 0);
}
{
  const d = plugged();
  TD.setCollectorPlugFault(d, 'big', 'dead');
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  check('not accused during spin-up', probs(d, 1000).length === 0);
  const p = probs(d, 1000 + GRACE);
  check('a dead blower past the grace is one problem', p.length === 1);
  check('coded collector-no-start', p[0].code === 'collector-no-start' && p[0].severity === 'bad' && p[0].subject.id === 'big');
  check('the device words the reason',
    p[0].text === 'Commanded on but drawing nothing — check the breaker, the cord and the remote.', p[0].text);
}
{
  const d = plugged();
  TD.setCollectorPlugFault(d, 'big', 'offline');
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  const p = probs(d, 1000 + GRACE);
  check('an unreachable plug while on is blind, not accused', p.length === 1 && p[0].code === 'collector-blind');
  TD.setProblem(d, 'rf:big', { code: 'rf-gave-up', severity: 'bad', subject: { type: 'system', id: 'big' }, text: 'Pressed 3 times.' });
  TD.setProblem(d, 'rf:big', { code: 'rf-gave-up', severity: 'bad', subject: { type: 'system', id: 'big' }, text: 'Pressed 3 times.' });
  check('a raised problem is listed once however often it is raised', probs(d, 1000 + GRACE).length === 2);
  TD.clearProblem(d, 'rf:big');
  check('and clears when the cause does', probs(d, 1000 + GRACE).length === 1);
}

let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
