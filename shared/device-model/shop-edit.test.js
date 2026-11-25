// shop-edit.test.js — every edit in shop-edit.js leaves a valid shop with nothing dangling. JS only: the brain never
// edits a layout, so there is no C++ twin. Run: `node shop-edit.test.js` (in `npm run model:test`).
'use strict';
const E = require('./shop-edit');
const { validateShop } = require('./shop');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) => check(name, JSON.stringify(got) === JSON.stringify(want), `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);
const valid = (name, doc) => { const v = validateShop(doc); check(`${name}: still a valid shop`, v.ok, JSON.stringify(v.errors)); eq(`${name}: nothing dangles`, E.danglingRefs(doc), []); };

// Two boards (PWM and serial), three gates, a clamp on the PWM board, cells for everything.
const shop = () => ({
  schemaVersion: 2, name: 't',
  controllers: [
    { id: 'brain', role: 'primary', name: 'Brain', board: 'pi' },
    { id: 'pwm', role: 'secondary', name: 'Wall', board: 'xiao_c5', link: { transport: 'wifi-ws', host: 'pwm' } },
    { id: 'bus', role: 'secondary', name: 'Serial', board: 'xiao_c5', drives: 'bus', chains: [['g2', 'g3']], link: { transport: 'wifi-ws', host: 'bus' } },
  ],
  systems: [{ id: 's1', name: 'DC', elements: [
    { id: 'dc', type: 'collector', name: 'DC' },
    ...['g1', 'g2', 'g3'].map((id, i) => ({ id, type: 'selector', name: id, controllerId: i ? 'bus' : 'pwm', kind: 'servoGate',
      states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
      branches: [{ id: 'b', opensState: 'open', role: 'tool' }], servo: i ? { bus: 2 + i, referenceAngle: 10 } : { channel: 0, referenceAngle: 10 } })),
    { id: 't1', type: 'tool', name: 'Saw', machineId: 't1' }, { id: 't2', type: 'tool', name: 'Sander', machineId: 't2' }, { id: 't3', type: 'tool', name: 'Miter', machineId: 't3' },
  ], ducts: [{ child: 'g1', parent: 'dc' }, { child: 'g2', parent: 'dc' }, { child: 'g3', parent: 'dc' },
    { child: 't1', parent: 'g1', parentBranch: 'b' }, { child: 't2', parent: 'g2', parentBranch: 'b' }, { child: 't3', parent: 'g3', parentBranch: 'b' }] }],
  machines: [{ id: 't1', name: 'Saw', sensor: { ct: { controllerId: 'pwm', channel: 0 } } }, { id: 't2', name: 'Sander' }, { id: 't3', name: 'Miter' }],
  devices: [],
  ui: { layout: { g1: { col: 1, row: 1 } }, wiring: { boards: { pwm: { col: 0, row: 0 }, bus: { col: 2, row: 0 } } } },
});

valid('the fixture', shop());

{ // REFS knows every place a board's id sits
  const where = E.refsTo(shop(), 'board', 'pwm').map((r) => r.where).sort();
  eq('a board is found in its gate, its clamp and its cell', where, ["a gate's board", "a tool's clamp", 'its canvas cell']);
  const d = shop(); d.machines[0].sensor.ct.controllerId = 'gone';
  eq('a clamp on a board the shop lacks is dangling', E.danglingRefs(d), [{ kind: 'board', id: 'gone', where: "a tool's clamp" }]);
}

{ // unwire
  const d = E.unwireGate(shop(), 'g2');
  valid('unwire a serial gate', d);
  const g = d.systems[0].elements.find((e) => e.id === 'g2');
  check('...it has no board and no servo number', !('controllerId' in g) && !('bus' in g.servo));
  check('...keeps its calibration', g.servo.referenceAngle === 10);
  eq('...and is off the chain', d.controllers[2].chains, [['g3']]);
  check('isWired says so', !E.isWired(g) && E.isWired(d.systems[0].elements.find((e) => e.id === 'g1')));
  E.unwireGate(d, 'g3');
  check('the last gate off a chain takes the chains with it', !('chains' in d.controllers[2]));
}

{ // wire
  const d = E.wireGate(E.unwireGate(shop(), 'g1'), 'g1', 'bus', { bus: 7 });
  valid('wire a PWM gate onto the serial board', d);
  const g = d.systems[0].elements.find((e) => e.id === 'g1');
  check('...it names the board and its servo number, and no channel', g.controllerId === 'bus' && g.servo.bus === 7 && !('channel' in g.servo));
  eq('...on the end of the chain', d.controllers[2].chains, [['g2', 'g3', 'g1']]);
  d.systems[0].elements.find((e) => e.id === 'g1').servo.maxAngle = 359;
  const back = E.wireGate(d, 'g1', 'pwm', { channel: 1 });
  check('...and drops a whole-circle range a PWM servo cannot turn', !('maxAngle' in back.systems[0].elements.find((e) => e.id === 'g1').servo));
  valid('wire it back as a PWM servo', back);
  eq('...off the chain again', back.controllers[2].chains, [['g2', 'g3']]);
}

{ // tie a servo already on the bus to the gate that lost its number
  const d = shop();
  delete d.systems[0].elements.find((e) => e.id === 'g3').servo.bus;
  E.setServoNumber(d, 'g3', 4);
  valid('tie servo 4 to a gate with none', d);
  const g = d.systems[0].elements.find((e) => e.id === 'g3');
  check('...it names servo 4 and keeps its positions', g.servo.bus === 4 && g.servo.referenceAngle === 10);
  eq('...and stays where it is on its chain', d.controllers[2].chains, [['g2', 'g3']]);
  let refused = '';
  try { E.setServoNumber(d, 'g3', 3); } catch (e) { refused = e.message; }
  check("a number another gate holds is refused, saying whose", /already turns g2/.test(refused), refused);
}

{ // an edited copy of a gate goes back into the layout itself
  const d = shop();
  const copy = JSON.parse(JSON.stringify(d.systems[0].elements.find((e) => e.id === 'g1')));
  copy.servo.referenceAngle = 42;
  check('replaceElement finds the gate in its system', E.replaceElement(d, copy));
  check('...and the LAYOUT now has the new positions, not a copy of it', d.systems[0].elements.find((e) => e.id === 'g1').servo.referenceAngle === 42);
  check('an id no system holds is refused', !E.replaceElement(d, { id: 'nope' }));
  valid('after putting the gate back', d);
}

{ // replace a board
  const d = E.replaceBoard(shop(), 'pwm', 'pwm2');
  valid('replace a board with one the layout did not have', d);
  check('...the old board is gone and the new one is there, named as the old', !d.controllers.some((c) => c.id === 'pwm') && d.controllers.find((c) => c.id === 'pwm2').name === 'Wall');
  check('...its gate, its clamp and its cell moved', d.systems[0].elements[1].controllerId === 'pwm2' && d.machines[0].sensor.ct.controllerId === 'pwm2' && !!d.ui.wiring.boards.pwm2 && !d.ui.wiring.boards.pwm);
  check('...and it dials the new board', d.controllers.find((c) => c.id === 'pwm2').link.host === 'pwm2');
  const b = E.replaceBoard(shop(), 'bus', 'bus2');
  valid('replace the serial board', b);
  eq('...its chains move with it', b.controllers.find((c) => c.id === 'bus2').chains, [['g2', 'g3']]);
}

let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
