/** The Brain log screen's bookkeeping (boards/serial-log.ts).
 *
 *  Plain TypeScript, no Angular, no browser — run by spec-runner.js.
 */

import { SerialLog, classify, copyText, matches, type LogEntry, type LogLine } from './serial-log';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
function group(name: string): void { console.log(`\n${name}`); }

const lines = (l: SerialLog) => l.entries.filter((e): e is LogLine => e.kind === 'line').map(e => e.text);
const marks = (l: SerialLog) => l.entries.filter(e => e.kind !== 'line').map(e => e.kind);

group('S1 bytes become lines');
{
  const l = new SerialLog();
  l.apply({ text: '[NODE] linked\r\n[WiFi] up\r\n', start: 0, next: 24, boot: 'a' }, 0);
  ok('two lines, CR stripped', JSON.stringify(lines(l)) === '["[NODE] linked","[WiFi] up"]', JSON.stringify(lines(l)));
  ok('the cursor moves to next', l.cursor === 24);
}

group('S2 a line split across two replies is drawn once, whole');
{
  const l = new SerialLog();
  l.apply({ text: '[NODE] all paired bo', start: 0, next: 20, boot: 'a' }, 0);
  ok('nothing drawn while the line is unfinished', lines(l).length === 0);
  l.apply({ text: 'ards linked (1)\n', start: 20, next: 36, boot: 'a' }, 0);
  ok('then the whole line', JSON.stringify(lines(l)) === '["[NODE] all paired boards linked (1)"]', JSON.stringify(lines(l)));
}

group('S3 missed bytes are marked, not hidden');
{
  const l = new SerialLog();
  l.apply({ text: 'one\n', start: 0, next: 4, boot: 'a' }, 0);
  l.apply({ text: 'later\n', start: 1004, next: 1010, boot: 'a' }, 0);
  ok('a gap between the two', JSON.stringify(marks(l)) === '["gap"]', JSON.stringify(marks(l)));
  const gap = l.entries.find(e => e.kind === 'gap');
  ok('it says how much', !!gap && gap.text.startsWith('1,000 characters missed'), gap?.text);
  ok('and the cursor follows the reply', l.cursor === 1010);
}

group('S4 a restart is marked, and the stale reply is thrown away');
{
  const l = new SerialLog();
  l.apply({ text: 'old boot\n', start: 0, next: 5000, boot: 'a' }, 0);
  const added = l.apply({ text: 'MIDDLE OF NEW BOOT\n', start: 5000, next: 5019, boot: 'b' }, 0);
  ok('a restart mark', JSON.stringify(marks(l)) === '["restart"]', JSON.stringify(marks(l)));
  ok('the reply read at the old cursor is not drawn', !lines(l).includes('MIDDLE OF NEW BOOT'));
  ok('only the mark was added', added === 1);
  ok('and the next ask starts the new boot from byte 0', l.cursor === 0);
  l.apply({ text: '=== DustGate primary ===\n', start: 0, next: 25, boot: 'b' }, 0);
  ok('which then reads normally, with no gap', lines(l).includes('=== DustGate primary ===') && !marks(l).includes('gap'));
}

group('S5 lines sort into the chips');
{
  ok('a NodeLink line is Boards', classify('[NODE←] ACK seq=47 ok').group === 'boards');
  ok('a WiFi line is Network', classify('[WiFi] Connected in 1250ms').group === 'network');
  ok('plug discovery is Network', classify('[DISCOVER] attempt 1/1').group === 'network');
  ok('a jog is neither', classify('[UI] Servo jog: ch=0').group === 'other');
  ok('the tag is split off for colouring', classify('[NODE] dark: x').tag === '[NODE]' && classify('[NODE] dark: x').rest === ' dark: x');
  ok('a core error has no tag but is an error',
     classify('[839711][E][NetworkClient.cpp:435] write(): fail').level === 'err' && classify('[839711][E][x] y').tag === '');
  ok('"Link lost" is a problem', classify('[NODE] Link lost: dustgate-drum-sander').level === 'err');
  ok('"backing off" is a warning', classify('[NODE] backing off x — retrying every 2000 ms').level === 'warn');
  ok('a plain ACK is neither', classify('[NODE←] ACK seq=5 ok').level === '');
  const parent = { kind: 'line', seq: 1, at: 0, text: '[NODE] Still dialling', ...classify('[NODE] Still dialling') } as LogLine;
  const cont = classify('       signs of life: mDNS 0s ago', parent);
  ok('an indented continuation belongs to the line it continues', cont.group === 'boards' && cont.tag === '');
}

group('S6 filters and copy');
{
  const l = new SerialLog();
  l.apply({ text: '[NODE] Link lost: x\n[WiFi] up\n[UI] jog\n', start: 0, next: 40, boot: 'a' }, 0);
  l.mark('gap', '5 characters missed here.', 0);
  const shown = (f: Parameters<typeof matches>[1], find = '') =>
    l.entries.filter(e => matches(e, f, find)).map(e => e.kind === 'line' ? e.text : e.kind);
  ok('Boards', JSON.stringify(shown('boards')) === '["[NODE] Link lost: x","gap"]', JSON.stringify(shown('boards')));
  ok('Problems', JSON.stringify(shown('problems')) === '["[NODE] Link lost: x","gap"]');
  ok('find is case-insensitive', JSON.stringify(shown('all', 'JOG')) === '["[UI] jog","gap"]');
  const copied = copyText(l.entries as LogEntry[]);
  ok('copy keeps marks, set apart', copied.endsWith('--- 5 characters missed here. ---'), copied);
}

group('S7 the list is capped');
{
  const l = new SerialLog(3);
  l.apply({ text: 'a\nb\nc\nd\ne\n', start: 0, next: 10, boot: 'a' }, 0);
  ok('oldest lines roll off', JSON.stringify(lines(l)) === '["c","d","e"]', JSON.stringify(lines(l)));
}

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exitCode = 1;
