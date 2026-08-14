// Suite for outlet-match.ts — the plug↔machine name guesser.
//
// Same contract as the other UI suites: throw nothing, print a tally, set
// process.exitCode on failure. Run by spec-runner.js.

import { nameScore, MATCH_MIN, bestMatch, matchAll } from './outlet-match';

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
check('a typo still matches',            nameScore('tabel saw', 'Table Saw') >= MATCH_MIN);
check('unrelated names do not match',    nameScore('Bench light', 'Table Saw') < MATCH_MIN);
check('a bare Shelly id does not match', nameScore('shellyplug-s-8f21c4', 'Table Saw') < MATCH_MIN);
eq('an empty name scores 0',             nameScore('', 'Table Saw'), 0);

// The adversarial pair, and the reason MATCH_MIN can't just be lowered until
// every typo matches: two REAL machines that can sit in one shop, sharing a
// word. Suggesting one for the other would be wrong every time.
check('Miter Saw does not match Table Saw',     nameScore('Miter Saw', 'Table Saw') < MATCH_MIN);
check('Bandsaw does not match Table Saw',       nameScore('Bandsaw', 'Table Saw') < MATCH_MIN);
check('Drill Press does not match Drum Sander', nameScore('Drill Press', 'Drum Sander') < MATCH_MIN);

// The band the threshold sits in. If a change ever narrows this, the scorer got
// worse even if every case above still passes.
check('true pairs stay clear of false ones',
      nameScore('tabel saw', 'Table Saw') - nameScore('Bandsaw', 'Table Saw') > 0.2,
      `worst true ${nameScore('tabel saw', 'Table Saw').toFixed(2)}, ` +
      `best false ${nameScore('Bandsaw', 'Table Saw').toFixed(2)}`);

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

console.log(`\n${pass}/${pass + fail} passed`);
if (fail) process.exitCode = 1;
