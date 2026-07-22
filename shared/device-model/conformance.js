#!/usr/bin/env node
// conformance.js — the executable contract for the DustGate device API.
//
// Runs a fixed set of behavioural scenarios over HTTP against ANY target that
// claims to be a DustGate device: the Node mock (tools/mock-api.js) in CI, or a
// real ESP32 on demand. The firmware (C++) can't import the canonical model, so
// THIS is how firmware drift is caught — same scenarios, same assertions, both
// against the mock and against real hardware.
//
// Usage:
//   node conformance.js [baseUrl] [apiKey]
//   node conformance.js                                  # defaults to the mock
//   node conformance.js http://192.168.1.42 <device-key> --force
//
// ⚠️  DESTRUCTIVE. This homes, jogs, moves, saves stops, configures outlets,
//     and clears calibration. Against a real device it PHYSICALLY MOVES THE
//     ACTUATOR and WIPES its configuration. For any non-localhost target you
//     must pass --force (or set CONFORMANCE_FORCE=1) to acknowledge that.
//
// What it asserts: the CONTRACT — response shapes, validation (400/401), and
// deterministic state transitions (home→homed, overlap-skip, clearcal reset).
// It deliberately does NOT assert simulation-only details (exact wattage,
// discovered device names/counts) that legitimately differ on real hardware;
// those are checked shape-only. A few sim-specific state flips are gated to
// localhost.

'use strict';

const DEFAULT_URL = 'http://localhost:3000';
const DEFAULT_KEY = 'dev-mock-key-1234';

const args = process.argv.slice(2).filter(a => a !== '--force');
const forced = process.argv.includes('--force') || process.env.CONFORMANCE_FORCE === '1';
const baseUrl = (args[0] || DEFAULT_URL).replace(/\/$/, '');
const apiKey  = args[1] || DEFAULT_KEY;
const isLocal = /^https?:\/\/(localhost|127\.0\.0\.1)(:|\/|$)/.test(baseUrl);

if (!isLocal && !forced) {
  console.error(`\n✋ Refusing to run against ${baseUrl} without --force.`);
  console.error(`   This suite PHYSICALLY MOVES the actuator and WIPES calibration.`);
  console.error(`   Re-run with --force (or CONFORMANCE_FORCE=1) if that's intended.\n`);
  process.exit(2);
}

// ── Tiny assertion harness ──────────────────────────────────────────────────
const results = [];
function check(name, cond, detail = '') { results.push({ name, ok: !!cond, detail }); }
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function req(method, path, body, { auth = true } = {}) {
  const headers = { 'Content-Type': 'application/json' };
  if (auth) headers['X-Api-Key'] = apiKey;
  const res = await fetch(baseUrl + path, {
    method, headers, body: body === undefined ? undefined : JSON.stringify(body),
  });
  let json = null;
  try { json = await res.json(); } catch { /* some endpoints may return empty */ }
  return { status: res.status, json };
}

async function pollStatus(pred, { timeoutMs = 30000, intervalMs = 300 } = {}) {
  const start = Date.now();
  let last;
  while (Date.now() - start < timeoutMs) {
    last = (await req('GET', '/api/status')).json;
    if (last && pred(last)) return last;
    await sleep(intervalMs);
  }
  return last; // timed out — caller asserts against whatever we last saw
}

const isNum = v => typeof v === 'number' && Number.isFinite(v);
const isStr = v => typeof v === 'string';
const isBool = v => typeof v === 'boolean';

// Wait for the target to answer before asserting — makes CI robust to however
// long the mock (or a booting device) takes to come up, instead of a fixed sleep.
async function waitForServer({ timeoutMs = 15000, intervalMs = 300 } = {}) {
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    try {
      const r = await fetch(baseUrl + '/api/info');
      if (r.ok) return true;
    } catch { /* not up yet */ }
    await sleep(intervalMs);
  }
  return false;
}

// ── Scenarios (ordered — device state builds up, cleared at the end) ─────────
async function run() {
  // 1. Auth — a protected route without the key is rejected.
  {
    const r = await req('GET', '/api/status', undefined, { auth: false });
    check('auth: /api/status without key → 401', r.status === 401, `got ${r.status}`);
  }

  // 2. Start clean so the rest is deterministic.
  {
    const r = await req('POST', '/api/clearcal');
    check('clearcal → ok', r.status === 200);
    const info = (await req('GET', '/api/info')).json;
    check('clearcal: info.numStops === 0', info && info.numStops === 0, `numStops=${info?.numStops}`);
    const s = (await req('GET', '/api/status')).json;
    check('clearcal: status homed=false currentStop=-1 outlets=[]',
      s && s.homed === false && s.currentStop === -1 && Array.isArray(s.outlets) && s.outlets.length === 0);
  }

  // 3. /api/info shape.
  {
    const i = (await req('GET', '/api/info')).json;
    check('info shape', i && isStr(i.apiKey) && isNum(i.numStops) && isStr(i.version)
      && isBool(i.homeOnRight) && isBool(i.motorInverted) && isNum(i.idleTimeoutSec)
      && isStr(i.manifoldModel) && isNum(i.stepsPerMm),
      JSON.stringify(i));
  }

  // 4. /api/status shape (unhomed).
  {
    const s = (await req('GET', '/api/status')).json;
    check('status shape', s && isStr(s.state) && isNum(s.currentStop) && isBool(s.homed)
      && isBool(s.enabled) && isBool(s.endstopHome) && Array.isArray(s.stops) && Array.isArray(s.outlets)
      && isBool(s.farEndstop) && isStr(s.manifoldModel) && isNum(s.stepsPerMm),
      JSON.stringify(s).slice(0, 140));
  }

  // 5. Config writes are reflected in /api/info.
  {
    await req('POST', '/api/config/gates', { numGates: 4 });
    await req('POST', '/api/config/orientation', { homeOnRight: true });
    await req('POST', '/api/config/motor', { invertDirection: true });
    await req('POST', '/api/config/idle-timeout', { seconds: 1800 });
    const i = (await req('GET', '/api/info')).json;
    check('config: numGates=4 → info.numStops', i && i.numStops === 4, `numStops=${i?.numStops}`);
    check('config: orientation → info.homeOnRight', i && i.homeOnRight === true);
    check('config: motor → info.motorInverted', i && i.motorInverted === true);
    check('config: idle-timeout → info.idleTimeoutSec', i && i.idleTimeoutSec === 1800, `got ${i?.idleTimeoutSec}`);
    // Reset motor direction so a real device isn't left inverted by the run.
    await req('POST', '/api/config/motor', { invertDirection: false });
  }

  // 6. Home lifecycle.
  {
    const r = await req('POST', '/api/home');
    check('home → ok', r.status === 200);
    const s = await pollStatus(x => x.state !== 'HOMING' && x.homed === true, { timeoutMs: 40000 });
    check('home: homed=true, currentStop=0', s && s.homed === true && s.currentStop === 0,
      `state=${s?.state} homed=${s?.homed} stop=${s?.currentStop}`);
    check('home: endstopHome truthy at home', s && s.endstopHome === true, `endstopHome=${s?.endstopHome}`);
  }

  // 7. Jog + setstop persists.
  {
    await req('POST', '/api/jog', { mm: 60 });
    await pollStatus(x => x.state === 'IDLE');
    const r = await req('POST', '/api/setstop', { index: 1 });
    check('setstop 1 → ok', r.status === 200);
    const stops = (await req('GET', '/api/stops')).json.stops;
    check('setstop: stop 1 saved (mm not null)', stops[1] && stops[1].mm !== null, `mm=${stops[1]?.mm}`);
  }

  // 8. Overlap backstop (CONTRACT): a save too close to another gate is skipped.
  {
    await req('POST', '/api/jog', { mm: 5 }); // ~65mm, within MIN_STOP_SEPARATION_MM (10) of stop 1
    await pollStatus(x => x.state === 'IDLE');
    await req('POST', '/api/setstop', { index: 2 });
    let stops = (await req('GET', '/api/stops')).json.stops;
    check('overlap: too-close save of stop 2 is skipped (stays null)', stops[2] && stops[2].mm === null,
      `mm=${stops[2]?.mm}`);
    // Now move well clear and it should save.
    await req('POST', '/api/jog', { mm: 30 }); // ~95mm, clear of stop 1
    await pollStatus(x => x.state === 'IDLE');
    await req('POST', '/api/setstop', { index: 2 });
    stops = (await req('GET', '/api/stops')).json.stops;
    check('overlap: clear save of stop 2 persists', stops[2] && stops[2].mm !== null, `mm=${stops[2]?.mm}`);
  }

  // 9. Move settles AT_STOP at a real gate.
  {
    await req('POST', '/api/move', { stop: 1 });
    const s = await pollStatus(x => x.state !== 'MOVING' && x.currentStop === 1);
    check('move: settles at stop 1 (AT_STOP)', s && s.currentStop === 1 && s.state === 'AT_STOP',
      `state=${s?.state} stop=${s?.currentStop}`);
    check('move: out-of-range stop → 400', (await req('POST', '/api/move', { stop: 99 })).status === 400);
  }

  // 10. configureOutlet validation + happy path.
  {
    check('outlet: empty name → 400', (await req('PUT', '/api/outlets/2', { name: '', stop: 2 })).status === 400);
    check('outlet: missing stop → 400', (await req('PUT', '/api/outlets/2', { name: 'X', stop: 0 })).status === 400);
    const r = await req('PUT', '/api/outlets/2', { name: 'TestTool', stop: 2, ip: '192.168.1.250', gen: 2, threshold: 600 });
    check('outlet: valid config → ok', r.status === 200);
    const s = (await req('GET', '/api/status')).json;
    const o = (s.outlets || []).find(x => x.slot === 2);
    check('outlet: appears in status with name', o && o.name === 'TestTool', JSON.stringify(o));
  }

  // 11. Discover shape (count/power/names vary on real hardware — shape only).
  {
    const r = await req('GET', '/api/outlets/discover');
    const arr = r.json;
    check('discover: 200 + array', r.status === 200 && Array.isArray(arr));
    const shapeOk = !Array.isArray(arr) ? false : arr.every(x =>
      isStr(x.ip) && isStr(x.hostname) && isStr(x.name) && isBool(x.reachable) && isNum(x.powerW) && isNum(x.gen));
    check('discover: item shape', shapeOk, JSON.stringify(arr).slice(0, 120));
    if (isLocal && Array.isArray(arr)) {
      check('discover [local]: tools off → 0W', arr.every(x => x.powerW === 0));
      const names = arr.filter(x => x.name).map(x => x.name);
      check('discover [local]: unique names', new Set(names).size === names.length);
    }
  }

  // 12. Ping shape (real hardware may be unreachable — shape still holds).
  {
    const r = await req('POST', '/api/outlets/ping', { ip: '192.168.1.250' });
    const p = r.json;
    check('ping: shape', r.status === 200 && p && isBool(p.reachable) && isNum(p.powerW) && isNum(p.gen) && isStr(p.name),
      JSON.stringify(p));
  }

  // 13. Dust collector config/switch/delete.
  {
    await req('PUT', '/api/dustcollector', { gen: 2, ip: '192.168.1.251' });
    let s = (await req('GET', '/api/status')).json;
    check('dc: config → dcConfigured=true', s && s.dcConfigured === true, `dcConfigured=${s?.dcConfigured}`);
    check('dc: missing ip → 400', (await req('PUT', '/api/dustcollector', { gen: 2 })).status === 400);
    const sw = await req('POST', '/api/dustcollector/switch', { on: true });
    check('dc: switch → ok', sw.status === 200);
    if (isLocal) {
      s = (await req('GET', '/api/status')).json;
      check('dc [local]: switch on → dcOn=true', s && s.dcOn === true);
    }
    await req('DELETE', '/api/dustcollector');
    s = (await req('GET', '/api/status')).json;
    check('dc: delete → dcConfigured=false', s && s.dcConfigured === false);
  }

  // 14. Dual-endstop reference sweep auto-calibrates + auto-places gates.
  {
    const r = await req('POST', '/api/calibrate', { model: 'rockler-2.5', gateCount: 4 });
    check('calibrate → ok', r.status === 200);
    const s = await pollStatus(x => x.state !== 'HOMING' && x.homed === true, { timeoutMs: 40000 });
    check('calibrate: homed at 0 after sweep', s && s.homed === true && s.currentStop === 0,
      `state=${s?.state} homed=${s?.homed}`);
    check('calibrate: steps/mm calibrated (>0)', s && isNum(s.stepsPerMm) && s.stepsPerMm > 0, `stepsPerMm=${s?.stepsPerMm}`);
    check('calibrate: span measured (>0)', s && isNum(s.measuredSpanSteps) && s.measuredSpanSteps > 0);
    const i = (await req('GET', '/api/info')).json;
    check('calibrate: gate count from sweep', i && i.numStops === 4, `numStops=${i?.numStops}`);
    check('calibrate: gates auto-placed (known manifold)', s && s.stops[1] && s.stops[1].mm !== null,
      `stop1=${s?.stops?.[1]?.mm}`);
    check('calibrate: gate role present', s && s.stops[1] && isStr(s.stops[1].role), JSON.stringify(s?.stops?.[1]));
    check('calibrate: bad gateCount → 400', (await req('POST', '/api/calibrate', { model: 'rockler-2.5', gateCount: 0 })).status === 400);
  }

  // 15. Port roles.
  {
    const ok = await req('POST', '/api/config/port-role', { index: 2, role: 'blocked' });
    check('port-role → ok', ok.status === 200);
    const s = (await req('GET', '/api/status')).json;
    check('port-role: gate 2 blocked', s && s.stops[2] && s.stops[2].role === 'blocked', `role=${s?.stops?.[2]?.role}`);
    check('port-role: invalid role → 400', (await req('POST', '/api/config/port-role', { index: 2, role: 'bogus' })).status === 400);
  }

  // 16. Final clearcal leaves the device clean.
  {
    await req('POST', '/api/clearcal');
    const i = (await req('GET', '/api/info')).json;
    const s = (await req('GET', '/api/status')).json;
    check('final clearcal: reset (numStops 0, unhomed, no outlets, dc off, cal cleared)',
      i && i.numStops === 0 && s && s.homed === false && s.currentStop === -1
      && s.outlets.length === 0 && s.dcConfigured === false
      && s.measuredSpanSteps === null && s.manifoldModel === 'custom');
  }
}

// ── Entry ────────────────────────────────────────────────────────────────────
(async () => {
  console.log(`\nDustGate conformance → ${baseUrl}${isLocal ? '' : '  (⚠ real target, --force)'}\n`);
  const up = await waitForServer();
  if (!up) {
    console.error(`✗ target never became reachable at ${baseUrl}/api/info\n`);
    process.exit(1);
  }
  try {
    await run();
  } catch (e) {
    check(`runner crashed: ${e.message}`, false);
  }
  let passed = 0;
  for (const r of results) {
    console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
    if (r.ok) passed++;
  }
  const failed = results.length - passed;
  console.log(`\n${passed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}\n`);
  process.exit(failed ? 1 : 0);
})();
