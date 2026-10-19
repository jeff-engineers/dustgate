// plug-identity.test.js — PAIR: the "S" block of firmware/test/test_relocate.cpp, same cases in the same order.
'use strict';
const { samePlug } = require('./plug-identity');
const results = [];
const check = (name, cond) => results.push({ name, ok: !!cond });

check('S1 nothing answered: unknown', samePlug({ kind: 'tasmota', mac: 'AA:AA:AA:AA:AA:01' }, { reachable: false }) === 'unknown');
check('S2 both MACs, equal: yes', samePlug({ kind: 'tasmota', mac: 'aa:aa:aa:aa:aa:01' }, { reachable: true, kind: 'tasmota', mac: 'AAAAAAAAAA01' }) === 'yes');
check('S3 both MACs, different: no', samePlug({ kind: 'tasmota', mac: 'AA:AA:AA:AA:AA:01' }, { reachable: true, kind: 'tasmota', mac: 'AA:AA:AA:AA:AA:02' }) === 'no');
check('S4 both MACs equal, kinds differ (reflashed): yes', samePlug({ kind: '', mac: 'AA:AA:AA:AA:AA:01' }, { reachable: true, kind: 'tasmota', mac: 'AA:AA:AA:AA:AA:01' }) === 'yes');
check('S5 no MAC on file, a Tasmota where a Shelly was saved: no', samePlug({ kind: '', mac: '' }, { reachable: true, kind: 'tasmota', mac: 'B8:F0:09:10:BD:28' }) === 'no');
check('S6 no MAC on file, same kind: unknown', samePlug({ kind: 'tasmota', mac: '' }, { reachable: true, kind: 'tasmota', mac: 'B8:F0:09:10:BD:28' }) === 'unknown');
check('S7 a MAC on file, the plug would not say one, same kind: unknown', samePlug({ kind: 'tasmota', mac: 'AA:AA:AA:AA:AA:01' }, { reachable: true, kind: 'tasmota', mac: '' }) === 'unknown');
check('S8 a MAC on file, the plug would not say one, other kind: no', samePlug({ kind: 'shelly', mac: 'AA:AA:AA:AA:AA:01' }, { reachable: true, kind: 'tasmota', mac: '' }) === 'no');
check('S9 an absent kind is a Shelly', samePlug({ kind: '', mac: '' }, { reachable: true, kind: 'shelly', mac: '' }) === 'unknown');

let failed = 0;
for (const r of results) { if (!r.ok) failed++; console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}`); }
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
if (failed) process.exit(1);
