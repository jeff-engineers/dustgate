// topology.test.js — pure unit tests for the v2 topology contract + routing.
//
// No HTTP, no server — just the pure model (topology.js + routing.js) over
// hand-built fixtures. Run: `node topology.test.js` (exit 0 = all pass).
// This is the fast feedback loop that stress-tests the schema BEFORE the
// device-model migrate bakes it into mock/demo/firmware.

'use strict';

const { validateTopology, servoCommandAngle, airflowIssues } = require('./topology');
const { computeRouting } = require('./routing');
const { planTransition } = require('./sequencer');
const { createTopologyDevice, setToolPower, statusView } = require('./topology-device');
const { clone, star, feedChain, twoGates } = require('./topology.fixtures');

// ── tiny harness (same style as conformance.js) ─────────────────────────────
const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, JSON.stringify(got) === JSON.stringify(want), `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);
const hasCode = (res, code) => res.errors.some((e) => e.code === code);
/** clone feedChain and mutate for an invalid-case test */
const mut = (fn) => { const t = clone(feedChain); fn(t); return t; };
const elem = (t, id) => t.elements.find((e) => e.id === id);

// ── validation: valid fixtures pass ─────────────────────────────────────────
check('validate star ok', validateTopology(star).ok, JSON.stringify(validateTopology(star).errors));
check('validate feedChain ok', validateTopology(feedChain).ok, JSON.stringify(validateTopology(feedChain).errors));
check('validate twoGates ok', validateTopology(twoGates).ok, JSON.stringify(validateTopology(twoGates).errors));

// ── validation: each rule catches its violation ─────────────────────────────
{
  const t = mut((t) => t.elements.push({ id: 'dc2', type: 'collector', name: 'Second' }));
  const r = validateTopology(t);
  check('two collectors → invalid (element)', !r.ok && hasCode(r, 'element'));
}
{
  const t = mut((t) => { t.ducts.find((d) => d.child === 'lin').parent = 'ghost'; });
  const r = validateTopology(t);
  check('dangling duct parent → invalid (duct)', !r.ok && hasCode(r, 'duct'));
}
{
  const t = mut((t) => { elem(t, 'man').states.find((s) => s.id === 'left').isClosed = true; });
  const r = validateTopology(t);
  check('two isClosed states → invalid (selector)', !r.ok && hasCode(r, 'selector'));
}
{
  const t = mut((t) => { elem(t, 'man').branches.find((b) => b.id === 'mL').opensState = 'ghost'; });
  const r = validateTopology(t);
  check('branch opensState missing → invalid (selector)', !r.ok && hasCode(r, 'selector'));
}
{
  const t = mut((t) => { elem(t, 'lin').branches.find((b) => b.id === 'b2').role = 'tool'; }); // b2 child is a selector
  const r = validateTopology(t);
  check('feed-shaped branch labeled tool → invalid (role)', !r.ok && hasCode(r, 'role'));
}
{
  const t = mut((t) => { elem(t, 'lin').branches.find((b) => b.id === 'b1').role = 'blocked'; }); // b1 has a tool child
  const r = validateTopology(t);
  check('blocked branch with a child → invalid (role)', !r.ok && hasCode(r, 'role'));
}
{
  const t = mut((t) => { t.controllers[0].role = 'secondary'; });
  const r = validateTopology(t);
  check('no primary controller → invalid (controller)', !r.ok && hasCode(r, 'controller'));
}
{
  const t = mut((t) => { t.controllers[0].link = { transport: 'wifi-ws', host: 'shed.local', ip: '192.168.87.9' }; });
  const r = validateTopology(t);
  check('controller with valid link → valid', r.ok);
}
{
  const t = mut((t) => { t.controllers[0].link = { transport: 'carrier-pigeon' }; });
  const r = validateTopology(t);
  check('controller link bad transport → invalid (controller)', !r.ok && hasCode(r, 'controller'));
}
// ── per-host actuator budget: ≤4 servos + ≤1 stepper on one controller ──
{
  const hasMsg = (r, sub) => r.errors.some((e) => e.message.includes(sub));
  // A shop-in-a-box: one host, `servos` servo gates + `linears` linear selectors,
  // each feeding one tool. Valid apart from whatever count we're stressing.
  const buildHost = (servos, linears) => {
    const t = { schemaVersion: 1, name: 'budget', controllers: [{ id: 'h', role: 'primary' }],
      elements: [{ id: 'dc', type: 'collector' }], ducts: [] };
    let n = 0;
    for (let i = 0; i < servos; i++, n++) {
      const g = `g${n}`, tool = `t${n}`;
      t.elements.push({ id: g, type: 'selector', controllerId: 'h', kind: 'servoGate',
        states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
        branches: [{ id: 'b', opensState: 'open', role: 'tool' }] });
      t.elements.push({ id: tool, type: 'tool' });
      t.ducts.push({ child: g, parent: 'dc' }, { child: tool, parent: g, parentBranch: 'b' });
    }
    for (let i = 0; i < linears; i++, n++) {
      const g = `g${n}`, tool = `t${n}`;
      t.elements.push({ id: g, type: 'selector', controllerId: 'h', kind: 'linear',
        states: [{ id: 'home', isClosed: true, positionMm: 0 }, { id: 's1', isClosed: false, positionMm: 10 }],
        branches: [{ id: 'b', opensState: 's1', role: 'tool' }] });
      t.elements.push({ id: tool, type: 'tool' });
      t.ducts.push({ child: g, parent: 'dc' }, { child: tool, parent: g, parentBranch: 'b' });
    }
    return t;
  };
  check('4 servos on one host → valid (at budget)', validateTopology(buildHost(4, 0)).ok,
        JSON.stringify(validateTopology(buildHost(4, 0)).errors));
  const r5 = validateTopology(buildHost(5, 0));
  check('5 servos on one host → invalid (controller)', !r5.ok && hasCode(r5, 'controller') && hasMsg(r5, 'max 4'));
  check('1 linear on one host → valid (at budget)', validateTopology(buildHost(0, 1)).ok);
  const r2 = validateTopology(buildHost(0, 2));
  check('2 linears on one host → invalid (controller)', !r2.ok && hasCode(r2, 'controller') && hasMsg(r2, 'max 1'));
  // Mixed at budget: 4 servos + 1 linear on one host is fine.
  check('4 servos + 1 linear on one host → valid', validateTopology(buildHost(4, 1)).ok);
}
{
  const t = mut((t) => { t.ducts = t.ducts.filter((d) => d.child !== 'toolA'); }); // orphan toolA
  const r = validateTopology(t);
  check('orphaned element (no parent) → invalid (tree)', !r.ok && hasCode(r, 'tree'));
}
{
  const t = mut((t) => { elem(t, 'lin').branches = elem(t, 'lin').branches.filter((b) => b.id !== 'b3'); }); // state s3 now unopened
  const r = validateTopology(t);
  check('branch/state count mismatch → invalid (selector)', !r.ok && hasCode(r, 'selector'));
}

// ── routing: star (single linear actuator = mutual exclusion) ───────────────
{
  const r = computeRouting(star, []);
  eq('star idle: selector closed', r.states.sel, 'home');
  eq('star idle: no conflicts', r.conflicts, []);
}
{
  const r = computeRouting(star, ['toolA']);
  eq('star toolA: selector → s1', r.states.sel, 's1');
  check('star toolA: reachable', r.reachable.toolA === true);
}
// Priority-ordered: index 0 wins a contested selector (caller sorts by recency).
{
  const r = computeRouting(star, ['toolA', 'toolB']);   // toolA is higher priority
  check('star both: conflict on sel (A wins, B loses)',
    r.conflicts.length === 1 && r.conflicts[0].selectorId === 'sel'
    && r.conflicts[0].winner === 'toolA' && r.conflicts[0].losers.includes('toolB'));
  eq('star both: selector routes to the winner (s1)', r.states.sel, 's1');
  check('star both: winner reachable, loser not', r.reachable.toolA === true && r.reachable.toolB === false);
}
{
  const r = computeRouting(star, ['toolB', 'toolA']);   // reversed priority → B wins
  eq('star both reversed: selector routes to B (s2)', r.states.sel, 's2');
  check('star both reversed: B reachable, A not', r.reachable.toolB === true && r.reachable.toolA === false);
}

// ── routing: feedChain (multi-hop feed) ─────────────────────────────────────
{
  const r = computeRouting(feedChain, ['toolL']);
  eq('feed toolL: manifold → left', r.states.man, 'left');
  eq('feed toolL: linear → s2 (feed branch)', r.states.lin, 's2');
  check('feed toolL: reachable', r.reachable.toolL === true);
}
{
  const r = computeRouting(feedChain, ['toolL', 'toolR']);   // L wins the manifold
  check('feed L+R: conflict on manifold (L wins)',
    r.conflicts.some((c) => c.selectorId === 'man' && c.winner === 'toolL' && c.losers.includes('toolR')));
  eq('feed L+R: manifold routes to winner (left)', r.states.man, 'left');
  eq('feed L+R: linear still on the feed branch', r.states.lin, 's2');
  check('feed L+R: L reachable, R not', r.reachable.toolL === true && r.reachable.toolR === false);
}
{
  const r = computeRouting(feedChain, ['toolA', 'toolL']);   // A wins the linear
  check('feed A+L: conflict on linear (A wins, s1 vs s2)',
    r.conflicts.some((c) => c.selectorId === 'lin' && c.winner === 'toolA' && c.losers.includes('toolL')));
  eq('feed A+L: linear routes to winner (s1)', r.states.lin, 's1');
  eq('feed A+L: manifold stays closed (loser not routed)', r.states.man, 'closed');
  check('feed A+L: A reachable, L not', r.reachable.toolA === true && r.reachable.toolL === false);
}

// ── routing: twoGates (independent selectors run concurrently) ──────────────
{
  const r = computeRouting(twoGates, ['toolX', 'toolY']);
  eq('twoGates X+Y: gate1 open', r.states.gate1, 'open');
  eq('twoGates X+Y: gate2 open', r.states.gate2, 'open');
  eq('twoGates X+Y: no conflicts', r.conflicts, []);
  check('twoGates X+Y: both reachable', r.reachable.toolX === true && r.reachable.toolY === true);
}
{
  const r = computeRouting(twoGates, ['toolX']);
  eq('twoGates X: gate1 open', r.states.gate1, 'open');
  eq('twoGates X: gate2 closed', r.states.gate2, 'closed');
}

// ── sequencer: planTransition (make-before-break) ───────────────────────────
const phaseOf = (plan, sel) => (plan.moves.find((m) => m.selectorId === sel) || {}).phase;
const idxOf = (plan, sel) => plan.moves.findIndex((m) => m.selectorId === sel);
{
  // No change → no moves.
  const p = planTransition(twoGates, { gate1: 'open', gate2: 'closed' }, { gate1: 'open', gate2: 'closed' });
  eq('seq no-op: no moves', p.moves, []);
}
{
  // Both tools coming on: two opens, no breaks.
  const p = planTransition(twoGates, { gate1: 'closed', gate2: 'closed' }, { gate1: 'open', gate2: 'open' });
  check('seq open-both: two makes, no breaks', p.moves.length === 2 && p.moves.every((m) => m.phase === 'make'));
  check('seq open-both: no dead-head', p.deadHeadRisk === false);
}
{
  // Switchover X→Y: open gate2 (make) BEFORE closing gate1 (break).
  const p = planTransition(twoGates, { gate1: 'open', gate2: 'closed' }, { gate1: 'closed', gate2: 'open' },
    { collectorRunning: true });
  eq('seq switchover: gate2 is a make', phaseOf(p, 'gate2'), 'make');
  eq('seq switchover: gate1 is a break', phaseOf(p, 'gate1'), 'break');
  check('seq switchover: make ordered before break', idxOf(p, 'gate2') < idxOf(p, 'gate1'));
  check('seq switchover: no dead-head (a path stays open)', p.deadHeadRisk === false);
}
{
  // Everything closing while the blower runs → dead-head flag + all breaks.
  const p = planTransition(twoGates, { gate1: 'open', gate2: 'open' }, { gate1: 'closed', gate2: 'closed' },
    { collectorRunning: true });
  check('seq all-closed running: dead-head risk flagged', p.deadHeadRisk === true);
  check('seq all-closed running: all breaks', p.moves.length === 2 && p.moves.every((m) => m.phase === 'break'));
}
{
  // Linear moving between gates maintains flow → a "make", never a break.
  const p = planTransition(feedChain, { lin: 's1', man: 'closed' }, { lin: 's2', man: 'closed' });
  eq('seq linear move: phase is make (maintains flow)', phaseOf(p, 'lin'), 'make');
  check('seq linear move: only the linear moves', p.moves.length === 1);
}

// ── device sim: tool power → routing → actuators + collector ────────────────
{
  const d = createTopologyDevice(clone(twoGates));
  let s = statusView(d);
  check('dev init: both gates closed, collector off',
    s.actuators.gate1 === 'closed' && s.actuators.gate2 === 'closed' && s.collectorOn === false);

  setToolPower(d, 'toolX', 10);   // > threshold 5
  s = statusView(d);
  check('dev X on: gate1 open, gate2 closed, collector on',
    s.actuators.gate1 === 'open' && s.actuators.gate2 === 'closed' && s.collectorOn === true);

  setToolPower(d, 'toolY', 10);   // > threshold 9 — both run (independent gates)
  s = statusView(d);
  check('dev X+Y on: both gates open', s.actuators.gate1 === 'open' && s.actuators.gate2 === 'open');

  setToolPower(d, 'toolX', 0);    // X off, Y still on → focus suction on Y
  s = statusView(d);
  check('dev X off (Y on): gate1 closes, gate2 open, collector on',
    s.actuators.gate1 === 'closed' && s.actuators.gate2 === 'open' && s.collectorOn === true);

  setToolPower(d, 'toolY', 0);    // idle → HOLD positions, collector off
  s = statusView(d);
  check('dev idle: gates HELD (gate2 still open), collector off',
    s.actuators.gate1 === 'closed' && s.actuators.gate2 === 'open' && s.collectorOn === false);
}
{
  // Single linear actuator: most-recently-powered-on tool wins the shared selector.
  const d = createTopologyDevice(clone(star));
  setToolPower(d, 'toolA', 10);
  check('dev star A on: selector → s1', statusView(d).actuators.sel === 's1');

  setToolPower(d, 'toolB', 10);   // B activates second → newest → wins
  let s = statusView(d);
  check('dev star B on (newer): selector → s2 (B wins)', s.actuators.sel === 's2');
  check('dev star B on: B reachable, A not', s.reachable.toolB === true && s.reachable.toolA === false);

  setToolPower(d, 'toolB', 0);    // B off → only A active → selector back to A
  check('dev star B off: selector → s1 (A alone)', statusView(d).actuators.sel === 's1');
}

// ── servo resolver: state → angle (referenceAngle + offsetDeg, clamped) ─────
{
  const gate = twoGates.elements.find((e) => e.id === 'gate1');   // ref 10, open+0 / closed+90
  eq('servo gate open → 10', servoCommandAngle(gate, 'open'), 10);
  eq('servo gate closed → 100', servoCommandAngle(gate, 'closed'), 100);

  const man = feedChain.elements.find((e) => e.id === 'man');      // ref 5, left+0/closed+80/right+161
  eq('servo manifold left → 5', servoCommandAngle(man, 'left'), 5);
  eq('servo manifold closed → 85', servoCommandAngle(man, 'closed'), 85);
  eq('servo manifold right → 166', servoCommandAngle(man, 'right'), 166);

  const lin = star.elements.find((e) => e.id === 'sel');           // not a servo
  eq('servo resolver: null for linear selector', servoCommandAngle(lin, 's1'), null);

  // Clamp: referenceAngle + offset beyond maxAngle is clamped.
  const clamped = { kind: 'servoGate', states: [{ id: 'x', offsetDeg: 200 }], servo: { referenceAngle: 0, maxAngle: 180 } };
  eq('servo resolver: clamps to maxAngle', servoCommandAngle(clamped, 'x'), 180);
}

// ── airflow integrity: always-open (ungated) tools are leaks ────────────────
{
  // Fully-gated fixtures are clean.
  eq('airflow: star clean', airflowIssues(star), []);
  eq('airflow: feedChain clean (feed chain still gated)', airflowIssues(feedChain), []);
  eq('airflow: twoGates clean', airflowIssues(twoGates), []);

  // A tool wired straight to the collector = permanent leak.
  const leaky = mut((t) => { t.elements.push({ id: 'leak', type: 'tool', name: 'Leaky' }); t.ducts.push({ child: 'leak', parent: 'dc' }); });
  const li = airflowIssues(leaky);
  check('airflow: tool on collector flagged', li.length === 1 && li[0].id === 'leak' && li[0].kind === 'always-open');

  // A tool on a passive wye (junction) with no gate above = leak too.
  const wye = mut((t) => {
    t.elements.push({ id: 'wye', type: 'junction', name: 'Wye' });
    t.elements.push({ id: 'wt', type: 'tool', name: 'Wye Tool' });
    t.ducts.push({ child: 'wye', parent: 'dc' }, { child: 'wt', parent: 'wye' });
  });
  const wi = airflowIssues(wye);
  check('airflow: tool on ungated wye flagged', wi.length === 1 && wi[0].id === 'wt');

  // Same wye but fed THROUGH a gate → not a leak (gate above the wye covers it).
  const gatedWye = mut((t) => {
    t.elements.push({ id: 'wye', type: 'junction', name: 'Wye' });
    t.elements.push({ id: 'wt', type: 'tool', name: 'Wye Tool' });
    // hang the wye off an existing linear branch (feed), so a selector sits above it
    const b = elem(t, 'lin').branches.find((x) => x.role === 'blocked');
    b.role = 'feed';
    t.ducts.push({ child: 'wye', parent: 'lin', parentBranch: b.id }, { child: 'wt', parent: 'wye' });
  });
  eq('airflow: wye behind a gate is clean', airflowIssues(gatedWye), []);
}

// ── report ──────────────────────────────────────────────────────────────────
let passed = 0;
for (const r of results) {
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
  if (r.ok) passed++;
}
const failed = results.length - passed;
console.log(`\n${passed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}\n`);
process.exit(failed ? 1 : 0);
