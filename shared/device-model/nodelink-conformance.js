// nodelink-conformance.js — end-to-end contract test for the primary↔secondary link.
//
// The unit tests either side of the wire (nodelink.test.js in JS,
// test_nodebus.cpp in C++) pin the FRAME SHAPES. This suite pins the
// CONVERSATION: handshake, accept-vs-arrive, refusals, and the fail-safe. It
// plays the primary against any target that speaks NodeLink — the Node mock now
// (tools/mock-node.js), a real ESP32 secondary later:
//
//   node nodelink-conformance.js [ws://host:port] [http://host:port]
//
// The HTTP base is only used for the mock's /sim/servos affordance (a real node
// has no such endpoint), so those checks are skipped when it isn't reachable.
//
// The load-bearing assertions here are the ones that can't be checked by
// inspecting a frame in isolation:
//   • ACK means ACCEPTED — arrival is a separate, later STATE{moving:false}
//   • a refused SET never moves anything
//   • losing the link HOLDS every gate; it does not close them

'use strict';

// WebSocket client: prefer the runtime's built-in (Node 21+), fall back to the
// `ws` package installed under tools/. The sibling suites (conformance.js,
// topology-conformance.js) need no install at all because they only use the
// built-in fetch; this one keeps that property wherever the runtime allows it,
// rather than adding a dependency at the repo root.
const WebSocket = globalThis.WebSocket || (() => {
  const path = require('path');
  return require(require.resolve('ws', {
    paths: [path.join(__dirname, '..', '..', 'tools', 'node_modules')],
  }));
})();

const NL = require('./nodelink.js');
const { twoGates } = require('./topology.fixtures.js');
const { servoCommandAngle } = require('./topology.js');

const WS_URL   = (process.argv[2] || 'ws://localhost:3001/nodelink');
const HTTP_URL = (process.argv[3] || 'http://localhost:3001').replace(/\/$/, '');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });

// ── a tiny primary ──────────────────────────────────────────────────────────
// Collects every inbound frame so assertions can look for one that has already
// arrived (avoiding races) rather than only waiting for the next.
function connect(url = WS_URL) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    const frames = [];
    let closed = false;
    // addEventListener (not ws-specific .on) so this works against both the
    // built-in WebSocket and the `ws` package — see the shim above.
    ws.addEventListener('message', (ev) => {
      try { frames.push(JSON.parse(typeof ev.data === 'string' ? ev.data : ev.data.toString())); }
      catch { /* not JSON — ignore */ }
    });
    ws.addEventListener('close', () => { closed = true; });
    ws.addEventListener('error', reject);
    ws.addEventListener('open', () => resolve({
      ws,
      frames,
      isClosed: () => closed,
      send: (f) => ws.send(JSON.stringify(f)),
      /** Wait for a frame matching `pred`, or null on timeout. */
      await: (pred, timeoutMs = 2000) => new Promise((res) => {
        const hit = frames.find(pred);
        if (hit) return res(hit);
        const t0 = Date.now();
        const iv = setInterval(() => {
          const f = frames.find(pred);
          if (f) { clearInterval(iv); res(f); }
          else if (Date.now() - t0 > timeoutMs) { clearInterval(iv); res(null); }
        }, 10);
      }),
    }));
  });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function simServos() {
  try {
    const r = await fetch(HTTP_URL + '/sim/servos');
    return r.ok ? await r.json() : null;
  } catch { return null; }
}

async function waitForNode({ timeoutMs = 15000 } = {}) {
  const t = Date.now();
  while (Date.now() - t < timeoutMs) {
    try { const c = await connect(); c.ws.close(); return true; } catch { /* not up */ }
    await sleep(300);
  }
  return false;
}

// The gate the whole suite drives: referenceAngle 10, open +0 / closed +90.
const gate = twoGates.elements.find((e) => e.id === 'gate1');
const OPEN_ANGLE   = servoCommandAngle(gate, 'open');     // 10
const CLOSED_ANGLE = servoCommandAngle(gate, 'closed');   // 100

async function run() {
  if (!(await waitForNode())) {
    console.error(`No NodeLink target at ${WS_URL}`);
    process.exit(2);
  }

  // ── 1. Handshake ────────────────────────────────────────────────────────
  let c = await connect();
  {
    c.send(NL.hello('primary', 'node-under-test'));
    const w = await c.await((f) => f.t === 'WELCOME');
    check('handshake: HELLO → WELCOME', !!w);
    check('handshake: WELCOME is a valid s2p frame',
      w && NL.validateFrame(w, 's2p').length === 0,
      w ? JSON.stringify(NL.validateFrame(w, 's2p')) : 'no WELCOME');
    check('handshake: WELCOME reports the protocol version', w?.v === NL.NODELINK_VERSION);
    check('handshake: WELCOME identifies the node', typeof w?.nodeId === 'string' && !!w.nodeId);
    check('handshake: WELCOME declares a servo budget', (w?.caps?.servos ?? 0) > 0,
      JSON.stringify(w?.caps));
  }

  // ── 2. Liveness ─────────────────────────────────────────────────────────
  {
    c.send(NL.ping());
    const p = await c.await((f) => f.t === 'PONG');
    check('liveness: PING → PONG', !!p);
  }

  // ── 3. A resolved SET moves the channel — and ACK ≠ arrival ─────────────
  {
    // Built exactly the way the primary builds it: the angle is resolved HERE,
    // so the node receives a number and never interprets a state name.
    const f = NL.set(1, gate, 'open', OPEN_ANGLE);
    check('SET: primary resolves the angle before sending', f.angle === OPEN_ANGLE);
    c.send(f);

    const ack = await c.await((x) => x.t === 'ACK' && x.seq === 1);
    check('SET: acknowledged', ack?.ok === true, JSON.stringify(ack));

    const moving = await c.await((x) => x.t === 'STATE' && x.moving === true);
    check('SET: reports STATE{moving:true} before arrival', !!moving);
    check('SET: the moving report names the selector + target state',
      moving?.selectorId === 'gate1' && moving?.stateId === 'open');

    const settled = await c.await((x) => x.t === 'STATE' && x.moving === false);
    check('SET: reports STATE{moving:false} on arrival', !!settled);

    const sim = await simServos();
    if (sim) {
      check('SET: the commanded angle actually landed on the channel',
        sim.angles[f.channel] === OPEN_ANGLE, JSON.stringify(sim.angles));
    }
  }

  // ── 4. Refusals never move anything ─────────────────────────────────────
  {
    const before = (await simServos())?.angles ?? {};

    const bad = [
      ['no angle',            { t: 'SET', seq: 10, selectorId: 'g', stateId: 'open', drive: 'servo', channel: 0 }],
      ['angle out of range',  { t: 'SET', seq: 11, selectorId: 'g', stateId: 'open', drive: 'servo', channel: 0, angle: 400 }],
      ['unknown drive',       { t: 'SET', seq: 12, selectorId: 'g', stateId: 'open', drive: 'wat',   channel: 0, angle: 10 }],
      ['no selectorId',       { t: 'SET', seq: 13, stateId: 'open', drive: 'servo', channel: 0, angle: 10 }],
      ['channel out of range',{ t: 'SET', seq: 14, selectorId: 'g', stateId: 'open', drive: 'servo', channel: 9, angle: 10 }],
      // A servo-only node has no rack; it must say so rather than pretend.
      ['linear on a servo node', { t: 'SET', seq: 15, selectorId: 'g', stateId: 'a', drive: 'linear', channel: 0, positionMm: 120 }],
    ];

    let refused = 0;
    for (const [label, frame] of bad) {
      c.send(frame);
      const ack = await c.await((x) => x.t === 'ACK' && x.seq === frame.seq, 1000);
      if (ack && ack.ok === false) refused++;
      else check(`refusal: ${label} → ACK{ok:false}`, false, JSON.stringify(ack));
    }
    check(`refusal: every malformed/unsupported SET is refused (${refused}/${bad.length})`,
      refused === bad.length);

    const after = (await simServos())?.angles ?? {};
    check('refusal: nothing moved', JSON.stringify(before) === JSON.stringify(after),
      `${JSON.stringify(before)} → ${JSON.stringify(after)}`);
  }

  // ── 5. Unknown frames are ignored, not guessed at ───────────────────────
  {
    const n = c.frames.length;
    c.send({ t: 'NOPE', seq: 99 });
    await sleep(300);
    check('unknown frame type draws no reply', c.frames.length === n,
      JSON.stringify(c.frames.slice(n)));
  }

  // ── 6. FAIL-SAFE: losing the link HOLDS every gate ──────────────────────
  // The single most important behaviour in the protocol. A node that closed its
  // gates on disconnect would slam a blast gate shut on a running tool.
  {
    // Put the gate somewhere distinctive first.
    c.send(NL.set(20, gate, 'closed', CLOSED_ANGLE));
    await c.await((x) => x.t === 'STATE' && x.moving === false && x.stateId === 'closed');
    const held = (await simServos())?.angles ?? {};
    check('fail-safe: gate parked at the closed angle', held[0] === CLOSED_ANGLE,
      JSON.stringify(held));

    c.ws.close();
    await sleep(400);

    const afterDrop = (await simServos())?.angles ?? {};
    check('fail-safe: link loss does NOT move any gate',
      JSON.stringify(held) === JSON.stringify(afterDrop),
      `${JSON.stringify(held)} → ${JSON.stringify(afterDrop)}`);

    // And the node is still usable when the primary comes back.
    c = await connect();
    c.send(NL.hello('primary', 'node-under-test'));
    const w = await c.await((f) => f.t === 'WELCOME');
    check('fail-safe: node accepts a reconnecting primary', !!w);

    c.send(NL.set(21, gate, 'open', OPEN_ANGLE));
    const settled = await c.await((x) => x.t === 'STATE' && x.moving === false && x.stateId === 'open');
    check('fail-safe: gates are commandable again after reconnect', !!settled);
  }

  // ── 6b. THE CLAIM: a node belongs to one primary ────────────────────────
  //
  // The conversation this pins is the two-shop one: a bench brain and a shop
  // brain on the same LAN. Before claims, the second one to connect simply took
  // the board's servos, with neither told — the frame shapes alone can't catch
  // that, because every individual frame is perfectly valid.
  {
    // 'primary' owns the node by now: it completed the handshake in step 1.
    const intruder = await connect();
    intruder.send(NL.hello('dustgate-bench', 'node-under-test'));
    const w = await intruder.await((f) => f.t === 'WELCOME');

    check('claim: a second primary gets a WELCOME, not silence', !!w);
    check('claim: ...that refuses it', w?.accepted === false, JSON.stringify(w));
    check('claim: ...and names the owner so the UI can explain', !!w?.claimedBy,
      JSON.stringify(w));
    check('claim: the socket stays OPEN so the refusal is readable',
      !intruder.isClosed());

    // The load-bearing one: a refused primary cannot move a valve.
    intruder.send(NL.set(90, gate, 'closed', CLOSED_ANGLE));
    const ack = await intruder.await((f) => f.t === 'ACK' && f.seq === 90);
    check('claim: a refused primary\'s SET is REFUSED', ack && ack.ok === false,
      JSON.stringify(ack));
    check('claim: ...with a reason worth reading', !!ack?.err, JSON.stringify(ack));
    const moved = await intruder.await((f) => f.t === 'STATE', 600);
    check('claim: ...and nothing moved', !moved, JSON.stringify(moved));

    // The owner is unaffected by the intrusion — no lockout, no confusion.
    c.send(NL.set(91, gate, 'open', OPEN_ANGLE));
    const ownerAck = await c.await((f) => f.t === 'ACK' && f.seq === 91);
    check('claim: the OWNER still commands the node', ownerAck?.ok === true,
      JSON.stringify(ownerAck));
    await c.await((f) => f.t === 'STATE' && f.moving === false, 2000);

    // Takeover: explicit, user-confirmed, and it actually works — otherwise the
    // only way to move a board between shops would be a factory reset.
    intruder.send(NL.hello('dustgate-bench', 'node-under-test', true));
    const w2 = await intruder.await((f) => f.t === 'WELCOME' && f.accepted !== false);
    check('takeover: a confirmed HELLO is accepted', !!w2, JSON.stringify(w2));
    check('takeover: the node now names the new owner', w2?.claimedBy === 'dustgate-bench',
      JSON.stringify(w2));

    intruder.send(NL.set(92, gate, 'closed', CLOSED_ANGLE));
    const ack2 = await intruder.await((f) => f.t === 'ACK' && f.seq === 92);
    check('takeover: the new owner can drive it', ack2?.ok === true, JSON.stringify(ack2));
    await intruder.await((f) => f.t === 'STATE' && f.moving === false, 2000);

    // ...and the displaced primary is now the one refused. Ownership MOVED
    // rather than being shared, which is the whole point.
    c.send(NL.set(93, gate, 'open', OPEN_ANGLE));
    const ack3 = await c.await((f) => f.t === 'ACK' && f.seq === 93);
    check('takeover: the displaced primary is refused', ack3?.ok === false,
      JSON.stringify(ack3));

    // Hand it back so the rest of the suite (and a re-run) starts where it began.
    c.send(NL.hello('primary', 'node-under-test', true));
    await c.await((f) => f.t === 'WELCOME' && f.accepted !== false);
    intruder.ws.close();
  }

  // ── 7. A version mismatch is refused outright ───────────────────────────
  {
    const c2 = await connect();
    c2.send({ ...NL.hello('primary', 'n'), v: NL.NODELINK_VERSION + 99 });
    // Either the socket closes, or no WELCOME is ever sent. Both are refusals;
    // what must NOT happen is a WELCOME that implies agreement.
    const w = await c2.await((f) => f.t === 'WELCOME', 800);
    check('version mismatch: no WELCOME is issued', !w, JSON.stringify(w));
    check('version mismatch: the node hangs up', c2.isClosed());
  }

  c.ws.close();

  // ── report ────────────────────────────────────────────────────────────────
  let failed = 0;
  for (const r of results) {
    if (!r.ok) failed++;
    console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
  }
  console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
  process.exit(failed ? 1 : 0);
}

run().catch((e) => { console.error(e); process.exit(2); });
