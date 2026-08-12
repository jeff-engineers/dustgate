// nodelink.test.js — pure unit tests for the primary↔secondary frame contract.
//
// The C++ side (firmware/control/NodeLink.h, exercised by
// test_nodebus.cpp) must agree with this file value-for-value. Where a test here
// asserts a specific number, the same assertion exists on the firmware side —
// that pairing is the whole anti-drift mechanism, since the firmware can't
// import the JS.
//
// Run: `node nodelink.test.js` (exit 0 = all pass), or `npm run nodelink:test`.

'use strict';

const NL = require('./nodelink');
const { twoGates, star } = require('./topology.fixtures');
const { servoCommandAngle } = require('./topology');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, JSON.stringify(got) === JSON.stringify(want),
        `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

// ── SET frames carry a RESOLVED realization, never a state to interpret ─────
{
  const gate = twoGates.elements.find((e) => e.id === 'gate1');   // ref 10, open+0 / closed+90
  const open = NL.set(7, gate, 'open', servoCommandAngle(gate, 'open'));

  eq('SET.t', open.t, 'SET');
  eq('SET.seq echoes', open.seq, 7);
  eq('SET.selectorId', open.selectorId, 'gate1');
  eq('SET.stateId', open.stateId, 'open');
  eq('SET.drive for a servo gate', open.drive, 'servo');
  eq('SET.channel from servo.channel', open.channel, 0);
  // Matches test_nodebus.cpp "SET resolves open → angle 10".
  eq('SET.angle resolves to 10', open.angle, 10);
  eq('SET.holdAtRest defaults from the selector', open.holdAtRest, false);

  const closed = NL.set(8, gate, 'closed', servoCommandAngle(gate, 'closed'));
  // Matches test_nodebus.cpp "SET resolves closed → angle 100".
  eq('SET.angle resolves to 100', closed.angle, 100);

  check('SET for a servo carries no positionMm', closed.positionMm === undefined);
}

// ── linear selectors use positionMm on the same frame ───────────────────────
{
  const sel = star.elements.find((e) => e.id === 'sel');
  const st = sel.states.find((s) => !s.isClosed);
  const f = NL.set(1, sel, st.id, st.positionMm);
  eq('SET.drive for a linear selector', f.drive, 'linear');
  eq('SET.positionMm carried', f.positionMm, st.positionMm);
  check('SET for a linear carries no angle', f.angle === undefined);
}

// ── validateFrame: a secondary must never act on a malformed SET ────────────
{
  const gate = twoGates.elements.find((e) => e.id === 'gate1');
  const good = NL.set(1, gate, 'open', 10);
  eq('valid SET passes', NL.validateFrame(good, 'p2s'), []);

  const noAngle = { ...good }; delete noAngle.angle;
  check('SET with no angle is rejected', NL.validateFrame(noAngle, 'p2s').length > 0);

  check('SET with an out-of-range angle is rejected',
        NL.validateFrame({ ...good, angle: 400 }, 'p2s').length > 0);
  check('SET with an unknown drive is rejected',
        NL.validateFrame({ ...good, drive: 'wat' }, 'p2s').length > 0);
  check('SET with no selectorId is rejected',
        NL.validateFrame({ ...good, selectorId: '' }, 'p2s').length > 0);
  check('SET with an out-of-range channel is rejected',
        NL.validateFrame({ ...good, channel: 99 }, 'p2s').length > 0);

  const linear = { t: 'SET', seq: 1, selectorId: 's', stateId: 'a', drive: 'linear', channel: 0 };
  check('linear SET with no positionMm is rejected',
        NL.validateFrame(linear, 'p2s').length > 0);
  eq('linear SET with positionMm passes',
     NL.validateFrame({ ...linear, positionMm: 120.5 }, 'p2s'), []);
}

// ── direction is enforced: a secondary can't send a SET ─────────────────────
{
  const gate = twoGates.elements.find((e) => e.id === 'gate1');
  check('SET is rejected in the s2p direction',
        NL.validateFrame(NL.set(1, gate, 'open', 10), 's2p').length > 0);
  check('WELCOME is rejected in the p2s direction',
        NL.validateFrame(NL.welcome('n1', 'devkitc', '1.0.0', { servos: 4, linear: 1 }), 'p2s').length > 0);
  check('unknown frame type is rejected', NL.validateFrame({ t: 'NOPE' }, 'p2s').length > 0);
  check('non-object is rejected', NL.validateFrame(null, 'p2s').length > 0);
}

// ── handshake frames ────────────────────────────────────────────────────────
{
  eq('HELLO is valid', NL.validateFrame(NL.hello('primary', 'node2'), 'p2s'), []);
  eq('WELCOME is valid',
     NL.validateFrame(NL.welcome('node2', 'devkitc', '1.0.0', { servos: 4, linear: 1 }), 's2p'), []);
  eq('ACK is valid', NL.validateFrame(NL.ack(3, true), 's2p'), []);
  eq('ACK with an error is valid', NL.validateFrame(NL.ack(3, false, 'nope'), 's2p'), []);
  eq('STATE is valid', NL.validateFrame(NL.state('gate1', 'open', false), 's2p'), []);
  eq('PING is valid', NL.validateFrame(NL.ping(), 'p2s'), []);
  eq('PONG is valid', NL.validateFrame(NL.pong(), 's2p'), []);

  // A version mismatch must be caught, not half-understood.
  check('HELLO with a wrong version is rejected',
        NL.validateFrame({ ...NL.hello('p', 'n'), v: 99 }, 'p2s').length > 0);
  check('WELCOME with a wrong version is rejected',
        NL.validateFrame({ ...NL.welcome('n', 'b', 'f', { servos: 0, linear: 0 }), v: 99 }, 's2p').length > 0);
  check('WELCOME with no caps is rejected',
        NL.validateFrame({ t: 'WELCOME', v: NL.NODELINK_VERSION, nodeId: 'n', board: 'b' }, 's2p').length > 0);
}

// ── timing constants match the firmware (control/NodeLink.h) ────────────────
{
  eq('PING_INTERVAL_MS', NL.PING_INTERVAL_MS, 2000);
  eq('PONG_TIMEOUT_MS', NL.PONG_TIMEOUT_MS, 6000);
  eq('protocol version', NL.NODELINK_VERSION, 1);
}

// ── report ──────────────────────────────────────────────────────────────────
let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
