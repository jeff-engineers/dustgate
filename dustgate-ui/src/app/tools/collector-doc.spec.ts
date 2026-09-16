// Suite for collector-doc.ts — what the collector sheet writes.
//
// This is the half that can be silently wrong: a field in the wrong branch still
// validates, and the collector just never starts. Every case here is a document
// shape topology.js either requires or rejects, so a change that breaks one is a
// change that breaks a real shop rather than a test.
//
// Same contract as the other UI suites: throw nothing, print a tally, set
// process.exitCode on failure. Run by spec-runner.js.

import {
  CollectorForm, RawEl, DEFAULT_COAST_SEC, DEFAULT_CT_CHANNEL, ROCKLER_ADDRESS,
  fused, readCollector, writeCollector,
} from './collector-doc';
import { validateTopology } from '@topology';

// The model's OWN fixture, pulled in at run time. It is plain JS with no
// declaration file, and the two things this spec wants from it are easier to
// type here than to declare for the whole repo. spec-runner.js maps the name.
// eslint-disable-next-line @typescript-eslint/no-var-requires
const { clone, star } = require('@topology-fixtures') as {
  clone: <T>(o: T) => T;
  star: { elements: RawEl[] };
};

let pass = 0, fail = 0;
function check(what: string, ok: boolean, extra = ''): void {
  if (ok) { pass++; console.log(`  ✓ ${what}`); }
  else { fail++; console.error(`  ✗ ${what}${extra ? ' — ' + extra : ''}`); }
}
const eq = (what: string, got: unknown, want: unknown): void =>
  check(what, JSON.stringify(got) === JSON.stringify(want),
        `got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);

/** Key order is not part of the contract — JSON.stringify says it is, and a
 *  round-trip assertion written with it fails on a document that is identical in
 *  every way a reader cares about. Sorts keys at every depth, then compares. */
function sortKeys(v: unknown): unknown {
  if (Array.isArray(v)) return v.map(sortKeys);
  if (v && typeof v === 'object') {
    const o = v as Record<string, unknown>;
    return Object.fromEntries(Object.keys(o).sort().map(k => [k, sortKeys(o[k])]));
  }
  return v;
}
const sameDoc = (what: string, got: unknown, want: unknown): void =>
  check(what, JSON.stringify(sortKeys(got)) === JSON.stringify(sortKeys(want)),
        `got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);

const collector = (extra: RawEl = {}): RawEl =>
  ({ id: 'dc', type: 'collector', name: 'Cyclone', ...extra });

const shelly = (ip: string): RawEl => ({ gen: 2, ip });
const tasmota = (ip: string): RawEl => ({ gen: 0, ip, kind: 'tasmota' });

// ── reading ─────────────────────────────────────────────────────────────────
{
  const form = readCollector(collector());
  eq('a bare collector is switched by nothing', form.ctl, 'none');
  eq('...watched by nothing', form.sense, 'none');
  check('...with no bin', form.bin === false);
  eq('...and the default coast-down', form.coastSec, DEFAULT_COAST_SEC);
  eq('...and the Rockler address, matching the firmware fallback',
     form.rfAddress, ROCKLER_ADDRESS);
}

{
  const form = readCollector(collector({
    control: { outlet: shelly('10.0.0.9'), offDelayMs: 12000 },
  }));
  eq('a plug under control reads as switched by a plug', form.ctl, 'plug');
  eq('...and its ip comes through', form.ctlPlug.ip, '10.0.0.9');
  eq('...and the coast-down in whole seconds', form.coastSec, 12);
}

{
  // Not floored: the slider counts whole seconds, and 4.4 s must not read as 4
  // and then be written back as 4000, quietly editing a number nobody touched.
  eq('a fractional coast-down rounds', readCollector(collector({
    control: { offDelayMs: 4400 },
  })).coastSec, 4);
  eq('...and rounds up as well', readCollector(collector({
    control: { offDelayMs: 4600 },
  })).coastSec, 5);
}

{
  const form = readCollector(collector({
    control: { rf: { address: 200, pin: 9, data: 14 } },
    sensor: { outlet: tasmota('10.0.0.11') },
    bin: { sensor: { kind: 'threshold', controllerId: 'node-dc' } },
  }));
  eq('an rf block reads as switched by the remote', form.ctl, 'rf');
  eq('...with its address', form.rfAddress, 200);
  eq('...and pin/data kept aside, because no screen asks about them',
     form.rfRest, { pin: 9, data: 14 });
  eq('a sensor plug reads as watched by a plug', form.sense, 'plug');
  eq('...and keeps its kind', form.sensePlug.kind, 'tasmota');
  check('a bin reads as on', form.bin === true);
  eq('...on its board', form.binControllerId, 'node-dc');
}

{
  // Absent `kind` means shelly on BOTH sides, and an unknown string degrades the
  // same way — so a document from a newer UI falls back to the old behaviour
  // rather than to a plug that reads nothing.
  eq('an absent kind reads as shelly',
     readCollector(collector({ sensor: { outlet: { gen: 2, ip: '10.0.0.4' } } })).sensePlug.kind,
     'shelly');
  eq('an unknown kind degrades to shelly',
     readCollector(collector({ sensor: { outlet: { ip: '10.0.0.4', kind: 'zigbee' } } })).sensePlug.kind,
     'shelly');
}

// ── writing ─────────────────────────────────────────────────────────────────
const form = (over: Partial<CollectorForm> = {}): CollectorForm => ({
  ...readCollector(collector()), ...over,
});
const plug = (ip: string, kind: 'shelly' | 'tasmota' = 'shelly') =>
  ({ ip, host: '', label: '', gen: kind === 'tasmota' ? 0 : 2, kind });

{
  const el = writeCollector(collector(), form({ ctl: 'none', coastSec: 8 }));
  eq('nothing switches it, but the coast-down still lands',
     el['control'], { offDelayMs: 8000 });
  check('and no sensor branch is invented', el['sensor'] === undefined);
  check('and no bin branch is invented', el['bin'] === undefined);
}

{
  const el = writeCollector(collector(), form({
    ctl: 'plug', ctlPlug: plug('10.0.0.9'), coastSec: 10,
  }));
  eq('a switching plug is written under control',
     el['control'], { offDelayMs: 10000, outlet: { gen: 2, ip: '10.0.0.9' } });
  check('`kind` is omitted for a shelly — silence already says shelly',
     !('kind' in ((el['control'] as RawEl)['outlet'] as RawEl)));
}

{
  // THE BUG THE SHEET EXISTS TO PREVENT. A Tasmota has no relay, so naming one
  // as the switch describes a collector that can never start — validateTopology()
  // rejects the whole document. The sheet refuses the pick, and this is the
  // belt-and-braces: even a form that somehow says tasmota writes a shelly.
  const el = writeCollector(collector(), form({
    ctl: 'plug', ctlPlug: plug('10.0.0.11', 'tasmota'),
  }));
  check('a sense-only plug is never written as the switch',
     !('kind' in ((el['control'] as RawEl)['outlet'] as RawEl)));
}

{
  // topology.js REQUIRES control.rf.pin, and the firmware silently builds no
  // presser without one — so a sheet that never asks for it still has to write
  // it. Saving an rf collector produced exactly that invalid document until
  // 2026-09-14.
  const rf = (writeCollector(collector(), form({ ctl: 'rf' }))['control'] as RawEl)['rf'] as RawEl;
  // NO pin, since 2026-09-16 — the firmware supplies its own pad (PIN_RF_TX) and
  // the UI has no business naming a GPIO. Writing one is what stranded every
  // saved layout on D9 when the transmitter moved to D10.
  eq('an rf press carries no pin — the board supplies its own pad', rf['pin'], undefined);

  // ...and never overwrites one the document already had: a board with its
  // transmitter on another pad must survive someone opening the sheet.
  const moved = collector({ control: { rf: { address: 94, pin: 4 } } });
  const kept = (writeCollector(moved, readCollector(moved))['control'] as RawEl)['rf'] as RawEl;
  eq('a pad the document already names is kept', kept['pin'], 4);
}

{
  const el = writeCollector(collector(), form({
    ctl: 'rf', rfAddress: 94, rfRest: { pin: 9, data: 14 },
  }));
  eq('an rf press keeps pin and data untouched',
     (el['control'] as RawEl)['rf'], { pin: 9, data: 14, address: 94 });
  check('and never alongside an outlet — two ways to command one blower fight',
     ((el['control'] as RawEl)['outlet']) === undefined);
}

{
  // Moving from a plug to the remote has to REMOVE the plug, not merge beside it.
  const was = collector({ control: { outlet: shelly('10.0.0.9'), offDelayMs: 8000 } });
  const el = writeCollector(was, form({ ...readCollector(was), ctl: 'rf' }));
  check('switching to the remote drops the plug',
     ((el['control'] as RawEl)['outlet']) === undefined);
  check('...and the original element is not mutated',
     ((was['control'] as RawEl)['outlet'] as RawEl)['ip'] === '10.0.0.9');
}

{
  const f = form({ ctl: 'plug', ctlPlug: plug('10.0.0.9'),
                   sense: 'plug', sensePlug: plug('10.0.0.11', 'tasmota') });
  check('a switchable plug answers both questions', fused(f));
  const el = writeCollector(collector(), f);
  check('...so nothing is written to sensor, even with one picked',
     el['sensor'] === undefined);
}

{
  const el = writeCollector(collector(), form({
    ctl: 'rf', sense: 'plug', sensePlug: plug('10.0.0.11', 'tasmota'),
  }));
  eq('a watcher is written when the switch cannot answer for itself',
     el['sensor'], { outlet: { gen: 0, ip: '10.0.0.11', kind: 'tasmota' } });
}

{
  // gen 0 survives: a Tasmota reports 0 because it HAS no generation, and
  // coercing it to 2 writes a Shelly generation into a document describing a
  // device that has never heard of one.
  const outlet = (writeCollector(collector(), form({
    ctl: 'none', sense: 'plug', sensePlug: plug('10.0.0.11', 'tasmota'),
  }))['sensor'] as RawEl)['outlet'] as RawEl;
  eq('a tasmota keeps generation 0', outlet['gen'], 0);
}

{
  const el = writeCollector(collector(), form({ bin: true }));
  eq('a bin with no board named means "this board"',
     el['bin'], { sensor: { kind: 'threshold' } });
  const named = writeCollector(collector(), form({ bin: true, binControllerId: 'node-dc' }));
  eq('...and a named board is carried',
     named['bin'], { sensor: { kind: 'threshold', controllerId: 'node-dc' } });
}

{
  // A field written by a newer UI survives a round trip through this one rather
  // than being silently dropped on the next Save.
  const was = collector({ bin: { sensor: { kind: 'threshold', invert: true } } });
  const el = writeCollector(was, readCollector(was));
  sameDoc('an unknown bin field survives a round trip',
     el['bin'], { sensor: { invert: true, kind: 'threshold' } });
}

{
  const was = collector({ bin: { sensor: { kind: 'threshold' } } });
  const el = writeCollector(was, form({ ...readCollector(was), bin: false }));
  check('turning the bin off removes the branch', el['bin'] === undefined);
}

// ── the whole round trip ────────────────────────────────────────────────────
{
  // The collector actually on the bench: switched by its remote, watched by the
  // Athom, beam across the bin. Read it, write it back unchanged, and nothing
  // may move — this is what a user opening the sheet and pressing Save does.
  const was = collector({
    control: { rf: { address: 94, pin: 9, data: 14 }, offDelayMs: 8000 },
    sensor: { outlet: { gen: 0, ip: '192.168.87.44', kind: 'tasmota' } },
    bin: { sensor: { kind: 'threshold', controllerId: 'node-dc' } },
  });
  const el = writeCollector(was, readCollector(was));
  sameDoc('open and save changes nothing', el, was);
}

// ── against the real validator ──────────────────────────────────────────────
// The spec above asserts SHAPE; this asserts the shape is one topology.js
// accepts. Worth the extra import: every bug this file has caught so far
// produced a document that looked right and was either refused, or accepted and
// then silently ignored by the firmware.
//
// Built on the model's OWN `star` fixture rather than a topology hand-rolled
// here. The first attempt did roll one, and it failed on selector rules that
// have nothing to do with collectors — a test that fails for reasons it is not
// about is worse than no test.
{
  const ok = (what: string, el: RawEl) => {
    const t = clone(star) as { elements: RawEl[] };
    const i = t.elements.findIndex(e => e['id'] === 'dc');
    t.elements[i] = { ...t.elements[i], ...el };
    const r = validateTopology(t as never);
    check(what, r.ok, JSON.stringify(r.errors));
  };
  const dc = (over: Partial<CollectorForm>) =>
    writeCollector({ id: 'dc', type: 'collector' }, form(over));

  ok('a remote-pressed collector validates', dc({ ctl: 'rf' }));
  ok('...with a watcher alongside it',
     dc({ ctl: 'rf', sense: 'plug', sensePlug: plug('10.0.0.11', 'tasmota') }));
  ok('...and a bin on this board', dc({ ctl: 'rf', bin: true }));
  ok('...and a bin on a named board',
     dc({ ctl: 'rf', bin: true, binControllerId: 'primary' }));
  ok('a plug-switched collector validates',
     dc({ ctl: 'plug', ctlPlug: plug('10.0.0.9') }));
  ok('a collector nothing switches validates', dc({ ctl: 'none' }));

  // A named board has to resolve, for the same reason a selector's does: a typo
  // that silently means "local" is a shop where the wrong board is watching.
  const t = clone(star) as { elements: RawEl[] };
  const i = t.elements.findIndex(e => e['id'] === 'dc');
  t.elements[i] = { ...t.elements[i], ...dc({ bin: true, binControllerId: 'nope' }) };
  check('a bin on a board that does not exist is refused', !validateTopology(t as never).ok);
}

// ── sensor.ct: a clamp, for a blower with no plug to meter ──────────────────
//
// It matters MORE here than on a tool. Every way DustGate commands a blower is
// STATELESS — a servo pressing a fob, an RF frame — so what we sent proves
// nothing, and `sensor` is the only thing that can say the press landed. A 240V
// blower is exactly the case with no plug to pair.
{
  const bare = readCollector(collector({ sensor: { ct: { channel: 0 } } }));
  eq('a clamped collector reads as watched by a clamp', bare.sense, 'ct');
  eq('...on THIS board when no controllerId is written', bare.senseCtControllerId, '');

  const named = readCollector(collector({
    sensor: { ct: { controllerId: 'planer-node', channel: 0 } },
  }));
  eq('...or on the board it names', named.senseCtControllerId, 'planer-node');

  // WRITING
  sameDoc('a clamp on this board writes no controllerId',
    writeCollector({ id: 'dc', type: 'collector' },
                   form({ ctl: 'rf', sense: 'ct' }))['sensor'],
    { ct: { channel: DEFAULT_CT_CHANNEL } });

  sameDoc('...and names the board when one is chosen',
    writeCollector({ id: 'dc', type: 'collector' },
                   form({ ctl: 'rf', sense: 'ct', senseCtControllerId: 'planer-node' }))['sensor'],
    { ct: { channel: DEFAULT_CT_CHANNEL, controllerId: 'planer-node' } });

  // A clamp and a plug are ONE question — validateTopology() refuses both — so
  // choosing the clamp must REMOVE the plug rather than leave it behind.
  const wasPlug = writeCollector(
    collector({ sensor: { outlet: shelly('10.0.0.11') } }),
    form({ ctl: 'rf', sense: 'ct' }));
  sameDoc('switching a plug to a clamp drops the plug',
    wasPlug['sensor'], { ct: { channel: DEFAULT_CT_CHANNEL } });

  // ...and back again.
  const backToPlug = writeCollector(
    collector({ sensor: { ct: { channel: 0 } } }),
    form({ ctl: 'rf', sense: 'plug', sensePlug: plug('10.0.0.11', 'tasmota') }));
  check('switching a clamp back to a plug drops the clamp',
    (backToPlug['sensor'] as RawEl)['ct'] === undefined);

  // A SWITCHABLE plug already reports its own power, so the sheet collapses the
  // watch question entirely — and a clamp must not survive that collapse.
  const fusedDoc = writeCollector({ id: 'dc', type: 'collector' },
    form({ ctl: 'plug', ctlPlug: plug('10.0.0.9', 'shelly'), sense: 'ct' }));
  check('a fused collector writes no sensor at all', fusedDoc['sensor'] === undefined);

  // A field written by a newer UI survives a round trip through this one.
  const kept = writeCollector(
    collector({ sensor: { ct: { channel: 0, tuning: 'auto' } } }),
    readCollector(collector({ sensor: { ct: { channel: 0, tuning: 'auto' } } })));
  check('an unknown ct field survives a round trip',
    ((kept['sensor'] as RawEl)['ct'] as RawEl)['tuning'] === 'auto');
}

// ── and the real validator agrees ───────────────────────────────────────────
{
  const t = clone(star) as { elements: RawEl[] };
  const i = t.elements.findIndex(e => e['id'] === 'dc');
  t.elements[i] = { ...t.elements[i],
                    ...writeCollector({ id: 'dc', type: 'collector' },
                                      form({ ctl: 'rf', sense: 'ct' })) };
  check('a clamped collector validates', validateTopology(t as never).ok,
        JSON.stringify(validateTopology(t as never).errors));

  const bad = clone(star) as { elements: RawEl[] };
  const j = bad.elements.findIndex(e => e['id'] === 'dc');
  bad.elements[j] = { ...bad.elements[j],
                      ...writeCollector({ id: 'dc', type: 'collector' },
                        form({ ctl: 'rf', sense: 'ct', senseCtControllerId: 'nope' })) };
  check('a clamp on a board that does not exist is refused',
        !validateTopology(bad as never).ok);
}

console.log(`\n${pass}/${pass + fail} passed`);
if (fail) process.exitCode = 1;
