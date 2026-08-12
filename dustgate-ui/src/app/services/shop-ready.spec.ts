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

/** A complete, calibrated, one-system v1 shop: the "ready" baseline. */
const ready = () => JSON.parse(JSON.stringify({
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

const mut = (fn: (t: Record<string, unknown>) => void): Topology => {
  const t = ready() as unknown as Record<string, unknown>;
  fn(t);
  return t as unknown as Topology;
};
const els = (t: Topology) => (t as unknown as { elements: Record<string, unknown>[] }).elements;

// ── the ready case ──────────────────────────────────────────────────────────
{
  const r = shopReadiness(ready());
  check('a complete calibrated v1 shop is ready', r.ready, r.reason);
  eq('and says nothing', r.reason, '');
}

// ── not ready, with a reason a woodworker can act on ────────────────────────
{
  check('no layout at all', !shopReadiness(null).ready);
  eq('...and says so plainly', shopReadiness(null).reason, 'No layout saved yet.');

  // Machines, not ports: "no tools yet" is about things you can switch on.
  const noTools = mut(t => {
    (t['elements'] as Record<string, unknown>[]) = els(t as Topology).filter(e => e['type'] !== 'tool');
    (t['ducts'] as Record<string, unknown>[]) = [{ child: 'gate', parent: 'dc' }];
    (els(t as Topology).find(e => e['id'] === 'gate')!['branches'] as Record<string, unknown>[])[0]['role'] = 'blocked';
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
    delete (els(t as Topology).find(e => e['id'] === 'gate')!['servo'] as Record<string, unknown>)['referenceAngle'];
  });
  const r = shopReadiness(uncalibrated);
  check('an unmeasured gate blocks readiness', !r.ready);
  check('...and the message names the gate', r.reason.includes('Gate'), r.reason);
}
{
  // A tool with no gate between it and the collector leaks suction: it can never
  // be selected on its own.
  const leaky = mut(t => {
    els(t as Topology).push({ id: 'loose', type: 'tool', name: 'Loose tool' });
    (t['ducts'] as Record<string, unknown>[]).push({ child: 'loose', parent: 'dc' });
  });
  const r = shopReadiness(leaky);
  check('an ungated tool blocks readiness', !r.ready);
  check('...and the message names it', r.reason.includes('Loose tool'), r.reason);
}
{
  const broken = mut(t => { (t['elements'] as unknown[]) = []; (t['ducts'] as unknown[]) = []; });
  const r = shopReadiness(broken);
  check('a structurally invalid layout is not ready', !r.ready);
  check('...and leads with "the layout has a problem"',
    r.reason.startsWith('The layout has a problem'), r.reason);
}

// ── it migrates too ─────────────────────────────────────────────────────────
//
// This runs on the entry redirect, which may see a document straight off a board
// that has never been resaved. Before the migration was added here, a v1 layout
// validated as a shop, failed, and bounced a finished shop into the builder on
// every open.
{
  const shop = {
    schemaVersion: 2,
    name: 'Shop',
    controllers: [{ id: 'primary', role: 'primary', name: 'Brain', board: 'devkitc' }],
    systems: [{
      id: 'system-1',
      elements: els(ready()).map(e => (e['type'] === 'tool' ? { ...e, machineId: 'saw' } : e)),
      ducts: [{ child: 'gate', parent: 'dc' }, { child: 'saw', parent: 'gate', parentBranch: 'b1' }],
    }],
    machines: [{ id: 'saw', name: 'Table saw' }],
  } as unknown as Topology;

  const r = shopReadiness(shop);
  check('a native v2 shop is ready', r.ready, r.reason);

  const v1r = shopReadiness(ready());
  eq('and a v1 layout gives the SAME answer as its v2 form', v1r.ready, r.ready);
}

report();
