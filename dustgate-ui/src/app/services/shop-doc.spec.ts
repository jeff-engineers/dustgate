/** shop-doc — the UI's editing seam onto a schemaVersion-2 shop.
 *
 *  Plain TypeScript, no Angular, no browser. Run with `npm test`.
 *
 *  These are the cases the browser pass can only check by eye, and the ones that
 *  go wrong quietly: a machine left behind after its last port is deleted, a plug
 *  read from the port instead of the machine, a v1 layout that stops migrating.
 *  None of them throws — they just make the shop route nothing.
 */

import { suite } from '../../test-harness';
import { validateShop } from '@shop';
import {
  type ShopDoc, type RawEl,
  addMachineWithPort, addSupplementalPort, displayName, isPortEnabled, isShopDoc, machineById,
  machineIdOfPort, machineOfPort, machinesOf, outletOf, outletTakenByAnotherMachine,
  portsOf, primaryPortOf, removeMachine, removePort,
  healMachineNames, renameMachine, setOutlet,
  systemById, systemViews, systemsOf, toShop,
} from './shop-doc';

const { check, eq, report } = suite();

// A v1 topology, the shape every existing install is saved in.
const v1 = () => JSON.parse(JSON.stringify({
  schemaVersion: 1,
  name: 'Old Shop',
  controllers: [{ id: 'primary', role: 'primary', name: 'Brain', board: 'devkitc' }],
  elements: [
    { id: 'dc', type: 'collector', name: 'Cyclone' },
    {
      id: 'gate', type: 'selector', name: 'Gate', controllerId: 'primary', kind: 'servoGate',
      states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
      branches: [{ id: 'b1', opensState: 'open', role: 'tool' }],
      servo: { channel: 0, referenceAngle: 90 },
    },
    { id: 'saw', type: 'tool', name: 'Table saw', sensor: { outlet: { gen: 2, ip: '10.0.0.5', thresholdW: 50 } } },
  ],
  ducts: [{ child: 'gate', parent: 'dc' }, { child: 'saw', parent: 'gate', parentBranch: 'b1' }],
}));

// ── migration on read ───────────────────────────────────────────────────────
{
  const shop = toShop(v1())!;
  check('a v1 topology migrates to a shop', isShopDoc(shop));
  check('the migrated shop is valid', validateShop(shop).ok,
    JSON.stringify(validateShop(shop).errors));
  eq('its elements land in one system', systemsOf(shop).length, 1);

  // The id is REUSED on purpose (RFC §12): anything already holding it — a saved
  // ui.layout, a status blob, a bug report — has to keep resolving.
  const m = machineById(shop, 'saw');
  check('the tool became a machine with the same id', !!m);
  eq('the machine took the display name', m!.name, 'Table saw');
  eq('and the plug moved onto it', (m!.sensor?.outlet as RawEl)['ip'], '10.0.0.5');

  const port = systemsOf(shop)[0].elements.find(e => e['id'] === 'saw')!;
  eq('the port points back at the machine', port['machineId'], 'saw');
  check('and the port no longer carries the plug', port['sensor'] === undefined);

  check('an already-migrated shop passes through untouched', toShop(shop as never) === shop);
  check('null in, null out', toShop(null) === null);
}

// ── the plug lives on the machine ───────────────────────────────────────────
{
  const shop = toShop(v1())!;
  const port = systemsOf(shop)[0].elements.find(e => e['id'] === 'saw')!;
  const dc = systemsOf(shop)[0].elements.find(e => e['id'] === 'dc')!;

  eq('outletOf reads a port plug through its machine', outletOf(shop, port)!['ip'], '10.0.0.5');
  check('a collector with no switch reads null', outletOf(shop, dc) === null);

  // A collector belongs to exactly one system and has no machine to lift its
  // switch onto, so it keeps carrying its own. The asymmetry is the reason these
  // helpers exist rather than call sites reaching in.
  setOutlet(shop, dc, { gen: 2, ip: '10.0.0.9' });
  eq('a collector switch is written to the element', (dc['control'] as RawEl)['outlet'], { gen: 2, ip: '10.0.0.9' });
  check('and NOT to any machine', !machinesOf(shop).some(m => m.id === 'dc'));

  setOutlet(shop, port, { gen: 2, ip: '10.0.0.7', thresholdW: 20 });
  eq('a port plug is written to its machine', (machineById(shop, 'saw')!.sensor!.outlet as RawEl)['ip'], '10.0.0.7');
  check('and never onto the port', port['sensor'] === undefined);

  setOutlet(shop, port, null);
  check('detaching clears the machine sensor', machineById(shop, 'saw')!.sensor === undefined);

  setOutlet(shop, dc, null);
  check('detaching a collector switch leaves `control` behind',
    (dc['control'] as RawEl | undefined) !== undefined && (dc['control'] as RawEl)['outlet'] === undefined);
}

// ── add: a tool is a machine with one port ──────────────────────────────────
{
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  const before = machinesOf(shop).length;
  const port = addMachineWithPort(shop, sys, 'lathe', 'Lathe');
  sys.ducts.push({ child: 'lathe', parent: 'gate', parentBranch: 'b1' });

  eq('a machine was created alongside the port', machinesOf(shop).length, before + 1);
  eq('the port carries its machineId', port['machineId'], 'lathe');
  eq('the machine is findable from the port', machineOfPort(shop, port)!.id, 'lathe');
  eq('and it has exactly one port', portsOf(shop, 'lathe').length, 1);
  eq('which knows its system', portsOf(shop, 'lathe')[0].systemId, sys.id);
}

// ── delete: the machine goes, or a supplemental port goes ───────────────────
//
// A machine and its primary port are one thing (RFC §6.3): the primary is the
// machine's one required connection, so it is not deletable on its own and
// "delete this tool" removes the machine with every port it owns. A machine with
// no ports — or a port with no machine — is a document validateShop rejects, not
// a draft the canvas can leave behind.
{
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  addMachineWithPort(shop, sys, 'lathe', 'Lathe');
  sys.ducts.push({ child: 'lathe', parent: 'gate', parentBranch: 'b1' });

  removeMachine(shop, 'lathe');
  check('the port is gone', !sys.elements.some(e => e['id'] === 'lathe'));
  check('its duct went with it', !sys.ducts.some(d => d['child'] === 'lathe'));
  check('and so did its machine', !machineById(shop, 'lathe'));
  check('the shop is still valid afterwards', validateShop(shop).ok,
    JSON.stringify(validateShop(shop).errors));
}
{
  // A machine survives losing a SUPPLEMENTAL port. That is the bonus secondary port
  // coming off, and it is a non-event: the machine keeps its plug, its name and
  // its primary connection.
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  sys.elements.push({ id: 'saw-overarm', type: 'tool', name: 'Overarm', machineId: 'saw', supplemental: true });
  sys.ducts.push({ child: 'saw-overarm', parent: 'gate', parentBranch: 'b1' });
  eq('the saw now has two ports', portsOf(shop, 'saw').length, 2);

  check('a supplemental port can be removed', removePort(shop, 'saw-overarm'));
  check('removing it keeps the machine', !!machineById(shop, 'saw'));
  eq('and leaves the primary port', portsOf(shop, 'saw').length, 1);
}
{
  // The other half of the rule, and the one worth a backstop in the model: a
  // PRIMARY port cannot be deleted. removePort refuses and changes nothing —
  // the way to get rid of it is to get rid of the machine.
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  sys.elements.push({ id: 'saw-overarm', type: 'tool', name: 'Overarm', machineId: 'saw', supplemental: true });
  sys.ducts.push({ child: 'saw-overarm', parent: 'gate', parentBranch: 'b1' });

  eq('the primary is the un-flagged port', primaryPortOf(shop, 'saw')!['id'], 'saw');
  check('removePort refuses the primary', !removePort(shop, 'saw'));
  check('the port is untouched', sys.elements.some(e => e['id'] === 'saw'));
  check('the machine is untouched', !!machineById(shop, 'saw'));
  eq('and both ports are still there', portsOf(shop, 'saw').length, 2);

  removeMachine(shop, 'saw');
  check('deleting the machine is what removes them', !machineById(shop, 'saw'));
  check('the primary went', !sys.elements.some(e => e['id'] === 'saw'));
  check('the supplemental went too', !sys.elements.some(e => e['id'] === 'saw-overarm'));
  check('and its duct with it', !sys.ducts.some(d => d['child'] === 'saw-overarm'));
}

// ── naming ──────────────────────────────────────────────────────────────────
{
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  const cabinet = sys.elements.find(e => e['id'] === 'saw')!;
  // One port: the machine's name IS the answer. "Table saw", not "Table saw · port 1".
  eq('a single-port machine shows the machine name', displayName(shop, cabinet), 'Table saw');

  const overarm: RawEl = { id: 'saw-overarm', type: 'tool', name: 'Overarm · 2.5"', machineId: 'saw' };
  sys.elements.push(overarm);
  // Two ports: the port name distinguishes them, with the machine as the heading.
  eq('a multi-port machine shows the port name', displayName(shop, overarm), 'Overarm · 2.5"');
  eq('...for every port of it', displayName(shop, cabinet), 'Table saw');

  const unnamed: RawEl = { id: 'saw-third', type: 'tool', machineId: 'saw' };
  sys.elements.push(unnamed);
  eq('an unnamed port falls back to its machine', displayName(shop, unnamed), 'Table saw');
  eq('a non-port element uses its own name',
    displayName(shop, sys.elements.find(e => e['id'] === 'dc')!), 'Cyclone');
}

// ── enabled is opt-OUT ──────────────────────────────────────────────────────
// Absent means enabled, which is what keeps a migrated v1 document — where no
// port carries the field at all — routing exactly as it did before.
{
  check('a port with no `enabled` counts', isPortEnabled({ id: 'p' }));
  check('`enabled: true` counts', isPortEnabled({ id: 'p', enabled: true }));
  check('only an explicit false opts out', !isPortEnabled({ id: 'p', enabled: false }));
}

// ── system lookup and views ─────────────────────────────────────────────────
{
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  eq('systemById finds by id', systemById(shop, sys.id)!.id, sys.id);
  // Every caller is mid-edit and has nothing sensible to do with "no system",
  // and a shop always has at least one — so an unknown id falls back rather than
  // handing back null for the canvas to crash on.
  eq('an unknown id falls back to the first', systemById(shop, 'nope')!.id, sys.id);
  check('no systems at all is the one null case', systemById({ systems: [] } as unknown as ShopDoc, 'x') === null);

  const views = systemViews(shop);
  eq('one view per system', views.length, 1);
  // The view has to be a PLAIN topology, or the shop-blind analysers it exists to
  // feed (airflowIssues, redundantSelectors) find no elements and report all-clear.
  const view = views[0] as unknown as { elements?: unknown[]; ducts?: unknown[]; controllers?: unknown[] };
  check('a view exposes elements at its root', Array.isArray(view.elements) && view.elements.length > 0);
  check('and ducts', Array.isArray(view.ducts) && view.ducts.length > 0);
  check('and the shop-level controllers spliced in',
    Array.isArray(view.controllers) && view.controllers.length === 1);
}

// ── "is this outlet taken" is asked MACHINE to machine, never row to row ─────
//
// The tools screen lists PORTS, so a two-port saw is two rows — but the outlet
// belongs to the machine. Comparing rows made each of the saw's rows see the
// other as a different tool, so its OWN outlet read as taken, the picker refused
// to re-select it, and the save that followed deleted the pairing outright.
{
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  addSupplementalPort(shop, sys, 'saw', 'saw-oa', 'overarm');
  const primary = primaryPortOf(shop, 'saw')!['id'] as string;

  eq('both of a saw\'s ports report the same machine',
     machineIdOfPort(shop, primary), machineIdOfPort(shop, 'saw-oa'));
  eq('...and that machine is the saw', machineIdOfPort(shop, 'saw'), 'saw');
  // A port the document has never heard of compares as itself, so an unreadable
  // layout blocks nothing rather than blocking everything.
  eq('an unknown port falls back to its own id', machineIdOfPort(shop, 'ghost'), 'ghost');

  // Both rows carry the machine's outlet, which is exactly the shape that broke.
  const rows = [
    { id: primary,  ip: '10.0.0.5', hasPlug: true },
    { id: 'saw-oa', ip: '10.0.0.5', hasPlug: true },
  ];
  check('a tool\'s own outlet is NOT taken, seen from either of its ports',
    !outletTakenByAnotherMachine(shop, rows, '10.0.0.5', primary)
    && !outletTakenByAnotherMachine(shop, rows, '10.0.0.5', 'saw-oa'));

  // ...while a genuinely shared outlet still is. An outlet belongs to one tool.
  const withOther = rows.concat([{ id: 'jointer-port', ip: '10.0.0.5', hasPlug: true }]);
  check('but another MACHINE holding it still counts as taken',
    outletTakenByAnotherMachine(shop, withOther, '10.0.0.5', 'jointer-port'));
  check('a row with no outlet claims nothing',
    !outletTakenByAnotherMachine(shop, [{ id: 'x', ip: '10.0.0.9', hasPlug: false }], '10.0.0.9', primary));
  check('and an empty ip is never taken',
    !outletTakenByAnotherMachine(shop, rows, '', primary));
}

// ── renaming a machine reaches every copy of its name ───────────────────────
{
  const shop = toShop(v1())!;
  const sys = systemsOf(shop)[0];
  addSupplementalPort(shop, sys, 'saw', 'saw-oa', 'overarm');

  const port = () => portsOf(shop, 'saw').find(({ port: p }) => !p['supplemental'])!.port;
  const supp = () => portsOf(shop, 'saw').find(({ port: p }) => p['supplemental'])!.port;

  eq('the caption starts out quoting the machine', supp()['name'], 'Table saw · overarm');

  renameMachine(shop, 'saw', 'Cabinet saw');
  eq('the machine takes the new name', machineById(shop, 'saw')!.name, 'Cabinet saw');
  // The canvas draws the PORT's copy, so a rename that stopped at the machine left
  // the two screens showing different names for one saw (2026-08-22).
  eq('and so does its primary port', port()['name'], 'Cabinet saw');
  eq('the supplemental caption is re-derived, not left quoting the old name',
     supp()['name'], 'Cabinet saw · overarm');
  eq('...and it still names its role', supp()['role'], 'overarm');

  // Nothing else moves: a rename is a rename.
  eq('the port keeps its machine', port()['machineId'], 'saw');
  check('and its plug is untouched — a rename is a rename',
        (outletOf(shop, port()) as RawEl | null)?.['ip'] === '10.0.0.5');

  // A machine that has gone (deleted under a stale id) must not throw — the canvas
  // calls this from a text field, mid-edit.
  renameMachine(shop, 'nope', 'Nothing');
  eq('an unknown machine changes nothing', machineById(shop, 'saw')!.name, 'Cabinet saw');
}

// ── a shop saved before renameMachine gets its names back on read ───────────
{
  // Exactly what a canvas-built shop looked like: every port named, every machine
  // still on the creation default (jeff-s-shop-5.json, 2026-08-22).
  const damaged = {
    schemaVersion: 2, name: "Jeff's Shop",
    controllers: [{ id: 'primary', role: 'primary' }],
    systems: [{ id: 's1', name: 'Dust collection', elements: [
      { id: 'dc', type: 'collector', name: 'Cyclone' },
      { id: 't1', type: 'tool', name: 'Table Saw', machineId: 't1' },
      { id: 't2', type: 'tool', name: 'Bandsaw', machineId: 't2' },
      { id: 'p3', type: 'tool', name: 'New tool · Overarm', machineId: 't2',
        supplemental: true, role: 'Overarm' },
      { id: 't4', type: 'tool', name: 'New tool', machineId: 't4' },
      { id: 't5', type: 'tool', name: 'Old port name', machineId: 't5' },
    ], ducts: [] }],
    machines: [
      { id: 't1', name: 'New tool' }, { id: 't2', name: 'New tool' },
      { id: 't4', name: 'New tool' }, { id: 't5', name: 'Named in tool setup' },
    ],
  };
  const shop = toShop(damaged as unknown as Parameters<typeof toShop>[0])!;
  const m = (id: string) => machineById(shop, id)!.name;

  eq('a machine takes the name its port is carrying', m('t1'), 'Table Saw');
  eq('...for every machine, not just the first', m('t2'), 'Bandsaw');
  // The caption quoted the default, so healing the machine has to re-derive it.
  const supp = portsOf(shop, 't2').find(({ port }) => port['supplemental'])!.port;
  eq('and a supplemental caption stops quoting "New tool"', supp['name'], 'Bandsaw · Overarm');
  eq('a machine nobody has named at all is left alone', m('t4'), 'New tool');
  // The other direction is a real case — renamed in tool setup, never touched on
  // the canvas — and healing it too would make whichever screen ran last win.
  eq('a machine with a name of its own keeps it', m('t5'), 'Named in tool setup');
  eq('...and its port is not overwritten either',
     portsOf(shop, 't5')[0].port['name'], 'Old port name');
  // Idempotent: reading twice must not walk the names backwards.
  healMachineNames(shop);
  eq('healing again changes nothing', m('t1'), 'Table Saw');
}

report();
