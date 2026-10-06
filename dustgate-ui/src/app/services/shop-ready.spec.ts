/** shop-ready — "land on Live, or drop them in the layout tool?"
 *
 *  Plain TypeScript, no Angular, no browser. Run with `npm test`.
 *
 *  This runs on every app open and decides where the user ends up, so both wrong
 *  answers are bad in different ways: a false "ready" lands someone on a Live view
 *  that can't drive anything, and a false "not ready" sends a finished shop back
 *  to the builder every single time it opens.
 *
 *  The bar is higher than "the document is valid" — a layout can be perfectly
 *  well-formed and still have a gate nobody has measured.
 */

import { suite } from '../../test-harness';
import type { Topology } from '@topology';
import { shopReadiness } from './shop-ready';

const { check, eq, report } = suite();

// Single-system fixture lifted into a shop (the device refuses schemaVersion 1; this is test-only).
const { shopFromV1 } = require('@topology-fixtures') as { shopFromV1: (t: unknown) => unknown };

/** A complete, calibrated, one-system layout in the single-system fixture shape. */
const readyV1 = () => JSON.parse(JSON.stringify({
  schemaVersion: 1,
  name: 'Shop',
  controllers: [{ id: 'primary', role: 'primary', name: 'Brain', board: 'devkitc' }],
  elements: [
    { id: 'dc', type: 'collector', name: 'Cyclone' },
    {
      id: 'gate', type: 'selector', name: 'Gate', controllerId: 'primary', kind: 'servoGate',
      states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
      branches: [{ id: 'b1', opensState: 'open', role: 'tool' }],
      servo: { channel: 0, referenceAngle: 90 },
    },
    { id: 'saw', type: 'tool', name: 'Table saw' },
  ],
  ducts: [{ child: 'gate', parent: 'dc' }, { child: 'saw', parent: 'gate', parentBranch: 'b1' }],
})) as Topology;

/** The "ready" baseline: that layout as a shop. */
const ready = () => shopFromV1(readyV1()) as Topology;
// The one system's body: mutations below edit it in place.
const sys0 = (t: Topology) => (t as unknown as { systems: { elements: Record<string, unknown>[]; ducts: Record<string, unknown>[] }[] }).systems[0];
const mut = (fn: (t: Topology) => void): Topology => {
  const t = ready();
  fn(t);
  return t;
};
const els = (t: Topology) => sys0(t).elements;

// ── the ready case ──────────────────────────────────────────────────────────
{
  const r = shopReadiness(ready());
  check('a complete calibrated shop is ready', r.ready, r.reason);
  eq('and says nothing', r.reason, '');
}

// ── not ready, with a reason a woodworker can act on ────────────────────────
{
  check('no layout at all', !shopReadiness(null).ready);
  eq('...and says so plainly', shopReadiness(null).reason, 'No layout saved yet.');

  // Machines, not ports: "no tools yet" is about things you can switch on.
  const noTools = mut(t => {
    sys0(t).elements = els(t).filter(e => e['type'] !== 'tool');
    (t as unknown as { machines: unknown[] }).machines = [];
    sys0(t).ducts = [{ child: 'gate', parent: 'dc' }];
    (els(t).find(e => e['id'] === 'gate')!['branches'] as Record<string, unknown>[])[0]['role'] = 'blocked';
  });
  const r = shopReadiness(noTools);
  check('a shop with no tools is not ready', !r.ready);
  eq('...for the right reason', r.reason, 'No tools on the layout yet.');
}
{
  // A gate nobody has measured can't be driven, so the shop isn't ready even
  // though the document is fine. This is the common "almost done" case, and the
  // one a validity-only check would wave through.
  const uncalibrated = mut(t => {
    delete (els(t).find(e => e['id'] === 'gate')!['servo'] as Record<string, unknown>)['referenceAngle'];
  });
  const r = shopReadiness(uncalibrated);
  check('an unmeasured gate blocks readiness', !r.ready);
  check('...and the message names the gate', r.reason.includes('Gate'), r.reason);
}
{
  // A tool with no gate between it and the collector leaks suction: it can never
  // be selected on its own.
  const leaky = mut(t => {
    els(t).push({ id: 'loose', type: 'tool', name: 'Loose tool', machineId: 'loose' });
    (t as unknown as { machines: unknown[] }).machines.push({ id: 'loose', name: 'Loose tool' });
    sys0(t).ducts.push({ child: 'loose', parent: 'dc' });
  });
  const r = shopReadiness(leaky);
  check('an ungated tool blocks readiness', !r.ready);
  check('...and the message names it', r.reason.includes('Loose tool'), r.reason);
}
{
  const broken = mut(t => { sys0(t).elements = []; sys0(t).ducts = []; });
  const r = shopReadiness(broken);
  check('a structurally invalid layout is not ready', !r.ready);
  check('...and leads with "the layout has a problem"',
    r.reason.startsWith('The layout has a problem'), r.reason);
}

// ── a schemaVersion-1 layout ────────────────────────────────────────────────
//
// The device refuses one and the app no longer reads one, so a v1 document is NOT ready; it must not throw on the way to
// saying so (this runs on the entry redirect, on whatever a board hands back).
{
  const r = shopReadiness(readyV1());
  check('a v1 layout is not ready', !r.ready);
}

report();
