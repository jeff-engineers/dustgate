#!/usr/bin/env node
// mock-api.js — local dev server that mimics the ESP32 HTTP + WebSocket API.
//
// Usage:
//   cd tools && npm install && node mock-api.js
//   # Then in dustgate-ui:
//   ng serve --proxy-config proxy.conf.json
//
// Set ANTHROPIC_KEY env var to proxy real Claude responses through the
// /api/agent/chat endpoint; omit it for canned mock responses.
//
// State is in-memory and resets on restart.

// Load tools/.env regardless of cwd — keys never committed to git
try { require('dotenv').config({ path: require('path').join(__dirname, '.env') }); }
catch { /* dotenv not installed yet — run: cd tools && npm install */ }

const http   = require('http');
const https  = require('https');
const url    = require('url');
const { WebSocketServer } = require('ws');

const PORT   = 3000;
const API_KEY = 'dev-mock-key-1234';
const ANT_KEY = process.env.ANTHROPIC_KEY || '';
const NUM_STOPS = 16;  // compile-time max — matches firmware #define NUM_STOPS
const STEPS_PER_MM = 40;  // mock resolution — arbitrary, just needs to be consistent

// ── Simulated device state ────────────────────────────────────────────────
const state = {
  state:          'IDLE',   // matches firmware JSON key ("state", not "stateName")
  currentStop:    -1,       // -1 = unhomed
  targetStop:     0,
  positionSteps:  0,
  positionMM:     0,        // raw actuator position, independent of any saved stop
  homed:          false,
  enabled:        true,
  endstopHome:    false,
  manualOverride: false,
  homeOnRight:    false,
  motorInverted:  false,
  numActiveStops: 0,        // 0 = unconfigured until setup wizard runs
  idleTimeoutSec: 3600,     // matches firmware IDLE_TIMEOUT_SEC_DEFAULT
  dcConfigured:   false,    // set true once a dust collector plug is assigned
  dcOn:           false,    // dust collector switch state
  dcIp:           null,     // ip of the assigned dust collector plug, for ping simulation
  // mm: null = position not yet saved (distinct from a gate saved at 0.00).
  // Home (index 0) gets a real 0.00 once /api/home runs.
  stops: Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({
    index: i,
    mm: null,
  })),
  outlets: [],              // empty until setup wizard configures them
};

// ── Ping simulation (demonstrates the wizard's "no load yet, try again"
// behaviour without needing real hardware) ──────────────────────────────
// The first two distinct IPs ever pinged (excluding the dust collector's own
// plug, which already has real on/off-driven load below) simulate a tool
// that hasn't spun up yet: gate 1's outlet reads 0W once then a steady load,
// gate 2's outlet reads 0W twice then a random load. Any further IPs read a
// realistic load immediately, since the scenario's been demonstrated already.
const pingSim = { base: {}, count: {} };

function resetPingSim() {
  pingSim.base = {};
  pingSim.count = {};
}

// Power tools have real on/off switches — no standby draw. An outlet reads a
// clean 0W until its tool is switched on, then a stable running draw in the
// 500–1000W range (with a few percent of per-reading jiggle so repeated pings
// read consistently like real hardware). The setup flow asks the user to turn
// the tool on before capturing a threshold, so we model that: the FIRST ping
// to an outlet catches it still off (0W), and subsequent pings read its
// running draw once "switched on."
function simulatedPingPower(ip) {
  pingSim.count[ip] = (pingSim.count[ip] || 0) + 1;
  if (pingSim.count[ip] === 1) return 0;   // still off on the first read
  if (!(ip in pingSim.base)) pingSim.base[ip] = 500 + Math.random() * 500;
  return Math.round(pingSim.base[ip] * (1 + (Math.random() - 0.5) * 0.06)); // ±3%
}

function statusJson() {
  // Only meaningful once homed — before that, position is unknown, so the
  // sensor reads as untriggered rather than misleadingly "at home".
  state.endstopHome = state.homed && state.positionMM < 0.5;
  return JSON.stringify(state);
}

// ── mDNS discovery simulation ─────────────────────────────────────────────
// Mirrors /api/outlets/discover on real hardware: a handful of Shelly
// outlets "on the network," each with a plausible mDNS hostname
// (ShellyPlugUSG4-<MAC-shaped hex>, matching the real device naming scheme)
// and IP. Generated once per server run (not per request) so repeated scans
// return the same devices, like real mDNS would. Real-world testing found
// most Shelly Plugs never have a local device/switch name configured (the
// app's label is often cloud-only) even when the user thinks they renamed
// it, so most mock devices get name: '' too — only a couple get a name, to
// exercise both code paths in the wizard.
const MOCK_TOOL_NAMES = ['Table Saw', 'Drill Press', 'Router Table'];

function randomHex(len) {
  let s = '';
  for (let i = 0; i < len; i++) s += Math.floor(Math.random() * 16).toString(16);
  return s.toUpperCase();
}

function randomLanIp(usedIps) {
  let ip;
  do {
    ip = `192.168.87.${20 + Math.floor(Math.random() * 60)}`;
  } while (usedIps.has(ip));
  usedIps.add(ip);
  return ip;
}

let mockDiscovered = null; // lazily generated, stable for the life of the process

function ensureMockDiscovered() {
  if (mockDiscovered) return mockDiscovered;
  const usedIps = new Set();
  const count = 2 + Math.floor(Math.random() * 3); // 2-4, mirrors real mDNS variability
  // Names are drawn from a shuffled pool and assigned WITHOUT replacement, so
  // two devices never share a name (e.g. two "Drill Press" entries).
  const namePool = [...MOCK_TOOL_NAMES].sort(() => Math.random() - 0.5);
  const namedIdx = new Set();
  while (namedIdx.size < Math.min(2, count)) namedIdx.add(Math.floor(Math.random() * count));

  let nameCursor = 0;
  mockDiscovered = Array.from({ length: count }, (_, i) => ({
    ip:         randomLanIp(usedIps),
    hostname:   `ShellyPlugUSG4-${randomHex(12)}`,
    name:       namedIdx.has(i) ? namePool[nameCursor++] : '',
    reachable:  true,
    // Tools are off during the setup scan (real power switches, no standby),
    // so a freshly discovered outlet reads a clean 0W. Its running draw shows
    // up later at the threshold step, once the user switches the tool on.
    powerW:     0,
    gen:        2,
  }));
  return mockDiscovered;
}

// Real device name for a given IP if it's one of the discovered/simulated
// ones, else '' — used by both /api/outlets/discover and /api/outlets/ping
// so the two stay consistent for the same IP.
function nameForIp(ip) {
  const d = ensureMockDiscovered().find(d => d.ip === ip);
  return d ? d.name : '';
}

// ── WebSocket server ──────────────────────────────────────────────────────
const server = http.createServer(handler);
const wss    = new WebSocketServer({ server, path: '/ws' });

wss.on('connection', ws => {
  console.log('[WS] client connected');
  ws.send(statusJson());
});

// Push status to all clients every second (simulates outlet poll changes)
setInterval(() => {
  if (wss.clients.size === 0) return;
  const json = statusJson();
  wss.clients.forEach(c => { if (c.readyState === 1) c.send(json); });
}, 1000);

// ── HTTP handler ──────────────────────────────────────────────────────────
function handler(req, res) {
  const { pathname } = url.parse(req.url);

  // CORS
  res.setHeader('Access-Control-Allow-Origin',  '*');
  res.setHeader('Access-Control-Allow-Headers', 'X-Api-Key, Content-Type');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, PUT, DELETE, OPTIONS');

  if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }

  console.log(`${req.method} ${pathname}`);

  // ── Unauthenticated ──
  if (pathname === '/api/info' && req.method === 'GET') {
    return json(res, {
      apiKey:        API_KEY,
      numStops:      state.numActiveStops,   // runtime; not compile-time constant
      version:       '1.0.0-mock',
      homeOnRight:   state.homeOnRight,
      motorInverted: state.motorInverted,
      idleTimeoutSec: state.idleTimeoutSec,
    });
  }

  // ── Auth check ──
  // /api/claude is exempt: it mirrors the real Vercel serverless function,
  // which never requires X-Api-Key (the demo deployment gates via the
  // optional accessCode field instead — see api/claude.ts).
  if (pathname !== '/api/claude') {
    const key = req.headers['x-api-key'];
    if (key !== API_KEY) return json(res, { error: 'unauthorized' }, 401);
  }

  // ── Routes ───────────────────────────────────────────────────────────────

  if (pathname === '/api/status' && req.method === 'GET') {
    return json(res, state);
  }

  if (pathname === '/api/stops' && req.method === 'GET') {
    return json(res, { stops: state.stops });
  }

  if (pathname === '/api/home' && req.method === 'POST') {
    state.state          = 'HOMING';
    state.manualOverride = false;
    broadcast();
    setTimeout(() => {
      state.state         = 'IDLE';
      state.currentStop    = 0;
      state.targetStop     = 0;
      state.homed          = true;
      state.positionSteps  = 0;
      state.positionMM     = 0;
      state.dcOn           = false;   // home → dust collector off
      state.stops[0]       = { index: 0, mm: '0.00' };
      broadcast();
    }, 1500);
    return json(res, { ok: true });
  }

  if (pathname === '/api/enable'  && req.method === 'POST') { state.enabled = true;  return json(res, { ok: true }); }
  if (pathname === '/api/disable' && req.method === 'POST') { state.enabled = false; return json(res, { ok: true }); }
  if (pathname === '/api/estop'   && req.method === 'POST') {
    state.state = 'ERROR';   // firmware maps e-stop to STATE_ERROR -> "ERROR"
    broadcast();
    return json(res, { ok: true });
  }

  if (pathname === '/api/move' && req.method === 'POST') {
    return body(req, data => {
      const stop = data.stop ?? 0;
      const fromMm = parseFloat(state.stops[state.currentStop]?.mm ?? '0');
      const toMm   = parseFloat(state.stops[stop]?.mm ?? '0');
      const distMm = Math.abs(toMm - fromMm);
      const durationMs = Math.max(400, distMm * 20); // ~50 mm/s mock speed
      state.state          = 'MOVING';
      state.targetStop     = stop;
      state.manualOverride = true;
      broadcast();
      setTimeout(() => {
        // Firmware settles to AT_STOP at a real gate, plain IDLE at home (0).
        state.state          = stop > 0 ? 'AT_STOP' : 'IDLE';
        state.currentStop    = stop;
        state.positionMM     = toMm;
        state.positionSteps  = Math.round(toMm * STEPS_PER_MM);
        state.dcOn           = stop > 0;   // collector follows gate selection
        broadcast();
      }, durationMs);
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/jog' && req.method === 'POST') {
    return body(req, data => {
      const mm = data.mm ?? 0;
      console.log(`  jog ${mm} mm (mock)`);
      // Simulate MOVING state briefly so the jog widget disables during motion
      state.state = 'MOVING';
      broadcast();
      const durationMs = Math.max(200, Math.abs(mm) * 15);
      setTimeout(() => {
        state.positionMM    += mm;
        state.positionSteps  = Math.round(state.positionMM * STEPS_PER_MM);
        state.state = 'IDLE';
        broadcast();
      }, durationMs);
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/outlets' && req.method === 'GET') {
    return json(res, state);
  }

  if (pathname === '/api/outlets/discover' && req.method === 'GET') {
    // Off tools read a clean 0W; only a non-zero (running) draw gets the
    // slight per-call jitter a real poll would show. The device list itself
    // isn't regenerated (mDNS keeps finding the same physical devices).
    const results = ensureMockDiscovered().map(d => ({
      ...d,
      powerW: d.powerW === 0
        ? 0
        : Math.max(0, Math.round((d.powerW + (Math.random() - 0.5) * 2) * 10) / 10),
    }));
    return json(res, results);
  }

  if (pathname === '/api/outlets/ping' && req.method === 'POST') {
    return body(req, data => {
      // The dust collector's own plug follows its real on/off switch state.
      // Every other outlet runs through the ping simulation above, which
      // mimics tools that haven't spun up yet on the first ping or two.
      // Real generation auto-detect (Gen 1 then Gen 2) happens device-side;
      // mock always answers as Gen 2 since there's no real outlet to distinguish.
      let powerW = 0;
      if (data.ip && data.ip === state.dcIp) {
        powerW = state.dcOn ? 380 : 0;
      } else if (data.ip) {
        powerW = simulatedPingPower(data.ip);
      }
      const name = data.ip ? nameForIp(data.ip) : '';
      json(res, { reachable: true, powerW, gen: 2, name });
    });
  }

  if (pathname === '/api/outlets/save' && req.method === 'POST') {
    return json(res, { ok: true });
  }

  // PUT /api/outlets/:slot — configure or update a single outlet
  const outletPutMatch = pathname.match(/^\/api\/outlets\/(\d+)$/);
  if (outletPutMatch && req.method === 'PUT') {
    return body(req, data => {
      const slot = parseInt(outletPutMatch[1], 10);
      const ip = data.ip ?? '';
      const hasSwitch = ip.trim().length > 0;  // empty ip = name-only gate
      // Match firmware validation: name always required, stop must be >= 1.
      // ip is optional (empty = name-only gate). Firmware returns 400 here
      // rather than silently defaulting, so the mock must too.
      if (typeof data.name !== 'string' || data.name.trim().length === 0) {
        return json(res, { error: "missing 'name'" }, 400);
      }
      if (typeof data.stop !== 'number' || data.stop <= 0) {
        return json(res, { error: "missing 'stop'" }, 400);
      }
      const existing = state.outlets.findIndex(o => o.slot === slot);
      const record = {
        slot,
        name:       data.name,
        stop:       data.stop,
        powerW:     0,
        active:     false,
        reachable:  false,
        thresholdW: data.threshold ?? 5.0,
        gen:        data.gen    ?? 2,
        ip,
        host:       data.host ?? '',   // mDNS hostname, if the outlet came from a scan
        hasSwitch,
      };
      if (existing >= 0) {
        state.outlets[existing] = record;
      } else {
        state.outlets.push(record);
      }
      console.log(`  [mock] Outlet slot ${slot} configured: ${record.name} @ ${record.ip}`);
      broadcast();
      json(res, { ok: true });
    });
  }

  // DELETE /api/outlets/:slot — remove an outlet
  const outletDelMatch = pathname.match(/^\/api\/outlets\/(\d+)$/);
  if (outletDelMatch && req.method === 'DELETE') {
    const slot = parseInt(outletDelMatch[1], 10);
    state.outlets = state.outlets.filter(o => o.slot !== slot);
    console.log(`  [mock] Outlet slot ${slot} deleted`);
    broadcast();
    return json(res, { ok: true });
  }

  // ── Dust collector plug ──
  if (pathname === '/api/dustcollector' && req.method === 'PUT') {
    return body(req, data => {
      state.dcConfigured = true;
      state.dcIp = data.ip ?? '';
      state.dcHost = data.host ?? '';
      console.log(`  [mock] Dust collector configured: gen${data.gen ?? 2} @ ${data.ip ?? ''}`);
      broadcast();
      json(res, { ok: true });
    });
  }
  if (pathname === '/api/dustcollector' && req.method === 'DELETE') {
    state.dcConfigured = false;
    state.dcOn = false;
    state.dcIp = null;
    broadcast();
    return json(res, { ok: true });
  }
  if (pathname === '/api/dustcollector/switch' && req.method === 'POST') {
    return body(req, data => {
      state.dcOn = !!data.on;
      console.log(`  [mock] Dust collector manual → ${state.dcOn ? 'ON' : 'OFF'}`);
      broadcast();
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/setstop' && req.method === 'POST') {
    return body(req, data => {
      const idx = data.index ?? -1;
      if (idx < 1 || idx > NUM_STOPS) return json(res, { error: 'index out of range' }, 400);
      // Bug fix: this used to read state.stops[state.currentStop].mm, which is the
      // last CONFIRMED stop's position — but currentStop never changes during a raw
      // jog, so every save recorded the previous stop's mm instead of where the
      // actuator actually was. positionMM tracks the real jogged position.
      const currentMm = state.positionMM;
      state.stops[idx] = { index: idx, mm: currentMm.toFixed(2) };
      console.log(`  [mock] Stop ${idx} saved at ${currentMm.toFixed(2)} mm`);
      broadcast();
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/config/orientation' && req.method === 'POST') {
    return body(req, data => {
      state.homeOnRight = !!data.homeOnRight;
      console.log(`  [mock] Orientation: home on ${state.homeOnRight ? 'right' : 'left'}`);
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/config/motor' && req.method === 'POST') {
    return body(req, data => {
      state.motorInverted = !!data.invertDirection;
      console.log(`  [mock] Motor direction: ${state.motorInverted ? 'inverted' : 'normal'}`);
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/config/gates' && req.method === 'POST') {
    return body(req, data => {
      const n = data.numGates;
      if (n >= 1 && n <= NUM_STOPS) {
        state.numActiveStops = n;
        // Clear stale saved positions beyond the new count so they don't
        // reappear as phantom conflicts if the count is raised again later.
        for (const s of state.stops) {
          if (s.index > n) s.mm = null;
        }
        console.log(`  [mock] Active gates: ${n}`);
      }
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/config/idle-timeout' && req.method === 'POST') {
    return body(req, data => {
      const sec = data.seconds;
      if (typeof sec === 'number' && sec >= 0 && sec <= 86400) {
        state.idleTimeoutSec = sec;
        console.log(`  [mock] Idle timeout: ${sec === 0 ? 'disabled' : sec + 's'}`);
      }
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/wifi/reset' && req.method === 'POST') {
    console.log('[MOCK] WiFi reset requested — ignoring (no real WiFi to forget)');
    return json(res, { ok: true });
  }

  if (pathname === '/api/agent/key' && req.method === 'PUT') {
    console.log('[MOCK] Anthropic key update requested — ignoring (mock always uses ANTHROPIC_KEY env var)');
    return json(res, { ok: true });
  }

  if (pathname === '/api/clearcal' && req.method === 'POST') {
    // Reset to unconfigured — mirrors firmware behaviour on start-over
    state.numActiveStops = 0;
    state.stops    = Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({ index: i, mm: null }));
    state.outlets  = [];
    state.homed    = false;
    state.currentStop = -1;
    // Dust collector is part of the outlet config the firmware clears on
    // start-over — reset it here too so the mock matches.
    state.dcConfigured = false;
    state.dcOn         = false;
    state.dcIp         = null;
    resetPingSim();
    broadcast();
    return json(res, { ok: true });
  }

  if (pathname === '/api/reboot' && req.method === 'POST') {
    console.log('[MOCK] reboot requested — ignoring');
    return json(res, { ok: true });
  }

  // ── Claude proxy ─────────────────────────────────────────────────────────
  // /api/claude   — used by DemoApiService (mirrors the Vercel serverless function)
  // /api/agent/chat — used by ApiService (real ESP32 path, kept for compatibility)
  if ((pathname === '/api/claude' || pathname === '/api/agent/chat') && req.method === 'POST') {
    if (!ANT_KEY) {
      // Canned mock response when no key is provided
      const mock = {
        id: 'mock-msg-001',
        type: 'message',
        role: 'assistant',
        content: [{
          type: 'text',
          text: 'Hi! I\'m the DustGate setup assistant (running in mock mode — set ANTHROPIC_KEY env var for real responses). How can I help you configure your dust collection system?',
        }],
        model: 'claude-mock',
        stop_reason: 'end_turn',
        usage: { input_tokens: 0, output_tokens: 0 },
      };
      return json(res, mock);
    }

    // Forward to Anthropic with real key
    return body(req, rawBody => {
      const options = {
        hostname: 'api.anthropic.com',
        path:     '/v1/messages',
        method:   'POST',
        headers: {
          'Content-Type':      'application/json',
          'x-api-key':         ANT_KEY,
          'anthropic-version': '2023-06-01',
          'Content-Length':    Buffer.byteLength(JSON.stringify(rawBody)),
        },
      };
      const fwd = https.request(options, fwdRes => {
        let data = '';
        fwdRes.on('data', d => data += d);
        fwdRes.on('end', () => {
          res.writeHead(fwdRes.statusCode, { 'Content-Type': 'application/json' });
          res.end(data);
        });
      });
      fwd.on('error', e => json(res, { error: e.message }, 502));
      fwd.write(JSON.stringify(rawBody));
      fwd.end();
    });
  }

  json(res, { error: 'not found' }, 404);
}

// ── Helpers ───────────────────────────────────────────────────────────────
function json(res, obj, code = 200) {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
}

function body(req, cb) {
  let raw = '';
  req.on('data', d => raw += d);
  req.on('end', () => { try { cb(JSON.parse(raw)); } catch { cb({}); } });
}

function broadcast() {
  const json = statusJson();
  wss.clients.forEach(c => { if (c.readyState === 1) c.send(json); });
}

// ── Start ─────────────────────────────────────────────────────────────────
server.listen(PORT, () => {
  console.log(`\nDustGate mock API running on http://localhost:${PORT}`);
  console.log(`  API key: ${API_KEY}`);
  console.log(`  Claude: ${ANT_KEY ? 'PROXYING to Anthropic' : 'canned mock responses'}`);
  console.log(`\n  In dustgate-ui:  ng serve --proxy-config proxy.conf.json`);
  console.log(`  Or add ANTHROPIC_KEY=sk-ant-... to tools/.env\n`);
});
