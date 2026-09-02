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

// RFC §6.6: `enabled: false` is the hood-off-the-saw state, and that is a story
// about a bonus pickup. THE PRIMARY IS ALWAYS ENABLED — switching it off means
// "collect nothing from this tool", which is what deleting the machine is for.
check('disabling the primary port is an error',
  hasCode(validateShop(mut((s) => { el(s, 'big', 'ts-cabinet').enabled = false; })), 'port'));
check('disabling a SUPPLEMENTAL port is fine',
  validateShop(mut((s) => { el(s, 'small', 'ts-overarm').enabled = false; })).ok);
// Which makes "every port disabled" unreachable through the primary — it can
// only happen to a machine that has no primary either, and that is reported too.
check('all ports disabled is still an error',
  hasCode(validateShop(mut((s) => {
    el(s, 'big', 'ts-cabinet').enabled = false;
    el(s, 'small', 'ts-overarm').enabled = false;
  })), 'machine'));

// ── validation: one primary, 0–2 supplementals (RFC §6.3, §11.3) ────────────
//
// EXACTLY ONE PRIMARY is what makes the home-system rule structural rather than
// checked: the home system is the primary port's system, so a primary outside it
// is unrepresentable, and cross-system contention can only ever take a bonus
// pickup. Dropping `supplemental` from the overarm gives the saw two primaries,
// which is now the error in its own right.
check('a second primary port is rejected',
  hasCode(validateShop(mut((s) => { delete el(s, 'small', 'ts-overarm').supplemental; })), 'machine'));
check('...and the message names both offending ports',
  validateShop(mut((s) => { delete el(s, 'small', 'ts-overarm').supplemental; }))
    .errors.some((e) => e.message.includes('"ts-cabinet"') && e.message.includes('"ts-overarm"')));
// All-supplemental is a machine nothing is obliged to collect from, which makes
// every verdict about it meaningless — it can never be `stripped`.
check('a machine with no primary port is rejected',
  hasCode(validateShop(mut((s) => { el(s, 'big', 'ts-cabinet').supplemental = true; })), 'machine'));

// The cap is physical: a main port and a pickup or two is what fits on a
// machine. Two supplementals is the ceiling, three is a modelling mistake.
const auxPort = (s, id) => {
  const small = sys(s, 'small');
  small.elements.push({ id, type: 'tool', machineId: 'table-saw', name: id, supplemental: true });
  small.ducts.push({ child: id, parent: 'man', parentBranch: 'p1' });
};
check('a second supplemental port is allowed',
  validateShop(mut((s) => { auxPort(s, 'ts-aux'); })).ok);
check('a third supplemental port is rejected',
  hasCode(validateShop(mut((s) => { auxPort(s, 'ts-aux'); auxPort(s, 'ts-aux2'); })), 'machine'));

// A primary and a supplemental contending for one single-open selector is
// permanently degraded, not wrong — the user may well have meant it.
check('a supplemental sharing its machine\'s selector is allowed',
  validateShop(mut((s) => {
    const small = sys(s, 'small');
    small.elements.push({ id: 'drill-aux', type: 'tool', machineId: 'drill-press',
                          name: 'Aux', supplemental: true });
    small.ducts.push({ child: 'drill-aux', parent: 'man', parentBranch: 'p1' });
  })).ok);

// Systems share no duct. This is what makes that structural rather than
// conventional — without it a layout could plumb a 4" tool into the 2.5"
// manifold and every per-system invariant would still pass.
check('a duct spanning two systems is rejected',
  hasCode(validateShop(mut((s) => {
    sys(s, 'small').ducts.push({ child: 'ts-cabinet', parent: 'man', parentBranch: 'p1' });
  })), 'duct'));

// migrateToShop reuses a v1 tool's id for its machine ON PURPOSE, so the reuse
// itself must stay legal — only a collision with a DIFFERENT element is wrong.
check('a machine id colliding with an unrelated element is rejected',
  hasCode(validateShop(mut((s) => { s.machines[0].id = 'bv-cab';
    for (const x of sys(s, 'big').elements) if (x.machineId === 'table-saw') x.machineId = 'bv-cab';
    for (const x of sys(s, 'small').elements) if (x.machineId === 'table-saw') x.machineId = 'bv-cab';
  })), 'machine'));
check('a migrated shop, where machine ids ARE their port ids, stays valid',
  validateShop(migrateToShop(twoGates)).ok);

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
  // TWO now, not one: the shop's boards are shop-wide, and a slider needs its own
  // board alongside the PWM brain (see the two-boards note on twoSystemShop).
  // systemView hands the WHOLE controller list down rather than filtering to the
  // boards this system happens to use — which is what lets validateTopology below
  // resolve every selector's controllerId no matter which system it sits in.
  check('systemView carries shop controllers down', view.controllers.length === 2);
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

// Swap which of the saw's two ports is the primary: the overarm becomes the one
// it can't do without, the cabinet port the bonus. Keeps the "exactly one
// primary" invariant true, so the mutated shop still validates.
const swapSawRoles = (x) => {
  delete el(x, 'small', 'ts-overarm').supplemental;
  el(x, 'big', 'ts-cabinet').supplemental = true;
};
check('swapping which port is primary leaves a valid shop', validateShop(mut(swapSawRoles)).ok);

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
  // Same contention, but with the saw's roles swapped: the overarm is the
  // primary and the cabinet port the bonus. A machine still gets exactly one
  // primary (§6.3), so this is the only way to make the CONTENDED port the one
  // that matters. Losing it is the alarm case: a saw running with its gate shut.
  const s = mut(swapSawRoles);
  const r = routeShop(s, ['drill-press', 'table-saw']);
  eq('losing a primary port is stripped', r.machines['table-saw'].status, 'stripped');
}
// ── arbitration: primary beats supplemental (RFC §11.3) ────────────────────
{
  // The saw is listed FIRST, so plain most-recent-wins would hand the manifold
  // to its overarm — and leave the drill press, whose only port that manifold
  // feeds, running into a closed gate. Rule 1 exists to stop exactly that:
  // never trade someone's only collection for someone else's bonus.
  const r = routeShop(twoSystemShop, ['table-saw', 'drill-press']);
  eq('the primary port holds the manifold despite starting earlier', r.states['man'], 'm2');
  check('the drill press keeps its air', r.reachable['drill-port'] === true);
  check('the newer machine yields its bonus pickup', r.reachable['ts-overarm'] === false);
  eq('the yielding machine is only partial', r.machines['table-saw'].status, 'partial');
  eq('nobody is stripped', r.machines['drill-press'].status, 'routed');
  eq('and the conflict names the primary as winner', r.conflicts[0].winner, 'drill-port');
}
{
  // Rule 2 is untouched: among PRIMARIES, recency still decides. Making the
  // overarm the saw's primary puts two primaries on the manifold — one per
  // machine — and then the saw's later start does win, which is also what makes
  // the drill press `stripped` rather than merely degraded.
  const s = mut(swapSawRoles);
  const r = routeShop(s, ['table-saw', 'drill-press']);
  eq('among primaries, most-recent still wins', r.states['man'], 'm1');
  eq('so the older machine is stripped', r.machines['drill-press'].status, 'stripped');
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
