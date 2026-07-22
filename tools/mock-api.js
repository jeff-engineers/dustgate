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
// This is a THIN HTTP/WebSocket wrapper: all device behaviour lives in the
// canonical model at shared/device-model/device-model.js, which also drives
// the in-browser demo (dustgate-ui/.../demo-api.service.ts). Keep logic in the
// model, not here — this file only maps HTTP ↔ model calls and owns timing
// (setTimeout between begin*/complete* motion steps). State resets on restart.

// Load tools/.env regardless of cwd — keys never committed to git
try { require('dotenv').config({ path: require('path').join(__dirname, '.env') }); }
catch { /* dotenv not installed yet — run: cd tools && npm install */ }

const http   = require('http');
const https  = require('https');
const url    = require('url');
const { WebSocketServer } = require('ws');
const M = require('../shared/device-model/device-model.js');

const PORT    = 3000;
const API_KEY = 'dev-mock-key-1234';
const ANT_KEY = process.env.ANTHROPIC_KEY || '';
const VERSION = '1.0.0-mock';

// ── Canonical device instance ───────────────────────────────────────────────
const d = M.createDevice();

function statusJson() { return JSON.stringify(M.statusView(d)); }

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
    return json(res, M.infoView(d, API_KEY, VERSION));
  }

  // ── Auth check ──
  // /api/claude is exempt: it mirrors the real Vercel serverless function,
  // which never requires X-Api-Key (the demo deployment gates via the
  // optional accessCode field instead — see api/claude.ts).
  if (pathname !== '/api/claude') {
    const key = req.headers['x-api-key'];
    if (key !== API_KEY) return json(res, { error: 'unauthorized' }, 401);
  }

  // ── Routes (thin: parse → model call → respond) ────────────────────────────

  if (pathname === '/api/status' && req.method === 'GET') return json(res, M.statusView(d));
  if (pathname === '/api/stops'  && req.method === 'GET') return json(res, { stops: d.stops });
  // /api/outlets returns the live status blob (outlet list embedded), same as firmware.
  if (pathname === '/api/outlets' && req.method === 'GET') return json(res, M.statusView(d));

  if (pathname === '/api/home' && req.method === 'POST') {
    const durMs = M.beginHome(d);
    broadcast();
    setTimeout(() => { M.completeHome(d); broadcast(); }, durMs);
    return json(res, { ok: true });
  }

  if (pathname === '/api/enable'  && req.method === 'POST') { M.setEnabled(d, true);  return json(res, { ok: true }); }
  if (pathname === '/api/disable' && req.method === 'POST') { M.setEnabled(d, false); return json(res, { ok: true }); }
  if (pathname === '/api/estop'   && req.method === 'POST') { M.estop(d); broadcast(); return json(res, { ok: true }); }

  if (pathname === '/api/move' && req.method === 'POST') {
    return body(req, data => runModel(res, () => {
      const stop = data.stop ?? 0;
      const durMs = M.beginMove(d, stop);
      broadcast();
      setTimeout(() => { M.completeMove(d, stop); broadcast(); }, durMs);
      json(res, { ok: true });
    }));
  }

  if (pathname === '/api/jog' && req.method === 'POST') {
    return body(req, data => runModel(res, () => {
      const durMs = M.beginJog(d, data.mm ?? 0);
      broadcast();
      setTimeout(() => { M.completeJog(d); broadcast(); }, durMs);
      json(res, { ok: true });
    }));
  }

  if (pathname === '/api/outlets/discover' && req.method === 'GET') {
    return json(res, M.discoverOutlets(d));
  }

  if (pathname === '/api/outlets/ping' && req.method === 'POST') {
    return body(req, data => runModel(res, () => json(res, M.pingOutlet(d, data.ip))));
  }

  if (pathname === '/api/outlets/save' && req.method === 'POST') return json(res, { ok: true });

  // PUT /api/outlets/:slot — configure/update a single outlet
  const outletPut = pathname.match(/^\/api\/outlets\/(\d+)$/);
  if (outletPut && req.method === 'PUT') {
    return body(req, data => runModel(res, () => {
      M.configureOutlet(d, {
        slot: parseInt(outletPut[1], 10),
        name: data.name, stop: data.stop, ip: data.ip,
        host: data.host, gen: data.gen, threshold: data.threshold,
      });
      broadcast();
      json(res, { ok: true });
    }));
  }

  // DELETE /api/outlets/:slot
  const outletDel = pathname.match(/^\/api\/outlets\/(\d+)$/);
  if (outletDel && req.method === 'DELETE') {
    M.deleteOutlet(d, parseInt(outletDel[1], 10));
    broadcast();
    return json(res, { ok: true });
  }

  // ── Dust collector plug ──
  if (pathname === '/api/dustcollector' && req.method === 'PUT') {
    return body(req, data => runModel(res, () => {
      M.configureDustCollector(d, { gen: data.gen, ip: data.ip, host: data.host });
      broadcast();
      json(res, { ok: true });
    }));
  }
  if (pathname === '/api/dustcollector' && req.method === 'DELETE') {
    M.deleteDustCollector(d); broadcast(); return json(res, { ok: true });
  }
  if (pathname === '/api/dustcollector/switch' && req.method === 'POST') {
    return body(req, data => { M.switchDustCollector(d, data.on); broadcast(); json(res, { ok: true }); });
  }

  if (pathname === '/api/setstop' && req.method === 'POST') {
    return body(req, data => runModel(res, () => {
      M.saveStop(d, data.index); // overlap is silently skipped inside the model
      broadcast();
      json(res, { ok: true });
    }));
  }

  if (pathname === '/api/config/orientation' && req.method === 'POST') {
    return body(req, data => { M.setOrientation(d, data.homeOnRight); json(res, { ok: true }); });
  }
  if (pathname === '/api/config/motor' && req.method === 'POST') {
    return body(req, data => { M.setMotorInverted(d, data.invertDirection); json(res, { ok: true }); });
  }
  if (pathname === '/api/config/gates' && req.method === 'POST') {
    return body(req, data => { M.setNumGates(d, data.numGates); json(res, { ok: true }); });
  }
  if (pathname === '/api/calibrate' && req.method === 'POST') {
    return body(req, data => runModel(res, () => {
      const durMs = M.beginCalibrate(d, data.model, data.gateCount);
      broadcast();
      setTimeout(() => { M.completeCalibrate(d); broadcast(); }, durMs);
      json(res, { ok: true });
    }));
  }
  if (pathname === '/api/config/port-role' && req.method === 'POST') {
    return body(req, data => runModel(res, () => {
      M.setPortRole(d, data.index, data.role);
      broadcast();
      json(res, { ok: true });
    }));
  }
  if (pathname === '/api/config/idle-timeout' && req.method === 'POST') {
    return body(req, data => { M.setIdleTimeout(d, data.seconds); json(res, { ok: true }); });
  }

  if (pathname === '/api/wifi/reset' && req.method === 'POST') {
    console.log('[MOCK] WiFi reset requested — ignoring (no real WiFi to forget)');
    return json(res, { ok: true });
  }
  if (pathname === '/api/agent/key' && req.method === 'PUT') {
    console.log('[MOCK] Anthropic key update requested — ignoring (mock uses ANTHROPIC_KEY env var)');
    return json(res, { ok: true });
  }

  if (pathname === '/api/clearcal' && req.method === 'POST') {
    M.clearCal(d); broadcast(); return json(res, { ok: true });
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
      return json(res, {
        id: 'mock-msg-001', type: 'message', role: 'assistant',
        content: [{
          type: 'text',
          text: 'Hi! I\'m the DustGate setup assistant (running in mock mode — set ANTHROPIC_KEY env var for real responses). How can I help you configure your dust collection system?',
        }],
        model: 'claude-mock', stop_reason: 'end_turn',
        usage: { input_tokens: 0, output_tokens: 0 },
      });
    }

    // Forward to Anthropic with real key
    return body(req, rawBody => {
      const options = {
        hostname: 'api.anthropic.com', path: '/v1/messages', method: 'POST',
        headers: {
          'Content-Type':      'application/json',
          'x-api-key':         ANT_KEY,
          'anthropic-version': '2023-06-01',
          'Content-Length':    Buffer.byteLength(JSON.stringify(rawBody)),
        },
      };
      const fwd = https.request(options, fwdRes => {
        let data = '';
        fwdRes.on('data', c => data += c);
        fwdRes.on('end', () => { res.writeHead(fwdRes.statusCode, { 'Content-Type': 'application/json' }); res.end(data); });
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

// Run a model call, mapping a thrown model error (e.status) to an HTTP status.
function runModel(res, fn) {
  try { fn(); }
  catch (e) { json(res, { error: e.message }, e.status || 500); }
}

function broadcast() {
  const j = statusJson();
  wss.clients.forEach(c => { if (c.readyState === 1) c.send(j); });
}

// ── Start ─────────────────────────────────────────────────────────────────
server.listen(PORT, () => {
  console.log(`\nDustGate mock API running on http://localhost:${PORT}`);
  console.log(`  API key: ${API_KEY}`);
  console.log(`  Model:   shared/device-model (canonical)`);
  console.log(`  Claude:  ${ANT_KEY ? 'PROXYING to Anthropic' : 'canned mock responses'}`);
  console.log(`\n  In dustgate-ui:  ng serve --proxy-config proxy.conf.json`);
  console.log(`  Or add ANTHROPIC_KEY=sk-ant-... to tools/.env\n`);
});
