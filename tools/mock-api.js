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
const NUM_STOPS = 7;

// ── Simulated device state ────────────────────────────────────────────────
const state = {
  stateName:     'IDLE',
  currentStop:   0,
  targetStop:    0,
  positionSteps: 0,
  homed:         true,
  enabled:       true,
  endstopHome:   false,
  stops: Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({
    index: i,
    mm: (i * 25).toFixed(2),
  })),
  outlets: [
    { slot: 0, name: 'Table Saw',     stop: 1, powerW: '0.0', active: false, reachable: true },
    { slot: 1, name: 'Router Table',  stop: 2, powerW: '0.0', active: false, reachable: true },
  ],
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
    return json(res, { apiKey: API_KEY, numStops: NUM_STOPS, version: '1.0.0-mock' });
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
    state.stateName = 'HOMING';
    broadcast();
    setTimeout(() => {
      state.stateName   = 'IDLE';
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
    state.stateName = 'ESTOP';
    broadcast();
    return json(res, { ok: true });
  }

  if (pathname === '/api/move' && req.method === 'POST') {
    return body(req, data => {
      const stop = data.stop ?? 0;
      state.stateName  = 'MOVING';
      state.targetStop = stop;
      broadcast();
      setTimeout(() => {
        state.stateName   = 'IDLE';
        state.currentStop = stop;
        broadcast();
      }, 800);
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/jog' && req.method === 'POST') {
    return body(req, data => {
      console.log(`  jog ${data.mm} mm (mock)`);
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

  if (pathname === '/api/clearcal' && req.method === 'POST') {
    return json(res, { ok: true });
  }

  if (pathname === '/api/reboot' && req.method === 'POST') {
    console.log('[MOCK] reboot requested — ignoring');
    return json(res, { ok: true });
  }

  // ── Claude proxy ─────────────────────────────────────────────────────────
  if (pathname === '/api/agent/chat' && req.method === 'POST') {
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
