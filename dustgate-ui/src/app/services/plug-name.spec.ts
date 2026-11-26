/** plug-name — what a person reads for a plug. Plain TypeScript, run by `npm test`. */
import { isFactoryName, plugName } from './plug-name';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}

console.log('\nPN1 a factory hostname is not a name');
ok('a Shelly with its MAC', isFactoryName('ShellyPlugUSG4-84E6D7047170'));
ok('a Tasmota with its MAC and suffix', isFactoryName('tasmota-0F0E99-3737'));
ok('a name someone chose', !isFactoryName('Table saw'));
ok('a hostname someone chose', !isFactoryName('shellyplugus-tablesaw'));
ok('nothing is not a name', isFactoryName(''));

console.log('\nPN2 what a person reads');
ok('its own name wins', plugName({ name: 'Drill Press', hostname: 'ShellyPlugUSG4-84E6D7047170' }, 'Bandsaw') === 'Drill Press');
ok('a factory name falls back to the tool it is on', plugName({ name: 'ShellyPlugUSG4-84E6D7047170' }, 'Table Saw') === 'Table Saw plug');
ok('a chosen hostname is used', plugName({ hostname: 'shellyplugus-tablesaw' }) === 'shellyplugus-tablesaw');
ok('nothing to go on reads Unnamed plug, never a MAC', plugName({ hostname: 'ShellyPlugUSG4-84E6D7047170' }) === 'Unnamed plug');

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exitCode = 1;
