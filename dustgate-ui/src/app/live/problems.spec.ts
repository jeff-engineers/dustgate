/** Live view problem rows (live/problems.ts). Plain TypeScript, run by spec-runner.js. */
import { duration, headline, problemRows, whyFailed } from './problems';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
const P = (code: string, severity: 'bad' | 'warn', id: string, text = 't', forMs?: number) =>
  ({ code, severity, subject: { type: 'x', id }, text, forMs });
const nameOf = (_t: string, id: string) => id.toUpperCase();

console.log('\nL1 rows');
{
  const rows = problemRows([P('plug-unreachable', 'warn', 'a'), P('collector-no-start', 'bad', 'big', 'why', 125000)], nameOf);
  ok('reds sort first', rows[0].code === 'collector-no-start' && rows[1].code === 'plug-unreachable');
  ok('the headline names the thing', rows[0].what === "BIG isn't starting", rows[0].what);
  ok("the device's words are the reason", rows[0].why === 'why');
  ok('duration is spoken', rows[0].duration === 'for 2m', rows[0].duration);
  ok('no duration, no claim', rows[1].duration === '');
  ok('absent list is empty', problemRows(undefined, nameOf).length === 0);
  ok('an unknown code still reads', headline(P('board-fault', 'bad', 'x'), 'x') === 'board fault');
  ok('seconds', duration(30000) === 'for 30s');
  ok('hours', duration(3 * 3600000) === 'for 3h');
}

console.log('\nL2 why a request failed');
{
  ok('no answer', whyFailed({ status: 0 }) === "the controller didn't answer");
  ok('the device says why', whyFailed({ status: 409, error: { error: 'unknown selector' } }) === 'unknown selector');
  ok('a bare refusal still says something', whyFailed({ status: 500, error: null }).includes('HTTP 500'));
  ok('a string body', whyFailed({ status: 400, error: 'bad tool' }) === 'bad tool');
  ok('never empty', whyFailed(null).length > 0);
}

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exitCode = 1;
