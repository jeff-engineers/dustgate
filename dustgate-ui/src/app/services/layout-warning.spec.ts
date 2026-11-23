/** layout-warning — a layout saved with problems names the tools, gates, plugs, clamps and boards that may not work (D-88).
 *
 *  Plain TypeScript, no Angular, no browser. Run with `npm test`.
 *
 *  Driven by the REAL validator: each case breaks a whole shop the way the canvas or a stale document can, and asserts
 *  what validateShop's errors become. A model rewording that moves an id out of `ref` shows up here.
 */

import { suite } from '../../test-harness';
import { validateShop } from '@shop';
import type { ShopDoc } from './shop-doc';
import { affectedNames, layoutWarning } from './layout-warning';

const { check, eq, report } = suite();

/** A whole, valid shop: a PWM board with two gates, a bus board with one, three tools with plugs. */
const whole = (): ShopDoc => JSON.parse(JSON.stringify({
  schemaVersion: 2,
  name: 'Shop',
  controllers: [
    { id: 'primary', role: 'primary', name: 'Shop Brain' },
    { id: 'wall', role: 'secondary', name: 'Back wall', link: { transport: 'wifi-ws', host: 'wall' } },
    { id: 'bus', role: 'secondary', name: 'Serial 1', drives: 'bus', link: { transport: 'wifi-ws', host: 'bus' }, chains: [['g3']] },
  ],
  systems: [{
    id: 's1', name: 'Shop',
    elements: [
      { id: 'dc', type: 'collector', name: 'Cyclone' },
      { id: 'g1', type: 'selector', name: 'Saw gate', controllerId: 'wall', kind: 'servoGate',
        states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
        branches: [{ id: 'b1', opensState: 'open', role: 'tool' }], servo: { channel: 0, referenceAngle: 90 } },
      { id: 'g2', type: 'selector', name: 'Planer gate', controllerId: 'wall', kind: 'servoGate',
        states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
        branches: [{ id: 'b2', opensState: 'open', role: 'tool' }], servo: { channel: 1, referenceAngle: 90 } },
      { id: 'g3', type: 'selector', name: 'Bandsaw gate', controllerId: 'bus', kind: 'servoGate',
        states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
        branches: [{ id: 'b3', opensState: 'open', role: 'tool' }], servo: { bus: 2, referenceAngle: 90 } },
      { id: 'saw', type: 'tool', name: 'Table saw', machineId: 'saw' },
      { id: 'planer', type: 'tool', name: 'Planer', machineId: 'planer' },
      { id: 'bandsaw', type: 'tool', name: 'Bandsaw', machineId: 'bandsaw' },
    ],
    ducts: [
      { child: 'g1', parent: 'dc' }, { child: 'g2', parent: 'dc' }, { child: 'g3', parent: 'dc' },
      { child: 'saw', parent: 'g1', parentBranch: 'b1' },
      { child: 'planer', parent: 'g2', parentBranch: 'b2' },
      { child: 'bandsaw', parent: 'g3', parentBranch: 'b3' },
    ],
  }],
  machines: [
    { id: 'saw', name: 'Table saw', sensor: { outlet: { gen: 2, ip: '10.0.0.40' } } },
    { id: 'planer', name: 'Planer', sensor: { outlet: { gen: 2, ip: '10.0.0.41' } } },
    { id: 'bandsaw', name: 'Bandsaw' },
  ],
  devices: [],
})) as ShopDoc;
const els = (d: ShopDoc) => d.systems[0].elements as Record<string, unknown>[];
const errorsOf = (d: ShopDoc) => validateShop(d).errors;

{
  const v = validateShop(whole());
  check('the baseline is a whole shop', v.ok, JSON.stringify(v.errors));
}

// ── the bug that started this: a chain naming a gate that was deleted ──────
{
  const d = whole();
  (d['controllers'] as Record<string, unknown>[])[2]['chains'] = [['g3', 'sel13']];
  const errs = errorsOf(d);
  eq('a stale chain entry names its board, not the missing gate', affectedNames(d, errs), ['board Serial 1']);
  const w = layoutWarning(d, errs, true);
  eq('and says so in the shop’s words', w,
    'Saved — but board Serial 1 may not work properly until this is fixed: '
    + 'The servo chain on board “Serial 1” lists a gate that’s no longer on the layout. Take it off the chain.');
  check('no id reaches the sentence', !w.includes('sel13'), w);
  eq('away from the save it leads with what may not work', layoutWarning(d, errs).split(':')[0],
    'Board Serial 1 may not work properly until the layout is fixed');
}

// ── two gates on one servo channel: the brain moves neither, so both are named ──
{
  const d = whole();
  (els(d).find((e) => e['id'] === 'g2')!['servo'] as Record<string, unknown>)['channel'] = 0;
  eq('both gates and their board are named', affectedNames(d, errorsOf(d)), ['Planer gate', 'board Back wall', 'Saw gate']);
}
{
  // A gate named after its tool is still called a gate, or the sentence would accuse the tool.
  const d = whole();
  els(d).find((e) => e['id'] === 'g2')!['name'] = 'Planer';
  (els(d).find((e) => e['id'] === 'g2')!['servo'] as Record<string, unknown>)['channel'] = 0;
  check('a gate named after its tool reads as a gate', affectedNames(d, errorsOf(d)).includes('Planer gate'), JSON.stringify(affectedNames(d, errorsOf(d))));
}

// ── a plug on two tools: the plug is the tool's ─────────────────────────────
{
  const d = whole();
  ((d.machines[1] as Record<string, unknown>)['sensor'] as Record<string, Record<string, unknown>>)['outlet']['ip'] = '10.0.0.40';
  eq('a shared plug is named as each tool’s plug', affectedNames(d, errorsOf(d)), ['Planer’s plug', 'Table saw’s plug']);
}

// ── a tool with no duct: the tool, and not the collector its system is named after ──
{
  const d = whole();
  d.systems[0].ducts = d.systems[0].ducts.filter((x) => x['child'] !== 'bandsaw');
  const errs = errorsOf(d);
  const names = affectedNames(d, errs);
  check('an unpiped tool is named', names.includes('Bandsaw'), JSON.stringify(names));
  check('...and its collector is not accused', !names.includes('Cyclone'), JSON.stringify(names));
}

// ── many problems: the bar stays one line ───────────────────────────────────
{
  const d = whole();
  d.systems[0].ducts = [];
  const w = layoutWarning(d, errorsOf(d), true);
  check('past four names it counts the rest', / and \d+ more may not work properly/.test(w), w);
}

// ── nothing nameable ────────────────────────────────────────────────────────
{
  const w = layoutWarning(whole(), [{ code: 'shape', message: 'devMode must be true or false' }], true);
  check('a problem that names nothing still says the shop may misbehave', w.startsWith('Saved — but part of the shop may not work properly'), w);
}

report();
