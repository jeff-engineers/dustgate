// collector-plug.test.js — telling "we closed the relay" apart from "air is moving".
//
// A tool's plug SENSES; a collector's plug COMMANDS. Everything the shop page
// said about a blower used to come from the command, so a tripped breaker, a
// blower switched off at its own panel, or an unplugged cord all rendered as a
// confident green "Collecting". This file pins the predicate that reads the
// plug's own power back and says so instead.
//
// NO C++ PAIR, deliberately: the firmware reports the plug facts and does not
// judge them (see the note on COLLECTOR_RUNNING_W in topology-device.js). If the
// OLED ever needs the verdict too, this file gains a partner and CLAUDE.md gains
// a row.
//
// Run: `node collector-plug.test.js`, or `npm run plug:test`.

'use strict';

const TD = require('./topology-device');
const { clone, twoSystemShop } = require('./topology.fixtures');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, got === want, `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

const GRACE = TD.COLLECTOR_SPINUP_GRACE_MS;
const RUN_W = TD.COLLECTOR_RUNNING_W;
const st = TD.collectorPlugState;

// ── the predicate, in isolation ─────────────────────────────────────────────
{
  eq('no plug and not asking → noplug', st(undefined, false), 'noplug');
  // Asking, with nothing reporting back: exactly what we knew before the plug
  // was polled at all. Honest, and not an alarm.
  eq('no plug but asking → unknown', st(undefined, true), 'unknown');
  eq('plug not answering → unknown', st({ watts: 0, reachable: false, onForMs: 99999 }, true), 'unknown');
  eq('not asking → off', st({ watts: 0, reachable: true, onForMs: 0 }, false), 'off');

  eq('drawing running current → running',
     st({ watts: RUN_W, reachable: true, onForMs: 10 }, true), 'running');
  // The threshold is a floor, not a window: a big blower drawing far more is
  // still simply running.
  eq('drawing far more is still running',
     st({ watts: RUN_W * 20, reachable: true, onForMs: GRACE * 10 }, true), 'running');

  // ── the grace period, which is the whole reason this isn't a one-liner ────
  eq('just switched on, nothing yet → starting',
     st({ watts: 0, reachable: true, onForMs: 0 }, true), 'starting');
  eq('one tick before the grace expires → still starting',
     st({ watts: 0, reachable: true, onForMs: GRACE - 1 }, true), 'starting');
  eq('the moment the grace expires → notStarting',
     st({ watts: 0, reachable: true, onForMs: GRACE }, true), 'notStarting');
  eq('long past it, still nothing → notStarting',
     st({ watts: 0, reachable: true, onForMs: GRACE * 5 }, true), 'notStarting');
  // Below the running threshold but not zero: a relay's own standby draw, or a
  // motor that is humming and not turning. Still not running.
  eq('a trickle below threshold is not running',
     st({ watts: RUN_W - 1, reachable: true, onForMs: GRACE }, true), 'notStarting');
  // A device that predates the report sends no onForMs. Guessing "long enough"
  // would alarm on every start, so the unknown resolves toward silence.
  eq('no onForMs → starting, never an alarm',
     st({ watts: 0, reachable: true }, true), 'starting');
}

// ── through the device, where onForMs comes from ────────────────────────────
const plugged = () => {
  const shop = clone(twoSystemShop);
  for (const [sid, ip] of [['big', '10.0.0.50'], ['small', '10.0.0.51']]) {
    const c = shop.systems.find((s) => s.id === sid).elements.find((e) => e.type === 'collector');
    c.control = { outlet: { gen: 2, ip } };
  }
  return TD.createTopologyDevice(shop);
};
const sys = (d, id, t) => TD.statusView(d, t).systems[id];
const state = (d, id, t) => {
  const s = sys(d, id, t);
  return st(s.plug, s.collectorOn);
};

{
  const d = plugged();
  check('idle blower reports a plug at all', !!sys(d, 'big', 1000).plug);
  eq('and it is off', state(d, 'big', 1000), 'off');

  // A tool starts the blower. The grace only gates the ACCUSATION: current on
  // the wire is a positive observation and is believed the moment it appears,
  // whatever the clock says. The simulated plug is at running draw immediately
  // (a real motor ramps, but nothing renders `starting`, so simulating the ramp
  // would add a state no one can see).
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  eq('current on the wire is believed at once', state(d, 'big', 1000), 'running');
  eq('and it stays running', state(d, 'big', 1000 + GRACE), 'running');
  // The clock only matters when the watts are LOW — which is the dead-blower
  // case below, and the only case the grace exists for.
  check('the grace gates the accusation, not the good news',
        st({ watts: RUN_W, reachable: true, onForMs: 0 }, true) === 'running'
        && st({ watts: 0, reachable: true, onForMs: 0 }, true) === 'starting');
}

{
  // The one this whole change exists for: relay closed, nothing on the far side.
  const d = plugged();
  TD.setCollectorPlugFault(d, 'big', 'dead');
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  eq('a dead blower is not accused during spin-up', state(d, 'big', 1000), 'starting');
  eq('...but is once the grace is up', state(d, 'big', 1000 + GRACE), 'notStarting');
  check('and the device still claims it commanded it on', sys(d, 'big', 1000 + GRACE).collectorOn);
  eq('its watts are what give it away', sys(d, 'big', 1000 + GRACE).plug.watts, 0);
}

{
  const d = plugged();
  TD.setCollectorPlugFault(d, 'big', 'offline');
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  // Unreachable is NOT an accusation: the blower may well be running perfectly.
  eq('an unreachable plug never says notStarting', state(d, 'big', 1000 + GRACE * 5), 'unknown');
}

{
  // One blower's fault is not another's — the same isolation the container gives
  // every other per-system fact.
  const d = plugged();
  TD.setCollectorPlugFault(d, 'big', 'dead');
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  TD.setMachinePower(d, 'drill-press', 500, 1000);
  eq('the broken one is broken', state(d, 'big', 1000 + GRACE), 'notStarting');
  eq('the healthy one is not', state(d, 'small', 1000 + GRACE), 'running');
}

{
  // A hand-started blower is judged exactly like an automatic one — the plug does
  // not care who asked.
  const d = plugged();
  TD.setCollectorPlugFault(d, 'big', 'dead');
  TD.setCollectorManual(d, 'big', true, 1000);
  eq('a hand run gets the same grace', state(d, 'big', 1000), 'starting');
  eq('and the same verdict after it', state(d, 'big', 1000 + GRACE), 'notStarting');
}

{
  // Switching off must reset the clock, or the next start is judged against the
  // PREVIOUS one and alarms immediately.
  const d = plugged();
  TD.setCollectorPlugFault(d, 'big', 'dead');
  TD.setCollectorManual(d, 'big', true, 1000);
  eq('running long enough to be accused', state(d, 'big', 1000 + GRACE), 'notStarting');
  TD.setCollectorManual(d, 'big', false, 1000 + GRACE);
  eq('off is off', state(d, 'big', 1000 + GRACE), 'off');
  TD.setCollectorManual(d, 'big', true, 1000 + GRACE);
  eq('and the restart gets a fresh grace, not the old one',
     state(d, 'big', 1000 + GRACE), 'starting');
}

{
  // A collector with no plug named is a legitimate shop ("I start that one by
  // hand"). It must not report an all-zero plug, which would read as a dead
  // blower rather than an absent one.
  const d = TD.createTopologyDevice(clone(twoSystemShop));
  TD.setMachinePower(d, 'table-saw', 1500, 1000);
  check('no plug configured → nothing reported', sys(d, 'big', 1000).plug === undefined);
  eq('and the verdict is unknown, not an accusation', state(d, 'big', 1000 + GRACE * 5), 'unknown');
}

// ── report ──────────────────────────────────────────────────────────────────
let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
