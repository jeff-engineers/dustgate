/** Board-drives cases B1–B4.
 *
 *  Plain TypeScript on purpose — no Angular, no browser. Run with:
 *      npm run test:boards
 *
 *  B4 is the reason this file exists. Every symptom on 2026-09-02 — a slider node
 *  listed as "0 of 4 gates", a second sliding gate defaulting onto a servo board,
 *  and a save refused with "set up as a servo board but has a sliding gate on it" —
 *  came from the same gap: nothing tested that a PAIRED slider node survives a
 *  round trip to the validator. The unit cases below are cheap; B4 is the one that
 *  would have caught it.
 */

import {
  type Drives, DEFAULT_DRIVES, applyDrivesCache, canHost,
  drivesFromCaps, drivesFromHasLinear, portShortfalls, resolveDrives, shortfallText,
} from './board-drives';
import { validateTopology } from '@topology';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
function group(name: string): void { console.log(`\n${name}`); }

// ── B1 · reading a board's own report ────────────────────────────────────────
group('B1 a board reports what it drives; silence is not a report');
{
  ok('hasLinear true → linear', drivesFromHasLinear(true) === 'linear');
  ok('hasLinear false → servo', drivesFromHasLinear(false) === 'servo');
  // Undefined is a board that has NOT SAID — distinct from one saying "no slider".
  // Collapsing the two is what makes a cache overwrite itself with a guess.
  ok('hasLinear absent → no report', drivesFromHasLinear(undefined) === null);
  ok('caps.linear 1 → linear', drivesFromCaps({ linear: 1 }) === 'linear');
  ok('caps.linear 0 → servo', drivesFromCaps({ linear: 0 }) === 'servo');
  ok('no caps at all → no report', drivesFromCaps(null) === null);
  ok('caps without linear → no report', drivesFromCaps({}) === null);
}

// ── B2 · precedence ──────────────────────────────────────────────────────────
group('B2 the live report beats the cache, always');
{
  // The bug this encodes: the cache is only written when someone opens the Boards
  // screen, and nothing makes them. A freshly flashed slider therefore has a
  // 'servo' cache and a 'linear' report, and the report is the true one.
  ok('report wins over a stale cache', resolveDrives('linear', 'servo') === 'linear');
  ok('...and in the other direction too', resolveDrives('servo', 'linear') === 'servo');
  ok('cache carries a board that has not reported', resolveDrives(null, 'linear') === 'linear');
  ok('nothing anywhere → servo', resolveDrives(null, undefined) === DEFAULT_DRIVES);
  ok('the default is servo', DEFAULT_DRIVES === 'servo');
}

// ── B3 · what a board may host ───────────────────────────────────────────────
group('B3 a board hosts one kind of gate, never both');
{
  ok('a slider board takes a sliding gate', canHost('linear', 'linear'));
  ok('...and nothing else', !canHost('linear', 'servoGate') && !canHost('linear', 'servoManifold'));
  ok('a servo board takes valves', canHost('servo', 'servoGate') && canHost('servo', 'servoManifold'));
  ok('...and refuses a sliding gate', !canHost('servo', 'linear'));

  // The cache is written as an ABSENCE for the default, so a document round-trips
  // byte-identical to one saved before the field existed.
  const c: Record<string, unknown> = { id: 'n1', drives: 'linear' };
  applyDrivesCache(c, 'servo');
  ok('caching servo deletes the field', !('drives' in c));
  applyDrivesCache(c, 'linear');
  ok('caching linear writes it', c['drives'] === 'linear');
  applyDrivesCache(c, null);
  ok('no report leaves the cache alone', c['drives'] === 'linear');
}

// ── B4 · the round trip that broke ───────────────────────────────────────────
group('B4 a paired slider node survives a save');
{
  /** The shape mergePairedBoards() builds for a newly paired node, and the shape
   *  the canvas then saves. */
  const merge = (id: string, caps: { linear?: number }) => {
    const drives = drivesFromCaps(caps);
    const c: Record<string, unknown> = {
      id, role: 'secondary', name: id, link: { transport: 'wifi-ws', host: `${id}.local` },
    };
    applyDrivesCache(c, drives);
    return c;
  };

  const docWith = (controller: Record<string, unknown>) => ({
    schemaVersion: 1, name: 'paired slider',
    controllers: [{ id: 'primary', role: 'primary' }, controller],
    elements: [
      { id: 'dc', type: 'collector' },
      {
        id: 'slide', type: 'selector', kind: 'linear', controllerId: controller['id'],
        states: [{ id: 'home', isClosed: true, positionMm: 0 },
                 { id: 's1', isClosed: false, positionMm: 100 }],
        branches: [{ id: 'b', opensState: 's1', role: 'tool' }],
      },
      { id: 'saw', type: 'tool' },
    ],
    ducts: [{ child: 'slide', parent: 'dc' },
            { child: 'saw', parent: 'slide', parentBranch: 'b' }],
  });

  const slider = merge('dustgate-slider', { linear: 1 });
  ok('a slider node is merged in as a slider board', slider['drives'] === 'linear');

  const good = validateTopology(docWith(slider));
  ok('...and the layout it hosts validates', good.ok,
     good.errors.map(e => e.message).join('; '));

  // The regression, stated as the failure it actually produced. Before the fix,
  // mergePairedBoards() wrote no `drives` at all — the canvas still DREW the board
  // correctly, because it reads the live report, but the SAVED document said
  // nothing and the validator's `c.drives || 'servo'` default did the rest.
  const forgotten = merge('dustgate-slider', {});
  ok('a node that never reported caps carries no cache', !('drives' in forgotten));
  const bad = validateTopology(docWith(forgotten));
  ok('...and that is exactly what the device refused', !bad.ok);
  ok('...with the message that was on screen',
     bad.errors.some(e => /servo board but has a sliding gate/.test(e.message)),
     bad.errors.map(e => e.message).join('; '));
}

// ── B5 · counting ports against gates ────────────────────────────────────────
//
// The shortfall is what the guide bar says when the shop has run out of somewhere
// to plug a gate in, and the reason it counts each kind separately is the same
// reason canHost() is an equality test: a spare servo channel cannot take a rack.
group('B5 a shop can run out of ports, and of one kind at a time');
{
  const S = 4;   // SERVO_PORTS, passed in by the caller

  ok('one board, four valves, no shortfall',
     portShortfalls(['servo'], ['servoGate', 'servoGate', 'servoGate', 'servoManifold'], S).length === 0);

  const fifth = portShortfalls(['servo'], Array(5).fill('servoGate'), S);
  ok('...the fifth valve is one too many', fifth.length === 1);
  ok('...and it is counted, not just flagged',
     fifth[0].kind === 'servo' && fifth[0].gates === 5 && fifth[0].ports === 4);

  // The case a single total would get wrong, and the whole reason for two counts.
  const spare = portShortfalls(['servo'], ['servoGate', 'linear'], S);
  ok('a servo board with three channels free still cannot take a rack', spare.length === 1);
  ok('...and the shortfall names the slider, not the servos',
     spare[0].kind === 'linear' && spare[0].ports === 0);

  ok('a slider board takes exactly one rack',
     portShortfalls(['linear'], ['linear'], S).length === 0);
  ok('...and refuses the second', portShortfalls(['linear'], ['linear', 'linear'], S).length === 1);
  // A slider board contributes NO servo channels: the pads are the serial bus.
  const sliderOnly = portShortfalls(['linear'], ['servoGate'], S);
  ok('a slider board offers no servo channel at all',
     sliderOnly.length === 1 && sliderOnly[0].kind === 'servo' && sliderOnly[0].ports === 0);

  ok('both kinds short is reported as both',
     portShortfalls(['servo'], Array(5).fill('servoGate').concat(['linear']), S).length === 2);

  ok('an empty shop is not short of anything', portShortfalls([], [], S).length === 0);
  ok('...but a gate with no board at all is',
     portShortfalls([], ['servoGate'], S).length === 1);

  // Wording: the sentence has to survive the zero case, which is the one a shop
  // hits first — a rack drawn before any slider board is paired.
  ok('zero ports reads as "none", never "only 0"',
     /has none —/.test(shortfallText({ kind: 'linear', gates: 1, ports: 0 })));
  ok('one gate is singular',
     /1 sliding gate needs/.test(shortfallText({ kind: 'linear', gates: 1, ports: 0 })));
  ok('two gates are plural',
     /2 sliding gates need/.test(shortfallText({ kind: 'linear', gates: 2, ports: 1 })));
  ok('a servo shortfall says how many channels there are',
     /only 4 —/.test(shortfallText({ kind: 'servo', gates: 5, ports: 4 })));
}

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exitCode = 1;
