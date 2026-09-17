#!/usr/bin/env node
// mock-node.js — a fake DustGate SECONDARY node (the dumb end of the star).
//
// Usage:
//   cd tools && node mock-node.js [port]
//
// This is the JS twin of firmware/node/dustgate_node.cpp. It speaks the
// NodeLink protocol over a WebSocket at /nodelink and does exactly what the real
// servo-only board does: accept already-resolved SET frames and "move" a channel
// to an angle. It owns no topology, computes no routing, and never decides
// anything — that asymmetry IS the protocol (see shared/device-model/nodelink.js).
//
// Same discipline as mock-api.js: the frame shapes and validation come from the
// shared model (nodelink.js), not from hand-rolled JSON here, so this mock can't
// drift from the contract the firmware compiles against.
//
// Behaviours deliberately mirrored from the C++ node, because the conformance
// suite asserts each one:
//   • HELLO with a wrong protocol version → close the socket, don't half-speak it
//   • SET that fails validation           → ACK{ok:false, err}
//   • SET with drive:"linear"             → ACK{ok:false} (no stepper on a node)
//   • SET with an out-of-range channel    → ACK{ok:false}
//   • unknown frame type                  → ignored, no reply
//   • ACK means ACCEPTED; arrival is a separate STATE{moving:false}
//   • DISCONNECT → HOLD. No servo is moved. Ever.
//
// Sim affordance (no firmware analogue): GET /sim/servos returns the committed
// angle per channel, so a test can assert where a valve actually ended up rather
// than only that a frame was accepted.

'use strict';

const http = require('http');
const { WebSocketServer } = require('ws');
const NL = require('../shared/device-model/nodelink.js');

const PORT       = Number(process.argv[2] || 3001);
const NODE_ID    = process.env.MOCK_NODE_ID || 'dustgate-node-1';
const BOARD      = process.env.MOCK_NODE_BOARD || 'qtpy_s3';
const FW         = '1.0.0-mock';
const SERVO_COUNT = 4;
// How many current clamps this simulated board claims. One by default: the
// thing most worth exercising is a shop that HAS one.
const MOCK_CT = Number(process.env.MOCK_NODE_CT ?? 1);

// How long a simulated sweep takes. Short enough to keep the suite fast, long
// enough that "moving" is observably a state and not an instant.
const MOVE_MS = Number(process.env.MOCK_NODE_MOVE_MS || 120);

// Committed angle per channel; undefined until first commanded (matching a real
// servo, whose position is unknown until it's been driven once).
const servoAngles = {};
let holdAtRest = {};

// Everything the node is currently doing, so a disconnect can be asserted to
// change NOTHING about it.
let inFlight = null;   // { channel, selectorId, stateId, timer }

// ── sensors (nodelink.js CONFIG / SENSE) ───────────────────────────────────
//
// What the primary has told this board it is wired to, and what each sensor
// currently reads. The real node decides the bit from an RMS loop it runs
// itself; the mock STAGES it over HTTP, the same way it stages everything else
// it has no hardware for. That is the honest simulation — the wire shape is
// identical, and nothing here pretends to measure anything.
let sensors = [];          // [{ sensorId, kind, channel }]
const sensorOn = {};       // sensorId → boolean
let senseTimer = null;

const server = http.createServer((req, res) => {
  if (req.url === '/sim/servos') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ nodeId: NODE_ID, board: BOARD, angles: servoAngles, holdAtRest }));
    return;
  }
  // STAGE a sensor reading: POST/GET /sim/sense?id=planer-ct&on=1
  //
  // The node is the only thing that can decide this bit for real (RFC §5.4b),
  // so there is no way to "simulate a current" that would mean anything. Stage
  // the decision itself and let the frame be exactly what a real board sends.
  if (req.url && req.url.startsWith('/sim/sense')) {
    const q = new URL(req.url, 'http://localhost').searchParams;
    const id = q.get('id') || '';
    const on = q.get('on') === '1' || q.get('on') === 'true';
    if (!sensors.some((s) => s.sensorId === id)) {
      res.writeHead(404, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ ok: false, err: `no sensor "${id}" configured`, configured: sensors }));
      return;
    }
    setSense(id, on);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ ok: true, sensorId: id, on }));
    return;
  }
  if (req.url === '/sim/sensors') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ nodeId: NODE_ID, sensors, readings: sensorOn }));
    return;
  }
  res.writeHead(404); res.end();
});

const wss = new WebSocketServer({ server, path: '/nodelink' });

// THE CLAIM (nodelink.js hello / welcome). A node belongs to one primary; the
// first to complete a handshake owns it, and only a user-confirmed takeover
// moves ownership. The real node persists this in NVS; the mock keeps it in
// memory, which is the one difference and is stated in the log at startup.
//
// DUSTGATE_NODE_OWNER pre-claims the node, so the conformance suite (and anyone
// on the bench) can reproduce "this board already belongs to someone else"
// without needing a second primary running.
let owner = process.env.DUSTGATE_NODE_OWNER || '';
// Which socket passed the handshake as the owner. Holding a connection is not
// permission — an accepted WELCOME is.
let ownerSocket = null;

wss.on('connection', (ws) => {
  console.log('[NODE] Primary connected.');

  ws.on('message', (raw) => {
    let f;
    try { f = JSON.parse(raw.toString()); } catch { return; }   // malformed → ignore
    if (!f || typeof f !== 'object' || typeof f.t !== 'string') return;

    if (f.t === 'HELLO') {
      // Version mismatch is a refusal, not a negotiation. A node that
      // half-understands the primary is more dangerous than one that's absent.
      if (f.v !== NL.NODELINK_VERSION) {
        console.log(`[NODE] HELLO version ${f.v} != ${NL.NODELINK_VERSION} — refusing.`);
        ws.close();
        return;
      }
      const asker = f.primaryId || '';
      let accepted = true;
      if (!owner) {
        owner = asker;
        console.log(`[NODE] Adopted by ${asker}.`);
      } else if (owner === asker) {
        // ordinary reconnect
      } else if (f.takeover) {
        console.log(`[NODE] TAKEOVER (user-confirmed): ${owner} -> ${asker}`);
        owner = asker;
      } else {
        accepted = false;
        console.log(`[NODE] REFUSED ${asker} — this node belongs to ${owner}.`);
      }
      if (accepted) ownerSocket = ws;
      // The socket stays OPEN on a refusal: the refused primary has to read
      // claimedBy to tell its user who holds the board, and a closed socket is
      // indistinguishable from a node that is simply offline.
      // caps.ct: a clamp is DECLARED, never discovered — nothing on the network
      // can find one — so the board saying so is the only way the UI learns it
      // exists. MOCK_NODE_CT=0 turns it off, to see the empty tray.
      send(ws, NL.welcome(NODE_ID, BOARD, FW,
                          { servos: SERVO_COUNT, linear: 0, ct: MOCK_CT },
                          owner, accepted));
      return;
    }

    if (f.t === 'PING') { send(ws, NL.pong()); return; }

    if (f.t === 'CONFIG') {
      // Same gate as SET: an accepted WELCOME earns the right to configure, not
      // merely holding a socket. A board that anyone could re-point at a
      // different sensor is a board with no claim at all.
      if (ws !== ownerSocket) {
        console.log(`[NODE] CONFIG REFUSED — not the owner (${owner}).`);
        send(ws, NL.ack(f.seq ?? 0, false, 'not the owner of this node'));
        return;
      }
      // Validate through the SHARED validator — the same rules the firmware's
      // parseConfigFrame() enforces, which is what keeps the two from drifting.
      const errs = NL.validateFrame(f, 'p2s');
      if (errs.length) {
        console.log(`[NODE] CONFIG MALFORMED — ${errs[0]}`);
        send(ws, NL.ack(f.seq ?? 0, false, errs[0]));
        return;
      }
      // ALL OR NOTHING, and a WHOLE new list. An empty array means "report
      // nothing", which is the same state as never having been configured — so
      // there is no third case, here or in the firmware.
      // Spread, so the CT tuning (tripRatio / minCounts / clearRatio) is KEPT
      // and visible on /sim/servos rather than silently dropped. The mock does
      // not act on it and honestly cannot: it stages the bit instead of running
      // an RMS loop, so there is no floor to take a ratio against. Carrying it
      // still earns its keep — it is how you check from a browser that a primary
      // actually sent what it thinks it sent.
      sensors = f.sensors.map((sen) => ({ ...sen }));
      for (const id of Object.keys(sensorOn)) {
        if (!sensors.some((sen) => sen.sensorId === id)) delete sensorOn[id];
      }
      for (const sen of sensors) {
        if (sensorOn[sen.sensorId] === undefined) sensorOn[sen.sensorId] = false;
      }
      console.log(`[NODE] CONFIG: ${sensors.length ? sensors.map((x) => `${x.sensorId}@ch${x.channel}`).join(', ') : '(nothing)'}`);
      send(ws, NL.ack(f.seq, true));
      // Report immediately rather than waiting out a repeat interval: the
      // primary has just said what it is watching and should not have to sit
      // through SENSE_REPEAT_MS of not knowing.
      reportAll();
      startSenseTimer();
      return;
    }

    if (f.t === 'SET') {
      // An accepted WELCOME is what earns the right to command — not merely
      // having a socket open. Checked per SET, since that is the frame that
      // moves a real valve.
      if (ws !== ownerSocket) {
        console.log(`[NODE] SET REFUSED — not the owner (${owner}).`);
        send(ws, NL.ack(f.seq ?? 0, false, 'not the owner of this node'));
        return;
      }
      // Validate through the SHARED validator — the same rules the firmware's
      // parseSetFrame() enforces. A node moves only when told exactly where.
      const errs = NL.validateFrame(f, 'p2s');
      if (errs.length) { send(ws, NL.ack(f.seq ?? 0, false, errs[0])); return; }

      if (f.drive !== 'servo') {
        send(ws, NL.ack(f.seq, false, 'no linear actuator on this node'));
        return;
      }
      if (f.channel < 0 || f.channel >= SERVO_COUNT) {
        send(ws, NL.ack(f.seq, false, 'no such channel'));
        return;
      }

      send(ws, NL.ack(f.seq, true));   // accepted — not yet arrived

      // A newer SET supersedes an unfinished one (the primary serializes moves,
      // so this can only mean the earlier one is stale).
      if (inFlight) clearTimeout(inFlight.timer);

      send(ws, NL.state(f.selectorId, f.stateId, true));
      inFlight = {
        channel: f.channel,
        selectorId: f.selectorId,
        stateId: f.stateId,
        timer: setTimeout(() => {
          servoAngles[f.channel] = f.angle;
          holdAtRest[f.channel]  = !!f.holdAtRest;
          inFlight = null;
          send(ws, NL.state(f.selectorId, f.stateId, false));
        }, MOVE_MS),
      };
      return;
    }

    // Unknown frame type — ignore rather than guess. No reply.
  });

  ws.on('close', () => {
    // HOLD. Deliberately nothing here: no servo is moved, no state is reset, no
    // timer is cancelled. Losing the primary mid-cut must never slam a gate.
    console.log('[NODE] Primary disconnected — holding all gates.');
  });
});

function send(ws, frame) {
  if (ws.readyState === 1) ws.send(JSON.stringify(frame));
}

/**
 * SENSE on CHANGE, and again every SENSE_REPEAT_MS.
 *
 * The change is what makes a tool switching on a sub-second event. The repeat
 * only stops ONE dropped frame leaving the primary permanently wrong about a
 * tool, which an edge-only protocol would — see the constants in nodelink.js.
 *
 * Sent to the OWNER only. A refused primary holds an open socket so it can read
 * claimedBy, and feeding it another shop's tool states would be the same
 * silent-theft shape the claim exists to prevent.
 */
function reportSense(sensorId) {
  if (!ownerSocket) return;
  // `level` is omitted: a multiple of the trip point is a real measurement on a
  // real board, and inventing a plausible-looking one here is exactly the kind
  // of fake number a mock should never emit.
  // TELEMETRY IS SYNTHESISED, and honestly so. The mock has no ADC, so there is
  // nothing to measure — but a frame with no amps in it would make the Boards
  // page look broken against the mock while working against a board, which is
  // exactly the kind of difference that wastes an afternoon. The numbers track
  // the staged bit and are deliberately round: a real clamp never reads 5.00.
  const on = !!sensorOn[sensorId];
  const FLOOR_A = 0.20, TRIP_A = 0.80;
  send(ownerSocket, NL.sense(sensorId, on, on ? 6.2 : 0.26,
                             on ? 5.0 : FLOOR_A, FLOOR_A, TRIP_A));
}

function reportAll() {
  for (const sen of sensors) reportSense(sen.sensorId);
}

function setSense(sensorId, on) {
  const was = !!sensorOn[sensorId];
  sensorOn[sensorId] = !!on;
  if (was !== !!on) {
    console.log(`[NODE] SENSE ${sensorId}: ${on ? 'ON' : 'off'}`);
    reportSense(sensorId);   // on CHANGE, immediately
  }
}

function startSenseTimer() {
  if (senseTimer) clearInterval(senseTimer);
  senseTimer = null;
  if (!sensors.length) return;
  senseTimer = setInterval(reportAll, NL.SENSE_REPEAT_MS);
  // Don't hold the process open on the repeat alone — the suite starts and
  // stops this mock many times, and a live interval would keep node running
  // after the server closes.
  if (senseTimer.unref) senseTimer.unref();
}

server.listen(PORT, () => {
  console.log(`Mock DustGate node "${NODE_ID}" (${BOARD}) on ws://localhost:${PORT}/nodelink`);
  console.log(`  servo channels: ${SERVO_COUNT}, sim sweep: ${MOVE_MS}ms`);
  console.log(`  GET http://localhost:${PORT}/sim/servos — committed angles`);
  console.log(`  GET http://localhost:${PORT}/sim/sensors — configured sensors + readings`);
  console.log(`  GET http://localhost:${PORT}/sim/sense?id=<id>&on=1 — stage a tool on/off`);
});
