// adopt-outlets.test.js — the simulated network contains the plugs a saved shop
// is paired to (device-model.js: adoptOutlets).
//
// SIM ONLY — no C++ pair, and there shouldn't be one: on real hardware the plugs
// are found by mDNS because they physically exist. This exists because both
// runners (tools/mock-api.js and the browser demo) fake that sweep with random
// IPs, and a paired plug that the fake sweep has never heard of made every
// paired plug read as "not responding" — no wattage, no name, and rename,
// release and takeover all refused. That is a state the runners must be able to
// leave, or the plug screens can't be walked at all.
//
// Run: `node adopt-outlets.test.js`, or `npm run adopt:test`.

'use strict';

const M = require('./device-model');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, JSON.stringify(got) === JSON.stringify(want),
        `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

const find = (d, ip) => M.ensureDiscovered(d).find(x => x.ip === ip);

// ── a shop document ─────────────────────────────────────────────────────────
{
  const d = M.createDevice();
  const before = M.ensureDiscovered(d).length;
  M.adoptOutlets(d, {
    schemaVersion: 2,
    machines: [
      { id: 'saw',  sensor: { outlet: { gen: 2, ip: '10.0.0.30', host: 'plug-saw', name: 'Table Saw' } } },
      { id: 'band', sensor: { outlet: { gen: 2, ip: '10.0.0.31' } } },
      { id: 'hand' },   // no plug at all — the manual tool case
    ],
  });
  check('a paired plug is on the network', !!find(d, '10.0.0.30'));
  check('and so is one with no name', !!find(d, '10.0.0.31'));
  eq('two added, and nothing invented for the manual tool',
     M.ensureDiscovered(d).length, before + 2);

  const saw = find(d, '10.0.0.30');
  check('it answers', saw.reachable === true);
  eq('drawing nothing, like every plug during a scan', saw.powerW, 0);
  eq('claimed OURS — pairing a plug is what claiming it means', saw.claim, 'ours');
  check('not takeable from ourselves', saw.takeable === false);
  eq('the saved hostname is kept', saw.hostname, 'plug-saw');

  // The plug holds the full name; the scan strips the suffix on the way out.
  check('the plug itself carries the owner suffix', saw.name.endsWith(' · dustgate-demo'), saw.name);
  eq('and discover reports the bare label',
     M.discoverOutlets(d).find(x => x.ip === '10.0.0.30').name, 'Table Saw');
  eq('a plug saved without a name has none', find(d, '10.0.0.31').name, '');
}

// ── a v1 topology, where the plug hangs off the element ─────────────────────
{
  const d = M.createDevice();
  M.adoptOutlets(d, {
    schemaVersion: 1,
    elements: [
      { id: 'dc',  type: 'collector', control: { outlet: { gen: 2, ip: '10.0.0.50' } } },
      { id: 'saw', type: 'tool',      sensor:  { outlet: { gen: 2, ip: '10.0.0.51' } } },
    ],
  });
  check("the collector's switch counts too", !!find(d, '10.0.0.50'));
  check('as does a tool sensor', !!find(d, '10.0.0.51'));
}

// ── what it must NOT touch ──────────────────────────────────────────────────
{
  const d = M.createDevice();
  const existing = M.ensureDiscovered(d);
  // The last entry is deliberately foreign, so the refusal paths stay reachable
  // on the bench. Adopting a document that names it must not quietly hand it to us.
  const foreign = existing[existing.length - 1];
  const beforeLen = existing.length;
  M.adoptOutlets(d, { machines: [{ id: 'x', sensor: { outlet: { ip: foreign.ip } } }] });
  eq('a plug already on the network is not duplicated', M.ensureDiscovered(d).length, beforeLen);
  eq("and its ownership is left exactly as it was", find(d, foreign.ip).claim, foreign.claim);
}

// ── a rename survives re-adopting the layout ────────────────────────────────
//
// A PLUG'S NAME BELONGS TO THE PLUG, not to whatever the layout calls the
// machine it senses. The document's `name` is a DEFAULT for a plug nobody has
// named yet, and adopting is not a rename.
//
// The firmware had this wrong until 2026-08-24: syncTopologyOutlets() rebuilt
// every outlet from the layout on EVERY save, passing the machine's name, which
// discarded the rename and then wrote the machine name back onto the physical
// plug on the next provisioning pass. Intermittent, because provisioning skips a
// plug that is already push-connected — so whether the revert reached the device
// depended on how fast its WebSocket came back. This model always skipped a plug
// it already knew; the assertions were simply never written down.
{
  const d = M.createDevice();
  const doc = {
    machines: [{ id: 'saw', name: 'Table Saw',
                 sensor: { outlet: { gen: 2, ip: '10.0.0.30', host: 'plug-saw', name: 'Table Saw' } } }],
  };
  M.adoptOutlets(d, doc);
  eq('adopted under the machine name',
     M.discoverOutlets(d).find(x => x.ip === '10.0.0.30').name, 'Table Saw');

  M.nameOutlet(d, '10.0.0.30', 'Bench plug 2', false);
  eq('the rename lands', M.discoverOutlets(d).find(x => x.ip === '10.0.0.30').name, 'Bench plug 2');
  check('and the plug carries the owner suffix',
        find(d, '10.0.0.30').name.endsWith(' · dustgate-demo'), find(d, '10.0.0.30').name);

  // Saving the layout again — the exact act that used to revert it. Twice, since
  // the reported symptom was intermittent and a single pass could get lucky.
  M.adoptOutlets(d, doc);
  M.adoptOutlets(d, doc);
  eq('and re-adopting the SAME layout does not revert it',
     M.discoverOutlets(d).find(x => x.ip === '10.0.0.30').name, 'Bench plug 2');

  // Renaming the MACHINE is not renaming the plug either — they are separate
  // things that happen to have shared a default once.
  doc.machines[0].name = 'Cabinet Saw';
  doc.machines[0].sensor.outlet.name = 'Cabinet Saw';
  M.adoptOutlets(d, doc);
  eq('nor does renaming the machine it senses',
     M.discoverOutlets(d).find(x => x.ip === '10.0.0.30').name, 'Bench plug 2');
}

{
  const d = M.createDevice();
  const before = M.ensureDiscovered(d).length;
  M.adoptOutlets(d, null);
  M.adoptOutlets(d, {});
  M.adoptOutlets(d, { machines: [{ sensor: { outlet: {} } }] });   // outlet with no ip
  eq('an empty or plugless document adds nothing', M.ensureDiscovered(d).length, before);
}

// ── report ──────────────────────────────────────────────────────────────────
let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
