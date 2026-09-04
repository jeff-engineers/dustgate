/** Build-stamp cases S1–S3.
 *
 *  S1 mirrors the build-stamp block in firmware/test/test_statusscreen.cpp —
 *  THE SAME CASES, IN THE SAME ORDER, ASSERTING THE SAME STRINGS. That is the
 *  point of it: the footer and the OLED render the same fact, and the only thing
 *  keeping the two renderings identical is that both are pinned to these strings.
 *  Change one side's format and this file (or its C++ partner) fails, which is
 *  the reminder to change the other. See the note at the top of build-stamp.ts.
 */

import { formatBuildStamp, formatEpochStamp } from './build-stamp';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
function group(name: string): void { console.log(`\n${name}`); }

// ── S1 · the firmware's cases, verbatim ──────────────────────────────────────
group('S1 the device stamp renders exactly as the OLED renders it');
{
  const f = formatBuildStamp;
  ok('__DATE__/__TIME__ reads as a date', f('Aug 20 2026 07:25:00') === '8/20/26 7:25:00',
     f('Aug 20 2026 07:25:00'));
  // __DATE__ pads a single-digit day with a SPACE, which is the detail that
  // breaks parsing at a fixed offset.
  ok('a space-padded day parses', f('Aug  5 2026 23:04:09') === '8/5/26 23:04:09',
     f('Aug  5 2026 23:04:09'));
  ok('december, midnight', f('Dec 31 2026 00:00:00') === '12/31/26 0:00:00',
     f('Dec 31 2026 00:00:00'));
  // Empty, never half-formatted: a wrong date sends you to the wrong binary.
  ok('an unparseable month yields nothing', f('Xxx 20 2026 07:25:00') === '');
  ok('nulls yield nothing', f(null) === '' && f(undefined) === '' && f('') === '');
  ok('it fits the bottom row', '12/31/26 23:04:09'.length <= 21);
}

// ── S2 · the shapes a device can actually send ───────────────────────────────
group('S2 anything it cannot parse comes back empty');
{
  const f = formatBuildStamp;
  ok('a truncated stamp yields nothing', f('Aug 20 2026') === '');
  ok('a missing time yields nothing', f('Aug 20 2026  ') === '');
  ok('a junk time yields nothing', f('Aug 20 2026 7:25') === '');
  ok('a nonsense year yields nothing', f('Aug 20 1926 07:25:00') === '');
  ok('day 0 yields nothing', f('Aug 0 2026 07:25:00') === '');
  ok('day 32 yields nothing', f('Aug 32 2026 07:25:00') === '');
  // A year that rolls the century still reads two digits, zero-padded — "1/1/00",
  // never "1/1/0".
  ok('a century year pads to two digits', f('Jan 1 2100 01:02:03') === '1/1/00 1:02:03',
     f('Jan 1 2100 01:02:03'));
}

// ── S3 · the app's own stamp ─────────────────────────────────────────────────
group('S3 the bundle stamp reads the same way');
{
  // Built from local parts so the assertion holds in any timezone — the function
  // renders local time on purpose, so a fixed epoch would encode the runner's TZ.
  const d = new Date(2026, 8, 2, 7, 25, 0);      // 2 Sep 2026, 07:25:00 local
  ok('an epoch renders like a device stamp', formatEpochStamp(d.getTime()) === '9/2/26 7:25:00',
     formatEpochStamp(d.getTime()));
  const midnight = new Date(2026, 11, 31, 0, 0, 0);
  ok('december, midnight', formatEpochStamp(midnight.getTime()) === '12/31/26 0:00:00',
     formatEpochStamp(midnight.getTime()));
  // 0 is what an ungenerated stamp holds, and it must read as "no stamp" rather
  // than as 1970 — a 1970 date in the footer looks like a bug in the device.
  ok('no stamp yields nothing', formatEpochStamp(0) === '' && formatEpochStamp(undefined) === '');
  ok('nonsense yields nothing', formatEpochStamp(NaN) === '' && formatEpochStamp(-1) === '');
}

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exitCode = 1;
