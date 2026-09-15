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

// ── the claim: a node belongs to ONE primary ────────────────────────────────
//
// Before this, a node drove whatever primary connected most recently, so a
// bench brain and a shop brain could both command the same servos with neither
// told. Same silent-theft shape as an unclaimed smart plug (RFC §8), worse
// consequences: a gate that contradicts the routing of both shops.
{
  const h = NL.hello('dustgate-shop', 'node-1');
  eq('HELLO carries the claim', h.primaryId, 'dustgate-shop');
  check('and no takeover by default', h.takeover === undefined);
  eq('a plain HELLO validates', NL.validateFrame(h, 'p2s'), []);

  const t = NL.hello('dustgate-bench', 'node-1', true);
  check('takeover is explicit when asked for', t.takeover === true);
  eq('and still validates', NL.validateFrame(t, 'p2s'), []);
  eq('a non-boolean takeover is refused',
     NL.validateFrame({ ...h, takeover: 'yes' }, 'p2s').length, 1);
}
{
  const caps = { servos: 4, linear: 0 };
  const accepted = NL.welcome('node-1', 'qtpy_s3', '1.0.0', caps, 'dustgate-shop');
  check('an accepted WELCOME says who owns the node', accepted.claimedBy === 'dustgate-shop');
  check('and carries no refusal', accepted.accepted === undefined);
  check('welcomeAccepted reads it as yes', NL.welcomeAccepted(accepted));
  eq('valid', NL.validateFrame(accepted, 's2p'), []);

  const refused = NL.welcome('node-1', 'qtpy_s3', '1.0.0', caps, 'dustgate-shop', false);
  check('a refusal is explicit', refused.accepted === false);
  check('welcomeAccepted reads it as no', !NL.welcomeAccepted(refused));
  eq('valid', NL.validateFrame(refused, 's2p'), []);

  // A refusal that doesn't name the owner is unactionable: the UI can only
  // offer a takeover if it can say what that takeover would break.
  const anon = { ...refused }; delete anon.claimedBy;
  check('a refusal MUST name the owner', NL.validateFrame(anon, 's2p').length === 1,
        JSON.stringify(NL.validateFrame(anon, 's2p')));

  // The safe reading is the default: a node built before claims answers with
  // neither field, and its silence must mean "yes", not "maybe".
  const legacy = { t: 'WELCOME', v: NL.NODELINK_VERSION, nodeId: 'n', board: 'b', fw: '1', caps };
  check('a legacy WELCOME is accepted', NL.welcomeAccepted(legacy));
  eq('and still validates', NL.validateFrame(legacy, 's2p'), []);
}

// ── caps.ct: a clamp is DECLARED by its board, never discovered ────────────
{
  const w = NL.welcome('node-1', 'xiao_c5', '1.0.0', { servos: 4, linear: 0, ct: 1 });
  eq('a WELCOME may declare a clamp', NL.validateFrame(w, 's2p'), []);
  eq('and clampsOn reads it', NL.clampsOn(w), 1);

  // Absent means NONE, which is what every board flashed before this reports by
  // saying nothing. A tray with no clamp rows is then the correct empty state.
  const legacy = NL.welcome('node-1', 'xiao_c5', '1.0.0', { servos: 4, linear: 0 });
  eq('a board that says nothing has none', NL.clampsOn(legacy), 0);
  eq('and still validates', NL.validateFrame(legacy, 's2p'), []);
  eq('clampsOn survives a missing WELCOME', NL.clampsOn(null), 0);

  // Bounded by the same cap CONFIG is: a board cannot claim more clamps than it
  // could ever be configured for.
  check('more clamps than MAX_SENSORS_PER_NODE is refused',
        NL.validateFrame({ ...w, caps: { servos: 0, linear: 0, ct: NL.MAX_SENSORS_PER_NODE + 1 } },
                         's2p').length === 1);
  check('a non-numeric clamp count is refused',
        NL.validateFrame({ ...w, caps: { servos: 0, linear: 0, ct: 'yes' } }, 's2p').length === 1);
}

// ── CONFIG: what a node is WIRED TO, and nothing it could interpret ────────
{
  const one = [{ sensorId: 'planer-ct', kind: 'ct', channel: 0 }];
  const f = NL.config(7, one);
  eq('CONFIG is p2s', NL.validateFrame(f, 'p2s'), []);
  check('and only p2s', NL.validateFrame(f, 's2p').length === 1);
  eq('it echoes the seq', f.seq, 7);

  // The frame must carry NOTHING whose meaning the primary could change under a
  // node that was not reflashed — no states, no thresholds, no vocabulary. This
  // asserts the shape exactly, so a field added without thinking fails here.
  eq('the sensor spec is exactly id+kind+channel',
     Object.keys(f.sensors[0]).sort(), ['channel', 'kind', 'sensorId']);
  eq('and the frame itself carries nothing else',
     Object.keys(f).sort(), ['sensors', 'seq', 't']);

  // Extra properties on the caller's object are DROPPED rather than forwarded:
  // the builder is the one place that decides what goes on the wire.
  const smuggled = NL.config(1, [{ sensorId: 'a', kind: 'ct', channel: 0, thresholdW: 5 }]);
  check('a threshold cannot be smuggled through the builder',
        smuggled.sensors[0].thresholdW === undefined);

  // An empty list is VALID and means "report nothing" — the same state as a
  // node that has never been configured, so there is no third case.
  eq('an empty sensor list is valid', NL.validateFrame(NL.config(2, []), 'p2s'), []);
  eq('and is genuinely empty', NL.config(2, []).sensors, []);

  // Two entries under one id would make SENSE ambiguous in the only direction
  // that matters: which tool just started.
  const dup = NL.config(3, [{ sensorId: 'a', kind: 'ct', channel: 0 },
                            { sensorId: 'a', kind: 'ct', channel: 1 }]);
  check('a duplicate sensorId is refused', NL.validateFrame(dup, 'p2s').length === 1,
        JSON.stringify(NL.validateFrame(dup, 'p2s')));

  check('an unknown sensor kind is refused',
        NL.validateFrame({ t: 'CONFIG', seq: 1, sensors: [{ sensorId: 'a', kind: 'bin', channel: 0 }] },
                         'p2s').length === 1);
  check('a channel off the board is refused',
        NL.validateFrame({ t: 'CONFIG', seq: 1, sensors: [{ sensorId: 'a', kind: 'ct', channel: 99 }] },
                         'p2s').length === 1);
  check('sensors must be an array',
        NL.validateFrame({ t: 'CONFIG', seq: 1, sensors: {} }, 'p2s').length === 1);
}

// ── SENSE: one bit, decided on the node (RFC §5.4b) ────────────────────────
{
  const on = NL.sense('planer-ct', true, 2.8);
  eq('SENSE is s2p', NL.validateFrame(on, 's2p'), []);
  check('and only s2p', NL.validateFrame(on, 'p2s').length === 1);
  check('it carries the bit', on.on === true);
  eq('and echoes the id the primary chose', on.sensorId, 'planer-ct');

  // level is DIAGNOSTIC: a multiple of the trip point, not amps or watts, and
  // nothing may branch on it. A node with no trip point omits it rather than
  // sending a zero that reads like a measurement.
  const bare = NL.sense('planer-ct', false);
  check('level is optional', bare.level === undefined);
  eq('and the bare frame validates', NL.validateFrame(bare, 's2p'), []);
  eq('an off frame is still a report, not silence', bare.on, false);

  check('a non-boolean bit is refused',
        NL.validateFrame({ t: 'SENSE', sensorId: 'a', on: 1 }, 's2p').length === 1);
  check('a negative level is refused',
        NL.validateFrame({ t: 'SENSE', sensorId: 'a', on: true, level: -1 }, 's2p').length === 1);
}

// ── adding frames did NOT bump the version, on purpose ─────────────────────
{
  // Both ends ignore a frame type they don't know, so all four old/new
  // combinations degrade to something safe. A bump would force a flash of every
  // board in the shop to buy nothing — the exact cost this protocol avoids.
  eq('protocol version is unchanged by CONFIG/SENSE', NL.NODELINK_VERSION, 1);
  check('CONFIG is p2s only', NL.P2S.includes('CONFIG') && !NL.S2P.includes('CONFIG'));
  check('SENSE is s2p only', NL.S2P.includes('SENSE') && !NL.P2S.includes('SENSE'));
}

// ── timing constants match the firmware (control/NodeLink.h) ────────────────
{
  eq('PING_INTERVAL_MS', NL.PING_INTERVAL_MS, 2000);
  eq('PONG_TIMEOUT_MS', NL.PONG_TIMEOUT_MS, 6000);
  eq('SENSE_REPEAT_MS', NL.SENSE_REPEAT_MS, 5000);
  eq('SENSE_STALE_MS', NL.SENSE_STALE_MS, 15000);
  // Same 3x ratio as PING/PONG, and for the same reason: two reports may go
  // missing before anything is declared.
  eq('stale is 3x the repeat', NL.SENSE_STALE_MS / NL.SENSE_REPEAT_MS, 3);
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
