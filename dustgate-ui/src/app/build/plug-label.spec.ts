/** Fitting a plug name into the chip on a machine (build/plug-label.ts).
 *
 *  Plain TypeScript, no Angular, no browser — run by spec-runner.js.
 */

import { fitText, plugLabel, stripPlugPrefix, textWidth } from './plug-label';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
function group(name: string): void { console.log(`\n${name}`); }

// The chip as drawn: 9.5px text in the ~49 units the outlet icon leaves.
const FONT = 9.5;
const BOX = 49;

group('P1 the estimate tracks what actually renders');
{
  // Measured off the rendered SVG on 2026-08-22 (system-ui, macOS). The table is
  // rounded UP on purpose, so the estimate should sit at or above the real width —
  // never under it, which is what would let text out through the icon.
  const measured: Array<[string, number]> = [
    ['tablesaw', 40.9], ['bandsaw', 40.8], ['no plug', 34.4],
    ['Drum Sander', 60.3], ['Router Table', 58.0],
  ];
  for (const [s, real] of measured) {
    const est = textWidth(s, FONT);
    ok(`"${s}" is not under-estimated`, est >= real - 0.5, `est ${est.toFixed(1)} vs real ${real}`);
    ok(`"${s}" is not wildly over-estimated`, est <= real * 1.15,
       `est ${est.toFixed(1)} vs real ${real}`);
  }
  ok('a wide letter costs more than a narrow one', textWidth('W', FONT) > textWidth('i', FONT));
  ok('an empty string is nothing', textWidth('', FONT) === 0);
}

group('P2 anything that fits is left alone');
{
  ok('a short name is untouched', fitText('tablesaw', BOX, FONT) === 'tablesaw');
  ok('no plug is untouched', fitText('no plug', BOX, FONT) === 'no plug');
  ok('a wattage reading is untouched', fitText('1.2 kW', BOX, FONT) === '1.2 kW');
}

group('P3 anything that does not, fits after trimming');
{
  for (const s of ['Drum Sander', 'Router Table', 'WWWWWWWWWWWWWWWW',
                   'a very long plug name indeed', 'MMMMMMMMMM']) {
    const out = fitText(s, BOX, FONT);
    ok(`"${s}" fits the box`, textWidth(out, FONT) <= BOX,
       `"${out}" is ${textWidth(out, FONT).toFixed(1)} wide`);
    ok(`"${s}" says it was cut`, out.endsWith('…'));
    ok(`"${s}" keeps as much as it can`,
       textWidth(out, FONT) > BOX * 0.7, `"${out}"`);
  }
  // The ellipsis is measured, not assumed free — this is the case that catches a
  // budget computed before the ellipsis is appended.
  ok('a string of ellipsis width still fits', textWidth(fitText('…………………', BOX, FONT), FONT) <= BOX);
  ok('no room at all still says something is there', fitText('Drum Sander', 4, FONT) === '…');
  ok('the space before the cut goes with it', !fitText('Router Table', BOX, FONT).endsWith(' …'));
}

group('P4 a Shelly hostname identifies by its tail');
{
  ok('the model prefix goes', stripPlugPrefix('shellyplugus-tablesaw') === 'tablesaw');
  ok('...in its other spellings too',
     stripPlugPrefix('ShellyPlus-bandsaw') === 'bandsaw' && stripPlugPrefix('shellyplug-jointer') === 'jointer');
  ok('a name that is only the prefix is kept whole',
     stripPlugPrefix('shellyplugus') === 'shellyplugus');
  ok('a human name is untouched', stripPlugPrefix('Drum Sander') === 'Drum Sander');
  ok('and the whole job in one call fits the box',
     textWidth(plugLabel('shellyplugus-drum-sander-back-wall', BOX, FONT), FONT) <= BOX);
}

console.log(`\n${checks - failures}/${checks} checks passed`);
// exitCode, NOT exit(): spec-runner.js requires the suites into ONE process, so
// an outright exit here ends the run. This called process.exit(0) until
// 2026-09-02, which made plug-label permanently the last suite that could
// execute — two suites added after it in SUITES never ran and never said so.
if (failures) process.exitCode = 1;
