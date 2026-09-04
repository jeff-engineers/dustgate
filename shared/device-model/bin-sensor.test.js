// bin-sensor.test.js — the reported SHAPE of a dust bin: present, or absent.
//
// This is the JS half of a pair, and the halves assert DIFFERENT things on
// purpose (docs/shop-schema-rfc.md §7.5):
//
//   here                 the omission rule — `systems[].bin` appears only when a
//                        sensor watches that collector, and an unwatched bin is
//                        silent rather than "not full"
//   test_binsensor.cpp   the debounce, which has no JS partner because no model
//                        simulates a flickering beam
//
// So this is the collector-plug.test.js arrangement rather than the
// nodelink.test.js one: same rule, two engines, not the same numbers twice.
// `kBinDebounceMs` lives once, in C++, and has nothing to drift against.
//
// Run: `node bin-sensor.test.js`, or `npm run bin:test`.

'use strict';

const TD = require('./topology-device');
const { clone, twoSystemShop } = require('./topology.fixtures');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, got === want, `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

const sys = (d, id) => TD.statusView(d, 1000).systems[id];

/** twoSystemShop with a bin sensor bolted onto one collector. */
function withBin(systemId, sensor) {
  const doc = clone(twoSystemShop);
  const s = doc.systems.find((x) => x.id === systemId);
  s.elements.find((e) => e.type === 'collector').bin = { sensor };
  return doc;
}

// ── absent means absent ─────────────────────────────────────────────────────
{
  // The fixture names no bin sensor anywhere. Nothing may claim to know.
  const d = TD.createTopologyDevice(clone(twoSystemShop));
  check('no sensor → bin omitted entirely', sys(d, 'big').bin === undefined);
  check('...on every system',               sys(d, 'small').bin === undefined);
}

// ── a watched bin reports, and starts empty ─────────────────────────────────
{
  const d = TD.createTopologyDevice(withBin('big', { kind: 'threshold', controllerId: 'primary' }));
  check('a watched bin reports', sys(d, 'big').bin !== undefined);
  eq('and starts not full',      sys(d, 'big').bin.full, false);
  // The OTHER system still has no sensor, and must stay silent rather than
  // inherit its neighbour's. Two collectors, one bin, is the ordinary shape.
  check('the unwatched system stays silent', sys(d, 'small').bin === undefined);
}

// ── staging it ──────────────────────────────────────────────────────────────
{
  const d = TD.createTopologyDevice(withBin('big', { kind: 'threshold', controllerId: 'primary' }));
  eq('staging full is accepted', TD.setBinFull(d, 'big', true).ok, true);
  eq('and it reads full',        sys(d, 'big').bin.full, true);
  eq('emptying it clears',       (TD.setBinFull(d, 'big', false), sys(d, 'big').bin.full), false);
  eq('an unknown system is refused', TD.setBinFull(d, 'nope', true).ok, false);
}

// ── staging does not conjure a sensor ───────────────────────────────────────
{
  // The staging setter writes device state, not topology. A shop with no sensor
  // that somehow gets staged must STILL report nothing — otherwise the mock can
  // show the UI a state no real device could ever produce, which is the whole
  // failure mode the canonical model exists to prevent.
  const d = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setBinFull(d, 'big', true);
  check('staged, but unwatched → still omitted', sys(d, 'big').bin === undefined);
}

// ── the sensor is independent of the plug ───────────────────────────────────
{
  // A collector may have a bin sensor and no plug, which is exactly the shop
  // that starts its blower by hand. The two omission rules must not be wired
  // to each other.
  const d = TD.createTopologyDevice(withBin('big', { kind: 'threshold' }));
  const s = sys(d, 'big');
  check('bin present without a plug', s.bin !== undefined && s.plug === undefined);
  // No controllerId at all is legal — single-board shops never name one, and it
  // means "this board" on the firmware side too (localBinSystemId, BinSensor.h).
  eq('and it reports', s.bin.full, false);
}

// ── report ──────────────────────────────────────────────────────────────────
let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
