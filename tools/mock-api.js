#!/usr/bin/env node
// mock-api.js — local dev server that mimics the ESP32 HTTP + WebSocket API.
//
// Usage:
//   cd tools && npm install && node mock-api.js
//   # Then in dustgate-ui:
//   ng serve --proxy-config proxy.conf.json
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
const url    = require('url');
const { WebSocketServer } = require('ws');
const M = require('../shared/device-model/device-model.js');
const TOPO = require('../shared/device-model/topology.js');           // topology validator
const SHOP = require('../shared/device-model/shop.js');               // shop container + validator
const TD   = require('../shared/device-model/topology-device.js');    // topology device sim

const PORT    = 3000;
const API_KEY = 'dev-mock-key-1234';
const VERSION = '1.0.0-mock';

// ── Canonical device instance ───────────────────────────────────────────────
const d = M.createDevice();

// ── topology-native device (additive; null until a topology is PUT) ───────
let td = null;
// The document EXACTLY as it was PUT, served back verbatim by GET.
//
// The device sim normalises a v1 topology into a shop on the way in (asShop), so
// td.topology is no longer necessarily what the caller sent. The firmware has the
// same split — TopologyStore keeps the raw bytes it was handed, TopologyRuntime
// parses them — and it matters for the same reason: a GET that silently returned
// a migrated document would make an older board look like it had rewritten a
// layout nobody asked it to touch.
let rawTopology = null;

// Last angle commanded to each servo channel by the setup jog, keyed
// "<controllerId>:<channel>". Keyed by board because every board numbers its
// channels 0-3 — firmware had a bug here where a jog aimed at a node moved the
// primary's servo on the same channel, and a mock that ignores controllerId
// can't catch its return.
const servoAngles = {};

// ── Paired boards (mirrors control/NodeRegistry.h) ──────────────────────────
// DELIBERATELY NOT part of the topology. On the device this lives in NVS and
// survives a layout wipe; here it survives a PUT /api/topology. Keeping that
// independence in the mock is the point — the boards screen must work with no
// topology at all, and that's only tested if the mock can be in that state.
// A friendly `name` lives here too, for the same reason.
const pairedNodes = [];   // [{ host, name }]

// Boards "on the network" for discovery to find. dustgate-node-2 is deliberately
// unreachable once paired: an offline board is the interesting case for the UI
// and the hard one to stage on a bench where everything works.
const NETWORK_BOARDS = [
  { host: 'dustgate-node-1', ip: '192.168.87.61', board: 'qtpy_s3', servos: 4 },
  { host: 'dustgate-node-2', ip: '192.168.87.62', board: 'devkitc', servos: 4 },
  // A board that belongs to SOMEONE ELSE's primary. It is listed, not hidden:
  // a board sitting there powered and answering, absent from the list, looks
  // exactly like a board that never announced itself — and those two have
  // completely different fixes. `claimedBy` + `takeable` are the same fields
  // GET /api/nodes uses for a refused link, so there is one vocabulary for
  // "someone else has this" wherever it shows up. Firmware learns it from the
  // node's `owner` mDNS TXT record.
  { host: 'dustgate-node-3', ip: '192.168.87.63', board: 'xiao_c5', servos: 4,
    claimedBy: 'dustgate-garage', takeable: true },
  // Mirrors the demo's fourth board — see demoNodes in demo-api.service.ts. It
  // exists so there is always one board that is present, free, and NOT already
  // in the seed shop's controllers[].
  { host: 'dustgate-node-4', ip: '192.168.87.64', board: 'xiao_c5', servos: 4 },
];
const OFFLINE_HOSTS = ['dustgate-node-2'];

const bareHost = (h) => String(h || '').toLowerCase().replace(/\.local\.?$/, '');
const findPaired = (h) => pairedNodes.find(n => bareHost(n.host) === bareHost(h));

/** Link state for GET /api/nodes — the shape RemoteActuatorBus::info() feeds. */
function nodeLinkState(entry) {
  const known  = NETWORK_BOARDS.find(b => bareHost(b.host) === bareHost(entry.host));
  const online = !OFFLINE_HOSTS.includes(bareHost(entry.host));
  return {
    id:       bareHost(entry.host),   // the host IS the controllerId
    host:     entry.host,
    name:     entry.name || '',
    online,
    lastSeen: online ? Date.now() : 0,
    board:    known ? known.board : '',
    fw:       online ? '1.0.0-mock' : '',
    caps:     { servos: known ? known.servos : 0, linear: 0 },
  };
}

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
  const key = req.headers['x-api-key'];
  if (key !== API_KEY) return json(res, { error: 'unauthorized' }, 401);

  // ── Routes (thin: parse → model call → respond) ────────────────────────────

  if (pathname === '/api/motion' && req.method === 'GET') return json(res, M.statusView(d));
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

  // Rename a plug (label only — the device reattaches its own owner suffix).
  if (pathname === '/api/outlets/name' && req.method === 'POST') {
    return body(req, data => runModel(res, () =>
      json(res, M.nameOutlet(d, data.ip, data.label, data.takeover))));
  }

  // Repoint a plug that reports to another controller (RFC §8). Its own route,
  // never a flag on pairing — nothing automatic can reach it.
  if (pathname === '/api/outlets/takeover' && req.method === 'POST') {
    return body(req, data => runModel(res, () => json(res, M.takeoverOutlet(d, data.ip))));
  }

  // The device half of unpairing. Best-effort: the caller drops sensor.outlet
  // whatever this says, so an unplugged plug can still be detached.
  if (pathname === '/api/outlets/release' && req.method === 'POST') {
    return body(req, data => runModel(res, () => json(res, M.releaseOutlet(d, data.ip))));
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
    return body(req, data => { M.setHomedLeft(d, data.homedLeft); json(res, { ok: true }); });
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

  if (pathname === '/api/clearcal' && req.method === 'POST') {
    M.clearCal(d); broadcast(); return json(res, { ok: true });
  }
  if (pathname === '/api/reboot' && req.method === 'POST') {
    console.log('[MOCK] reboot requested — ignoring');
    return json(res, { ok: true });
  }

  // ── Topology API ─────────────────────────────────────────────────────────
  // PUT the whole topology (validated); GET it back; GET live status; and a
  // sim-only tool-power inject (real firmware gets power from Shelly plugs, so
  // it wouldn't implement /sim/tool — the demo/mock use it to drive routing).
  if (pathname === '/api/topology' && req.method === 'PUT') {
    return body(req, data => {
      // Accepts both shapes, like the firmware: a shop is validated as a shop, a
      // schemaVersion-1 topology as a topology.
      const v = SHOP.isShop(data) ? SHOP.validateShop(data) : TOPO.validateTopology(data);
      if (!v.ok) return json(res, { error: 'invalid topology', errors: v.errors }, 400);
      td = TD.createTopologyDevice(data);
      rawTopology = data;
      // The plugs this shop is paired to are on the simulated network from here
      // on — otherwise every paired plug reads as not responding and the rename
      // and release paths can't be walked at all. See adoptOutlets().
      M.adoptOutlets(d, data);
      json(res, { ok: true });
    });
  }
  if (pathname === '/api/topology' && req.method === 'GET') {
    return td ? json(res, rawTopology) : json(res, { error: 'no topology configured' }, 404);
  }
  if (pathname === '/api/status' && req.method === 'GET') {
    return td ? json(res, TD.statusView(td)) : json(res, { error: 'no topology configured' }, 404);
  }
  // Setup-only servo jog. Nothing to move here, so just range-check like the firmware
  // does and remember the angle — enough for the gate configurator to be walked end to
  // end against the mock.
  if (pathname === '/api/servo/jog' && req.method === 'POST') {
    return body(req, data => {
      const ch = Number(data.channel);
      if (!Number.isInteger(ch) || ch < 0 || ch >= 4) return json(res, { error: 'channel out of range' }, 400);
      // controllerId says WHICH board. Absent (or the primary) means this one; a
      // named board must actually be paired, or the jog would silently land on
      // the primary's servo of the same channel — the bug this field exists to
      // prevent, so the mock refuses it rather than quietly accepting.
      const ctrl = String(data.controllerId || '').trim();
      const remote = ctrl && ctrl !== 'primary';
      if (remote && !findPaired(ctrl))
        return json(res, { error: `no paired board '${ctrl}'` }, 404);
      const key = `${remote ? bareHost(ctrl) : 'primary'}:${ch}`;
      if (data.detach === true) { return json(res, { ok: true }); }
      const angle = Number(data.angle);
      if (!Number.isFinite(angle) || angle < 0 || angle > 180)
        return json(res, { error: 'angle out of range (0-180)' }, 400);
      servoAngles[key] = angle;
      json(res, { ok: true });
    });
  }
  // ── Paired boards ─────────────────────────────────────────────────────────
  // NOTE for anyone porting a new /api/nodes/<x> route to the firmware:
  // ESPAsyncWebServer matches by PREFIX and tries handlers in registration
  // order, so "/api/nodes" will happily swallow "/api/nodes/discover"
  // unless the longer path is registered first. This mock compares exact
  // strings and doesn't care — which is exactly why the trap went unnoticed
  // here once already.
  if (pathname === '/api/nodes/discover' && req.method === 'GET') {
    // Every board on the "network", paired or not. The UI filters out the ones
    // it already has; firmware likewise reports what the mDNS sweep saw.
    return json(res, NETWORK_BOARDS.map(b => ({ ...b })));
  }
  if (pathname === '/api/nodes/pair' && req.method === 'POST') {
    return body(req, data => {
      const host = String(data.host || '').trim();
      if (!host) return json(res, { error: "missing 'host'" }, 400);
      if (data.remove === true) {
        const i = pairedNodes.findIndex(n => bareHost(n.host) === bareHost(host));
        if (i >= 0) pairedNodes.splice(i, 1);
        return json(res, { ok: true });
      }
      // Idempotent, and re-pairing with a name is how a rename is applied —
      // one code path for both, same as the device.
      const existing = findPaired(host);
      if (existing) {
        if (data.name) existing.name = String(data.name);
      } else {
        if (pairedNodes.length >= 3) return json(res, { error: 'no free link slots' }, 409);
        pairedNodes.push({ host, name: data.name ? String(data.name) : '' });
      }
      json(res, { ok: true });
    });
  }
  if (pathname === '/api/nodes' && req.method === 'GET') {
    // Always 200, never 404: an empty list means "nothing paired", which is a
    // legitimate state the boards screen renders. The device publishes this
    // outside its topology gate for the same reason.
    return json(res, { nodes: pairedNodes.map(nodeLinkState) });
  }

  // Manual tool switch — the Live view's rows. Same lever as /sim/tool, but it's
  // a REAL control path that firmware implements, where /sim/tool is mock-only.
  // Modelled as a synthetic wattage so there's one definition of "active".
  if (pathname === '/api/tool' && req.method === 'POST') {
    return body(req, data => {
      if (!td) return json(res, { error: 'no topology configured' }, 404);
      if (!data.toolId) return json(res, { error: "missing 'toolId'" }, 400);
      if (typeof data.on !== 'boolean') return json(res, { error: "missing 'on'" }, 400);
      // An unknown id would set a wattage nothing reads and route nothing — a
      // switch that reports success and does nothing. Refuse it instead.
      // Checked against MACHINES: `toolId` names the thing you switch on, which
      // is a machine even when it has one port.
      const known = SHOP.machinesOf(td.topology).some(m => m.id === data.toolId);
      if (!known) return json(res, { error: `no tool '${data.toolId}'` }, 404);
      // 3x the trip point, not a 100000 sentinel — the canvas draws this number
      // on the tool, and 100 kW is 833 A at 120 V. Matches the firmware's
      // manualWattsFor() and the demo service.
      const trip = TD.toolThreshold(td.topology, data.toolId) || 0;
      TD.setToolPower(td, data.toolId, data.on ? Math.round((trip > 0 ? trip * 3 : 15)) : 0);
      json(res, { ok: true });
    });
  }

  // POST /api/collector { systemId?, on } — run ONE system's blower by hand.
  // `systemId` is optional: a shop with one blower has exactly one answer.
  if (pathname === '/api/collector' && req.method === 'POST') {
    return body(req, data => {
      if (!td) return json(res, { error: 'no topology configured' }, 404);
      if (typeof data.on !== 'boolean') return json(res, { error: "missing 'on'" }, 400);
      const systems = SHOP.systemsOf(td.topology);
      const sysId = data.systemId || (systems[0] && systems[0].id);
      // Refused rather than ignored, like an unknown toolId: a switch that reports
      // success and does nothing is the failure mode worth being loud about.
      if (!systems.some(sys => sys.id === sysId)) {
        return json(res, { error: `no system '${data.systemId || ''}'` }, 404);
      }
      TD.setCollectorManual(td, sysId, data.on);
      json(res, { ok: true });
    });
  }

  if (pathname === '/api/sim/tool' && req.method === 'POST') {
    return body(req, data => {
      if (!td) return json(res, { error: 'no topology configured' }, 404);
      TD.setToolPower(td, data.toolId, Number(data.watts) || 0);
      json(res, TD.statusView(td));
    });
  }

  // Sim-only, like /sim/tool: stage a collector plug failure so the states that
  // matter can be seen without tripping a real breaker. `fault` is null (healthy),
  // 'dead' (relay closes, nothing draws) or 'offline' (the plug stops answering).
  // Real firmware has no analogue — its plug either works or it doesn't.
  if (pathname === '/api/sim/collector' && req.method === 'POST') {
    return body(req, data => {
      if (!td) return json(res, { error: 'no topology configured' }, 404);
      if (!data.systemId) return json(res, { error: "missing 'systemId'" }, 400);
      const r = TD.setCollectorPlugFault(td, data.systemId, data.fault || null);
      if (!r.ok) return json(res, { error: 'unknown system' }, 404);
      json(res, TD.statusView(td));
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
  console.log(`\n  In dustgate-ui:  ng serve --proxy-config proxy.conf.json\n`);
});
