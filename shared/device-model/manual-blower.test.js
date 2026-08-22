// manual-blower.test.js — running ONE system's blower by hand (topology-device.js).
//
// The C++ side (firmware/control/TopologyRuntime.h, exercised by
// test_manual_blower.cpp) must agree with this file case-for-case. The two
// engines can't share code, so the paired assertions ARE the anti-drift
// mechanism — same shape as nodelink.test.js / NodeLink.h.
//
// Run: `node manual-blower.test.js`, or `npm run blower:test`.

'use strict';

const TD = require('./topology-device');
const T = require('./topology');
const S = require('./shop');
const { clone, twoSystemShop } = require('./topology.fixtures');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, JSON.stringify(got) === JSON.stringify(want),
        `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

const dev = () => TD.createTopologyDevice(clone(twoSystemShop));
// Every read carries the SIMULATED clock. statusView expires a finished coast on
// read (tickCollector), so a helper that quietly used Date.now() would report
// every coast in these tests as already over — the model owns no clock, and a
// test that forgets that is testing the wall clock.
const on = (d, sysId, t) => TD.statusView(d, t).systems[sysId].collectorOn;
const manual = (d, sysId, t) => TD.statusView(d, t).systems[sysId].manual;
const coasting = (d, sysId, t) => TD.statusView(d, t).systems[sysId].coasting;
/** Is anything in this system open — the same test planTransition calls "not sealed". */
function openIn(d, sysId) {
  const shop = d.topology;
  const view = S.systemView(shop, S.systemsOf(shop).find((x) => x.id === sysId));
  const sels = T.selectorsOf(view);
  return sels.some((sel) => {
    const closed = T.closedState(sel);
    return !closed || d.actuatorStates[sel.id] !== closed.id;
  });
}

// ── it runs, and only the system asked for ──────────────────────────────────
{
  const d = dev();
  check('a fresh shop has both blowers idle', !on(d, 'big', 0) && !on(d, 'small', 0));
  TD.setCollectorManual(d, 'big', true, 1000);
  check('the one switched on runs', on(d, 'big', 1000));
  check('and says it is running by hand', manual(d, 'big', 1000));
  check('the OTHER system is untouched', !on(d, 'small', 1000) && !manual(d, 'small', 1000));
  // The whole reason the container exists: one blower is not the shop.
  check('...which is the point of per-system control', !on(d, 'small', 1000));
}

// ── NEVER dead-head: a manual start opens a path first ──────────────────────
{
  const d = dev();
  // createTopologyDevice closes everything, which is exactly the state a
  // freshly-adopted layout is in — and the state a blower must not start into.
  check('a fresh system is sealed', !openIn(d, 'big'));
  TD.setCollectorManual(d, 'big', true, 1000);
  check('the manual start opened a path', openIn(d, 'big'));
  check('and only then is the blower running', on(d, 'big', 1000));
  // The other system was sealed and stays that way — nothing opened for a blower
  // that is not running.
  check('a system nobody started is left sealed', !openIn(d, 'small'));
}

{
  // Already open: hold, don't re-route. Idle-hold means the shop rests where it
  // was, and a manual start that reshuffled the gates would move a valve nobody
  // asked it to move.
  const d = dev();
  TD.setMachinePower(d, 'jointer', 800, 1000);      // routes 'big' to the jointer
  const routed = { ...d.actuatorStates };
  TD.setMachinePower(d, 'jointer', 0, 2000);        // idle-hold keeps the positions
  TD.setCollectorManual(d, 'big', true, 3000);
  eq('an open system keeps the positions it had', d.actuatorStates, routed);
}

// ── it HOLDS ────────────────────────────────────────────────────────────────
{
  const d = dev();
  TD.setCollectorManual(d, 'big', true, 1000);
  // A coast-down is what happens after a CUT. Nothing was being cut.
  check('a manual run is not coasting', !coasting(d, 'big', 1000));
  // Time passing is what ends a coast; it must not end this.
  TD.tickCollector(d, 61_000);
  check('and it does not time out', on(d, 'big', 61_000), 'still on a minute later');

  // A tool starting on the same blower routes over the top; the run persists.
  TD.setMachinePower(d, 'jointer', 800, 70_000);
  check('a tool can start while it runs', on(d, 'big', 70_000));
  check('...and the hand-switch is still set', manual(d, 'big', 70_000));
  TD.setMachinePower(d, 'jointer', 0, 80_000);
  // Without the manual flag this is where the coast-down would begin.
  check('the blower stays on after that tool stops', on(d, 'big', 86_000));
  check('and it is not coasting down', !coasting(d, 'big', 86_000));
}

// ── switching it off ────────────────────────────────────────────────────────
{
  const d = dev();
  TD.setCollectorManual(d, 'big', true, 1000);
  TD.setCollectorManual(d, 'big', false, 2000);
  check('off means off', !on(d, 'big', 2000));
  check('and it stops claiming to be manual', !manual(d, 'big', 2000));
  // No coast on the way out: the coast catches dust still in the pipe after a
  // cut, and there was none.
  check('it does not coast down afterwards', !coasting(d, 'big', 2000));
  // The gates stay where the run left them — the shop rests open.
  check('the path it opened is left open', openIn(d, 'big'));
}

{
  // Switching the hand-run off while a TOOL is running must not stop the blower:
  // the tool still needs it, and this switch was never about the tool.
  const d = dev();
  TD.setCollectorManual(d, 'big', true, 1000);
  TD.setMachinePower(d, 'jointer', 800, 2000);
  TD.setCollectorManual(d, 'big', false, 3000);
  check('a tool still holds the blower on', on(d, 'big', 3000));
  check('but the hand-run is over', !manual(d, 'big', 3000));
  // ...and now the ordinary coast-down applies again.
  TD.setMachinePower(d, 'jointer', 0, 4000);
  check('so the next idle coasts as usual', coasting(d, 'big', 4000));
}

// ── nonsense in, nothing out ────────────────────────────────────────────────
{
  const d = dev();
  TD.setCollectorManual(d, 'no-such-system', true, 1000);
  check('an unknown system changes nothing', !on(d, 'big', 1000) && !on(d, 'small', 1000));
}

// ── report ──────────────────────────────────────────────────────────────────
let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
