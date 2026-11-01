/** Bus servos on the Boards page, cases BS1–BS3. Plain TypeScript — run with `npm test`. */
import { busServoLines, usualLoad } from './bus-servos';
import type { ConfigurableSelector } from '../gates/selector-types';
import type { BusServoState } from '../services/bus-types';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
function group(name: string): void { console.log(`\n${name}`); }

const valve = (id: string, bus?: number): ConfigurableSelector => ({
  id, type: 'selector', name: id, controllerId: 'wall', kind: 'servoGate', states: [], branches: [],
  servo: bus === undefined ? {} : { bus },
});
const slider = (id: string, bus: number): ConfigurableSelector => ({
  id, type: 'selector', name: id, controllerId: 'wall', kind: 'linear', states: [], branches: [], linear: { bus },
});
const rep = (id: number, history: number[], ok = true): BusServoState => ({
  id, ok, peak: history[history.length - 1] ?? 0, missed: 0, reads: 10, moves: history.length, history,
});

group('BS1 what it usually needs');
ok('nothing to say before three earlier moves', usualLoad([15, 16, 30]) === null);
ok('the median of the moves before the last', usualLoad([14, 15, 16, 34]) === 15);
ok('an even count takes the middle two', usualLoad([14, 16, 15, 17, 40]) === 16);

group('BS2 listed by gate, never by number');
{
  const lines = busServoLines([valve('Drum sander valve', 4), slider('Slider', 2)], [rep(4, [12, 13, 14, 13]), rep(2, [17])]);
  ok('each gate is listed by its name', lines.map((l) => l.name).join(',') === 'Drum sander valve,Slider');
  ok('...with its own report', lines[0].state?.id === 4 && lines[0].last === 13 && lines[0].usual === 13);
  ok('a servo with one move has a last but no usual yet', lines[1].last === 17 && lines[1].usual === null);
  ok('kinds are a woodworker\'s words', lines[0].kind === 'ball valve' && lines[1].kind === 'slider');
}

group('BS3 the layout and the board disagree, and the list says how');
{
  const lines = busServoLines([valve('Bandsaw valve'), valve('Router', 5)], [rep(9, [20])]);
  ok('a gate with no servo yet has no number and no report', lines[0].bus === undefined && !lines[0].state);
  ok('a gate whose servo has not reported has its number but no report', lines[1].bus === 5 && !lines[1].state);
  ok('a servo no gate holds is listed, and says so', lines[2].name === 'A servo not tied to a gate' && lines[2].bus === 9);
  const wired = busServoLines([valve('a', 2), valve('b', 3), valve('c', 4)], [], [['c'], ['a']]);
  ok('chain order comes first, as wired', wired.map((l) => l.name).join('') === 'cab');
}

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exitCode = 1;
