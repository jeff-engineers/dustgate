// topology-conformance.js — v2 topology API contract test (over HTTP).
//
// Certifies any target that speaks the v2 topology API — the Node mock now, a
// real v2 firmware later. Same discipline as conformance.js (the flat/v1 suite),
// but for /api/v2/*. The /sim/tool inject is a mock/demo affordance (real
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
    const r = await req('PUT', '/api/v2/topology', bad);
    check('v2: invalid topology → 400 + errors', r.status === 400 && Array.isArray(r.json?.errors));
  }

  // 1b. Two servo gates on one board can't share a PWM channel — they'd move together.
  {
    const dup = fixtures.clone(fixtures.twoGates);
    dup.elements.find((e) => e.id === 'gate2').servo.channel = 0;   // gate1 already holds 0
    const r = await req('PUT', '/api/v2/topology', dup);
    check('v2: duplicate servo channel on one host → 400', r.status === 400,
      `status=${r.status}`);
  }

  // 2. PUT/GET roundtrip + initial status.
  {
    const r = await req('PUT', '/api/v2/topology', fixtures.twoGates);
    check('v2: PUT twoGates → ok', r.status === 200 && r.json?.ok === true, `status=${r.status}`);
    const g = await req('GET', '/api/v2/topology');
    check('v2: GET topology roundtrip', g.json?.name === 'twoGates' && Array.isArray(g.json?.elements));
    const s = await req('GET', '/api/v2/status');
    check('v2: initial status all closed, collector off',
      s.json?.actuators?.gate1 === 'closed' && s.json?.actuators?.gate2 === 'closed' && s.json?.collectorOn === false);
  }

  // 2b. Setup-only servo jog. A build without servo support answers 501 to all of
  // these, which is a valid target too — assert the contract, not the hardware.
  {
    const ok = await req('POST', '/api/v2/servo/jog', { channel: 0, angle: 45 });
    const noServos = ok.status === 501;
    check('v2: servo jog accepted (or 501 without servo support)',
      ok.status === 200 || noServos, `status=${ok.status}`);

    const badCh = await req('POST', '/api/v2/servo/jog', { channel: 9, angle: 45 });
    check('v2: servo jog bad channel → 400', noServos ? badCh.status === 501 : badCh.status === 400,
      `status=${badCh.status}`);

    const badAngle = await req('POST', '/api/v2/servo/jog', { channel: 0, angle: 400 });
    check('v2: servo jog bad angle → 400', noServos ? badAngle.status === 501 : badAngle.status === 400,
      `status=${badAngle.status}`);

    const det = await req('POST', '/api/v2/servo/jog', { channel: 0, detach: true });
    check('v2: servo detach accepted (or 501)', det.status === 200 || det.status === 501,
      `status=${det.status}`);
  }

  // 3. Tool power drives routing (independent gates run concurrently).
  {
    let r = await req('POST', '/api/v2/sim/tool', { toolId: 'toolX', watts: 10 });
    check('v2: X on → gate1 open, collector on', r.json?.actuators?.gate1 === 'open' && r.json?.collectorOn === true);
    r = await req('POST', '/api/v2/sim/tool', { toolId: 'toolY', watts: 10 });
    check('v2: X+Y on → both gates open', r.json?.actuators?.gate1 === 'open' && r.json?.actuators?.gate2 === 'open');
    r = await req('POST', '/api/v2/sim/tool', { toolId: 'toolX', watts: 0 });
    check('v2: X off (Y on) → gate1 closes, gate2 open',
      r.json?.actuators?.gate1 === 'closed' && r.json?.actuators?.gate2 === 'open' && r.json?.collectorOn === true);
    r = await req('POST', '/api/v2/sim/tool', { toolId: 'toolY', watts: 0 });
    // Idle HOLDS the gates, and the blower COASTS rather than cutting — a bandsaw
    // spinning down still throws dust. Polled rather than clock-injected because
    // this suite also certifies real firmware, where the only clock is the wall.
    check('v2: idle → gate2 HELD open (idle-hold), collector still coasting',
      r.json?.actuators?.gate2 === 'open' &&
      r.json?.collectorOn === true && r.json?.collectorCoasting === true);

    const off = await waitFor(async () => {
      const s = await req('GET', '/api/v2/status');
      return s.json?.collectorOn === false;
    }, { timeoutMs: 12000 });
    check('v2: collector switches off once the coast-down expires', off);
  }

  // 4. Most-recent-wins on a single shared selector.
  {
    await req('PUT', '/api/v2/topology', fixtures.star);
    let r = await req('POST', '/api/v2/sim/tool', { toolId: 'toolA', watts: 10 });
    check('v2 star: A on → selector s1', r.json?.actuators?.sel === 's1');
    r = await req('POST', '/api/v2/sim/tool', { toolId: 'toolB', watts: 10 });
    check('v2 star: B on (newer) wins → selector s2', r.json?.actuators?.sel === 's2' && r.json?.reachable?.toolB === true);
    r = await req('POST', '/api/v2/sim/tool', { toolId: 'toolB', watts: 0 });
    check('v2 star: B off → selector back to s1 (A alone)', r.json?.actuators?.sel === 's1');
  }
}

(async () => {
  console.log(`\nDustGate v2 topology conformance → ${baseUrl}\n`);
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
