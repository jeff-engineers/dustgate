// layout-save.test.js — a layout SAVE is not a reboot (topology-device.js, createTopologyDevice with `prev`).
//
// The C++ side (TopologyRuntime::adopt, exercised by firmware/test/test_layout_save.cpp) must agree with this file
// case-for-case, in the same order. The two engines can't share code, so the paired assertions ARE the anti-drift
// mechanism — same shape as manual-blower.test.js ↔ test_manual_blower.cpp.
//
// What has to hold, in both engines:
//   • a save while a tool runs leaves its blower running and its gate where it is
//   • ...and the blower still coasts down and stops when the tool stops (it used to read as started by a person, and
//     nothing pressed it off)
//   • a blower switched on by hand stays on, and stays "by hand"
//   • a gate the save changed is forgotten (seeded closed); one it only renamed is kept
//   • a new trip point takes effect at once, and a tool now under it coasts down like one switched off
//   • a FIRST layout, or one after a reset, still starts from nothing
//   • ...and a first layout settles: one gate open per system, the rest closed, blowers off
//   • sameHardware(): everything but the name, with key order and 90 vs 90.0 not counting as changes
//
// Run: `node layout-save.test.js` (also part of `npm run model:test` in tools/).

'use strict';

const TD = require('./topology-device');
const S = require('./shop');
const { clone, twoSystemShop } = require('./topology.fixtures');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });

const OFF_DELAY = TD.DEFAULT_COLLECTOR_OFF_DELAY_MS;
const on = (d, sysId, t) => TD.statusView(d, t).systems[sysId].collectorOn;
const coasting = (d, sysId, t) => TD.statusView(d, t).systems[sysId].coasting;
const manual = (d, sysId, t) => TD.statusView(d, t).systems[sysId].manual;
const element = (shop, id) => S.systemsOf(shop).flatMap((s) => s.elements).find((e) => e.id === id);
const machine = (shop, id) => shop.machines.find((m) => m.id === id);

// ── a save while a tool runs ─────────────────────────────────────────────────
{
  let d = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setMachinePower(d, 'jointer', 800, 1000);
  check('the jointer runs its blower', on(d, 'big', 1000));
  check('...through its own gate', d.actuatorStates['bv-jnt'] === 'open');

  d = TD.createTopologyDevice(clone(twoSystemShop), d, 2000);   // the same layout, saved again
  check('after a save the blower is still running', on(d, 'big', 2000));
  check('...and not coasting', !coasting(d, 'big', 2000));
  check('the gate is still open', d.actuatorStates['bv-jnt'] === 'open');
  check('the jointer still reads as running', TD.activeMachines(d).includes('jointer'));

  // The bug this exists for: the blower read as started by a person after a save, and nothing turned it off.
  TD.setMachinePower(d, 'jointer', 0, 3000);
  check('when the tool stops after the save, the blower coasts', on(d, 'big', 3000) && coasting(d, 'big', 3000));
  check('...and stops when the coast is over', !on(d, 'big', 3000 + OFF_DELAY + 1));
}

// ── a blower switched on by hand ─────────────────────────────────────────────
{
  let d = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setCollectorManual(d, 'big', true, 1000);
  d = TD.createTopologyDevice(clone(twoSystemShop), d, 2000);
  check('a hand-run blower is still running after a save', on(d, 'big', 2000));
  check('...and still by hand', manual(d, 'big', 2000));
  check('...and it does not time out', on(d, 'big', 2000 + 61000));
}

// ── a gate the save changed, and one it only renamed ─────────────────────────
{
  let d = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setMachinePower(d, 'table-saw', 800, 1000);   // the cabinet port on 'big', the overarm on the slider in 'small'
  TD.setMachinePower(d, 'table-saw', 0, 2000);     // idle HOLDS the gates where they are
  check('the cabinet gate was left open', d.actuatorStates['bv-cab'] === 'open');
  check('the slider was left at a stop', d.actuatorStates['man'] !== 'home');
  check('both blowers are off once the coast is over', !on(d, 'big', 2000 + OFF_DELAY + 1) && !on(d, 'small', 2000 + OFF_DELAY + 1));

  const edited = clone(twoSystemShop);
  element(edited, 'bv-cab').name = 'Cabinet saw valve';            // a label: the same gate
  element(edited, 'man').states[1].positionMm = 14.0;              // a stop moved: maybe not where it was
  d = TD.createTopologyDevice(edited, d, 2000 + OFF_DELAY + 2);
  check('a gate that was only renamed keeps its position', d.actuatorStates['bv-cab'] === 'open');
  check('a gate whose stops changed is forgotten (closed)', d.actuatorStates['man'] === 'home');
  check('an untouched closed gate stays closed', d.actuatorStates['bv-jnt'] === 'closed');
}

// ── a new trip point takes effect at once ────────────────────────────────────
{
  let d = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setMachinePower(d, 'jointer', 800, 1000);
  const raised = clone(twoSystemShop);
  machine(raised, 'jointer').sensor.outlet.thresholdW = 1000;     // tuned from the phone, with the jointer running
  d = TD.createTopologyDevice(raised, d, 2000);
  check('a tool now under its trip point is no longer running', !TD.activeMachines(d).includes('jointer'));
  check('...so its blower coasts', on(d, 'big', 2000) && coasting(d, 'big', 2000));
  check('...and stops when the coast is over', !on(d, 'big', 2000 + OFF_DELAY + 1));
}

// ── a FIRST layout, or one after a reset, starts from nothing ────────────────
{
  const d0 = TD.createTopologyDevice(clone(twoSystemShop));
  check('a first layout has every blower off', !on(d0, 'big', 0) && !on(d0, 'small', 0));
  check('...and every gate closed', d0.actuatorStates['bv-cab'] === 'closed' && d0.actuatorStates['bv-jnt'] === 'closed' && d0.actuatorStates['man'] === 'home');

  const running = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setMachinePower(running, 'jointer', 800, 1000);
  const fresh = TD.createTopologyDevice(clone(twoSystemShop));   // a reset hands nothing on
  check('a layout after a reset has the blower off', !on(fresh, 'big', 2000));
  check('...and the gate closed', fresh.actuatorStates['bv-jnt'] === 'closed');
  check('...and no tool running', TD.activeMachines(fresh).length === 0);
}

// ── a first layout settles (2026-10-08) ─────────────────────────────────────
{
  const d = TD.settleAtBoot(TD.createTopologyDevice(clone(twoSystemShop)));
  check('a first layout settles: the path to the first machine opens', d.actuatorStates['bv-cab'] === 'open');
  check('...every other gate closed', d.actuatorStates['bv-jnt'] === 'closed');
  check('...and the other system settles too', d.actuatorStates['man'] !== 'home');
  check('...with every blower off', !on(d, 'big', 0) && !on(d, 'small', 0));
}

// ── sameHardware ─────────────────────────────────────────────────────────────
{
  const gate = () => clone(element(twoSystemShop, 'bv-cab'));
  check('a gate is the same gate as itself', TD.sameHardware(gate(), gate()));
  const renamed = gate(); renamed.name = 'Something else';
  check('a new name is the same gate', TD.sameHardware(gate(), renamed));
  const angle = gate(); angle.states[1].offsetDeg = 85;
  check('a new closed angle is not', !TD.sameHardware(gate(), angle));
  const channel = gate(); channel.servo.channel = 1;
  check('a new channel is not', !TD.sameHardware(gate(), channel));
  const board = gate(); board.controllerId = 'dustgate-planer';
  check('a new board is not', !TD.sameHardware(gate(), board));
  const reordered = Object.fromEntries(Object.entries(gate()).reverse());
  check('key order does not count', TD.sameHardware(gate(), reordered));
  check('90 and 90.0 are the same number', TD.sameHardware(JSON.parse('{"a":90}'), JSON.parse('{"a":90.0}')));
}

let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
