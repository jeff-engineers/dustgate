// Suite for outlet-match.ts — the plug↔machine name guesser.
//
// Same contract as the other UI suites: throw nothing, print a tally, set
// process.exitCode on failure. Run by spec-runner.js.

import { nameScore, MATCH_MIN, MATCH_MARGIN, bestMatch, matchAll } from './outlet-match';

let pass = 0, fail = 0;
function check(what: string, ok: boolean, extra = ''): void {
  if (ok) { pass++; console.log(`  ✓ ${what}`); }
  else { fail++; console.error(`  ✗ ${what}${extra ? ' — ' + extra : ''}`); }
}
const eq = (what: string, got: unknown, want: unknown): void =>
  check(what, got === want, `got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`);

// ── scoring ─────────────────────────────────────────────────────────────────
// The cases that actually turn up: a Shelly named by someone in a hurry.
eq('identical names score 1',            nameScore('Table Saw', 'Table Saw'), 1);
eq('case and spacing are noise',         nameScore('tablesaw', 'Table Saw'), 1);
eq('dashes are noise',                   nameScore('Table-Saw', 'Table Saw'), 1);
eq('underscores are noise',              nameScore('table_saw', 'Table Saw'), 1);
check('a prefix scores high',            nameScore('Table Saw 240v', 'Table Saw') >= 0.9);
check('containment scores high',         nameScore('Shop Table Saw Plug', 'Table Saw') >= 0.85);
// A single transposed letter does NOT clear the bar, and that is deliberate: it
// scores 0.571, below two false pairs we have to reject (0.556). Documented as a
// known cost, not an oversight — see MATCH_MIN.
check('a transposition falls below the bar', nameScore('tabel saw', 'Table Saw') < MATCH_MIN);
check('...but is still clearly related',     nameScore('tabel saw', 'Table Saw') > 0.5);
check('a dropped letter still matches',      nameScore('Jointr', 'Jointer') >= MATCH_MIN);
check('unrelated names do not match',    nameScore('Bench light', 'Table Saw') < MATCH_MIN);
check('a bare Shelly id does not match', nameScore('shellyplug-s-8f21c4', 'Table Saw') < MATCH_MIN);
eq('an empty name scores 0',             nameScore('', 'Table Saw'), 0);

// ── the pairs that set the bar ──────────────────────────────────────────────
// Two REAL machines that can sit in one shop, sharing a word. Every one of these
// must stay below MATCH_MIN, because Match by name ASSIGNS: getting one wrong
// puts a wrong plug on a real machine.
//
// "Table Saw" ~ "Router table" scores 0.471: harmless at today's bar, and a
// misassignment waiting to happen at the 0.45 this file used to carry, any time
// the saw's own machine was already paired.
check('Table Saw does not match Router table',  nameScore('Table Saw', 'Router table') < MATCH_MIN);
check('Drum Sander does not match Disc Sander', nameScore('Drum Sander', 'Disc Sander') < MATCH_MIN);
check('Belt Sander does not match Drum Sander', nameScore('Belt Sander', 'Drum Sander') < MATCH_MIN);
check('Miter Saw does not match Table Saw',     nameScore('Miter Saw', 'Table Saw') < MATCH_MIN);
check('Bandsaw does not match Table Saw',       nameScore('Bandsaw', 'Table Saw') < MATCH_MIN);
check('Drill Press does not match Drum Sander', nameScore('Drill Press', 'Drum Sander') < MATCH_MIN);

// The uncomfortable fact this file exists to record: the two populations are
// NOT separable. The worst true pair and the best false one are 0.015 apart —
// far too close to put a bar between, so the bar is a choice about which error
// to make rather than a line that divides them.
{
  const worstTrue = nameScore('tabel saw', 'Table Saw');       // 0.571
  const bestFalse = nameScore('Drum Sander', 'Disc Sander');   // 0.556
  check('worst true and best false are within a rounding error of each other',
        Math.abs(worstTrue - bestFalse) < 0.05,
        `worst true ${worstTrue.toFixed(3)}, best false ${bestFalse.toFixed(3)}`);
  check('so the bar clears every false pair, and loses that true one',
        MATCH_MIN > bestFalse && MATCH_MIN > worstTrue);
}

// ── bestMatch ───────────────────────────────────────────────────────────────
const MACHINES = [
  { id: 'saw',    name: 'Table Saw' },
  { id: 'planer', name: 'Planer' },
  { id: 'sander', name: 'Drum Sander' },
];

eq('picks the obvious machine',
   bestMatch('tablesaw', MACHINES)?.item.id, 'saw');
eq('returns null when nothing is close',
   bestMatch('Bench light', MACHINES), null);
eq('skips a machine that is already taken',
   bestMatch('tablesaw', MACHINES, new Set(['saw'])), null);
eq('picks the BEST, not the first that clears the bar',
   bestMatch('Drum Sander', MACHINES)?.item.id, 'sander');

// ── matchAll ────────────────────────────────────────────────────────────────
{
  const outlets = [
    { id: 'o1', name: 'TableSaw' },
    { id: 'o2', name: 'planer' },
    { id: 'o3', name: 'Bench light' },
  ];
  const got = matchAll(outlets, MACHINES);
  eq('matches every plug it can', got.length, 2);
  eq('  TableSaw → saw',  got.find(p => p.outletId === 'o1')?.machineId, 'saw');
  eq('  planer → planer', got.find(p => p.outletId === 'o2')?.machineId, 'planer');
  check('leaves the unmatchable one alone', !got.some(p => p.outletId === 'o3'));
}

{
  // The reason matchAll sorts globally instead of walking the list in order: a
  // weak match appears FIRST and wants the same machine as a perfect one. Greedy
  // in list order gives it away, and the user watches an obviously-right pairing
  // lose to one they'd have rejected.
  const outlets = [
    { id: 'weak',   name: 'saw' },          // partial — clears the bar
    { id: 'strong', name: 'Table Saw' },    // exact
  ];
  const got = matchAll(outlets, [{ id: 'saw', name: 'Table Saw' }]);
  eq('the strongest match wins the machine', got[0]?.outletId, 'strong');
  eq('and the weaker one is left free',      got.length, 1);
}

{
  const got = matchAll([{ id: 'o1', name: 'TableSaw' }], MACHINES, new Set(['saw']));
  eq('a machine already paired is not offered again', got.length, 0);
}

{
  eq('no outlets, no pairs', matchAll([], MACHINES).length, 0);
  eq('no machines, no pairs', matchAll([{ id: 'o1', name: 'Table Saw' }], []).length, 0);
}

// ── ambiguity ───────────────────────────────────────────────────────────────
{
  // "Sander" is 0.88 against both by containment. Confident about the name, and
  // no idea which machine — so it must not be assigned to either.
  const two = [{ id: 'drum', name: 'Drum Sander' }, { id: 'disc', name: 'Disc Sander' }];
  eq('an ambiguous plug is left alone', matchAll([{ id: 'o', name: 'Sander' }], two).length, 0);

  // Same plug, only one candidate: no ambiguity, so it pairs.
  eq('...but pairs when only one machine could own it',
     matchAll([{ id: 'o', name: 'Sander' }], [two[0]])[0]?.machineId, 'drum');

  check('MATCH_MARGIN is what makes that the rule', MATCH_MARGIN > 0);
}

{
  // The regression that started all this, end to end: the saw's own machine is
  // already paired, so the only machine left with a word in common is the router
  // table. It must come back empty rather than pairing them.
  const got = matchAll(
    [{ id: 'plug', name: 'Table Saw' }],
    [{ id: 'router', name: 'Router table' }, { id: 'planer', name: 'Planer' }],
  );
  eq('a taken machine does not push its plug onto a lookalike', got.length, 0);
}

console.log(`\n${pass}/${pass + fail} passed`);
if (fail) process.exitCode = 1;
