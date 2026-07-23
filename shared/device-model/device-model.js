// device-model.js — THE canonical DustGate device model.
//
// This is the single source of truth for how the device *behaves*: its state
// shape, its command transitions, and its outlet ping/discover simulation.
// Both simulators wrap it:
//   - tools/mock-api.js            (Node HTTP + WebSocket server)
//   - dustgate-ui/.../demo-api.service.ts  (in-browser Angular service)
//
// The real firmware (C++) can't import this, but it implements the SAME
// contract — see conformance.js, which certifies any HTTP target (a mock or a
// real device) against a shared set of behavioral scenarios.
//
// ── Design rules ────────────────────────────────────────────────────────────
//  1. PURE. No HTTP, no WebSocket, no Angular, no wall-clock timers. Every
//     function takes a device object `d` and mutates it synchronously.
//  2. Caller owns timing. Multi-step motions are split into begin*/complete*
//     so each consumer supplies its own delay (setTimeout vs await) between
//     them — the two async styles never fight over shared code.
//  3. Faithful to firmware, not to convenience. Where the device does
//     something surprising (e.g. silently skipping an overlapping stop save),
//     the model does the same, so the mocks can't paper over real behavior.
//
// CommonJS so Node can `require()` it with no build step; a hand-written
// device-model.d.ts gives TypeScript consumers full types.

'use strict';

// ── Constants (mirror linear_actuator/config.h where noted) ─────────────────
const NUM_STOPS = 16;              // compile-time max stops (config.h NUM_STOPS)
const STEPS_PER_MM = 40;           // mock-only resolution; not real hardware (see TODO.md)
const MIN_STOP_SEPARATION_MM = 10; // config.h MIN_STOP_SEPARATION_MM — overlap backstop
const IDLE_TIMEOUT_SEC_DEFAULT = 3600; // config.h IDLE_TIMEOUT_SEC_DEFAULT
const HOME_MS = 1500;              // simulated homing duration
const CALIBRATE_MS = 4000;         // simulated reference-sweep duration
const TOOL_NAMES = ['Table Saw', 'Drill Press', 'Router Table'];

// Per-port role — what a linear-actuator port/gate is used for. Lets the
// actuator act as a node in the larger v2 topology graph (see v2 RFC §5.2).
const PORT_ROLES = ['tool', 'unassigned', 'blocked', 'feed'];

// Manifold geometry profiles: (model, gateCount) → mm positions referenced to the
// near endstop trigger. Used for reference-sweep auto-placement (see
// docs/dual-endstop-calibration.md). NUMBERS ARE PLACEHOLDERS — measure the real
// Rockler manifolds on the reference build and replace. 'custom' has no profile
// (→ manual jog, but still gets span + steps/mm calibration).
const MANIFOLD_PROFILES = {
  // rockler-2.5 MEASURED on the reference build: symmetric. Two direct measurements —
  // trigger-to-trigger span = 84.9mm at 2 gates, and gate-to-gate pitch = 82.9mm —
  // fix the trigger→gate offset at (84.9 − 82.9)/2 = 1mm per side. span(N) = 2 + (N−1)·82.9.
  // NB: the switch backoff (HOME_BACKOFF_STEPS) does NOT enter the pitch (it cancels);
  // it only affects steps/mm — the sweep must add HOME_BACKOFF_STEPS back to the
  // home→far step count before dividing by the 84.9mm span. (Pitch validated at 2 gates.)
  'rockler-2.5': { firstGateOffsetMm: 1,  gatePitchMm: 82.9, endMarginMm: 1 },
  // rockler-4 pitch = Rockler 10" manifold width ÷ 2 gates = 5" = 127mm center-to-
  // center; same rack pitch + endstop margin as 2.5", so offset/end-margin = 1mm.
  // Unconfirmed on hardware (4" slider not built yet); 4" path disabled in the UI.
  'rockler-4':   { firstGateOffsetMm: 1,  gatePitchMm: 127,  endMarginMm: 1 },
};

/** (model, gateCount) → { spanMm, gatesMm[] }, or null for custom/unknown. */
function manifoldProfile(model, gateCount) {
  const p = MANIFOLD_PROFILES[model];
  if (!p || !Number.isInteger(gateCount) || gateCount < 1) return null;
  const gatesMm = [];
  for (let i = 0; i < gateCount; i++) gatesMm.push(p.firstGateOffsetMm + i * p.gatePitchMm);
  const spanMm = p.firstGateOffsetMm + (gateCount - 1) * p.gatePitchMm + p.endMarginMm;
  return { spanMm, gatesMm };
}

// ── Construction ────────────────────────────────────────────────────────────

/** Create a fresh device in its power-on (unhomed, unconfigured) state. */
function createDevice() {
  return {
    // ── wire state (projected by statusView / infoView) ──
    state:          'IDLE',   // IDLE|HOMING|MOVING|AT_STOP|ERROR|STARTUP|DISABLED
    currentStop:    -1,       // -1 = unhomed
    targetStop:     0,
    positionSteps:  0,
    positionMM:     0,
    homed:          false,
    enabled:        true,
    manualOverride: false,
    motorInverted:  false,
    numActiveStops: 0,        // runtime-active gate count (0 = unconfigured)
    idleTimeoutSec: IDLE_TIMEOUT_SEC_DEFAULT,
    // ── dual-endstop calibration (see docs/dual-endstop-calibration.md) ──
    farEndstop:        false, // far-end limit switch triggered
    manifoldModel:     'custom',
    measuredSpanSteps: null,  // null until a reference sweep runs
    stepsPerMm:        STEPS_PER_MM, // calibrated by the sweep; nominal until then
    dcConfigured:   false,
    dcOn:           false,
    dcIp:           null,
    dcHost:         '',
    // mm: null = position not yet saved (distinct from a stop saved at 0.00).
    // role: per-port purpose (index 0 = home). Gates default 'unassigned'.
    stops:   Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({
      index: i, mm: null, role: i === 0 ? 'home' : 'unassigned',
    })),
    outlets: [],
    // ── internal sim state (never sent on the wire; underscore-prefixed) ──
    _discovered: null,        // lazily generated discover list, stable per device
    _pingCount:  {},          // pings seen per IP (drives the turn-on model)
    _pingBase:   {},          // stable running draw (W) per IP
  };
}

// ── Wire projections ────────────────────────────────────────────────────────

/** The status object pushed over WebSocket and returned by GET /api/status. */
function statusView(d) {
  return {
    state:          d.state,
    currentStop:    d.currentStop,
    targetStop:     d.targetStop,
    positionSteps:  d.positionSteps,
    positionMM:     d.positionMM,
    homed:          d.homed,
    enabled:        d.enabled,
    // Only meaningful once homed — before that the sensor reads untriggered
    // rather than misleadingly "at home".
    endstopHome:    d.homed && d.positionMM < 0.5,
    manualOverride: d.manualOverride,
    farEndstop:     d.farEndstop,
    manifoldModel:  d.manifoldModel,
    measuredSpanSteps: d.measuredSpanSteps,
    stepsPerMm:     d.stepsPerMm,
    dcConfigured:   d.dcConfigured,
    dcOn:           d.dcOn,
    stops:          d.stops,
    outlets:        d.outlets,
  };
}

/** The unauthenticated GET /api/info payload. */
function infoView(d, apiKey, version) {
  return {
    apiKey,
    numStops:       d.numActiveStops,
    version,
    motorInverted:  d.motorInverted,
    idleTimeoutSec: d.idleTimeoutSec,
    manifoldModel:  d.manifoldModel,
    stepsPerMm:     d.stepsPerMm,
  };
}

// ── Motion (begin*/complete* — caller supplies the delay between them) ──────

/** Start homing. Returns the simulated duration (ms) before completeHome. */
function beginHome(d) {
  d.state = 'HOMING';
  d.manualOverride = false;
  return HOME_MS;
}

/** Finish homing: at home (stop 0), zeroed, dust collector off. */
function completeHome(d) {
  d.state         = 'IDLE';
  d.currentStop   = 0;
  d.targetStop    = 0;
  d.homed         = true;
  d.positionSteps = 0;
  d.positionMM    = 0;
  d.dcOn          = false;
  d.stops[0]      = { index: 0, mm: '0.00' };
}

/**
 * Start a move to a numbered stop. Throws { status, error } if out of range.
 * Returns the simulated travel duration (ms) before completeMove.
 */
function beginMove(d, stop) {
  if (!Number.isInteger(stop) || stop < 0 || stop > NUM_STOPS) {
    throw badRequest('stop out of range');
  }
  const fromMm = parseFloat(d.stops[d.currentStop]?.mm ?? '0');
  const toMm   = parseFloat(d.stops[stop]?.mm ?? '0');
  d.state          = 'MOVING';
  d.targetStop     = stop;
  d.manualOverride = true; // a commanded move latches manual override
  return Math.max(400, Math.abs(toMm - fromMm) * 20); // ~50 mm/s
}

/** Finish a move: settle AT_STOP at a real gate, IDLE at home (0). */
function completeMove(d, stop) {
  const toMm = parseFloat(d.stops[stop]?.mm ?? '0');
  d.state         = stop > 0 ? 'AT_STOP' : 'IDLE';
  d.currentStop   = stop;
  d.positionMM    = toMm;
  d.positionSteps = Math.round(toMm * STEPS_PER_MM);
  d.dcOn          = stop > 0; // collector follows gate selection
}

/** Start a relative jog. Throws if mm missing. Returns duration (ms). */
function beginJog(d, mm) {
  if (typeof mm !== 'number' || !Number.isFinite(mm)) throw badRequest("missing 'mm'");
  d.state = 'MOVING';
  d._jogMM = mm;
  return Math.max(200, Math.abs(mm) * 15);
}

/** Finish a jog: apply the relative move, back to IDLE. */
function completeJog(d) {
  d.positionMM   += d._jogMM || 0;
  d.positionSteps = Math.round(d.positionMM * STEPS_PER_MM);
  d.state         = 'IDLE';
  d._jogMM        = 0;
}

// ── Calibration ─────────────────────────────────────────────────────────────

/**
 * Save the current jogged position as stop `index` (1..NUM_STOPS).
 *
 * Faithful to firmware: an overlapping save (too close to another saved gate)
 * is SILENTLY skipped — the device acks the request but doesn't persist it.
 * The Angular UI does its own friendlier pre-check before ever calling this;
 * this is the device-level backstop. Returns { ok, skipped }.
 */
function saveStop(d, index) {
  if (!Number.isInteger(index) || index < 1 || index > NUM_STOPS) {
    throw badRequest('index out of range');
  }
  const mm = d.positionMM;
  for (let j = 1; j <= d.numActiveStops && j <= NUM_STOPS; j++) {
    if (j === index) continue;
    const other = d.stops[j].mm;
    if (other === null) continue;
    if (Math.abs(mm - parseFloat(other)) < MIN_STOP_SEPARATION_MM) {
      return { ok: true, skipped: true }; // matches firmware silent-skip
    }
  }
  d.stops[index] = { index, mm: mm.toFixed(2), role: d.stops[index].role };
  if (index > d.numActiveStops) d.numActiveStops = index;
  return { ok: true, skipped: false };
}

/** Software e-stop — firmware maps this to STATE_ERROR ("ERROR"). */
function estop(d) { d.state = 'ERROR'; return { ok: true }; }

/** Vestigial enable/disable (firmware's isEnabled() is hardcoded true). */
function setEnabled(d, on) { d.enabled = !!on; return { ok: true }; }

// Record which side the actuator homed to. Home is always the user's LEFT endstop
// and gates are numbered 1..N left→right from it, so there's nothing to reorder in
// the sim — the firmware handles the physical datum/direction. No-op for the model.
function setHomedLeft(_d, _homedLeft) {
  return { ok: true };
}
function setMotorInverted(d, invert)    { d.motorInverted = !!invert;    return { ok: true }; }

function setNumGates(d, n) {
  if (Number.isInteger(n) && n >= 1 && n <= NUM_STOPS) {
    d.numActiveStops = n;
    // Clear stale saved positions/roles beyond the new count so they don't
    // reappear as phantom overlap conflicts if the count is later raised again.
    for (const s of d.stops) if (s.index > n) { s.mm = null; s.role = 'unassigned'; }
  }
  return { ok: true };
}

function setIdleTimeout(d, seconds) {
  if (typeof seconds === 'number' && seconds >= 0 && seconds <= 86400) {
    d.idleTimeoutSec = seconds;
  }
  return { ok: true };
}

/** Reset to unconfigured — mirrors firmware clearAllOutlets + calibration wipe. */
function clearCal(d) {
  d.numActiveStops = 0;
  d.stops = Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({
    index: i, mm: null, role: i === 0 ? 'home' : 'unassigned',
  }));
  d.outlets = [];
  d.homed        = false;
  d.currentStop  = -1;
  d.positionMM   = 0;
  d.positionSteps = 0;
  d.farEndstop        = false;
  d.manifoldModel     = 'custom';
  d.measuredSpanSteps = null;
  d.stepsPerMm        = STEPS_PER_MM;
  d.dcConfigured = false;
  d.dcOn         = false;
  d.dcIp         = null;
  d.dcHost       = '';
  d._pingCount = {};
  d._pingBase  = {};
  return { ok: true };
}

// ── Outlets ─────────────────────────────────────────────────────────────────

/**
 * Configure/replace an outlet in a slot. name required, stop must be >= 1;
 * ip optional (empty = name-only gate). Throws { status:400 } like firmware.
 */
function configureOutlet(d, cmd) {
  const slot = cmd.slot;
  if (!Number.isInteger(slot) || slot < 0 || slot >= NUM_STOPS) throw badRequest('slot out of range');
  if (typeof cmd.name !== 'string' || cmd.name.trim().length === 0) throw badRequest("missing 'name'");
  if (typeof cmd.stop !== 'number' || cmd.stop <= 0) throw badRequest("missing 'stop'");

  const ip = cmd.ip ?? '';
  const record = {
    slot,
    name:       cmd.name,
    stop:       cmd.stop,
    powerW:     0,
    active:     false,
    reachable:  ip.trim().length > 0 ? false : false,
    thresholdW: cmd.threshold ?? 5.0,
    gen:        cmd.gen ?? 2,
    ip,
    host:       cmd.host ?? '',
    hasSwitch:  ip.trim().length > 0, // empty ip = name-only gate
  };
  const existing = d.outlets.findIndex(o => o.slot === slot);
  if (existing >= 0) d.outlets[existing] = record; else d.outlets.push(record);
  // Assigning a tool marks that gate's role as 'tool' (unless deliberately blocked).
  const gate = d.stops[cmd.stop];
  if (gate && gate.role !== 'blocked') gate.role = 'tool';
  return { ok: true };
}

// ── Calibration sweep + port roles (dual-endstop) ───────────────────────────

/**
 * Begin the reference sweep: auto motor-direction → home → sweep to far endstop.
 * Sets manifold model + gate count up front. Returns the simulated duration.
 * The real device measures the span physically; the sim fills it in on complete.
 */
function beginCalibrate(d, model, gateCount) {
  if (!Number.isInteger(gateCount) || gateCount < 1 || gateCount > NUM_STOPS) {
    throw badRequest('gateCount out of range');
  }
  d.manifoldModel = (model in MANIFOLD_PROFILES) ? model : 'custom';
  d._calGateCount = gateCount;
  d.state = 'HOMING'; // the sweep starts by homing to the near endstop
  d.manualOverride = false;
  return CALIBRATE_MS;
}

/**
 * Finish the sweep: record the measured span, derive steps/mm, and (for a known
 * manifold) auto-place every gate by proportion of the span. 'custom' still gets
 * span + steps/mm but leaves gate positions for manual jog.
 */
function completeCalibrate(d) {
  const gateCount = d._calGateCount || d.numActiveStops || 1;
  const prof = manifoldProfile(d.manifoldModel, gateCount);
  // Physical span in mm: from the profile if known, else a plausible custom span.
  const spanMm = prof ? prof.spanMm : (1 + (gateCount - 1) * 82.9 + 1);
  // Simulate a measured step count with a little per-unit variance, exactly the
  // kind of real-world deviation from nominal that proportional placement absorbs.
  const measuredSpanSteps = Math.round(spanMm * STEPS_PER_MM * (1 + (Math.random() - 0.5) * 0.04));

  d.measuredSpanSteps = measuredSpanSteps;
  d.stepsPerMm        = measuredSpanSteps / spanMm;
  d.numActiveStops    = gateCount;

  d.stops = Array.from({ length: NUM_STOPS + 1 }, (_, i) => ({
    index: i, mm: null, role: i === 0 ? 'home' : 'unassigned',
  }));
  d.stops[0] = { index: 0, mm: '0.00', role: 'home' };
  if (prof) {
    for (let i = 1; i <= gateCount; i++) {
      // Proportional placement: gateSteps = span * (gateMm/spanMm); mm = gateMm.
      d.stops[i] = { index: i, mm: prof.gatesMm[i - 1].toFixed(2), role: 'unassigned' };
    }
  }

  d.state        = 'IDLE';
  d.homed        = true;
  d.currentStop  = 0;
  d.positionMM   = 0;
  d.positionSteps = 0;
  d.farEndstop   = false;
  d._calGateCount = undefined;
}

/** Set a port's role: tool | unassigned | blocked | feed (home/0 excluded). */
function setPortRole(d, index, role) {
  if (!Number.isInteger(index) || index < 1 || index > NUM_STOPS) throw badRequest('index out of range');
  if (!PORT_ROLES.includes(role)) throw badRequest(`invalid role: ${role}`);
  d.stops[index].role = role;
  return { ok: true };
}

function deleteOutlet(d, slot) {
  d.outlets = d.outlets.filter(o => o.slot !== slot);
  return { ok: true };
}

function configureDustCollector(d, cmd) {
  const ip = cmd.ip ?? '';
  if (ip.trim().length === 0) throw badRequest("missing 'ip'");
  d.dcConfigured = true;
  d.dcIp   = ip;
  d.dcHost = cmd.host ?? '';
  return { ok: true };
}

function deleteDustCollector(d) {
  d.dcConfigured = false;
  d.dcOn = false;
  d.dcIp = null;
  d.dcHost = '';
  return { ok: true };
}

function switchDustCollector(d, on) { d.dcOn = !!on; return { ok: true }; }

// ── Ping / discover simulation ──────────────────────────────────────────────
// Power tools have real on/off switches — no standby draw. An outlet reads a
// clean 0W until its tool is switched on, then a stable running draw in the
// 500–1000W range (with a few percent per-reading jiggle). Tools are OFF during
// the discovery scan, so discover reports 0W; the running draw appears at the
// threshold step, where the first ping to an outlet catches it still off (0W)
// and later pings read its running draw once "switched on".

function _randHex(len) {
  let s = '';
  for (let i = 0; i < len; i++) s += Math.floor(Math.random() * 16).toString(16);
  return s.toUpperCase();
}

function ensureDiscovered(d) {
  if (d._discovered) return d._discovered;
  const usedIps = new Set();
  const count = 2 + Math.floor(Math.random() * 3); // 2-4, mirrors real mDNS variability
  // Names drawn WITHOUT replacement so two devices never share a name.
  const namePool = TOOL_NAMES.slice().sort(() => Math.random() - 0.5);
  const namedIdx = new Set();
  while (namedIdx.size < Math.min(2, count)) namedIdx.add(Math.floor(Math.random() * count));

  let nameCursor = 0;
  d._discovered = Array.from({ length: count }, (_, i) => {
    let ip;
    do { ip = `192.168.87.${20 + Math.floor(Math.random() * 60)}`; } while (usedIps.has(ip));
    usedIps.add(ip);
    return {
      ip,
      hostname:  `ShellyPlugUSG4-${_randHex(12)}`,
      name:      namedIdx.has(i) ? namePool[nameCursor++] : '',
      reachable: true,
      powerW:    0, // off during the scan (real power switch, no standby)
      gen:       2,
    };
  });
  return d._discovered;
}

/** GET /api/outlets/discover — the discovered list (tools off → 0W). */
function discoverOutlets(d) {
  return ensureDiscovered(d).map(x => ({ ...x }));
}

/** Shelly-app device name for an IP if it's one we've discovered, else ''. */
function nameForIp(d, ip) {
  const hit = (d._discovered || []).find(x => x.ip === ip);
  return hit ? hit.name : '';
}

function _runningWatts(d, ip) {
  if (!(ip in d._pingBase)) d._pingBase[ip] = 500 + Math.random() * 500;
  return Math.round(d._pingBase[ip] * (1 + (Math.random() - 0.5) * 0.06)); // ±3%
}

/** POST /api/outlets/ping — first ping to an IP is 0W (off), then running draw. */
function pingOutlet(d, ip) {
  if (!ip) throw badRequest("missing 'ip'");
  // The dust collector's own plug follows its real on/off switch state.
  if (ip === d.dcIp) {
    return { reachable: true, powerW: d.dcOn ? 380 : 0, gen: 2, name: nameForIp(d, ip) };
  }
  d._pingCount[ip] = (d._pingCount[ip] || 0) + 1;
  const powerW = d._pingCount[ip] === 1 ? 0 : _runningWatts(d, ip);
  return { reachable: true, powerW, gen: 2, name: nameForIp(d, ip) };
}

// ── Helpers ─────────────────────────────────────────────────────────────────

/** A thrown validation error carrying an HTTP status for the wrapper to map. */
function badRequest(msg) {
  const e = new Error(msg);
  e.status = 400;
  return e;
}

module.exports = {
  // constants
  NUM_STOPS, STEPS_PER_MM, MIN_STOP_SEPARATION_MM, IDLE_TIMEOUT_SEC_DEFAULT, HOME_MS,
  CALIBRATE_MS, PORT_ROLES, MANIFOLD_PROFILES,
  // lifecycle
  createDevice, statusView, infoView,
  // motion
  beginHome, completeHome, beginMove, completeMove, beginJog, completeJog, estop, setEnabled,
  // calibration / config
  saveStop, setHomedLeft, setMotorInverted, setNumGates, setIdleTimeout, clearCal,
  // dual-endstop calibration + port roles
  manifoldProfile, beginCalibrate, completeCalibrate, setPortRole,
  // outlets
  configureOutlet, deleteOutlet, configureDustCollector, deleteDustCollector, switchDustCollector,
  ensureDiscovered, discoverOutlets, pingOutlet, nameForIp,
};
