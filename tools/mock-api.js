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

// ── Simulated device state ────────────────────────────────────────────────
const state = {
  state:          'IDLE',   // matches firmware JSON key ("state", not "stateName")
  currentStop:    -1,       // -1 = unhomed
  targetStop:     0,
  positionSteps:  0,
  homed:          false,
  enabled:        true,
  endstopHome:    false,
  manualOverride: false,
  homeOnRight:    false,
  motorInverted:  false,
  numActiveStops: 0,        // 0 = unconfigured until setup wizard runs
  stops: Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({
    index: i,
    mm: (i * 25).toFixed(2),
  })),
  outlets: [],              // empty until setup wizard configures them
};

function statusJson() { return JSON.stringify(state); }

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
    });
  }

  // ── Auth check ──
  const key = req.headers['x-api-key'];
  if (key !== API_KEY) return json(res, { error: 'unauthorized' }, 401);

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
      state.state       = 'IDLE';
      state.currentStop = 0;
      state.targetStop  = 0;
      state.homed       = true;
      broadcast();
    }, 1500);
    return json(res, { ok: true });
  }

  if (pathname === '/api/enable'  && req.method === 'POST') { state.enabled = true;  return json(res, { ok: true }); }
  if (pathname === '/api/disable' && req.method === 'POST') { state.enabled = false; return json(res, { ok: true }); }
  if (pathname === '/api/estop'   && req.method === 'POST') {
    state.state = 'ESTOP';
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
        state.state          = 'IDLE';
        state.currentStop    = stop;
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
        state.positionSteps += Math.round(mm * 40); // 40 steps/mm mock resolution
        state.state = 'IDLE';
        broadcast();
      }, durationMs);
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/outlets' && req.method === 'GET') {
    return json(res, state);
  }

  if (pathname === '/api/outlets/ping' && req.method === 'POST') {
    return body(req, data => {
      json(res, { reachable: true, powerW: 0 });
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
      const existing = state.outlets.findIndex(o => o.slot === slot);
      const record = {
        slot,
        name:       data.name   ?? `Gate ${slot + 1}`,
        stop:       data.stop   ?? slot + 1,
        powerW:     0,
        active:     false,
        reachable:  false,
        thresholdW: data.threshold ?? 5.0,
        gen:        data.gen    ?? 2,
        ip:         data.ip     ?? '',
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

  if (pathname === '/api/setstop' && req.method === 'POST') {
    return body(req, data => {
      const idx = data.index ?? -1;
      if (idx < 1 || idx > NUM_STOPS) return json(res, { error: 'index out of range' }, 400);
      const currentMm = parseFloat(state.stops[state.currentStop]?.mm ?? '0');
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
        console.log(`  [mock] Active gates: ${n}`);
      }
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/clearcal' && req.method === 'POST') {
    // Reset to unconfigured — mirrors firmware behaviour on start-over
    state.numActiveStops = 0;
    state.stops    = Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({ index: i, mm: (i * 25).toFixed(2) }));
    state.outlets  = [];
    state.homed    = false;
    state.currentStop = -1;
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
