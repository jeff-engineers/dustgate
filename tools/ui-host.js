#!/usr/bin/env node
// =============================================================================
// ui-host.js — host the app on a computer and proxy it to the REAL brain.
//
// A MOCK-UP of "split the UI off the brain onto a Pi" (2026-10-03), run on a Mac so
// the idea can be tried before any hardware is bought. The brain then serves only
// its small JSON API and ONE WebSocket, however many browsers are open:
//
//   browsers ──► this server ──► the brain
//     N pages       static files        (nothing but /api and one /ws)
//                   /api/status cached a moment, coalesced
//                   N browser /ws sockets fanned out from ONE upstream /ws
//
// WHY. The app is ~230 KB and every browser opens a WebSocket; each costs the board
// internal RAM it does not have (a cold page load crashed it in a loop). Nothing
// here is the brain's job — serving files, caching a poll, holding sockets.
//
//   node tools/ui-host.js [--brain dustgate.local] [--port 8080] [--cache-ms 500]
//
// Needs the app built:  cd dustgate-ui && npm run build
// Prints, every 10 s, how many requests browsers made against how many reached the
// brain — that ratio is the point.
// =============================================================================
'use strict';

const http = require('http');
const fs   = require('fs');
const path = require('path');
const { WebSocket, WebSocketServer } = require('ws');

const arg = (name, dflt) => {
  const i = process.argv.indexOf('--' + name);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt;
};
const BRAIN    = arg('brain', 'dustgate.local');
const PORT     = +arg('port', 8080);
const CACHE_MS = +arg('cache-ms', 500);
const DIST     = path.resolve(arg('dist', path.join(__dirname, '..', 'dustgate-ui', 'dist', 'dustgate-ui', 'browser')));

if (!fs.existsSync(path.join(DIST, 'index.html'))) {
  console.error(`No built app at ${DIST}\n  build it first:  cd dustgate-ui && npm run build`);
  process.exit(1);
}

const stats = { browserApi: 0, brainApi: 0, cacheHits: 0, files: 0, wsBrowsers: 0, brainWsMsgs: 0, fanned: 0, errors: 0 };

// Two sockets to the brain at most: it has little RAM per connection.
const agent = new http.Agent({ keepAlive: true, maxSockets: 2 });

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css',
                '.json': 'application/json', '.ico': 'image/x-icon', '.svg': 'image/svg+xml',
                '.png': 'image/png', '.txt': 'text/plain' };

// ── the brain, over HTTP ────────────────────────────────────────────────────
function toBrain(req, body, done) {
  stats.brainApi++;
  let finished = false;
  const cb = (e, r) => { if (finished) return; finished = true; done(e, r); };
  const up = http.request({ host: BRAIN, port: 80, path: req.url, method: req.method, agent,
    headers: { ...req.headers, host: BRAIN, connection: 'keep-alive' }, timeout: 8000,
    // The board's web server frames some replies loosely (a stray byte after the
    // headers); curl and browsers shrug, Node's strict parser does not.
    insecureHTTPParser: true }, (res) => {
    const chunks = [];
    res.on('data', (c) => chunks.push(c));
    res.on('end', () => cb(null, { status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }));
  });
  up.on('timeout', () => up.destroy(new Error('the brain did not answer in 8 s')));
  up.on('error', (e) => { stats.errors++; cb(e); });
  if (body && body.length) up.write(body);
  up.end();
}

// A short cache for the two polls every page makes, with callers that arrive while
// one is in flight sharing that one answer. Keyed by path + API key.
const cache = new Map();   // key -> { at, res, waiting: [] | null }
function cachedGet(req, cb) {
  const key = req.url + '|' + (req.headers['x-api-key'] || '');
  const hit = cache.get(key);
  const now = Date.now();
  if (hit && !hit.waiting && now - hit.at < CACHE_MS) { stats.cacheHits++; return cb(null, hit.res); }
  if (hit && hit.waiting) { stats.cacheHits++; hit.waiting.push(cb); return; }
  const entry = { at: now, res: null, waiting: [cb] };
  cache.set(key, entry);
  toBrain(req, null, (err, res) => {
    const waiting = entry.waiting; entry.waiting = null;
    if (!waiting) return;                 // the brain's request can report twice (timeout, then error)
    if (err) cache.delete(key); else { entry.res = res; entry.at = Date.now(); }
    for (const w of waiting) w(err, res);
  });
}

const CACHEABLE = /^\/api\/(status|motion)(\?|$)/;

const server = http.createServer((req, res) => {
  if (req.url === '/__stats') {
    res.setHeader('content-type', 'application/json');
    return res.end(JSON.stringify({ ...stats, cacheMs: CACHE_MS, brain: BRAIN }));
  }

  if (req.url.startsWith('/api/')) {
    stats.browserApi++;
    const send = (err, r) => {
      if (err) { res.writeHead(502, { 'content-type': 'application/json' });
                 return res.end(JSON.stringify({ error: 'proxy: ' + err.message })); }
      const h = { ...r.headers }; delete h['transfer-encoding']; delete h.connection;
      h['content-length'] = r.body.length;
      res.writeHead(r.status, h);
      res.end(r.body);
    };
    if (req.method === 'GET' && CACHEABLE.test(req.url)) return cachedGet(req, send);
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => toBrain(req, Buffer.concat(chunks), send));
    return;
  }

  // Static files; unknown paths fall back to index.html (the app uses hash routes).
  let rel = decodeURIComponent(req.url.split('?')[0]);
  if (rel === '/' || rel.includes('..')) rel = '/index.html';
  let file = path.join(DIST, rel);
  if (!fs.existsSync(file) || fs.statSync(file).isDirectory()) file = path.join(DIST, 'index.html');
  stats.files++;
  res.writeHead(200, {
    'content-type': TYPES[path.extname(file)] || 'application/octet-stream',
    // hashed bundles never change; index.html must be re-read
    'cache-control': /index\.html$/.test(file) ? 'no-cache' : 'max-age=31536000, immutable',
  });
  fs.createReadStream(file).pipe(res);
});

// ── ONE WebSocket to the brain, fanned out to every browser ─────────────────
const wss = new WebSocketServer({ noServer: true });
let upstream = null, lastMsg = null, retryMs = 1000;
function connectUpstream() {
  const ws = new WebSocket(`ws://${BRAIN}/ws`);
  upstream = ws;
  ws.on('open', () => { retryMs = 1000; console.log(`[ws] upstream connected to ${BRAIN}`); });
  ws.on('message', (data, isBinary) => {
    stats.brainWsMsgs++;
    lastMsg = { data, isBinary };
    for (const c of wss.clients) if (c.readyState === 1) { c.send(data, { binary: isBinary }); stats.fanned++; }
  });
  const again = () => { if (upstream !== ws) return; upstream = null;
    setTimeout(connectUpstream, retryMs); retryMs = Math.min(retryMs * 2, 15000); };
  ws.on('close', again);
  ws.on('error', () => { try { ws.close(); } catch (_) {} again(); });
}
connectUpstream();

server.on('upgrade', (req, socket, head) => {
  if (req.url !== '/ws') return socket.destroy();
  wss.handleUpgrade(req, socket, head, (c) => {
    stats.wsBrowsers++;
    if (lastMsg) c.send(lastMsg.data, { binary: lastMsg.isBinary });   // a new page need not wait for the next push
    c.on('close', () => { stats.wsBrowsers--; });
  });
});

server.listen(PORT, () => {
  console.log(`DustGate UI host: http://localhost:${PORT}   (brain: ${BRAIN}, status cache ${CACHE_MS} ms)`);
  console.log('Open it from another device with this machine\'s LAN address.');
});

let last = { ...stats };
setInterval(() => {
  const d = (k) => stats[k] - last[k];
  console.log(`[10s] browsers asked ${d('browserApi')} API calls -> brain saw ${d('brainApi')}` +
    ` (cache ${d('cacheHits')}); ${stats.wsBrowsers} browser socket(s) on 1 upstream; errors ${d('errors')}`);
  last = { ...stats };
}, 10000).unref();
