// shop.test.js — pure unit tests for the SHOP contract (shop.js).
//
// Same style and harness as topology.test.js: no HTTP, no server, hand-built
// fixtures, `node shop.test.js` and exit 0 means all pass. This is the fast loop
// that stress-tests schemaVersion 2 BEFORE firmware and the canvas are migrated
// onto it — see the ordering note in TODO §5.

'use strict';

const {
  validateShop, routeShop, planShopTransition,
  migrateToShop, isShop, asShop, portsByMachine, portEnabled, systemView,
  SHOP_SCHEMA_VERSION,
} = require('./shop');
const { validateTopology } = require('./topology');
const { clone, star, twoGates, twoSystemShop } = require('./topology.fixtures');

// ── tiny harness ────────────────────────────────────────────────────────────
const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, JSON.stringify(got) === JSON.stringify(want), `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);
const hasCode = (res, code) => res.errors.some((e) => e.code === code);
/** clone the two-system shop and mutate for an invalid-case test */
const mut = (fn) => { const s = clone(twoSystemShop); fn(s); return s; };
const sys = (s, id) => s.systems.find((x) => x.id === id);
const el  = (s, sid, eid) => sys(s, sid).elements.find((e) => e.id === eid);

// ── validation: the good case ───────────────────────────────────────────────
{
  const r = validateShop(twoSystemShop);
  check('twoSystemShop validates', r.ok, JSON.stringify(r.errors));
}

// ── validation: shape ───────────────────────────────────────────────────────
check('null shop rejected', !validateShop(null).ok);
check('missing systems rejected', hasCode(validateShop({ schemaVersion: 2, controllers: [], machines: [] }), 'shape'));
check('devices must be an array',
  hasCode(validateShop(mut((s) => { s.devices = 'nope'; })), 'shape'));
check('zero systems rejected',
  hasCode(validateShop(mut((s) => { s.systems = []; })), 'system'));
check('duplicate system id rejected',
  hasCode(validateShop(mut((s) => { s.systems[1].id = 'big'; })), 'system'));

// Element ids are shop-unique, not system-unique — they're referenced from
// shop-level places where a collision would be silently ambiguous.
check('element id reused across systems rejected',
  hasCode(validateShop(mut((s) => { sys(s, 'small').elements[0].id = 'dc-big'; })), 'element'));

// ── validation: the per-system body is still topology.js, unchanged ─────────
// This is the §4.2 claim under test: lifting the container above the airflow
// invariants must leave them working exactly as written, per system.
check('two collectors in ONE system still rejected',
  hasCode(validateShop(mut((s) => {
    sys(s, 'big').elements.push({ id: 'dc-extra', type: 'collector', name: 'Nope' });
  })), 'element'));
check('a system with no collector rejected',
  hasCode(validateShop(mut((s) => {
    const b = sys(s, 'big');
    b.elements = b.elements.filter((e) => e.id !== 'dc-big');
    b.ducts = b.ducts.filter((d) => d.parent !== 'dc-big');
  })), 'element'));
{
  // And the shop-level message says WHICH system, or with N systems it's useless.
  const r = validateShop(mut((s) => {
    sys(s, 'small').elements.push({ id: 'dc-extra', type: 'collector', name: 'Nope' });
  }));
  check('system errors name their system',
    r.errors.some((e) => /system "small"/.test(e.message)), JSON.stringify(r.errors));
}

// ── validation: machines and ports ──────────────────────────────────────────
check('duplicate machine id rejected',
  hasCode(validateShop(mut((s) => { s.machines[1].id = 'table-saw'; })), 'machine'));
check('port with no machineId rejected',
  hasCode(validateShop(mut((s) => { delete el(s, 'big', 'ts-cabinet').machineId; })), 'port'));
check('port referencing unknown machine rejected',
  hasCode(validateShop(mut((s) => { el(s, 'big', 'ts-cabinet').machineId = 'ghost'; })), 'port'));
check('non-boolean enabled rejected',
  hasCode(validateShop(mut((s) => { el(s, 'big', 'ts-cabinet').enabled = 'no'; })), 'port'));
check('machine with no ports rejected',
  hasCode(validateShop(mut((s) => {
    s.machines.push({ id: 'orphan', name: 'Orphan' });
  })), 'machine'));
check('two machines sharing one plug rejected',
  hasCode(validateShop(mut((s) => {
    s.machines[1].sensor.outlet.ip = s.machines[0].sensor.outlet.ip;
  })), 'machine'));

// RFC §6.6: every port disabled is an ERROR, not a silent no-op — the machine
// would run with all its gates shut.
check('all ports disabled is an error',
  hasCode(validateShop(mut((s) => {
    el(s, 'big', 'ts-cabinet').enabled = false;
    el(s, 'small', 'ts-overarm').enabled = false;
  })), 'machine'));
check('SOME ports disabled is fine',
  validateShop(mut((s) => { el(s, 'small', 'ts-overarm').enabled = false; })).ok);

// ── port helpers ────────────────────────────────────────────────────────────
{
  const ports = portsByMachine(twoSystemShop);
  eq('table saw has two ports across two systems',
    (ports.get('table-saw') || []).map((p) => [p.systemId, p.port.id]),
    [['big', 'ts-cabinet'], ['small', 'ts-overarm']]);
  eq('single-port machine', (ports.get('jointer') || []).map((p) => p.port.id), ['jnt-port']);
  check('absent enabled means enabled', portEnabled({ id: 'x' }) === true);
  check('enabled false means disabled', portEnabled({ id: 'x', enabled: false }) === false);
}

// ── systemView: what the per-system functions actually receive ──────────────
{
  const view = systemView(twoSystemShop, sys(twoSystemShop, 'big'));
  check('systemView carries shop controllers down', view.controllers.length === 1);
  check('systemView is a valid topology on its own', validateTopology(view).ok,
    JSON.stringify(validateTopology(view).errors));
}

// ── routing: a machine opens every enabled port, in every system ────────────
{
  const r = routeShop(twoSystemShop, ['table-saw']);
  // Cabinet valve opens on the 4" side AND the manifold moves on the 2.5" side:
  // one machine, two systems, no shared air.
  eq('table saw opens its 4" valve', r.states['bv-cab'], 'open');
  eq('table saw moves the 2.5" manifold to its overarm', r.states['man'], 'm1');
  eq('idle jointer valve settles closed', r.states['bv-jnt'], 'closed');
  check('both saw ports reachable', r.reachable['ts-cabinet'] && r.reachable['ts-overarm']);
  eq('table saw fully routed', r.machines['table-saw'].status, 'routed');
}
{
  const r = routeShop(twoSystemShop, []);
  eq('nothing running → 4" valve closed', r.states['bv-cab'], 'closed');
  eq('nothing running → manifold home', r.states['man'], 'home');
  eq('no machines reported', Object.keys(r.machines), []);
}
{
  // Two machines in DIFFERENT systems run simultaneously with no interaction —
  // they share no duct, so neither can take a selector from the other.
  const r = routeShop(twoSystemShop, ['jointer', 'drill-press']);
  eq('jointer valve open', r.states['bv-jnt'], 'open');
  eq('manifold at the drill press', r.states['man'], 'm2');
  eq('no conflicts across systems', r.conflicts.length, 0);
  eq('jointer routed', r.machines['jointer'].status, 'routed');
  eq('drill press routed', r.machines['drill-press'].status, 'routed');
}

// ── routing: disabled ports are skipped ─────────────────────────────────────
{
  const s = mut((x) => { el(x, 'small', 'ts-overarm').enabled = false; });
  const r = routeShop(s, ['table-saw']);
  eq('disabled overarm does not move the manifold', r.states['man'], 'home');
  eq('cabinet still opens', r.states['bv-cab'], 'open');
  eq('machine still fully routed — the disabled port does not count against it',
    r.machines['table-saw'].status, 'routed');
}

// ── routing: partial vs stripped (RFC §10.3) ───────────────────────────────
{
  // Drill press wins the manifold (listed first = higher priority), so the saw's
  // SUPPLEMENTAL overarm loses. Its cabinet port still has air → partial, the
  // "degraded but fine" answer.
  const r = routeShop(twoSystemShop, ['drill-press', 'table-saw']);
  eq('manifold went to the drill press', r.states['man'], 'm2');
  eq('cabinet still open for the saw', r.states['bv-cab'], 'open');
  check('overarm lost', r.reachable['ts-overarm'] === false);
  eq('losing only a supplemental port is partial', r.machines['table-saw'].status, 'partial');
  eq('the winner is unaffected', r.machines['drill-press'].status, 'routed');
  check('conflict is attributed to a system',
    r.conflicts.length === 1 && r.conflicts[0].systemId === 'small',
    JSON.stringify(r.conflicts));
}
{
  // Same contention, but the overarm is NOT marked supplemental. Losing a
  // primary port is the alarm case: a saw running with a gate shut.
  const s = mut((x) => { delete el(x, 'small', 'ts-overarm').supplemental; });
  const r = routeShop(s, ['drill-press', 'table-saw']);
  eq('losing a primary port is stripped', r.machines['table-saw'].status, 'stripped');
}
{
  // A machine with every port disabled is invalid, but routing still has to
  // answer for it rather than omit it and look like it was never asked.
  const s = mut((x) => {
    el(x, 'big', 'ts-cabinet').enabled = false;
    el(x, 'small', 'ts-overarm').enabled = false;
  });
  const r = routeShop(s, ['table-saw']);
  eq('all-disabled machine reports stripped', r.machines['table-saw'].status, 'stripped');
  eq('and routes nowhere', r.machines['table-saw'].routed, []);
}

// ── transition planning ─────────────────────────────────────────────────────
{
  // Everything shut → run the table saw. Both systems move.
  const cur = { 'bv-cab': 'closed', 'bv-jnt': 'closed', man: 'home' };
  const want = routeShop(twoSystemShop, ['table-saw']).states;
  const plans = planShopTransition(twoSystemShop, cur, want);
  eq('one plan per system that moves', plans.map((p) => p.systemId), ['big', 'small']);
  // Plans are CONCATENATED, never interleaved — a sibling system's break must
  // not land between this system's make and break.
  check('each plan only touches its own selectors',
    plans.every((p) => p.moves.every((m) => {
      const owner = twoSystemShop.systems.find((s) => s.elements.some((e) => e.id === m.selectorId));
      return owner.id === p.systemId;
    })), JSON.stringify(plans));
  const big = plans.find((p) => p.systemId === 'big');
  eq('the 4" valve is a make', big.moves.map((m) => [m.selectorId, m.phase]), [['bv-cab', 'make']]);
}
{
  // Dead-head is PER SYSTEM: with two blowers there are genuinely two answers,
  // and collapsing them would hide which one is at risk.
  const cur  = { 'bv-cab': 'open', 'bv-jnt': 'closed', man: 'm1' };
  const want = { 'bv-cab': 'closed', 'bv-jnt': 'closed', man: 'm1' };
  const plans = planShopTransition(twoSystemShop, cur, want, {
    collectorRunning: { big: true, small: false },
  });
  const big = plans.find((p) => p.systemId === 'big');
  check('big system flags dead-head', big && big.deadHeadRisk === true, JSON.stringify(plans));
  const small = plans.find((p) => p.systemId === 'small');
  check('small system does not', !small || small.deadHeadRisk === false, JSON.stringify(plans));
}
{
  // A boolean collectorRunning applies to every system — the common case where
  // the caller has one answer for the whole shop.
  const cur  = { 'bv-cab': 'open', man: 'm1' };
  const want = { 'bv-cab': 'closed', man: 'home' };
  const plans = planShopTransition(twoSystemShop, cur, want, { collectorRunning: true });
  check('boolean collectorRunning reaches every system',
    plans.length === 2 && plans.every((p) => p.deadHeadRisk === true), JSON.stringify(plans));
}

// ── migration v1 → v2 ───────────────────────────────────────────────────────
{
  const shop = migrateToShop(star);
  check('migrated shop validates', validateShop(shop).ok, JSON.stringify(validateShop(shop).errors));
  eq('schemaVersion bumped', shop.schemaVersion, SHOP_SCHEMA_VERSION);
  eq('one system', shop.systems.length, 1);
  eq('controllers stay shop-level', shop.controllers.map((c) => c.id), ['primary']);
  eq('one machine per v1 tool', shop.machines.map((m) => m.id), ['toolA', 'toolB']);
  eq('machine keeps the tool id so existing references resolve',
    shop.machines[0].id, 'toolA');
  eq('machine takes the plug', shop.machines[0].sensor.outlet.ip, '192.168.87.27');
  const port = shop.systems[0].elements.find((e) => e.id === 'toolA');
  eq('port points at its machine', port.machineId, 'toolA');
  check('plug moved OFF the port', port.sensor === undefined);
  eq('port keeps its display name', port.name, 'Bandsaw');
}
{
  // Behaviour has to survive the migration, or the container was not free.
  const shop = migrateToShop(twoGates);
  const before = require('./routing').computeRouting(twoGates, ['toolX']);
  const after  = routeShop(shop, ['toolX']);
  eq('migrated routing matches v1', after.states, before.states);
  eq('migrated reachability matches v1', after.reachable, before.reachable);
}
{
  check('isShop distinguishes the shapes', isShop(twoSystemShop) && !isShop(star));
  check('asShop passes a shop through unchanged', asShop(twoSystemShop) === twoSystemShop);
  check('asShop migrates a v1 doc', isShop(asShop(star)));
  eq('migration is not destructive', star.elements.find((e) => e.id === 'toolA').sensor.outlet.ip,
    '192.168.87.27');
}

// ── report ──────────────────────────────────────────────────────────────────
let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed`);
process.exit(failed ? 1 : 0);
