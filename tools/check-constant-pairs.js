#!/usr/bin/env node
'use strict';
// Reads every constant listed in shared/device-model/constant-pairs.json out of its source file and fails if a group disagrees.
// Works on the SOURCE TEXT (JS, TypeScript, C++ headers), so it needs no build and sees the numbers the compilers will. A value it
// cannot read is a failure too: a renamed constant must not silently drop out of the check.
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');
const manifest = JSON.parse(fs.readFileSync(path.join(root, 'shared/device-model/constant-pairs.json'), 'utf8'));

const cache = {};
const read = (f) => cache[f] || (cache[f] = fs.readFileSync(path.join(root, f), 'utf8'));
const esc = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');

// The text of the right-hand side of NAME's definition: `const NAME = ...;`, `static const T kName = ...;`, `#define NAME ...`.
function rhs(file, name) {
  const src = read(file);
  // `profile::field` reads one field of an object-literal entry: `'rockler-4': { gatePitchMm: 127, ... }`.
  if (name.includes('::')) {
    const [obj, field] = name.split('::');
    const om = src.match(new RegExp(`'${esc(obj)}'\\s*:\\s*\\{([^}]*)\\}`));
    const fm = om && om[1].match(new RegExp(`\\b${esc(field)}\\s*:\\s*([^,}]+)`));
    return fm ? fm[1].trim() : null;
  }
  const n = esc(name);
  // A #define first: config.h's comments discuss old values in prose ("SERVO_COUNT = 3 ...;") that an assignment match would read.
  let m = src.match(new RegExp(`^\\s*#define\\s+${n}\\s+([^\\n/]+)`, 'm'));
  if (!m) m = src.match(new RegExp(`^[^/\\n]*\\b${n}\\s*=\\s*([^;]+);`, 'm'));
  return m ? m[1].replace(/\/\/.*$/, '').trim() : null;
}

// A tiny arithmetic evaluator: numbers (hex, suffixes u/l/f), + - * / << ( ), and OTHER constants from the same file.
function value(file, name, depth = 0) {
  const text = rhs(file, name);
  if (text == null) throw new Error(`${name} not found in ${file}`);
  if (depth > 6) throw new Error(`${name}: too deep`);
  let expr = text.replace(/\b(\d+\.?\d*|0x[0-9a-fA-F]+)[uUlLfF]+\b/g, '$1')
                 .replace(/\b[A-Za-z_]\w*\b/g, (id) => (/^0x/i.test(id) ? id : `(${value(file, id, depth + 1)})`));
  if (!/^[0-9a-fA-FxX+\-*/().<>\s]+$/.test(expr)) throw new Error(`${name} in ${file}: cannot evaluate "${text}"`);
  // eslint-disable-next-line no-new-func
  return Function(`"use strict"; return (${expr});`)();
}

let bad = 0, total = 0;
for (const g of manifest.groups) {
  total++;
  const vals = [];
  try { for (const [f, n] of g.refs) vals.push([f, n, value(f, n)]); }
  catch (e) { console.error(`  FAIL ${g.name}: ${e.message}`); bad++; continue; }
  if (vals.every((v) => v[2] === vals[0][2])) console.log(`  ok   ${g.name} = ${vals[0][2]}`);
  else { bad++; console.error(`  FAIL ${g.name}: ` + vals.map(([f, n, v]) => `${path.basename(f)}:${n}=${v}`).join('  vs  ')); }
}
// RELATIONSHIPS rather than equalities, which a pair table cannot say (CLAUDE.md names this one): the homing travel must exceed the
// span of the biggest rack anyone builds, or a healthy home on that rack fails.
total++;
try {
  const homing = value('firmware/config.h', 'HOMING_MAX_TRAVEL_MM');
  const DM = require(path.join(root, 'shared/device-model/device-model.js'));
  const span = DM.manifoldProfile('rockler-4', DM.NUM_STOPS).spanMm;
  if (homing > span) console.log(`  ok   homing travel ${homing} mm exceeds the largest rack (${span.toFixed(0)} mm)`);
  else { bad++; console.error(`  FAIL HOMING_MAX_TRAVEL_MM ${homing} mm does not exceed an ${DM.NUM_STOPS}-gate 4" rack (${span.toFixed(0)} mm)`); }
} catch (e) { bad++; console.error(`  FAIL homing relation: ${e.message}`); }

console.log(`\n${total - bad}/${total} constant checks pass`);
process.exit(bad ? 1 : 0);
