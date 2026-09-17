// nodelink.js — the primary↔secondary node protocol.
//
// DustGate is a STAR: one primary owns the GUI, the topology, the Shelly
// polling and the routing brain. This file is the contract between them — the
// frames, their shapes, and the invariant that makes the split work.
//
// THE LOAD-BEARING DESIGN DECISION: the primary resolves every state into a
// CONCRETE REALIZATION before sending it. A SET frame carries `angle` (already
// referenceAngle + offsetDeg, clamped) or `positionMm`, never "put gate3 in the
// open state". A secondary therefore needs no topology, no router, no schema
// version, and no calibration data — it needs a PWM channel and a number. That
// is what lets the cheap servo-only node exist at all, and it means a schema
// change never has to be rolled out to every board in the shop.
//
// WHAT THAT DOES AND DOES NOT SAY (sharpened 2026-09-14). This header used to
// call secondaries "dumb actuator banks", and that description has now been
// wrong twice. The slider node owns a homing sweep; the CT tool-sensing node
// owns a 60 Hz RMS loop. Each was read, at the time, as the boundary eroding.
// Neither is. The invariant was never about intelligence — it is about SCHEMA
// OWNERSHIP:
//
//   A node may own any control loop whose time constant is faster than a WiFi
//   round trip. A node may own no interpretation of the document.
//
// Homing cannot round-trip per step; neither can mains-frequency sampling. Both
// nodes still take resolved numbers off the wire, and report values whose
// meaning does not change when the schema does — so both obey the rule that
// actually earns its keep, which is that the primary can be reflashed alone.
// Stated this way, the next sensor is PREDICTED rather than excused, which is
// the same job CLAUDE.md's constant table does for shared numbers.
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
const P2S = ['HELLO', 'SET', 'CONFIG', 'PING'];
/** Frame types, secondary → primary. */
const S2P = ['WELCOME', 'ACK', 'STATE', 'SENSE', 'PONG'];

// CONFIG and SENSE were added 2026-09-14 WITHOUT bumping NODELINK_VERSION, and
// that is deliberate rather than an oversight. Both ends ignore a frame type
// they do not know (`dustgate_node.cpp`: "unknown frame — ignore rather than
// guess"), so the four combinations all degrade to something safe: an old node
// is never configured and never reports, which leaves it exactly the actuator
// bank it already was; a new node talking to an old primary sends SENSE into a
// void. Nothing silently misbehaves, so a version bump would only force a flash
// of every board in the shop to buy nothing — the precise cost the header says
// this protocol exists to avoid. Bump it when an EXISTING frame changes shape.

/**
 * Liveness. The primary PINGs this often; a secondary that hasn't answered
 * within PONG_TIMEOUT_MS is considered offline and its selectors unreachable.
 * Deliberately tighter than a tool change is long: the UI must be able to grey
 * out a dead board before someone switches on a saw behind it.
 */
const PING_INTERVAL_MS = 2000;
const PONG_TIMEOUT_MS = 6000;

/**
 * How often a node REPEATS its current sensor reading, and how long the primary
 * waits before calling that reading stale.
 *
 * SENSE is sent on every change — that is what makes a tool switching on a
 * sub-second event rather than a polling interval. The repeat exists only so a
 * single dropped frame cannot leave the primary permanently wrong about a tool,
 * which an edge-only protocol would. Same 3× ratio as PING/PONG, for the same
 * reason: two may go missing before anything is declared.
 *
 * Stale is NOT the same as off, and the primary must not conflate them. A node
 * that has stopped reporting while still answering PINGs is a fault; a node that
 * has gone away entirely is the planer being switched off at the wall, which is
 * normal and handled by `Controller.intermittent`. See RFC §5.6a.
 */
const SENSE_REPEAT_MS = 5000;
const SENSE_STALE_MS = 15000;

/**
 * How many sensors one node will accept in a CONFIG.
 *
 * ⚠️ JS↔C++ PAIR — `kMaxSensorsPerNode` in firmware/control/NodeLink.h.
 * The firmware parses into a fixed array, so without the same cap on this side
 * a primary could send eight specs to a board that keeps four and says nothing
 * — the node would report on some tools and be silently deaf to the rest. The
 * refusal has to be symmetrical or it is not a refusal.
 */
const MAX_SENSORS_PER_NODE = 4;

/** Reconnect backoff for a primary that can't reach a secondary. */
const RECONNECT_MIN_MS = 1000;
const RECONNECT_MAX_MS = 15000;

/**
 * @typedef {Object} HelloFrame     P→S, first frame after the socket opens.
 * @property {'HELLO'} t
 * @property {number}  v            NODELINK_VERSION
 * @property {string}  primaryId    controllerId of the primary — ALSO THE CLAIM
 * @property {string}  nodeId       controllerId the primary believes this node is
 * @property {boolean}[takeover]    take this node from its current owner. User-
 *                                  confirmed only; see the claim note below.
 *
 * @typedef {Object} WelcomeFrame   S→P, the answer to HELLO.
 * @property {'WELCOME'} t
 * @property {number}  v
 * @property {string}  nodeId       who this board actually is
 * @property {string}  board        build target ("devkitc", "qtpy_s3", …)
 * @property {string}  fw           firmware version string
 * @property {{servos:number, linear:number, ct?:number}} caps   what this board HAS:
 *                                  its actuator budget, and how many current
 *                                  clamps are wired to it. `ct` absent means 0 —
 *                                  every board that answered before 2026-09-15.
 * @property {string} [claimedBy]   the primary this node belongs to
 * @property {boolean}[accepted]    false = you are NOT my owner; SETs will be
 *                                  refused. Absent means accepted (legacy).
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
 *
 * @typedef {Object} SensorSpec     one entry in CONFIG.sensors.
 * @property {string}  sensorId     OPAQUE TO THE NODE — echoed back in SENSE and
 *                                  never interpreted, exactly as SET.selectorId
 *                                  is. It is the primary's vocabulary.
 * @property {'ct'}    kind         what is wired. 'ct' is the only one so far.
 * @property {number}  channel      which input on THIS BOARD — a hardware fact,
 *                                  the same shape as SET.channel.
 * @property {number} [tripRatio]   OPTIONAL TUNING, all three. Multiple of the
 *                                  board's OWN learned noise floor at which a
 *                                  clamp reads as a running motor.
 * @property {number} [minCounts]   absolute guard in that board's ADC counts,
 *                                  because a ratio against a floor that learns
 *                                  near zero trips on nothing.
 * @property {number} [clearRatio]  hysteresis: the fraction of the trip point it
 *                                  must fall back BELOW before reading off.
 *
 *   Omitting them is normal and means "use whatever you were built with" — the
 *   only thing a primary older than 2026-09-17 can say. They exist so that
 *   retuning a shop is a PRIMARY reflash instead of a ladder and a USB cable at
 *   every node, which is the whole reason a node's firmware can be called
 *   finished. See sensing/CtTrip.h.
 *
 * @typedef {Object} ConfigFrame    P→S, sent AFTER an accepted WELCOME.
 * @property {'CONFIG'}  t
 * @property {number}    seq        ACKed like a SET
 * @property {SensorSpec[]} sensors WHOLE new list — an empty array means "report
 *                                  nothing", which is how sensing is turned off.
 *
 * @typedef {Object} SenseFrame     S→P, on change and every SENSE_REPEAT_MS.
 * @property {'SENSE'} t
 * @property {string}  sensorId     echoed from CONFIG
 * @property {boolean} on           THE ANSWER. One bit, decided on the node.
 * @property {number} [level]       DIAGNOSTIC ONLY — see the builder.
 * @property {number} [amps]        what the clamp reads now. DIAGNOSTIC ONLY.
 * @property {number} [floorA]      the board's learned noise floor, in amps.
 * @property {number} [tripA]       the point `amps` is judged against, in amps.
 * @property {boolean} [fault]      no floor could be learnt; floorA/tripA absent.
 */

/** Frames the primary sends. */
/**
 * HELLO — and, with it, a CLAIM.
 *
 * A node belongs to ONE primary (RFC §8's rule, applied to boards instead of
 * plugs). Before this, a node pointed its "linked client" at whichever primary
 * connected most recently, so a bench brain and a shop brain could both hold
 * sockets and both drive the same servos, with neither told. Same silent-theft
 * shape as an unclaimed smart plug, and worse consequences: two brains fighting
 * over one valve is a gate that contradicts the routing of both shops.
 *
 * FIRST COMPLETED HANDSHAKE WINS, and the node persists that owner, so the
 * claim survives a reboot rather than being re-raced on every power cut.
 *
 * @param {boolean} [takeover]  user-confirmed takeover. NEVER set this
 *   automatically: a primary that retries with `takeover` on refusal would
 *   reduce the whole claim to "whoever asks twice", which is no claim at all.
 */
function hello(primaryId, nodeId, takeover = false) {
  const f = { t: 'HELLO', v: NODELINK_VERSION, primaryId, nodeId };
  if (takeover) f.takeover = true;
  return f;
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

/**
 * CONFIG — tell a node what it is WIRED TO, which is the one thing it cannot
 * work out for itself.
 *
 * Sent after an accepted WELCOME, never before: an unclaimed or refused node has
 * no business being configured, and the primary needs `caps` back before it can
 * say anything sensible anyway.
 *
 * THIS IS NOT TOPOLOGY, and the distinction is the whole reason the frame can
 * exist at all. It carries no elements, no routing and no states — nothing whose
 * MEANING the primary could change underneath a node that was not reflashed.
 * `sensorId` is opaque, `kind` names hardware, `channel` is a pad.
 *
 * THE THRESHOLD SENTENCE, SHARPENED (2026-09-17). This used to read "and no
 * threshold", written when the only threshold in the system was `thresholdW` —
 * and that one is still barred, permanently: watts name a MACHINE, they come out
 * of the document, and a node that acted on one would be interpreting the
 * schema. What the frame now also carries is `tripRatio` / `minCounts` /
 * `clearRatio`, and those are a different animal despite the word: a multiple of
 * THIS BOARD's own learned noise floor, and a guard in THIS BOARD's own ADC
 * counts. They mean nothing off the board they describe, no document supplies
 * them, and they sit in exactly the same class as `channel`. The test to apply
 * to the next field that wants in here is not "is it a number the primary
 * chose" but "could a node act on it without reading the document" — and a
 * node still owns no interpretation of the document, which is the invariant at
 * the top of this file.
 *
 * Why it had to exist: before it, every new node capability had to smuggle its
 * configuration through SET or invent a bespoke frame. This one addition covers
 * the CT tool sensor (RFC §5.6), a bin sensor on a node, and whatever is next.
 *
 * The list is WHOLE, not a delta. A primary that sends `[]` has said "report
 * nothing", and a node that has never been sent a CONFIG reports nothing — so
 * silence and an explicit empty list agree, which is what stops a half-applied
 * configuration from being a state anyone has to reason about.
 *
 * @param {number} seq
 * @param {SensorSpec[]} sensors
 * @returns {ConfigFrame}
 */
function config(seq, sensors) {
  return { t: 'CONFIG', seq, sensors: (sensors || []).map((s) => {
    const out = { sensorId: s.sensorId, kind: s.kind, channel: s.channel };
    // OMITTED RATHER THAN NULLED when a caller has nothing to say. An absent key
    // is what tells a board to keep its own value, and writing `tripRatio: null`
    // would be a present key carrying a number that fails validation — a frame
    // the node refuses WHOLE, taking the sensor list down with it.
    for (const k of ['tripRatio', 'minCounts', 'clearRatio']) {
      if (typeof s[k] === 'number') out[k] = s[k];
    }
    return out;
  }) };
}

/**
 * Frames the secondary sends.
 *
 * @param {string} [claimedBy]  the primary that owns this node
 * @param {boolean} [accepted]  false when the asker is NOT the owner. The
 *   socket is deliberately left OPEN on a refusal: the primary needs to read
 *   `claimedBy` to tell its user who has the board, and a closed socket would
 *   look identical to a node that is simply offline.
 */
/**
 * @param {{servos:number, linear:number, ct?:number}} caps
 *
 * `caps.ct` is the one field here that is not about moving something, and it is
 * the reason a clamp can appear in the UI at all. A plug is DISCOVERED — a
 * subnet sweep finds it at an IP — but a clamp has no address and never will;
 * somebody soldered it to a board. So the board is the only thing that can say
 * it exists, and it says so here, next to the servo count it already reports.
 *
 * REPORTED, NOT CHOSEN, exactly like `caps.servos`: it comes from the pin map,
 * so it cannot disagree with the hardware, and a tray listing no clamps is then
 * the correct empty state rather than a feature nobody turned on.
 */
function welcome(nodeId, board, fw, caps, claimedBy, accepted = true) {
  const f = { t: 'WELCOME', v: NODELINK_VERSION, nodeId, board, fw, caps };
  if (claimedBy) f.claimedBy = claimedBy;
  if (!accepted) f.accepted = false;
  return f;
}

/**
 * Does this WELCOME say we may drive the node?
 *
 * Absent `accepted` means yes — a node built before claims answers exactly as
 * it always did, and an old primary talking to a new node reads the refusal it
 * cannot understand as... a refusal, because `accepted:false` is present. The
 * asymmetry is deliberate: the safe reading is the default one.
 */
const welcomeAccepted = (f) => !!f && f.accepted !== false;

/** How many clamps a board says it has. Absent means none, which is what every
 *  board flashed before 2026-09-15 reports by saying nothing. */
const clampsOn = (w) => (w && w.caps && typeof w.caps.ct === 'number') ? w.caps.ct : 0;
function ack(seq, ok, err) {
  const f = { t: 'ACK', seq, ok: !!ok };
  if (err) f.err = err;
  return f;
}
function state(selectorId, stateId, moving) {
  return { t: 'STATE', selectorId, stateId, moving: !!moving };
}

/**
 * SENSE — "this sensor says on, or off".
 *
 * ONE BIT, AND THE NODE DECIDES IT. That is not a shortcut, it is RFC §5.4b: a
 * CT measures current, watts need voltage and power factor it cannot give, and
 * a woodworking tool's standby sits below the noise floor anyway. There is no
 * threshold worth sending and nothing for the primary to interpret. It is also
 * the only arrangement with usable latency — mains-frequency RMS cannot
 * round-trip per sample, so the loop lives where the ADC is.
 *
 * `level` is a MULTIPLE OF THE TRIP POINT, and deliberately not amps, watts or
 * a raw count. Two reasons. It makes commissioning possible without a serial
 * console at the machine — "2.8" says comfortable, "1.05" says move the clamp —
 * and its units are self-evidently not a measurement, so nothing can mistake it
 * for one. **Nothing may branch on it.** A primary that thresholds `level` has
 * re-derived, worse and a network away, the bit already sitting next to it in
 * `on` — and has quietly moved the decision back to the side of the wire that
 * cannot see the waveform.
 *
 * TELEMETRY (2026-09-17) rides alongside, IN AMPS, and weakens none of the
 * above. `amps` / `floorA` / `tripA` exist so a person can see what a clamp is
 * doing from the Boards page instead of a serial cable — is it fitted, is it
 * reading, how close is it — which is the question you ask standing at a
 * machine. Jeff's framing decided the units: *nobody but you and I care about
 * counts*. The bench rule that says log rmsCounts rather than amps is about the
 * CONSOLE, where counts are the measurement and amps an interpretation that has
 * been wrong before; it does not extend to a screen a woodworker reads.
 *
 * The NODE converts, from its own board's measured amps-per-count, so nothing
 * downstream owns a hardware constant. **The no-branching rule covers these
 * exactly as it covers `level`** — they are strictly for a human. A primary that
 * thresholds `amps` has moved the decision back to the side of the wire that
 * cannot see the waveform, which is the whole point of `on`.
 *
 * `fault` is the one that is not a number: the board refused to learn a floor
 * because the clamp reads far too much for a board at rest (sensing/CtTrip.h).
 * Omitted when false, so a healthy board says nothing and a faulted one is
 * legible. When it IS set, `floorA` and `tripA` are absent — there is no floor.
 *
 * @param {string} sensorId
 * @param {boolean} on
 * @param {number} [level]  multiple of trip; omitted when the node has none
 * @param {number} [amps]   what the clamp reads right now
 * @param {number} [floorA] the board's learned noise floor
 * @param {number} [tripA]  the point `amps` is judged against
 * @param {boolean} [fault] no floor could be learnt — see above
 * @returns {SenseFrame}
 */
function sense(sensorId, on, level, amps, floorA, tripA, fault) {
  const f = { t: 'SENSE', sensorId, on: !!on };
  if (typeof level === 'number') f.level = level;
  // OMITTED, never zeroed: 0 A is a real reading and "no floor yet" is not.
  if (typeof amps   === 'number') f.amps   = amps;
  if (typeof floorA === 'number') f.floorA = floorA;
  if (typeof tripA  === 'number') f.tripA  = tripA;
  if (fault) f.fault = true;
  return f;
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
      if (f.takeover !== undefined && typeof f.takeover !== 'boolean') {
        errs.push('HELLO.takeover must be a boolean');
      }
      break;
    case 'WELCOME':
      if (f.v !== NODELINK_VERSION) errs.push(`WELCOME.v ${f.v} != ${NODELINK_VERSION}`);
      str('nodeId'); str('board');
      if (!f.caps || typeof f.caps.servos !== 'number' || typeof f.caps.linear !== 'number') {
        errs.push('WELCOME.caps must be {servos:number, linear:number}');
      }
      // OPTIONAL, and absent means none — a board flashed before clamps existed
      // answers exactly as it always did rather than being refused.
      if (f.caps && f.caps.ct !== undefined &&
          (typeof f.caps.ct !== 'number' || f.caps.ct < 0 || f.caps.ct > MAX_SENSORS_PER_NODE)) {
        errs.push(`WELCOME.caps.ct must be a number 0..${MAX_SENSORS_PER_NODE}`);
      }
      if (f.claimedBy !== undefined && typeof f.claimedBy !== 'string') {
        errs.push('WELCOME.claimedBy must be a string');
      }
      if (f.accepted !== undefined && typeof f.accepted !== 'boolean') {
        errs.push('WELCOME.accepted must be a boolean');
      }
      // A refusal that doesn't say who has the board is unactionable: the UI
      // can only offer a takeover if it can name what it would break.
      if (f.accepted === false && !f.claimedBy) {
        errs.push('WELCOME.accepted=false requires claimedBy');
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
    case 'CONFIG':
      num('seq', 0, Number.MAX_SAFE_INTEGER);
      if (!Array.isArray(f.sensors)) {
        errs.push('CONFIG.sensors must be an array');
      } else if (f.sensors.length > MAX_SENSORS_PER_NODE) {
        errs.push(`CONFIG.sensors has ${f.sensors.length}, max ${MAX_SENSORS_PER_NODE}`);
      } else {
        // An EMPTY list is valid and meaningful — "report nothing".
        const seen = new Set();
        f.sensors.forEach((sen, i) => {
          const at = `CONFIG.sensors[${i}]`;
          if (!sen || typeof sen !== 'object') { errs.push(`${at} must be an object`); return; }
          if (typeof sen.sensorId !== 'string' || !sen.sensorId) {
            errs.push(`${at}.sensorId must be a non-empty string`);
          } else if (seen.has(sen.sensorId)) {
            // Two entries under one id would make SENSE ambiguous in the only
            // direction that matters: the primary could not tell which tool
            // just started.
            errs.push(`${at}.sensorId "${sen.sensorId}" is duplicated`);
          } else {
            seen.add(sen.sensorId);
          }
          if (sen.kind !== 'ct') errs.push(`${at}.kind must be ct`);
          if (typeof sen.channel !== 'number' || Number.isNaN(sen.channel)) {
            errs.push(`${at}.channel must be a number`);
          } else if (sen.channel < 0 || sen.channel > 15) {
            errs.push(`${at}.channel out of range (0..15)`);
          }
          // TUNING — optional, but nonsense is refused rather than clamped, and
          // refusing takes the WHOLE frame with it. Clamping would leave the
          // primary believing it had retuned a board that quietly did something
          // else, which is the same silent disagreement a half-applied sensor
          // list would be. Ranges mirror parseConfigFrame in control/NodeLink.h.
          //
          //   tripRatio  <= 1 trips on the floor itself, or on nothing at all
          //   minCounts  >= 4095 is past full scale on a 12-bit ADC: deaf, not
          //              merely insensitive
          //   clearRatio 1 is no hysteresis (the defect this fixed); above 1
          //              releases ABOVE the trip point, so a tool never stops
          const tune = [['tripRatio', 1, 100], ['minCounts', 0, 4095],
                        ['clearRatio', 0, 1]];
          for (const [key, lo, hi] of tune) {
            if (sen[key] === undefined) continue;
            if (typeof sen[key] !== 'number' || Number.isNaN(sen[key])) {
              errs.push(`${at}.${key} must be a number`);
            } else if (sen[key] <= lo || sen[key] >= hi) {
              errs.push(`${at}.${key} out of range (${lo} exclusive .. ${hi} exclusive)`);
            }
          }
        });
      }
      break;
    case 'SENSE':
      str('sensorId');
      if (typeof f.on !== 'boolean') errs.push('SENSE.on must be a boolean');
      // `level` is optional — a node with no trip point to divide by omits it
      // rather than sending a zero that reads like a measurement.
      if (f.level !== undefined) num('level', 0, 1000);
      // Telemetry, all optional, all for a human to read. The upper bound is
      // generous on purpose — a 30 A clamp saturates at 45-50 A of inrush
      // (RFC §5.5a) and refusing the whole frame for reporting that honestly
      // would lose the `on` bit riding with it, which is the part that matters.
      if (f.amps   !== undefined) num('amps',   0, 1000);
      if (f.floorA !== undefined) num('floorA', 0, 1000);
      if (f.tripA  !== undefined) num('tripA',  0, 1000);
      if (f.fault !== undefined && typeof f.fault !== 'boolean') {
        errs.push('SENSE.fault must be a boolean');
      }
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
  SENSE_REPEAT_MS, SENSE_STALE_MS, MAX_SENSORS_PER_NODE,
  hello, welcome, set, config, ack, state, sense, ping, pong, welcomeAccepted, clampsOn,
  validateFrame,
};
