// problems.test.js — the Live view's "needs attention" list (topology-device.js,
// "Problems"). PAIRED with the "problems —" block in firmware/test/test_nodebus.cpp:
// same cases, same order, text asserted literally so a one-sided edit fails here.
//
// Run: `node problems.test.js`.
'use strict';

const TD = require('./topology-device');
const { clone, twoGates, twoSystemShop } = require('./topology.fixtures');

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
{
  // A blower DustGate cannot switch is run by hand: watched and not drawing, the person is ASKED, never accused.
  // The model simulates a plug only for a collector it can switch, so the reading is handed to problemsView directly.
  const d = TD.createTopologyDevice(clone(twoSystemShop));
  d.collectors.big.on = true;
  const watching = (reachable) => ({ big: { plug: { watts: 0, reachable, onForMs: GRACE } } });
  const p = TD.problemsView(d, watching(true));
  check('a hand-run blower that is not drawing is asked for, not accused', p.length === 1 && p[0].code === 'collector-needs-start' && p[0].severity === 'warn' && p[0].subject.id === 'big');
  check('the device words the request',
    p[0].text === "A tool is running and the dust collector isn't — please turn it on.", p[0].text);
  check('a hand-run blower whose plug is silent says nothing (the plug has its own problem)', TD.problemsView(d, watching(false)).length === 0);
}
{
  // A full bin is a fact the beam reports: red, worded by the device, and only for a collector something watches.
  const shop = clone(twoSystemShop);
  shop.systems.find((s) => s.id === 'big').elements.find((e) => e.type === 'collector').bin = { sensor: { kind: 'threshold' } };
  const d = TD.createTopologyDevice(shop);
  TD.setBinFull(d, 'big', true);
  const p = probs(d, 0);
  check('a full bin is one problem', p.length === 1 && p[0].code === 'bin-full' && p[0].severity === 'bad' && p[0].subject.id === 'big');
  check('the device words the bin', p[0].text === 'The dust bin is full — empty it before the next cut.', p[0].text);
  TD.setBinFull(d, 'big', false);
  check('and clears when it is emptied', probs(d, 0).length === 0);
}
{
  // Two gates on one board port (D-88): the layout is kept, neither gate is moved, and each is a problem the device words.
  const shop = clone(twoGates);
  shop.systems[0].elements.find((e) => e.id === 'gate2').servo.channel = 0;
  const d = TD.settleAtBoot(TD.createTopologyDevice(shop));
  const p = probs(d, 0).filter((x) => x.code === 'gate-shares-channel');
  check('two gates on one port are a problem each',
    p.length === 2 && p[0].subject.id === 'gate1' && p[1].subject.id === 'gate2' && p[0].severity === 'bad' && p[0].subject.type === 'selector');
  check('the device words the fix',
    p[0].text === "Wired to the same board port as another gate, so DustGate won't move either — give each gate its own port on the layout.", p[0].text);
  TD.setMachinePower(d, 'toolX', 1500, 1000);
  check('and the gate its tool needs is not moved', d.actuatorStates.gate1 === 'closed', d.actuatorStates.gate1);
}

let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
