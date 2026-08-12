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
const BOARD      = process.env.MOCK_NODE_BOARD || 'qtpy_c3';
const FW         = '1.0.0-mock';
const SERVO_COUNT = 4;

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

const server = http.createServer((req, res) => {
  if (req.url === '/sim/servos') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ nodeId: NODE_ID, board: BOARD, angles: servoAngles, holdAtRest }));
    return;
  }
  res.writeHead(404); res.end();
});

const wss = new WebSocketServer({ server, path: '/nodelink' });

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
      send(ws, NL.welcome(NODE_ID, BOARD, FW, { servos: SERVO_COUNT, linear: 0 }));
      return;
    }

    if (f.t === 'PING') { send(ws, NL.pong()); return; }

    if (f.t === 'SET') {
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

server.listen(PORT, () => {
  console.log(`Mock DustGate node "${NODE_ID}" (${BOARD}) on ws://localhost:${PORT}/nodelink`);
  console.log(`  servo channels: ${SERVO_COUNT}, sim sweep: ${MOVE_MS}ms`);
  console.log(`  GET http://localhost:${PORT}/sim/servos — committed angles`);
});
