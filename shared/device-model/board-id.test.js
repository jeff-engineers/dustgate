// board-id.test.js — PAIR: firmware/test/test_boardid.cpp, same cases in the same order.
'use strict';
const { bareHost, isOwnBoard, sameBoard } = require('./board-id');
const results = [];
const check = (name, cond) => results.push({ name, ok: !!cond });

// B1 one canonical spelling
check('a bare name stays', bareHost('dustgate-node-1') === 'dustgate-node-1');
check('.local comes off', bareHost('dustgate-node-1.local') === 'dustgate-node-1');
check('case is folded', bareHost('DustGate-Node-1.LOCAL') === 'dustgate-node-1');
check('a trailing dot goes first', bareHost('dustgate-node-1.local.') === 'dustgate-node-1');
check('null is empty', bareHost(null) === '');
check('.local alone is left as it is', bareHost('.local') === '.local');
// B2 is this controllerId THIS board?
check('absent means this board', isOwnBoard('', 'primary'));
check('its own id is this board', isOwnBoard('primary', 'primary'));
check('in any spelling', isOwnBoard('Primary.local', 'primary'));
check('another board is not', !isOwnBoard('node-1', 'primary'));
check('a board that does not know its own id claims no NAMED board', !isOwnBoard('node-1', ''));
check('...but absent is still this board', isOwnBoard('', ''));
// B3 do two ids name the same board?
check('one board in two spellings', sameBoard('node-1', 'NODE-1.local', 'primary'));
check('two different boards', !sameBoard('node-1', 'node-2', 'primary'));
check('absent and the board\'s own id are the same board', sameBoard('', 'primary', 'primary'));
check('absent and a node are not', !sameBoard('', 'node-1', 'primary'));
check('two absents', sameBoard('', '', 'primary'));

let failed = 0;
for (const r of results) { if (!r.ok) failed++; console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}`); }
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
