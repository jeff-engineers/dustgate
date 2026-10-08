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

  // HOME FIRST (2026-10-07): setting a slider up starts from a fresh datum. Same cases as the "home first" block of
  // test_nodebus.cpp, same order.
  eq('a linear SET may ask to find home first', NL.validateFrame({ ...linear, positionMm: 10, home: true }, 'p2s'), []);
  check('a home that is not true|false is rejected', NL.validateFrame({ ...linear, positionMm: 10, home: 'yes' }, 'p2s').length > 0);
  check('home on a servo SET is rejected', NL.validateFrame({ ...good, home: true }, 'p2s').length > 0);
  const sel = star.elements.find((e) => e.id === 'sel');
  check('the builder sets it only when asked', NL.set(1, sel, 'a', 10, { home: true }).home === true && NL.set(1, sel, 'a', 10).home === undefined);
  check('absent means no', NL.validateFrame({ ...linear, positionMm: 10 }, 'p2s').length === 0 && !('home' in { ...linear, positionMm: 10 }));
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

// ── boot info: a node says why it last reset (withBootInfo) ──────────────────
// PAIR: test_nodebus.cpp's "boot info" block — same cases, same order.
{
  const w = NL.withBootInfo(NL.welcome('node-1', 'xiao_c5', '1.0.0', { servos: 2, linear: 0, ct: 1 },
                                       'dustgate-shop'), 12, 'brownout');
  eq('a WELCOME may carry boot info', NL.validateFrame(w, 's2p'), []);
  check('upS is whole seconds', w.upS === 12);
  check('rst is carried', w.rst === 'brownout');
  check('boot info does not displace the clamp', NL.clampsOn(w) === 1);

  const none = NL.welcome('node-1', 'xiao_c5', '1.0.0', { servos: 2, linear: 0 });
  check('absent means unknown — nothing is written', none.upS === undefined && none.rst === undefined);
  eq('and a WELCOME without it still validates', NL.validateFrame(none, 's2p'), []);

  check('rst is capped at MAX_RST_LEN', NL.MAX_RST_LEN === 16 &&
        NL.withBootInfo({}, 1, 'x'.repeat(40)).rst.length === 16);
  check('a negative upS is refused',
        NL.validateFrame({ ...none, upS: -1 }, 's2p').length > 0);
  check('a non-string rst is refused',
        NL.validateFrame({ ...none, rst: 7 }, 's2p').length > 0);
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
        NL.validateFrame({ t: 'CONFIG', seq: 1, sensors: [{ sensorId: 'a', kind: 'laser', channel: 0 }] },
                         'p2s').length === 1);
  check('a channel off the board is refused',
        NL.validateFrame({ t: 'CONFIG', seq: 1, sensors: [{ sensorId: 'a', kind: 'ct', channel: 99 }] },
                         'p2s').length === 1);
  check('sensors must be an array',
        NL.validateFrame({ t: 'CONFIG', seq: 1, sensors: {} }, 'p2s').length === 1);

  // ── CT TUNING (2026-09-17) — the numbers that used to force a node reflash.
  //
  // PAIRED with test_nodebus.cpp's parseConfigFrame cases: same fields, same
  // bounds, same order. The point of carrying them is that retuning a shop is a
  // primary reflash and nobody climbs to a node, so the rules have to agree on
  // both sides or a primary sends a frame its own boards refuse.
  const tuned = NL.config(9, [{ sensorId: 'planer-ct', kind: 'ct', channel: 0,
                                tripRatio: 3.5, minCounts: 12, clearRatio: 0.8 }]);
  eq('a tuned CONFIG is valid', NL.validateFrame(tuned, 'p2s'), []);
  eq('and the tuning rides the spec',
     Object.keys(tuned.sensors[0]).sort(),
     ['channel', 'clearRatio', 'kind', 'minCounts', 'sensorId', 'tripRatio']);

  // ABSENT, not null. An absent key is how a board is told to keep its own
  // value; a present null is a number that fails validation and takes the WHOLE
  // frame down, sensor list and all.
  const partial = NL.config(10, [{ sensorId: 'a', kind: 'ct', channel: 0, tripRatio: 5 }]);
  eq('tuning is omitted per field, not all or nothing',
     Object.keys(partial.sensors[0]).sort(), ['channel', 'kind', 'sensorId', 'tripRatio']);
  eq('and a partially tuned frame is valid', NL.validateFrame(partial, 'p2s'), []);

  const badTune = (k, v) => NL.validateFrame(
    { t: 'CONFIG', seq: 1, sensors: [{ sensorId: 'a', kind: 'ct', channel: 0, [k]: v }] }, 'p2s');
  // A ratio of 1 trips on the floor itself; below it, on nothing at all.
  check('tripRatio at 1 is refused', badTune('tripRatio', 1).length === 1);
  check('tripRatio at 100 is refused', badTune('tripRatio', 100).length === 1);
  eq('tripRatio between them is fine', badTune('tripRatio', 4), []);
  // 4095 is full scale on a 12-bit ADC: a guard there can never be exceeded, so
  // the board is deaf rather than merely insensitive.
  check('minCounts at 0 is refused', badTune('minCounts', 0).length === 1);
  check('minCounts at full scale is refused', badTune('minCounts', 4095).length === 1);
  // 1 is no hysteresis at all, which is the defect this was added to fix; above
  // 1 releases ABOVE the trip point, so a tool could never read as stopped.
  check('clearRatio at 1 is refused', badTune('clearRatio', 1).length === 1);
  check('clearRatio above 1 is refused', badTune('clearRatio', 1.2).length === 1);
  eq('clearRatio inside the band is fine', badTune('clearRatio', 0.75), []);
  check('a non-numeric tuning is refused', badTune('tripRatio', '4').length === 1);

  // The sharpened stance: hardware tuning may ride this frame, a WATTAGE
  // threshold still may not. Both are numbers the primary chose; only one of
  // them names a machine, and that is the line.
  check('thresholdW is still refused entry',
        NL.config(11, [{ sensorId: 'a', kind: 'ct', channel: 0, thresholdW: 900 }])
          .sensors[0].thresholdW === undefined);
}

// ── SENSE: one bit, decided on the node (RFC §5.4b) ────────────────────────
{
  // ── TELEMETRY (2026-09-17) — amps for a human, and nothing may branch on it.
  //
  // PAIRED with the SENSE cases in test_nodebus.cpp: same fields, same
  // omitted-not-zeroed rule, same order.
  {
    const t = NL.sense('planer-ct', false, 0.26, 0.21, 0.20, 0.80);
    eq('a SENSE with telemetry is valid', NL.validateFrame(t, 's2p'), []);
    eq('and carries all of it',
       Object.keys(t).sort(),
       ['amps', 'floorA', 'level', 'on', 'sensorId', 't', 'tripA']);

    // OMITTED, NOT ZEROED, and this is the case that matters: 0 A is a real
    // reading from an idle tool, where "this board has no floor" is not a
    // reading at all. Zeroing would make a faulted board and a quiet one
    // identical on screen — the same lie `reported` exists to prevent.
    const bare = NL.sense('planer-ct', false);
    eq('a node with nothing to report omits every field',
       Object.keys(bare).sort(), ['on', 'sensorId', 't']);
    eq('...and is still valid', NL.validateFrame(bare, 's2p'), []);
    const zero = NL.sense('planer-ct', false, 0, 0, 0, 0);
    eq('but a genuine zero IS carried', Object.keys(zero).sort(),
       ['amps', 'floorA', 'level', 'on', 'sensorId', 't', 'tripA']);
    eq('and remains valid', NL.validateFrame(zero, 's2p'), []);

    // A board that could not learn a floor. floorA/tripA are absent because
    // there is no floor — the fault is the reason, not an extra flag beside one.
    const faulted = NL.sense('planer-ct', false, undefined, 2.2, undefined, undefined, true);
    eq('a faulted clamp reports the fault', faulted.fault, true);
    eq('with what it reads but no baseline',
       Object.keys(faulted).sort(), ['amps', 'fault', 'on', 'sensorId', 't']);
    eq('and is valid', NL.validateFrame(faulted, 's2p'), []);
    check('a non-boolean fault is refused',
          NL.validateFrame({ t: 'SENSE', sensorId: 'a', on: false, fault: 'yes' },
                           's2p').length === 1);

    // Generous upper bound ON PURPOSE: a 30 A clamp sees 45-50 A of inrush, and
    // refusing the whole frame for reporting that honestly would discard the
    // `on` bit riding with it — the one thing that actually matters.
    eq('an inrush-sized reading is accepted',
       NL.validateFrame(NL.sense('a', true, 60, 48, 0.2, 0.8), 's2p'), []);
    check('a negative reading is refused',
          NL.validateFrame({ t: 'SENSE', sensorId: 'a', on: true, amps: -1 },
                           's2p').length === 1);
  }

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

// ── plugs: a node polls a smart plug on the primary's behalf ────────────────
// PAIR: test_nodebus.cpp's "plug" block — same cases, same order, same literals.
{
  const plug = { sensorId: 'saw', kind: 'plug', ip: '192.168.86.40', plug: 'tasmota', thresholdW: 25 };
  const ct = { sensorId: 'planer', kind: 'ct', channel: 0 };
  eq('a plug sensor validates', NL.validateFrame(NL.config(1, [plug]), 'p2s'), []);
  eq('a plug and a clamp share one CONFIG', NL.validateFrame(NL.config(1, [ct, plug]), 'p2s'), []);
  eq('a plug carries no channel on the wire', NL.config(1, [plug]).sensors[0].channel, undefined);
  eq('the bounds', [NL.MAX_PLUG_THRESHOLD_W, NL.MAX_PLUG_WATTS, NL.MAX_PLUG_IP_LEN], [10000, 20000, 15]);
  const bad = (patch) => NL.validateFrame(NL.config(1, [{ ...plug, ...patch }]), 'p2s').length === 1;
  check('a hostname is not an ip', bad({ ip: 'saw.local' }));
  check('an unknown protocol is refused', bad({ plug: 'kasa' }));
  check('a zero threshold is refused', bad({ thresholdW: 0 }));
  check('a threshold past the bound is refused', bad({ thresholdW: 10001 }));
  check('a threshold AT the bound is fine', !bad({ thresholdW: 10000 }));
  const w = NL.sense('saw', true, undefined, undefined, undefined, undefined, false, 412.5, true);
  eq('a plug reading carries watts', w.watts, 412.5);
  eq('and says it is a plug', w.plug, true);
  eq('and validates', NL.validateFrame(w, 's2p'), []);
  eq('a clamp reading carries none', 'watts' in NL.sense('planer', true, 2.1), false);
  check('watts past the bound is refused',
        NL.validateFrame({ t: 'SENSE', sensorId: 'a', on: true, watts: 20001 }, 's2p').length === 1);
  const un = NL.sense('saw', false, undefined, undefined, undefined, undefined, true, undefined, true);
  check('an unreachable plug is a fault, still marked a plug', un.fault === true && un.plug === true && !('watts' in un));
  eq('a clamp report is not marked', 'plug' in NL.sense('planer', true, 2.1), false);
  const wl = NL.welcome('n1', 'xiao_c5', 'x', { servos: 1, linear: 0, plug: 1 });
  eq('a WELCOME may say the board polls plugs', NL.validateFrame(wl, 's2p'), []);
  check('and it is read back', NL.pollsPlugs(wl));
  check('absent means no — an old board stays brain-polled',
        !NL.pollsPlugs(NL.welcome('n1', 'xiao_c5', 'x', { servos: 1, linear: 0 })));
  check('a plug cap of 2 is refused',
        NL.validateFrame(NL.welcome('n1', 'b', 'x', { servos: 1, linear: 0, plug: 2 }), 's2p').length === 1);
}

// ── OTA: a node is told to pull an image, and reports how it went ───────────
// PAIR: test_nodebus.cpp's "ota" block — same cases, same order, same literals.
{
  const md5 = '0123456789abcdef0123456789abcdef';
  const f = NL.ota(7, '/node-pwm.bin', 1302295, md5, '3b738a5 1003-1200');
  eq('a well-formed OTA validates', NL.validateFrame(f, 'p2s'), []);
  check('OTA is p2s only', NL.P2S.includes('OTA') && !NL.S2P.includes('OTA'));
  check('OTASTATE is s2p only', NL.S2P.includes('OTASTATE') && !NL.P2S.includes('OTASTATE'));
  eq('the path bound', NL.MAX_OTA_PATH, 48);
  eq('the smallest image', NL.MIN_OTA_BYTES, 102400);
  eq('the largest image is the slot', NL.MAX_OTA_BYTES, 0x1E0000);
  check('a relative path is refused',
        NL.validateFrame(NL.ota(1, 'node.bin', 1302295, md5, 'x'), 'p2s').length === 1);
  check('a path past the bound is refused',
        NL.validateFrame(NL.ota(1, '/' + 'a'.repeat(48), 1302295, md5, 'x'), 'p2s').length === 1);
  check('a path AT the bound is fine',
        NL.validateFrame(NL.ota(1, '/' + 'a'.repeat(47), 1302295, md5, 'x'), 'p2s').length === 0);
  check('an image that is not an image (a 404 page) is refused',
        NL.validateFrame(NL.ota(1, '/n.bin', 102399, md5, 'x'), 'p2s').length === 1);
  check('an image bigger than the slot is refused',
        NL.validateFrame(NL.ota(1, '/n.bin', 0x1E0001, md5, 'x'), 'p2s').length === 1);
  check('a short md5 is refused',
        NL.validateFrame(NL.ota(1, '/n.bin', 1302295, md5.slice(1), 'x'), 'p2s').length === 1);
  check('an upper-case md5 is refused (one spelling on the wire)',
        NL.validateFrame(NL.ota(1, '/n.bin', 1302295, md5.toUpperCase(), 'x'), 'p2s').length === 1);

  eq('start validates', NL.validateFrame(NL.otaState('start'), 's2p'), []);
  eq('progress carries a percentage', NL.otaState('progress', 41.6).pct, 42);
  eq('a percentage is clamped', NL.otaState('progress', 140).pct, 100);
  eq('fail carries a sentence', NL.otaState('fail', undefined, 'no room').err, 'no room');
  eq('a long reason is cut to 64', NL.otaState('fail', undefined, 'x'.repeat(100)).err.length, 64);
  check('an unknown state is refused',
        NL.validateFrame({ t: 'OTASTATE', state: 'maybe' }, 's2p').length === 1);
  eq('OTA did not bump the version', NL.NODELINK_VERSION, 1);
}

// ── node-initiated links: JOIN, REFUSE, WHERE, caps.join ────────────────────
// PAIR: test_nodebus.cpp's "join" block — same cases, same order, same literals.
{
  eq('a JOIN validates', NL.validateFrame(NL.join('dustgate-planer'), 's2p'), []);
  check('JOIN is s2p only', NL.S2P.includes('JOIN') && !NL.P2S.includes('JOIN'));
  check('a JOIN with no node id is refused', NL.validateFrame(NL.join(''), 's2p').length === 1);
  check('a JOIN from another protocol version is refused',
        NL.validateFrame({ t: 'JOIN', v: 2, nodeId: 'x' }, 's2p').length === 1);

  eq('REFUSE not-paired validates', NL.validateFrame(NL.refuse('not-paired'), 'p2s'), []);
  eq('REFUSE duplicate validates', NL.validateFrame(NL.refuse('duplicate'), 'p2s'), []);
  eq('REFUSE busy validates', NL.validateFrame(NL.refuse('busy'), 'p2s'), []);
  check('REFUSE is p2s only', NL.P2S.includes('REFUSE') && !NL.S2P.includes('REFUSE'));
  check('a reason nobody defined is refused', NL.validateFrame(NL.refuse('go-away'), 'p2s').length === 1);

  eq('a WHERE validates', NL.validateFrame(NL.where('dustgate', '192.168.86.46', 80), 'p2s'), []);
  check('WHERE is p2s only', NL.P2S.includes('WHERE') && !NL.S2P.includes('WHERE'));
  eq('the address bound', NL.MAX_WHERE_IP_LEN, 15);
  eq('the beacon port', NL.BEACON_PORT, 41234);
  check('a name where an address belongs is refused',
        NL.validateFrame(NL.where('dustgate', 'dustgate.local', 80), 'p2s').length === 1);
  check('a port of 0 is refused', NL.validateFrame(NL.where('dustgate', '192.168.86.46', 0), 'p2s').length === 1);
  check('a port past 65535 is refused', NL.validateFrame(NL.where('dustgate', '192.168.86.46', 65536), 'p2s').length === 1);
  check('a WHERE with no primary is refused', NL.validateFrame(NL.where('', '192.168.86.46', 80), 'p2s').length === 1);

  check('a board that says nothing is not dialling in', NL.dialsIn(NL.welcome('n', 'b', 'f', { servos: 2, linear: 0 })) === false);
  check('caps.join 1 means it dials in', NL.dialsIn(NL.welcome('n', 'b', 'f', { servos: 2, linear: 0, join: 1 })) === true);
  eq('caps.join 1 validates', NL.validateFrame(NL.welcome('n', 'b', 'f', { servos: 2, linear: 0, join: 1 }), 's2p'), []);
  check('caps.join 2 is refused', NL.validateFrame(NL.welcome('n', 'b', 'f', { servos: 2, linear: 0, join: 2 }), 's2p').length === 1);
  eq('JOIN did not bump the version', NL.NODELINK_VERSION, 1);
}

// ── the collector's jobs on a node: PRESS, a bin sensor, caps.rf, caps.bin ───
// PAIR: test_nodebus.cpp's "collector node" block — same cases, same order, same literals.
{
  eq('a PRESS validates', NL.validateFrame(NL.press(7, 94, 14, 270, 24), 'p2s'), []);
  check('PRESS is p2s only', NL.P2S.includes('PRESS') && !NL.S2P.includes('PRESS'));
  eq('the smallest tick', NL.MIN_RF_TICK_US, 50);
  eq('the largest tick', NL.MAX_RF_TICK_US, 1000);
  eq('the most repeats', NL.MAX_RF_REPEATS, 60);
  check('an address past 8 bits is refused', NL.validateFrame(NL.press(1, 256, 14, 270, 24), 'p2s').length === 1);
  check('data past 4 bits is refused', NL.validateFrame(NL.press(1, 94, 16, 270, 24), 'p2s').length === 1);
  check('a tick under the floor is refused', NL.validateFrame(NL.press(1, 94, 14, 49, 24), 'p2s').length === 1);
  check('a tick over the ceiling is refused', NL.validateFrame(NL.press(1, 94, 14, 1001, 24), 'p2s').length === 1);
  check('a tick AT the floor is fine', NL.validateFrame(NL.press(1, 94, 14, 50, 24), 'p2s').length === 0);
  check('zero repeats is refused', NL.validateFrame(NL.press(1, 94, 14, 270, 0), 'p2s').length === 1);
  check('more repeats than the bound is refused', NL.validateFrame(NL.press(1, 94, 14, 270, 61), 'p2s').length === 1);
  check('repeats AT the bound is fine', NL.validateFrame(NL.press(1, 94, 14, 270, 60), 'p2s').length === 0);

  eq('a bin sensor validates', NL.validateFrame(NL.config(1, [{ sensorId: 'bin:sys', kind: 'bin', invert: true }]), 'p2s'), []);
  eq('a bin sensor needs no invert', NL.validateFrame(NL.config(1, [{ sensorId: 'bin:sys', kind: 'bin' }]), 'p2s'), []);
  check('invert must be a boolean',
        NL.validateFrame({ t: 'CONFIG', seq: 1, sensors: [{ sensorId: 'b', kind: 'bin', invert: 1 }] }, 'p2s').length === 1);
  eq('a bin sensor keeps its invert on the wire', NL.config(1, [{ sensorId: 'bin:sys', kind: 'bin', invert: false }]).sensors[0].invert, false);
  eq('a bin sensor is not given a channel', 'channel' in NL.config(1, [{ sensorId: 'bin:sys', kind: 'bin' }]).sensors[0], false);

  const w0 = NL.welcome('n', 'b', 'f', { servos: 2, linear: 0 });
  check('a board that says nothing has no transmitter', NL.pressesRf(w0) === false);
  check('and no bin pad', NL.watchesBin(w0) === false);
  const w1 = NL.welcome('n', 'b', 'f', { servos: 2, linear: 0, rf: 1, bin: 1 });
  check('caps.rf 1 means a transmitter', NL.pressesRf(w1) === true);
  check('caps.bin 1 means a bin pad', NL.watchesBin(w1) === true);
  eq('both caps validate', NL.validateFrame(w1, 's2p'), []);
  check('caps.rf 2 is refused', NL.validateFrame(NL.welcome('n', 'b', 'f', { servos: 2, linear: 0, rf: 2 }), 's2p').length === 1);
  eq('PRESS did not bump the version', NL.NODELINK_VERSION, 1);
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
