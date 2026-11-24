// The shop as it stands: every tool, one at a time, the way Jeff uses it.
'use strict';
const { jeffsShop, GATE_OF } = require('./_shop');

module.exports = {
  title: '01 Your shop runs: each tool opens its own gate and starts the collector',
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    const gateAt = async (g) => (await bench.status()).actuators[g];

    for (const [tool, plug] of [['saw', 'saw'], ['sander', 'sander'], ['miterSaw', 'miterSaw']]) {
      await ctx.step(`switch on the ${tool}`, async () => {
        bench.power(plug, 600);
        await bench.until(async () => (await gateAt(GATE_OF[tool])) === 'open' && bench.plugs.collector.on);
      });
      ctx.expect((await gateAt(GATE_OF[tool])) === 'open', `the ${tool}'s gate is open`);
      ctx.expect(bench.plugs.collector.on, `the collector runs for the ${tool}`);
      await ctx.step(`switch off the ${tool}`, async () => {
        bench.power(plug, 0);
        await bench.until(() => !bench.plugs.collector.on, 15000);
      });
      ctx.expect(!bench.plugs.collector.on, `the collector stops after the ${tool}`);
      ctx.expect((await gateAt(GATE_OF[tool])) === 'open', `the ${tool}'s gate stays open at rest (the system rests open)`);
    }

    await ctx.step('switch on the router table: the manifold turns to the router side', async () => {
      bench.power('router', 600);
      await bench.until(async () => (await gateAt('manifold')) === 'left');
    });
    ctx.expect((await gateAt('manifold')) === 'left', 'the manifold is on the router side');
    await ctx.step('switch the router off and the jointer on: the manifold turns over', async () => {
      bench.power('router', 0); bench.power('jointer', 600);
      await bench.until(async () => (await gateAt('manifold')) === 'right');
    });
    ctx.expect((await gateAt('manifold')) === 'right', 'the manifold is on the jointer side');
    await ctx.step('switch off the jointer', async () => { bench.power('jointer', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });

    await ctx.step('switch the planer on from the app (it has no plug)', async () => {
      await bench.manual('planer', true);
      await bench.until(async () => (await gateAt('planerGate')) === 'open' && bench.plugs.collector.on);
    });
    ctx.expect(bench.plugs.collector.on && (await gateAt('planerGate')) === 'open', 'the planer gets suction');
    await ctx.step('switch the planer off from the app', async () => { await bench.manual('planer', false); await bench.until(() => !bench.plugs.collector.on, 15000); });
    ctx.expect(!bench.plugs.collector.on, 'the collector stops after the planer');
  },
};
