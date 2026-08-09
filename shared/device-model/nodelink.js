// nodelink.js — the primary↔secondary node protocol (v2 Phase 3).
//
// DustGate v2 is a STAR: one primary owns the GUI, the topology, the Shelly
// polling and the routing brain; secondaries are dumb actuator banks. This file
// is the contract between them — the frames, their shapes, and the invariants
// that make a secondary genuinely dumb.
//
// THE LOAD-BEARING DESIGN DECISION: the primary resolves every state into a
// CONCRETE REALIZATION before sending it. A SET frame carries `angle` (already
// referenceAngle + offsetDeg, clamped) or `positionMm`, never "put gate3 in the
// open state". A secondary therefore needs no topology, no router, no schema
// version, and no calibration data — it needs a PWM channel and a number. That
// is what lets the cheap servo-only node exist at all, and it means a schema
// change never has to be rolled out to every board in the shop.
//
// Transport is one persistent WebSocket, primary → secondary (the primary dials
// out; secondaries just listen). Framing is JSON, one frame per message.
// ESP-NOW can replace the transport later without touching these shapes — the
// frames are the contract, the socket is an implementation detail.
//
// PURE. No I/O. Consumed by the firmware (as a spec), the mock secondary, and
// the conformance suite.

'use strict';

/** Protocol version. Bump on any incompatible frame change. */
const NODELINK_VERSION = 1;

/** Frame types, primary → secondary. */
const P2S = ['HELLO', 'SET', 'PING'];
/** Frame types, secondary → primary. */
const S2P = ['WELCOME', 'ACK', 'STATE', 'PONG'];

/**
 * Liveness. The primary PINGs this often; a secondary that hasn't answered
 * within PONG_TIMEOUT_MS is considered offline and its selectors unreachable.
 * Deliberately tighter than a tool change is long: the UI must be able to grey
 * out a dead board before someone switches on a saw behind it.
 */
const PING_INTERVAL_MS = 2000;
const PONG_TIMEOUT_MS = 6000;

/** Reconnect backoff for a primary that can't reach a secondary. */
const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 15000;

/**
 * @typedef {Object} HelloFrame     P→S, first frame after the socket opens.
 * @property {'HELLO'} t
 * @property {number}  v            NODELINK_VERSION
 * @property {string}  primaryId    controllerId of the primary
 * @property {string}  nodeId       controllerId the primary believes this node is
 *
 * @typedef {Object} WelcomeFrame   S→P, the answer to HELLO.
 * @property {'WELCOME'} t
 * @property {number}  v
 * @property {string}  nodeId       who this board actually is
 * @property {string}  board        build target ("devkitc", "qtpy_c3", …)
 * @property {string}  fw           firmware version string
 * @property {{servos:number, linear:number}} caps   actuator budget on this board
 *
 * @typedef {Object} SetFrame       P→S, move one actuator. Already resolved.
 * @property {'SET'}   t
 * @property {number}  seq          monotonic per connection; echoed in ACK
 * @property {string}  selectorId   opaque to the secondary — used only in reports
 * @property {string}  stateId      opaque; carried so STATE reports are meaningful
 * @property {number}  channel      which servo channel (or stepper index)
 * @property {'servo'|'linear'} drive
 * @property {number} [angle]       drive==='servo': absolute degrees, 0–180
 * @property {number} [positionMm]  drive==='linear': absolute mm from the datum
 * @property {boolean}[holdAtRest]  servo only; default false (move then detach)
 *
 * @typedef {Object} AckFrame       S→P, the SET was accepted or refused.
 * @property {'ACK'}   t
 * @property {number}  seq
 * @property {boolean} ok
 * @property {string} [err]
 *
 * @typedef {Object} StateFrame     S→P, unsolicited on arrival at a state.
 * @property {'STATE'} t
 * @property {string}  selectorId
 * @property {string}  stateId
 * @property {boolean} moving
 */

/** Frames the primary sends. */
function hello(primaryId, nodeId) {
  return { t: 'HELLO', v: NODELINK_VERSION, primaryId, nodeId };
}
function ping() {
  return { t: 'PING' };
}

/**
 * Build a SET frame from a selector + target state. This is the one place that
 * turns model concepts into wire values; keeping it here means the firmware and
 * the mock resolve angles identically or the conformance suite fails.
 *
 * @param {number} seq
 * @param {import('./topology').Selector} sel
 * @param {string} stateId
 * @param {number|null} realization  resolved angle (servo) or mm (linear)
 * @returns {SetFrame}
 */
function set(seq, sel, stateId, realization) {
  const isServo = sel.kind === 'servoGate' || sel.kind === 'servoManifold';
  const f = {
    t: 'SET',
    seq,
    selectorId: sel.id,
    stateId,
    channel: isServo ? (sel.servo && sel.servo.channel) || 0
                     : (sel.linear && sel.linear.channel) || 0,
    drive: isServo ? 'servo' : 'linear',
  };
  if (isServo) {
    f.angle = realization;
    f.holdAtRest = !!(sel.servo && sel.servo.holdAtRest);
  } else {
    f.positionMm = realization;
  }
  return f;
}

/** Frames the secondary sends. */
function welcome(nodeId, board, fw, caps) {
  return { t: 'WELCOME', v: NODELINK_VERSION, nodeId, board, fw, caps };
}
function ack(seq, ok, err) {
  const f = { t: 'ACK', seq, ok: !!ok };
  if (err) f.err = err;
  return f;
}
function state(selectorId, stateId, moving) {
  return { t: 'STATE', selectorId, stateId, moving: !!moving };
}
function pong() {
  return { t: 'PONG' };
}

/**
 * Validate a decoded frame. Returns an array of problem strings (empty = valid).
 * Both ends validate: a secondary must never act on a malformed SET, and a
 * primary must never trust a WELCOME that claims a different protocol version.
 *
 * @param {any} f
 * @param {'p2s'|'s2p'} direction  which way the frame is travelling
 */
function validateFrame(f, direction) {
  const errs = [];
  if (!f || typeof f !== 'object') return ['frame must be an object'];
  const allowed = direction === 'p2s' ? P2S : S2P;
  if (!allowed.includes(f.t)) {
    return [`unknown frame type "${f.t}" for direction ${direction}`];
  }

  const num = (k, lo, hi) => {
    if (typeof f[k] !== 'number' || Number.isNaN(f[k])) errs.push(`${f.t}.${k} must be a number`);
    else if (f[k] < lo || f[k] > hi) errs.push(`${f.t}.${k} out of range (${lo}..${hi})`);
  };
  const str = (k) => {
    if (typeof f[k] !== 'string' || !f[k]) errs.push(`${f.t}.${k} must be a non-empty string`);
  };

  switch (f.t) {
    case 'HELLO':
      if (f.v !== NODELINK_VERSION) errs.push(`HELLO.v ${f.v} != ${NODELINK_VERSION}`);
      str('primaryId'); str('nodeId');
      break;
    case 'WELCOME':
      if (f.v !== NODELINK_VERSION) errs.push(`WELCOME.v ${f.v} != ${NODELINK_VERSION}`);
      str('nodeId'); str('board');
      if (!f.caps || typeof f.caps.servos !== 'number' || typeof f.caps.linear !== 'number') {
        errs.push('WELCOME.caps must be {servos:number, linear:number}');
      }
      break;
    case 'SET':
      num('seq', 0, Number.MAX_SAFE_INTEGER);
      str('selectorId'); str('stateId');
      if (f.drive !== 'servo' && f.drive !== 'linear') errs.push('SET.drive must be servo|linear');
      num('channel', 0, 15);
      // A SET with no realization is the failure this protocol exists to
      // prevent: it would leave the secondary guessing where to point a valve.
      if (f.drive === 'servo') num('angle', 0, 180);
      if (f.drive === 'linear') num('positionMm', -10000, 10000);
      break;
    case 'ACK':
      num('seq', 0, Number.MAX_SAFE_INTEGER);
      if (typeof f.ok !== 'boolean') errs.push('ACK.ok must be a boolean');
      break;
    case 'STATE':
      str('selectorId'); str('stateId');
      if (typeof f.moving !== 'boolean') errs.push('STATE.moving must be a boolean');
      break;
    case 'PING':
    case 'PONG':
      break;
  }
  return errs;
}

module.exports = {
  NODELINK_VERSION, P2S, S2P,
  PING_INTERVAL_MS, PONG_TIMEOUT_MS, RECONNECT_MIN_MS, RECONNECT_MAX_MS,
  hello, welcome, set, ack, state, ping, pong,
  validateFrame,
};
