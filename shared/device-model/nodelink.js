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
const P2S = ['HELLO', 'SET', 'CONFIG', 'PING', 'OTA', 'REFUSE', 'WHERE', 'PRESS', 'ALERT'];

// THE PWM SERVO PULSE RANGE (2026-10-07): what angle 0 and angle 180 map to, shop-wide (the layout's `servo` block, set on
// the Settings page), carried on every servo SET. The DEFAULT applies when the layout says nothing; the BOUNDS refuse a SET
// whole. PAIR: kDefaultServoMinUs / kDefaultServoMaxUs / kMinServoUs / kMaxServoUs / kMinServoSpanUs in NodeLink.h.
const DEFAULT_SERVO_MIN_US = 400;
const DEFAULT_SERVO_MAX_US = 2600;
const MIN_SERVO_US = 300;
const MAX_SERVO_US = 2800;
const MIN_SERVO_SPAN_US = 500;
/** Is this a pulse range a servo may be driven over? */
function servoRangeOk(minUs, maxUs) {
  return Number.isInteger(minUs) && Number.isInteger(maxUs) &&
         minUs >= MIN_SERVO_US && maxUs <= MAX_SERVO_US && maxUs - minUs >= MIN_SERVO_SPAN_US;
}
/** Frame types, secondary → primary. */
const S2P = ['WELCOME', 'ACK', 'STATE', 'SENSE', 'CLAMP', 'PONG', 'OTASTATE', 'JOIN'];

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
 * normal: such a board is OPTIONAL (shop.js isOptionalBoard, docs/optional-nodes-plan.md). See RFC §5.6a.
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

/**
 * Bounds on a PLUG sensor — what a node will poll on the primary's behalf.
 *
 * ⚠️ JS↔C++ PAIR — `kMaxPlugThresholdW`, `kMaxPlugWatts`, `kMaxPlugIpLen` in
 * firmware/control/NodeLink.h. A node refuses an out-of-range CONFIG WHOLE, taking
 * its clamp with it, so the bounds must agree exactly. `watts` bounds a REPORT:
 * refusing a SENSE for being too large would lose the `on` bit riding with it.
 */
const MAX_PLUG_THRESHOLD_W = 10000;
const MAX_PLUG_WATTS = 20000;
const MAX_PLUG_IP_LEN = 15;
const PLUG_KINDS = ['shelly', 'tasmota'];

/**
 * The bounds on an OTA frame — the image a node is told to pull.
 *
 * ⚠️ JS↔C++ PAIR — `kMaxOtaPath`, `kMinOtaBytes`, `kMaxOtaBytes` in
 * firmware/control/NodeLink.h. The node refuses a frame outside them WHOLE, so a
 * primary that sent a longer path than a node would parse would read as a node
 * that never starts updating, with nothing on the wire to say why. Size is
 * bounded below because anything smaller than 100 KB is not a firmware image
 * (it is the 404 page), and above by the node's app slot (1.9 MiB) so the node
 * says no before it has erased anything.
 */
const MAX_OTA_PATH = 48;
const MIN_OTA_BYTES = 100 * 1024;
const MAX_OTA_BYTES = 0x1E0000;
const OTA_STATES = ['start', 'progress', 'done', 'fail'];

/**
 * NODE-INITIATED LINKS (2026-10-04) — JOIN, REFUSE, WHERE and `caps.join`.
 *
 * Until now the primary dialled every node, which cost it one task and ~7-10 KB
 * per node and failed whenever the path worked in one direction only. Now the
 * NODE dials the primary and the primary just listens:
 *
 *   node → JOIN {nodeId}            first frame on a node-initiated socket
 *   primary → HELLO (as always)     if it has that node paired, else
 *   primary → REFUSE {reason}       and closes
 *   node → WELCOME (as always)      the claim is still decided ON THE NODE
 *
 * and everything after the WELCOME is the frames it always was. A primary that
 * wants a node that is down sends WHERE on a short-lived connection to the node's
 * own listener — "I am at <ip>:<port>, come and find me" — and hangs up, so
 * seeking costs it a transient rather than a resident socket per node.
 *
 * NOT A VERSION BUMP, same reasoning as CONFIG and SENSE above: every frame here
 * is new and an old end ignores a type it does not know, so the four pairings of
 * old and new all degrade to the link that already worked (the primary dials).
 * `caps.join` is how a primary learns that a node will dial in, so it can stop
 * dialling it; absent means NO, so a board that predates it is dialled as before.
 *
 * ⚠️ JS↔C++ PAIR — `kMaxWhereIpLen`, `kRefuseReasons` in firmware/control/NodeLink.h.
 */
const MAX_WHERE_IP_LEN = 15;
/** The UDP port a primary broadcasts "DGB1|<primaryId>|<ip>|<port>" on. ⚠️ JS↔C++ PAIR — `kBeaconPort`. */
const BEACON_PORT = 41234;
const REFUSE_REASONS = ['not-paired', 'duplicate', 'busy'];

/**
 * THE COLLECTOR'S JOBS ON A NODE (2026-10-04) — PRESS, a `bin` sensor, `caps.rf`, `caps.bin`.
 *
 * Until now the RF transmitter that presses the collector's remote and the dust-bin
 * level sensor worked only on a PRIMARY, so a board at the collector had to BE the
 * brain. These are the two frames that let it be an ordinary node:
 *
 *   PRESS {seq, address, data, tickUs, repeats}   primary → node. Key the transmitter ONCE.
 *       The node answers ACK{seq, ok} when the frame has gone out. It is a PRIMITIVE and
 *       nothing else: WHEN to press, whether it worked and when to try again is the
 *       primary's policy (control/CollectorPress.h) because only the primary can read
 *       the plug that says whether the blower agreed. The address and data ride the
 *       frame so a node stays stateless about which fob it is pressing.
 *   CONFIG sensor {sensorId, kind:'bin', invert?}   report the bin beam as a SENSE bit
 *       (`on` = the bin is FULL), debounced on the node (utils/BinSensor.h).
 *   WELCOME caps.rf / caps.bin   the board has a transmitter / a bin pad. Absent means NO.
 *
 * Not a version bump, same reasoning as every frame added since CONFIG: an old node
 * ignores a PRESS (and the primary sees that in `caps.rf` being absent, so it never
 * sends one), and refuses a CONFIG it cannot parse WHOLE — which is why the primary
 * only sends a `bin` sensor to a board that said `caps.bin`.
 *
 * ⚠️ JS↔C++ PAIR — `kMinRfTickUs`, `kMaxRfTickUs`, `kMaxRfRepeats` in firmware/control/NodeLink.h.
 */
const MIN_RF_TICK_US = 50;
const MAX_RF_TICK_US = 1000;
const MAX_RF_REPEATS = 60;

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
 * @property {boolean}[home]        drive==='linear' only: find the datum AGAIN before this move, even if the node has one.
 *                                  Absent means no. Setting a slider up from the app starts this way (2026-10-07): a datum
 *                                  left from before a jam is a count nobody should calibrate against. Additive — a node
 *                                  that predates it ignores the field and moves from the datum it has.
 * @property {boolean}[holdAtRest]  servo only; default false (move then detach)
 * @property {number} [minUs]       servo only, with maxUs: the shop's pulse range (DEFAULT_SERVO_MIN_US..). Absent = keep
 *                                  what the node has; a node from before 2026-10-07 ignores both
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
 * @property {'ct'|'plug'} kind     what is watched. 'ct' is wired to this board; a
 *                                  'plug' is a smart plug on the network that THIS
 *                                  BOARD polls on the primary's behalf (2026-10-03):
 *                                  the board that controls a tool handles its plug,
 *                                  so the brain's poll load stops growing with the
 *                                  shop. A plug sensor carries `ip`, `plug` and
 *                                  `thresholdW` INSTEAD of `channel`.
 * @property {number} [channel]     'ct' only: which input on THIS BOARD — a
 *                                  hardware fact, the same shape as SET.channel.
 * @property {string} [ip]          'plug' only: dotted quad.
 * @property {'shelly'|'tasmota'} [plug]  'plug' only: which protocol it speaks.
 * @property {number} [thresholdW]  'plug' only: the watts at or above which the
 *                                  tool is ON. The primary owns the NUMBER (it is a
 *                                  layout fact the user edits); the node applies it
 *                                  so it can send on CHANGE instead of every poll.
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
 * @property {boolean} [fault]      CT: no floor could be learnt; floorA/tripA absent.
 *                                  PLUG: the plug did not answer.
 * @property {boolean} [plug]        true on every report from a PLUG sensor.
 * @property {number} [watts]       PLUG only: what it reads now. The primary hands
 *                                  this to the same place a polled wattage goes.
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

/** A node dialling its primary. The only thing the primary needs is who is knocking. */
function join(nodeId) {
  return { t: 'JOIN', v: NODELINK_VERSION, nodeId };
}
/** The primary declining a node-initiated socket, then closing it. */
function refuse(reason) {
  return { t: 'REFUSE', reason };
}
/** "The primary <primaryId> is at <ip>:<port> now" — a node acts on it only if
 *  `primaryId` is its OWNER; the claim check still decides every command. */
function where(primaryId, ip, port = 80) {
  return { t: 'WHERE', primaryId, ip, port };
}
/** Key the node's transmitter once. See the block at MIN_RF_TICK_US. */
function press(seq, address, data, tickUs, repeats) {
  return { t: 'PRESS', seq, address, data, tickUs, repeats };
}

/**
 * Tell a node to update itself (2026-10-03). The node PULLS the image over plain
 * HTTP from whoever is on the other end of this socket — a 1.4 MB image through a
 * WebSocket frame would be a protocol to write and get right, where a GET is one
 * the ESP32 core already ships. This frame carries only what the node needs to
 * fetch and to check what it fetched:
 *
 * @param {number} seq
 * @param {string} path   absolute URL path on the primary, e.g. "/node-pwm.bin"
 * @param {number} size   bytes — the node refuses to start unless this fits its
 *                        spare slot, and refuses the download if it is not this long
 * @param {string} md5    32 hex chars of the whole image, verified before the
 *                        slot is made bootable
 * @param {string} fw     the build stamp the image carries — echoed back in
 *                        OTASTATE so a progress report names what it is installing
 */
function ota(seq, path, size, md5, fw) {
  return { t: 'OTA', seq, path, size, md5, fw };
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
 * @param {{home?: boolean, minUs?: number, maxUs?: number}} [opts]  linear: find the datum again first (SetFrame.home);
 *                                  servo: the shop's pulse range
 * @returns {SetFrame}
 */
function set(seq, sel, stateId, realization, opts = {}) {
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
    if (opts.minUs !== undefined && opts.maxUs !== undefined) { f.minUs = opts.minUs; f.maxUs = opts.maxUs; }
  } else {
    f.positionMm = realization;
    if (opts.home) f.home = true;
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
    if (s.kind === 'plug') {
      return { sensorId: s.sensorId, kind: 'plug', ip: s.ip, plug: s.plug, thresholdW: s.thresholdW };
    }
    if (s.kind === 'bin') {
      const b = { sensorId: s.sensorId, kind: 'bin' };
      if (typeof s.invert === 'boolean') b.invert = s.invert;
      return b;
    }
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
 * A node's own account of its last boot, added to a WELCOME. (2026-09-27)
 *
 * `upS` — whole seconds since the node booted. `rst` — why it last reset, as a
 * short word (`poweron`, `brownout`, `panic`, `wdt`, `sw`, …; free text, never
 * branched on). BOTH OPTIONAL, and absent means UNKNOWN — every board flashed
 * before this answers as it always did.
 *
 * WHY IT RIDES THE WELCOME. A primary sees a node vanish and come back and cannot
 * tell "the planer was switched off at the wall" (normal: a CT node is powered by
 * its tool) from "the node crashed" (a bug) — both are a dropped socket and a new
 * one. The node knows which, and the WELCOME is the one frame it sends on every
 * (re)connect, so the primary's link log can say "rebooted 12 s ago: brownout"
 * instead of "reconnected". Diagnostic only: NOTHING routes on these.
 */
function withBootInfo(frame, upS, rst) {
  if (typeof upS === 'number') frame.upS = Math.max(0, Math.floor(upS));
  if (typeof rst === 'string' && rst) frame.rst = rst.slice(0, MAX_RST_LEN);
  return frame;
}
const MAX_RST_LEN = 16;

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

/** Can this board poll plugs for the primary? Absent means NO — every board
 *  flashed before 2026-10-03 stays brain-polled rather than being handed a CONFIG
 *  it would refuse whole (and its clamp with it). */
const pollsPlugs = (w) => !!(w && w.caps && w.caps.plug === 1);

/** Will this board dial its primary itself? Absent means NO — every board flashed
 *  before 2026-10-04 is still dialled, which is the link that already worked. */
const dialsIn = (w) => !!(w && w.caps && w.caps.join === 1);

/** Does this board have a transmitter for the collector's remote? Absent means NO. */
const pressesRf = (w) => !!(w && w.caps && w.caps.rf === 1);
/** Does this board have a dust-bin sensor pad? Absent means NO. */
const watchesBin = (w) => !!(w && w.caps && w.caps.bin === 1);
/**
 * CLAMP — "is a clamp plugged into my jack?" (2026-10-09). S→P, on change and every SENSE_REPEAT_MS.
 *
 * `caps.ct` says a board HAS a clamp input; it is the pin map, true of every PWM board whether or not
 * anything is plugged in. This says whether something IS. A switched 3.5 mm jack ties its tip to 3V3
 * through 10 kΩ with no plug in (WIRING.md §8), and the node also enables D0's own pull-up, so an empty
 * jack and an empty pad both read railed high; a clamp holds the pin at the bias midpoint. The node
 * reports it whether or not a CONFIG names a clamp — that is the point: it is how a person sees that a
 * clamp is there before the layout uses it, and it replaced the per-board "clamp" switch in the layout.
 *
 * A board that has never sent one is UNKNOWN (an old firmware), which the primary must not read as
 * unplugged. NOT a version bump: a new frame type, which an old primary ignores.
 */
function clamp(plugged) {
  return { t: 'CLAMP', in: !!plugged };
}

/**
 * ALERT — the primary tells a board what its pixel should say about the SHOP, as opposed to about itself (2026-10-10).
 * `bin` is true while a dust bin on a system this board serves is full: the board blinks red (StatusLed.h — a fault
 * pulses red, a full bin blinks it on and off; RFC §"binNearFull", system scope). Which boards is the primary's call
 * (Shop.h binAlertBoards()): the collector's own board and every board with a gate on that system. Sent on change and
 * again on every link-up, as a whole state rather than an edge, so a rebooted node or a dropped frame cannot leave a
 * board blinking for a bin that was emptied. A node that predates it ignores the type; NOT a version bump.
 */
function alert(binFull) {
  return { t: 'ALERT', bin: !!binFull };
}

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
function sense(sensorId, on, level, amps, floorA, tripA, fault, watts, plug) {
  const f = { t: 'SENSE', sensorId, on: !!on };
  if (typeof watts === 'number') f.watts = watts;   // a PLUG's reading; absent for a clamp
  // Says WHAT reported. An unreachable plug has no watts to give it away, and its
  // `fault` would otherwise read as a clamp that could not learn a floor — which
  // the Boards screen would then draw as a clamp that does not exist.
  if (plug) f.plug = true;
  if (typeof level === 'number') f.level = level;
  // OMITTED, never zeroed: 0 A is a real reading and "no floor yet" is not.
  if (typeof amps   === 'number') f.amps   = amps;
  if (typeof floorA === 'number') f.floorA = floorA;
  if (typeof tripA  === 'number') f.tripA  = tripA;
  if (fault) f.fault = true;
  return f;
}
/**
 * A node's account of an update it was told to run. `pct` (0..100) rides
 * 'progress'; `err` (a short sentence for a person) rides 'fail'. 'done' means the
 * image is written and verified and the node is ABOUT to reboot — the next thing
 * the primary sees is a WELCOME carrying the new `fw`, which is the real proof.
 */
function otaState(state, pct, err) {
  const f = { t: 'OTASTATE', state };
  if (typeof pct === 'number') f.pct = Math.max(0, Math.min(100, Math.round(pct)));
  if (err) f.err = String(err).slice(0, 64);
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
      if (f.caps && f.caps.plug !== undefined && f.caps.plug !== 0 && f.caps.plug !== 1) {
        errs.push('WELCOME.caps.plug must be 0 or 1');
      }
      if (f.caps && f.caps.join !== undefined && f.caps.join !== 0 && f.caps.join !== 1) {
        errs.push('WELCOME.caps.join must be 0 or 1');
      }
      for (const k of ['rf', 'bin']) {
        if (f.caps && f.caps[k] !== undefined && f.caps[k] !== 0 && f.caps[k] !== 1) {
          errs.push(`WELCOME.caps.${k} must be 0 or 1`);
        }
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
      // Boot info (withBootInfo) — optional, diagnostic only.
      if (f.upS !== undefined && (typeof f.upS !== 'number' || f.upS < 0)) {
        errs.push('WELCOME.upS must be a non-negative number');
      }
      if (f.rst !== undefined && (typeof f.rst !== 'string' || f.rst.length > MAX_RST_LEN)) {
        errs.push(`WELCOME.rst must be a string of at most ${MAX_RST_LEN} chars`);
      }
      break;
    case 'JOIN':
      if (f.v !== NODELINK_VERSION) errs.push(`JOIN.v ${f.v} != ${NODELINK_VERSION}`);
      str('nodeId');
      break;
    case 'PRESS':
      num('seq', 0, Number.MAX_SAFE_INTEGER);
      num('address', 0, 255); num('data', 0, 15);
      num('tickUs', MIN_RF_TICK_US, MAX_RF_TICK_US);
      num('repeats', 1, MAX_RF_REPEATS);
      break;
    case 'REFUSE':
      if (!REFUSE_REASONS.includes(f.reason)) errs.push(`REFUSE.reason must be one of ${REFUSE_REASONS.join('|')}`);
      break;
    case 'WHERE':
      str('primaryId');
      if (typeof f.ip !== 'string' || !/^\d{1,3}(\.\d{1,3}){3}$/.test(f.ip) || f.ip.length > MAX_WHERE_IP_LEN) {
        errs.push('WHERE.ip must be a dotted quad');
      }
      num('port', 1, 65535);
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
      if ('home' in f && typeof f.home !== 'boolean') errs.push('SET.home must be true|false');
      if (f.home === true && f.drive !== 'linear') errs.push('SET.home is for a linear drive');
      if ('minUs' in f || 'maxUs' in f) {
        if (!servoRangeOk(f.minUs, f.maxUs)) errs.push('SET.minUs/maxUs must be a servo pulse range within bounds');
      }
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
          if (sen.kind === 'plug') {
            if (typeof sen.ip !== 'string' || !/^\d{1,3}(\.\d{1,3}){3}$/.test(sen.ip) || sen.ip.length > MAX_PLUG_IP_LEN) {
              errs.push(`${at}.ip must be a dotted quad`);
            }
            if (!PLUG_KINDS.includes(sen.plug)) errs.push(`${at}.plug must be one of ${PLUG_KINDS.join('|')}`);
            if (typeof sen.thresholdW !== 'number' || Number.isNaN(sen.thresholdW) ||
                sen.thresholdW <= 0 || sen.thresholdW > MAX_PLUG_THRESHOLD_W) {
              errs.push(`${at}.thresholdW out of range (0 exclusive .. ${MAX_PLUG_THRESHOLD_W})`);
            }
            return;   // none of the clamp's fields apply to a plug
          }
          if (sen.kind === 'bin') {
            // The bin beam. Its pad is the board's own, so there is no channel, and the
            // only thing the layout says is which way it reads.
            if (sen.invert !== undefined && typeof sen.invert !== 'boolean') errs.push(`${at}.invert must be a boolean`);
            return;
          }
          if (sen.kind !== 'ct') errs.push(`${at}.kind must be ct, plug or bin`);
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
    case 'CLAMP':
      if (typeof f.in !== 'boolean') errs.push('CLAMP.in must be a boolean');
      break;
    case 'ALERT':
      if (typeof f.bin !== 'boolean') errs.push('ALERT.bin must be a boolean');
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
      if (f.watts !== undefined) num('watts', 0, MAX_PLUG_WATTS);
      if (f.plug !== undefined && typeof f.plug !== 'boolean') errs.push('SENSE.plug must be a boolean');
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
    case 'OTA':
      num('seq', 0, Number.MAX_SAFE_INTEGER);
      if (typeof f.path !== 'string' || !f.path.startsWith('/') || f.path.length > MAX_OTA_PATH) {
        errs.push(`OTA.path must be an absolute path of at most ${MAX_OTA_PATH} chars`);
      }
      num('size', MIN_OTA_BYTES, MAX_OTA_BYTES);
      if (typeof f.md5 !== 'string' || !/^[0-9a-f]{32}$/.test(f.md5)) {
        errs.push('OTA.md5 must be 32 lowercase hex chars');
      }
      str('fw');
      break;
    case 'OTASTATE':
      if (!OTA_STATES.includes(f.state)) errs.push(`OTASTATE.state must be one of ${OTA_STATES.join('|')}`);
      if (f.pct !== undefined) num('pct', 0, 100);
      if (f.err !== undefined && typeof f.err !== 'string') errs.push('OTASTATE.err must be a string');
      break;
    case 'PING':
    case 'PONG':
      break;
  }
  return errs;
}

module.exports = {
  NODELINK_VERSION, P2S, S2P,
  DEFAULT_SERVO_MIN_US, DEFAULT_SERVO_MAX_US, MIN_SERVO_US, MAX_SERVO_US, MIN_SERVO_SPAN_US, servoRangeOk,
  PING_INTERVAL_MS, PONG_TIMEOUT_MS, RECONNECT_MIN_MS, RECONNECT_MAX_MS,
  SENSE_REPEAT_MS, SENSE_STALE_MS, MAX_SENSORS_PER_NODE,
  MAX_PLUG_THRESHOLD_W, MAX_PLUG_WATTS, MAX_PLUG_IP_LEN, PLUG_KINDS, pollsPlugs,
  MAX_RST_LEN, MAX_OTA_PATH, MIN_OTA_BYTES, MAX_OTA_BYTES, OTA_STATES,
  MAX_WHERE_IP_LEN, BEACON_PORT, REFUSE_REASONS, dialsIn, join, refuse, where,
  MIN_RF_TICK_US, MAX_RF_TICK_US, MAX_RF_REPEATS, pressesRf, watchesBin, press,
  hello, welcome, withBootInfo, set, config, ack, state, sense, clamp, alert, ping, pong, ota, otaState, welcomeAccepted, clampsOn,
  validateFrame,
};
