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
  drivesFromCaps, drivesFromHasLinear, resolveDrives,
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

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exitCode = 1;
