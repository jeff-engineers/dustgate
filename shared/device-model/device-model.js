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

// ── Constants (mirror firmware/config.h where noted) ─────────────────
const NUM_STOPS = 16;              // compile-time max stops (config.h NUM_STOPS)
const STEPS_PER_MM = 40;           // mock-only resolution; not real hardware (see TODO.md)
const MIN_STOP_SEPARATION_MM = 10; // config.h MIN_STOP_SEPARATION_MM — overlap backstop
const IDLE_TIMEOUT_SEC_DEFAULT = 3600; // config.h IDLE_TIMEOUT_SEC_DEFAULT
const HOME_MS = 1500;              // simulated homing duration
const CALIBRATE_MS = 4000;         // simulated reference-sweep duration
const TOOL_NAMES = ['Table Saw', 'Drill Press', 'Router Table'];

// Per-port role — what a linear-actuator port/gate is used for. Lets the
// actuator act as a node in the larger topology graph (see architecture-rfc.md §5.2).
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
  // gatePitchMm 83.57 — CORRECTED ON HARDWARE 2026-08-28, was 82.9.
  // On a real 4-gate rack the outer gates landed ~1mm toward the centre while the
  // inner two were dead on; placement centres the array in the measured span, so
  // that signature is pitch and nothing else (spread comes from pitch, centre from
  // span). 83.57 = 82.9 + 2/3 of the observed 1mm — a closed-loop trim, not a
  // measurement. The measured span implies 83.33, so treat this as the top of a
  // 83.33-83.57 range until someone measures gate 1 to gate 4 and divides by 3.
  // firstGateOffsetMm/endMarginMm are now inconsistent with it ((84.9−83.57)/2 =
  // 0.67) and left alone: the firmware never uses them to place a gate.
  // PAIR: firmware/config.h MANIFOLD_2_5_GATE_PITCH_MM. Change both — see CLAUDE.md.
  'rockler-2.5': { firstGateOffsetMm: 1,  gatePitchMm: 83.57, endMarginMm: 1 },
  // rockler-4 pitch = Rockler 10" manifold width ÷ 2 gates = 5" = 127mm center-to-
  // center; same rack pitch + endstop margin as 2.5", so offset/end-margin = 1mm.
  // Unconfirmed on hardware (4" slider not built yet); 4" path disabled in the UI.
  'rockler-4':   { firstGateOffsetMm: 1,  gatePitchMm: 127,  endMarginMm: 1 },
};

/** True for a known Rockler manifold profile (ships in 2-gate units → even count). */
function isRocklerModel(model) { return model in MANIFOLD_PROFILES; }

/** Round a gate count up to the next even number (Rockler manifolds pair gates). */
function roundUpEven(n) { return n % 2 === 0 ? n : n + 1; }

/**
 * Physical gate count for a model: Rockler profiles are even (round odd up — the
 * extra port is a spare, capped/unused); 'custom' is left as-is. Clamped to NUM_STOPS.
 */
function physicalGateCount(model, n) {
  const g = isRocklerModel(model) ? roundUpEven(n) : n;
  return Math.min(g, NUM_STOPS);
}

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
    // Does the simulated board drive a sliding gate? The mock and the demo are a
    // slider brain, because that is what the linear vocabulary they expose is
    // for — and a mock reporting `false` here would draw four servo ports under
    // a canvas full of sliding-gate controls.
    hasLinear:      true,
    enabled:        true,
    manualOverride: false,
    numActiveStops: 0,        // runtime-active gate count (0 = unconfigured)
    idleTimeoutSec: IDLE_TIMEOUT_SEC_DEFAULT,
    // ── dual-endstop calibration (see docs/dual-endstop-calibration.md) ──
    farEndstop:        false, // far-end limit switch triggered
    manifoldModel:     'custom',
    measuredSpanSteps: null,  // null until a reference sweep runs
    stepsPerMm:        STEPS_PER_MM, // calibrated by the sweep; nominal until then
    dcConfigured:   false,
    dcOn:           false,
    // The network the board joined. The simulated device is always "on" one; the
    // firmware reports WiFi.SSID(), and an unprovisioned board reports ''.
    ssid:           'Shop-WiFi',
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

/** The status object pushed over WebSocket and returned by GET /api/motion. */
function statusView(d) {
  return {
    state:          d.state,
    currentStop:    d.currentStop,
    targetStop:     d.targetStop,
    positionSteps:  d.positionSteps,
    positionMM:     d.positionMM,
    homed:          d.homed,
    hasLinear:      d.hasLinear !== false,
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
    ssid:           d.ssid,
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
    idleTimeoutSec: d.idleTimeoutSec,
    manifoldModel:  d.manifoldModel,
    stepsPerMm:     d.stepsPerMm,
    // The owner suffix this brain stamps on plugs it owns — see nameOutlet().
    owner:          OUR_NAME,
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
// setEnabled went with /api/enable and /api/disable on 2026-08-28 — nothing
// called them, and the firmware's flags were never consumed, so the endpoints
// answered 200 and did nothing. `enabled` itself STAYS in the status: the
// firmware reports control.isEnabled() there for real.

// Record which side the actuator homed to. Home is always the user's LEFT endstop
// and gates are numbered 1..N left→right from it, so there's nothing to reorder in
// the sim — the firmware handles the physical datum/direction. No-op for the model.
function setHomedLeft(_d, _homedLeft) {
  return { ok: true };
}

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
  d.manifoldModel = isRocklerModel(model) ? model : 'custom';
  // Rockler manifolds ship in pairs → physical gate count is EVEN. Round an odd
  // request up; the extra port is a spare the user caps/leaves unused. 'custom'
  // has no fixed geometry, so it's left as entered.
  d._calGateCount = physicalGateCount(d.manifoldModel, gateCount);
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

// The owner suffix DustGate appends to a plug it owns. U+00B7 MIDDLE DOT,
// spaced — must match OWNER_SEP in plug-claim.js and ownerSep() in PlugClaim.h.
const OWNER_SEP = ' \u00b7 ';
// Who this simulated brain says it is, in that suffix.
const OUR_NAME = 'dustgate-demo';

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
      // Ownership (RFC §8). The LAST plug in the list always belongs to someone
      // else, so the refusal paths — you cannot rename or release a plug another
      // controller owns — are reachable on the bench without staging a second
      // brain on the network. Everything else is out of the box.
      claim:     i === count - 1 ? 'foreign' : 'unclaimed',
      holder:    i === count - 1 ? 'home-assistant.local' : null,
      // Takeable, but only by a human who has been told what breaks. Refusing
      // outright would just move the same repoint into the Shelly app, where
      // there is no record of it at all — see plug-claim.js.
      takeable:  i === count - 1,
      claimReason: i === count - 1
        ? 'shared with home-assistant.local — polled, not pushed' : undefined,
    };
  });
  return d._discovered;
}

/**
 * POST /api/outlets/name — rename a plug.
 *
 * The owner suffix is the device's business, not the caller's: `label` is the
 * human half and the suffix is reattached here, so a round trip can never double
 * it. Mirrors the main-loop handler in firmware.ino, including the rule that an
 * UNCLAIMED plug gets the bare label — we have not earned the right to stamp our
 * name on a plug we have not paired.
 */
function nameOutlet(d, ip, label, takeover) {
  if (!ip) throw badRequest("missing 'ip'");
  const hit = (d._discovered || []).find(x => x.ip === ip);
  if (!hit) return { ok: false, error: 'not responding' };
  // Refused unless a human explicitly overrode it. Only the NAME is written
  // either way — the plug keeps reporting to whoever owns it.
  const owned = hit.claim === 'foreign' || hit.claim === 'dustgate';
  if (owned && !takeover) {
    return { ok: false, error: `owned by ${hit.holder || 'another controller'}` };
  }
  // The owner suffix means "this plug is being USED by that brain", so it goes
  // on only when that is true — never on an overridden rename or an unclaimed
  // plug named before pairing.
  const full = hit.claim === 'ours' && label
    ? `${label}${OWNER_SEP}${OUR_NAME}`
    : String(label || '');
  hit.name = full;
  return { ok: true, name: full, label: String(label || '') };
}

/**
 * POST /api/outlets/takeover — repoint a plug that reports to someone else.
 *
 * The loud one: unlike a name override, this is what actually breaks the other
 * controller. On real hardware the approval is one-shot and the repoint lands on
 * the next provisioning pass; here it takes effect at once, which is the only
 * honest simplification available without simulating a provisioning cycle.
 */
function takeoverOutlet(d, ip) {
  if (!ip) throw badRequest("missing 'ip'");
  const hit = (d._discovered || []).find(x => x.ip === ip);
  if (!hit) return { ok: false, error: 'not responding' };
  if (!hit.takeable) {
    return { ok: false, error: 'nothing to take — no other controller has this outlet' };
  }
  hit.claim = 'ours';
  hit.holder = null;
  hit.takeable = false;
  hit.claimReason = undefined;
  hit.prevPushUrl = 'ws://home-assistant.local:80/shelly-rpc';   // so release can hand it back
  return { ok: true, claim: 'ours' };
}

/**
 * POST /api/outlets/release — the device half of unpairing.
 *
 * Best-effort by design: the layout half happens whatever this returns, because
 * a plug you have physically unplugged is exactly when you want to detach it.
 */
function releaseOutlet(d, ip) {
  if (!ip) throw badRequest("missing 'ip'");
  const hit = (d._discovered || []).find(x => x.ip === ip);
  if (!hit) return { ok: false, released: false, error: 'not responding' };
  if (hit.claim === 'foreign' || hit.claim === 'dustgate') {
    return { ok: true, released: false,
             note: 'polled only — nothing was written to this plug' };
  }
  const i = hit.name.lastIndexOf(OWNER_SEP);
  if (i >= 0) hit.name = hit.name.slice(0, i);
  const restored = !!hit.prevPushUrl;
  hit.claim = 'unclaimed';
  hit.holder = null;
  return { ok: true, released: true, restored };
}

/** GET /api/outlets/discover — the discovered list (tools off → 0W).
 *
 * The name is reported with any owner suffix STRIPPED, exactly as the firmware
 * does (plugclaim::decide → claim.label, firmware.ino). The suffix is our
 * bookkeeping: shown in the picker it would look like part of the tool's name,
 * then get saved back and doubled. The stored entry keeps the full name, because
 * that is what the plug itself actually holds.
 */
function discoverOutlets(d) {
  const suffix = OWNER_SEP + OUR_NAME;
  return ensureDiscovered(d).map(x => ({
    ...x,
    name: x.name && x.name.endsWith(suffix) ? x.name.slice(0, -suffix.length) : x.name,
  }));
}

/**
 * Put the plugs a saved shop is ALREADY PAIRED TO on the simulated network.
 *
 * ensureDiscovered() invents 2-4 plugs at random IPs, which is the right shape
 * for "sweep the network and see what's out there" — and it meant a shop loaded
 * from a document could never see its own plugs. The demo shop pairs the table
 * saw to 192.168.87.30; the simulated network had no such plug; so every screen
 * that shows a paired plug showed it as not responding, with no wattage and no
 * name, and rename, release and takeover all answered "not responding". A saved
 * plug that the network has never heard of is a state real hardware can be in
 * (unplugged), but it can't be the state EVERY paired plug is in on a runner
 * whose whole job is to be explorable.
 *
 * Adopted plugs are claimed OURS: pairing a plug is what claiming it means, and
 * anything else would make the demo's own seeded shop look stolen. An IP already
 * on the network is left exactly as it is — including the deliberately foreign
 * last entry, which is the only way the refusal paths stay reachable.
 *
 * Takes the document, so callers don't each re-derive where a plug hides. Both
 * shapes: a v1 topology (tool.sensor.outlet / collector.control.outlet) and a
 * shop (machines[].sensor.outlet), since either can be PUT.
 */
function adoptOutlets(d, doc) {
  const list = ensureDiscovered(d);
  for (const o of _docOutlets(doc)) {
    if (!o.ip || list.some(x => x.ip === o.ip)) continue;
    list.push({
      ip: o.ip,
      hostname: o.host || `ShellyPlugUSG4-${_randHex(12)}`,
      // The plug holds the FULL name, suffix and all — discoverOutlets() strips
      // it on the way out, exactly as the firmware does.
      name: o.name ? `${o.name}${OWNER_SEP}${OUR_NAME}` : '',
      reachable: true,
      powerW: 0,          // tools are off during a scan; same rule as the rest
      gen: o.gen || 2,
      claim: 'ours',
      holder: null,
      takeable: false,
    });
  }
  return list;
}

/** Every smart outlet a document mentions, whichever shape it is in. */
function _docOutlets(doc) {
  if (!doc || typeof doc !== 'object') return [];
  const out = [];
  const take = (o) => { if (o && o.ip) out.push(o); };
  for (const m of doc.machines || []) take(m && m.sensor && m.sensor.outlet);
  const systems = doc.systems || [doc];
  for (const sys of systems) {
    for (const e of (sys && sys.elements) || []) {
      if (!e) continue;
      take(e.sensor && e.sensor.outlet);
      take(e.control && e.control.outlet);
    }
  }
  return out;
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
  beginHome, completeHome, beginMove, completeMove, beginJog, completeJog, estop,
  // calibration / config
  saveStop, setHomedLeft, setNumGates, setIdleTimeout, clearCal,
  // dual-endstop calibration + port roles
  manifoldProfile, beginCalibrate, completeCalibrate, setPortRole,
  isRocklerModel, roundUpEven, physicalGateCount,
  // outlets
  configureOutlet, deleteOutlet, configureDustCollector, deleteDustCollector, switchDustCollector,
  ensureDiscovered, discoverOutlets, adoptOutlets, pingOutlet, nameForIp,
  nameOutlet, releaseOutlet, takeoverOutlet,
};
