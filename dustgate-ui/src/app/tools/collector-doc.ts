// collector-doc.ts — reading and writing a collector element, without Angular.
//
// The sheet's screen is one thing; WHAT IT WRITES is another, and this is the
// half that can be silently wrong. A misplaced field here produces a document
// that still validates and a collector that never starts — so it lives in a
// plain module with a spec (collector-doc.spec.ts), the same way the routing
// maths does, rather than inside a component nothing can call.
//
// The shape it moves between:
//
//   control.outlet  a plug we command. SHELLY ONLY — a no-relay plug named here
//                   describes a collector that can never start, and
//                   validateTopology() rejects the whole document.
//   control.rf      the remote's frame, transmitted. Never alongside an outlet:
//                   two ways to command one blower fight each other.
//   control.offDelayMs  coast-down. Survives every switching choice, because it
//                   describes the blower rather than how we reach it.
//   sensor.outlet   a plug we only WATCH. Independent of control, because a
//                   press is an EDGE against a TOGGLE and proves nothing.
//   bin.sensor      the beam across the bin.
//
// See docs/mockups/collector-setup.html for the decisions and topology.js for
// the rules that reject the combinations this deliberately cannot express.

export type RawEl = Record<string, unknown>;

/** How the collector is SWITCHED. 'servo' is in the union because the sheet
 *  draws it — greyed, with a reason — and leaving it out would mean a disabled
 *  option that the type system says cannot exist. It has no schema yet, so
 *  writing it is refused below rather than guessed at. */
export type CtlKind   = 'plug' | 'rf' | 'servo' | 'none';
/** How it is WATCHED. 'ct' is here for the same reason 'servo' is. */
export type SenseKind = 'plug' | 'ct' | 'none';

export interface PlugForm {
  ip: string;
  host: string;
  /** Cached display name. NOT the source of truth — the plug holds that — but it
   *  keeps a name on screen for one that is switched off or missed by a sweep. */
  label: string;
  gen: number;
  kind: 'shelly' | 'tasmota';
}

export interface CollectorForm {
  name: string;
  ctl: CtlKind;
  ctlPlug: PlugForm;
  /** 0-255. Only meaningful when ctl === 'rf'. */
  rfAddress: number;
  /** Everything else that was in `control.rf` — `pin` above all, which is a
   *  property of how the board is built and never something a screen asks. */
  rfRest: RawEl;
  sense: SenseKind;
  sensePlug: PlugForm;
  bin: boolean;
  /** '' means "this board", matching every selector and NodeBus's own rule. */
  binControllerId: string;
  /** Anything else already on `bin.sensor`, so a field written by a newer UI
   *  survives a round trip through an older one. */
  binRest: RawEl;
  coastSec: number;
}

/** Coast-down when the document is silent. Mirrors kDefaultCollectorOffDelayMs
 *  (firmware) and DEFAULT_COLLECTOR_OFF_DELAY_MS (topology-device.js). */
export const DEFAULT_COAST_SEC = 8;

/**
 * The pad the 315 MHz transmitter is wired to, when the document does not say.
 *
 * ⚠️ THIS IS A JS↔C++ PAIR — `PIN_RF_TX` in firmware/boards/xiao_c5.h. Registered
 * in CLAUDE.md's table; move the pad on the board and every layout this UI has
 * ever written still names GPIO 9, and the collector silently never starts.
 *
 * It has to be written, and that is the awkward part: `pin` is a property of how
 * the BOARD is built, not of the user's remote, so no screen asks for it — but
 * topology.js requires it (`no pin → invalid` in topology.test.js) and the
 * firmware skips building the presser entirely when it is absent, without
 * logging anything. A document with `control.rf` and no `pin` therefore
 * validates on the UI side, saves, and produces a collector that never runs.
 * Found by saving one, 2026-09-14.
 *
 * The alternative — make `pin` optional and let the firmware fall back to its
 * own PIN_RF_TX — is arguably better, since it puts the board's fact back on the
 * board and deletes this pair. It changes a decided validation rule, so it is
 * not taken here.
 */
export const DEFAULT_RF_PIN = 9;

/** The Rockler fob's own setting — rockers 1, 6 and 8 closed = 0b01011110.
 *  Matches RfCollectorPresser::kRocklerAddress, so a sheet that has never been
 *  opened and the firmware's fallback agree. */
export const ROCKLER_ADDRESS = 94;

const emptyPlug = (): PlugForm => ({ ip: '', host: '', label: '', gen: 2, kind: 'shelly' });

function readPlug(outlet: RawEl | undefined): PlugForm {
  if (!outlet) return emptyPlug();
  // Absent `kind` means shelly on BOTH sides (topology.js, outletKindFromName()),
  // and an unknown string degrades the same way — so a document from a newer UI
  // falls back to the old behaviour rather than to a plug that reads nothing.
  const kind = (outlet['kind'] as string) === 'tasmota' ? 'tasmota' : 'shelly';
  return {
    ip:    (outlet['ip'] as string) ?? '',
    host:  (outlet['host'] as string) ?? '',
    label: (outlet['name'] as string) ?? '',
    gen:   (outlet['gen'] as number) ?? 2,
    kind,
  };
}

function writePlug(p: PlugForm, extra?: RawEl): RawEl {
  const outlet: RawEl = { gen: p.gen, ip: p.ip, ...(extra ?? {}) };
  // OMITTED WHEN SHELLY, on purpose: absent already says shelly, so writing it
  // would add a field to every existing document to repeat what silence said.
  if (p.kind === 'tasmota') outlet['kind'] = 'tasmota';
  if (p.host) outlet['host'] = p.host;
  if (p.label) outlet['name'] = p.label;
  return outlet;
}

/** A switchable plug answers both questions — it reports its own power — so the
 *  sheet collapses the watch question rather than asking twice, and nothing is
 *  written to `sensor`. The firmware already behaves this way, falling back to
 *  the control plug when `sensor.outlet` is absent
 *  (TopologyRuntime::collectorSensorOutlet). Writing the same plug into both
 *  would be two places to change one fact. */
export function fused(form: CollectorForm): boolean {
  return form.ctl === 'plug' && !!form.ctlPlug.ip;
}

/** Read a collector element into the form the sheet edits. */
export function readCollector(el: RawEl): CollectorForm {
  const control = (el['control'] as RawEl | undefined) ?? {};
  const ctlOutlet = control['outlet'] as RawEl | undefined;
  const rf = control['rf'] as RawEl | undefined;
  const senseOutlet = (el['sensor'] as RawEl | undefined)?.['outlet'] as RawEl | undefined;
  const binSensor = (el['bin'] as RawEl | undefined)?.['sensor'] as RawEl | undefined;

  const { address, ...rfRest } = rf ?? {};
  const { kind: _binKind, controllerId, ...binRest } = binSensor ?? {};
  const ms = control['offDelayMs'];

  return {
    name: (el['name'] as string) || 'Dust collector',
    ctl: ctlOutlet ? 'plug' : rf ? 'rf' : 'none',
    ctlPlug: readPlug(ctlOutlet),
    rfAddress: typeof address === 'number' ? address : ROCKLER_ADDRESS,
    rfRest: rfRest as RawEl,
    sense: senseOutlet ? 'plug' : 'none',
    sensePlug: readPlug(senseOutlet),
    bin: !!binSensor,
    binControllerId: (controllerId as string) ?? '',
    binRest: binRest as RawEl,
    // Rounded, not floored: the slider counts whole seconds and nobody sets a
    // coast-down to 4.25 s — the same call settings.component.ts makes.
    coastSec: typeof ms === 'number' ? Math.round(ms / 1000) : DEFAULT_COAST_SEC,
  };
}

/** Write the form back onto a COPY of the element. */
export function writeCollector(el: RawEl, form: CollectorForm): RawEl {
  const out: RawEl = { ...el };

  // ── control ────────────────────────────────────────────────────────────
  // Rebuilt rather than merged: moving from a plug to a remote has to REMOVE the
  // plug, and the model refuses a collector carrying both anyway.
  const control: RawEl = { offDelayMs: form.coastSec * 1000 };
  if (form.ctl === 'plug' && form.ctlPlug.ip) {
    control['outlet'] = writePlug({ ...form.ctlPlug, kind: 'shelly' });
  } else if (form.ctl === 'rf') {
    // `pin` FIRST so a document that already carries one keeps it: a board whose
    // transmitter is on a different pad must not be rewritten to the default
    // just because someone opened the sheet.
    control['rf'] = { pin: DEFAULT_RF_PIN, ...form.rfRest, address: form.rfAddress };
  }
  // 'servo' falls through to a bare control: there is no schema to write, and
  // inventing one now is what RFC §4.2c says not to do.
  out['control'] = control;

  // ── sensor ─────────────────────────────────────────────────────────────
  if (!fused(form) && form.sense === 'plug' && form.sensePlug.ip) {
    out['sensor'] = { outlet: writePlug(form.sensePlug) };
  } else {
    delete out['sensor'];
  }

  // ── bin ────────────────────────────────────────────────────────────────
  // `kind` is 'threshold' for the diffuse beam actually in hand: it answers
  // "dust at this height, y/n", not a distance, so it carries none of the
  // emptyMm/fullMm/warnPct a rangefinder would.
  if (form.bin) {
    const sensor: RawEl = { ...form.binRest, kind: 'threshold' };
    if (form.binControllerId) sensor['controllerId'] = form.binControllerId;
    out['bin'] = { sensor };
  } else {
    delete out['bin'];
  }

  return out;
}
