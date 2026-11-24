// _shop.js — every scenario starts from Jeff's shop as it is today (jeffs-shop.js): its boards on and paired, its plugs
// standing, the layout saved. Returns the plug ports so a scenario can rebuild the layout from the same fixture.
'use strict';
const { shop, BOARDS, PLUGS } = require('../jeffs-shop');

async function jeffsShop(ctx) {
  const { bench } = ctx;
  const ports = await bench.addPlugs(PLUGS);
  await bench.startBrain();
  for (const id of Object.values(BOARDS)) await bench.pair(id);   // as the app pairs them (Boards → Pair), so it's saved
  bench.addNode(BOARDS.pwmA, 'pwm');
  bench.addNode(BOARDS.pwmB, 'pwm');
  bench.addNode(BOARDS.bus, 'bus');
  ctx.expect(await bench.waitOnline(Object.values(BOARDS)), 'every board comes online');
  await ctx.step('save the shop', () => ctx.save(shop(ports)));
  return ports;
}

/** A tool's machine id → the gate that feeds it, in Jeff's shop. */
const GATE_OF = { planer: 'planerGate', saw: 'sawGate', sander: 'sanderGate', miterSaw: 'miter', router: 'manifold', jointer: 'manifold' };

module.exports = { jeffsShop, shop, BOARDS, PLUGS, GATE_OF };
