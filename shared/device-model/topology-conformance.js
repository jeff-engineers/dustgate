// topology-conformance.js — topology API contract test (over HTTP).
//
// Certifies any target that speaks the topology API — the Node mock now, a
// real firmware later. Same discipline as conformance.js (the motion/outlet suite),
// but for /api/*. The /sim/tool inject is a mock/demo affordance (real
// firmware gets tool power from Shelly plugs), so the routing-behaviour checks
// here are meaningful against the mock; a real target would drive tools physically.
//
//   node topology-conformance.js [baseUrl]

'use strict';

const baseUrl  = (process.argv[2] || 'http://localhost:3000').replace(/\/$/, '');
const fixtures = require('./topology.fixtures.js');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });

let API_KEY = '';
async function req(method, path, bodyObj) {
  const res = await fetch(baseUrl + path, {
    method,
    headers: { 'Content-Type': 'application/json', ...(API_KEY ? { 'X-Api-Key': API_KEY } : {}) },
    body: bodyObj !== undefined ? JSON.stringify(bodyObj) : undefined,
  });
  let json = null;
  try { json = await res.json(); } catch { /* non-JSON */ }
  return { status: res.status, json };
}

async function waitForServer({ timeoutMs = 15000, intervalMs = 300 } = {}) {
  const t = Date.now();
  while (Date.now() - t < timeoutMs) {
    try { const r = await fetch(baseUrl + '/api/info'); if (r.ok) return true; } catch { /* not up */ }
    await new Promise((r) => setTimeout(r, intervalMs));
  }
  return false;
}

/** Poll until `fn()` is true, or give up. For behaviour that is real elapsed
 *  time on hardware and can't be faked over HTTP. */
async function waitFor(fn, { timeoutMs = 10000, intervalMs = 250 } = {}) {
  const t = Date.now();
  while (Date.now() - t < timeoutMs) {
    if (await fn()) return true;
    await new Promise((r) => setTimeout(r, intervalMs));
  }
  return false;
}

async function run() {
  API_KEY = (await (await fetch(baseUrl + '/api/info')).json()).apiKey || '';

  // 1. Validation is enforced at the API boundary.
  {
    const bad = { schemaVersion: 1, controllers: [], elements: [], ducts: [] }; // no collector, no primary
    const r = await req('PUT', '/api/topology', bad);
    check('invalid topology → 400 + errors', r.status === 400 && Array.isArray(r.json?.errors));
  }

  // 1b. Two servo gates on one board can't share a PWM channel — they'd move together.
  {
    const dup = fixtures.clone(fixtures.twoGates);
    dup.elements.find((e) => e.id === 'gate2').servo.channel = 0;   // gate1 already holds 0
    const r = await req('PUT', '/api/topology', dup);
    check('duplicate servo channel on one host → 400', r.status === 400,
      `status=${r.status}`);
  }

  // 2. PUT/GET roundtrip + initial status.
  {
    const r = await req('PUT', '/api/topology', fixtures.twoGates);
    check('PUT twoGates → ok', r.status === 200 && r.json?.ok === true, `status=${r.status}`);
    const g = await req('GET', '/api/topology');
    check('GET topology roundtrip', g.json?.name === 'twoGates' && Array.isArray(g.json?.elements));
    const s = await req('GET', '/api/status');
    check('initial status all closed, collector off',
      s.json?.actuators?.gate1 === 'closed' && s.json?.actuators?.gate2 === 'closed' && s.json?.collectorOn === false);
  }

  // 2b. Setup-only servo jog. A build without servo support answers 501 to all of
  // these, which is a valid target too — assert the contract, not the hardware.
  {
    const ok = await req('POST', '/api/servo/jog', { channel: 0, angle: 45 });
    const noServos = ok.status === 501;
    check('servo jog accepted (or 501 without servo support)',
      ok.status === 200 || noServos, `status=${ok.status}`);

    const badCh = await req('POST', '/api/servo/jog', { channel: 9, angle: 45 });
    check('servo jog bad channel → 400', noServos ? badCh.status === 501 : badCh.status === 400,
      `status=${badCh.status}`);

    const badAngle = await req('POST', '/api/servo/jog', { channel: 0, angle: 400 });
    check('servo jog bad angle → 400', noServos ? badAngle.status === 501 : badAngle.status === 400,
      `status=${badAngle.status}`);

    const det = await req('POST', '/api/servo/jog', { channel: 0, detach: true });
    check('servo detach accepted (or 501)', det.status === 200 || det.status === 501,
      `status=${det.status}`);
  }

  // 3. Tool power drives routing, and ONE MACHINE PER SYSTEM gets the air.
  {
    let r = await req('POST', '/api/sim/tool', { toolId: 'toolX', watts: 10 });
    check('X on → gate1 open, collector on', r.json?.actuators?.gate1 === 'open' && r.json?.collectorOn === true);
    // These gates contest no selector, so both used to open — co-open, half the
    // velocity at each. Y is newer, so Y takes the air and X's gate shuts even
    // though nothing was competing for it. ↔ topology.test.js and
    // test_topology_controller.cpp assert the same switchover.
    r = await req('POST', '/api/sim/tool', { toolId: 'toolY', watts: 10 });
    check('X+Y on → only the NEWER tool gets a gate',
      r.json?.actuators?.gate1 === 'closed' && r.json?.actuators?.gate2 === 'open');
    check('X+Y on → X is drawing but not reachable',
      r.json?.tools?.toolX?.active === true && r.json?.reachable?.toolX === false);
    r = await req('POST', '/api/sim/tool', { toolId: 'toolX', watts: 0 });
    check('X off (Y on) → gate1 closed, gate2 open',
      r.json?.actuators?.gate1 === 'closed' && r.json?.actuators?.gate2 === 'open' && r.json?.collectorOn === true);
    r = await req('POST', '/api/sim/tool', { toolId: 'toolY', watts: 0 });
    // Idle HOLDS the gates, and the blower COASTS rather than cutting — a bandsaw
    // spinning down still throws dust. Polled rather than clock-injected because
    // this suite also certifies real firmware, where the only clock is the wall.
    check('idle → gate2 HELD open (idle-hold), collector still coasting',
      r.json?.actuators?.gate2 === 'open' &&
      r.json?.collectorOn === true && r.json?.collectorCoasting === true);

    const off = await waitFor(async () => {
      const s = await req('GET', '/api/status');
      return s.json?.collectorOn === false;
    }, { timeoutMs: 12000 });
    check('collector switches off once the coast-down expires', off);
  }

  // 3b. Running a blower BY HAND — the Live view's collector card.
  //
  // Certified here rather than only in the unit tests because the interesting
  // half is the HTTP contract and the fact that it STICKS: on real firmware the
  // main loop re-asserts every collector plug from the routing runtime on every
  // pass, so a switch made anywhere else is undone microseconds later and would
  // pass a unit test while doing nothing on a board.
  {
    await req('PUT', '/api/topology', fixtures.twoGates);
    let r = await req('POST', '/api/collector', { on: true });
    check('collector switch accepted', r.status === 200, `status=${r.status}`);

    // Polled: the device answers the POST before its loop has routed, and the
    // path has to be opened before the blower may start.
    const started = await waitFor(async () => {
      const s = await req('GET', '/api/status');
      return s.json?.collectorOn === true;
    });
    check('a hand-started blower runs', started);

    const st = await req('GET', '/api/status');
    const sysId = Object.keys(st.json?.systems || {})[0];
    check('and says it is running by hand', st.json?.systems?.[sysId]?.manual === true,
      JSON.stringify(st.json?.systems));
    // The rule this project has a hard line about: something must be open.
    check('...against an open path, never a sealed shop',
      Object.values(st.json?.actuators || {}).some((v) => v !== null && v !== 'closed'),
      JSON.stringify(st.json?.actuators));
    check('it is not coasting — nothing was being cut', st.json?.collectorCoasting !== true);

    // It HOLDS. A coast-down would have expired long before this.
    await new Promise((res) => setTimeout(res, 7000));
    const held = await req('GET', '/api/status');
    check('and it holds rather than timing out', held.json?.collectorOn === true);

    r = await req('POST', '/api/collector', { on: false });
    const stopped = await waitFor(async () => {
      const s = await req('GET', '/api/status');
      return s.json?.collectorOn === false;
    });
    check('switching it off stops it', stopped);

    check('an unknown system is refused',
      (await req('POST', '/api/collector', { systemId: 'no-such-system', on: true })).status === 404);
    check("a body with no 'on' is refused",
      (await req('POST', '/api/collector', {})).status === 400);
  }

  // 4. Most-recent-wins on a single shared selector.
  {
    await req('PUT', '/api/topology', fixtures.star);
    let r = await req('POST', '/api/sim/tool', { toolId: 'toolA', watts: 10 });
    check('star: A on → selector s1', r.json?.actuators?.sel === 's1');
    r = await req('POST', '/api/sim/tool', { toolId: 'toolB', watts: 10 });
    check('star: B on (newer) wins → selector s2', r.json?.actuators?.sel === 's2' && r.json?.reachable?.toolB === true);
    r = await req('POST', '/api/sim/tool', { toolId: 'toolB', watts: 0 });
    check('star: B off → selector back to s1 (A alone)', r.json?.actuators?.sel === 's1');
  }
}

(async () => {
  console.log(`\nDustGate topology conformance → ${baseUrl}\n`);
  if (!(await waitForServer())) { console.error(`✗ target unreachable at ${baseUrl}/api/info\n`); process.exit(1); }
  try { await run(); } catch (e) { check(`runner crashed: ${e.message}`, false); }
  let passed = 0;
  for (const r of results) {
    console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
    if (r.ok) passed++;
  }
  const failed = results.length - passed;
  console.log(`\n${passed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}\n`);
  process.exit(failed ? 1 : 0);
})();
