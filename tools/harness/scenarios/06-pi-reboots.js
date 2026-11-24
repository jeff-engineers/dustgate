// The Pi loses power and comes back: the layout and the pairings survive, the boards rejoin, the tools work.
'use strict';
const { jeffsShop, BOARDS } = require('./_shop');

module.exports = {
  title: '06 The brain restarts (a Pi power cut), and the shop picks up where it was',
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    await ctx.step('the brain restarts', async () => { await bench.restartBrain(); await bench.waitOnline(Object.values(BOARDS), 12000); await ctx.sleep(1500); });
    ctx.expect(Object.values(bench.nodes).every((n) => n.online), 'every board rejoins on its own');
    await ctx.step('switch on the miter saw', async () => { bench.power('miterSaw', 600); await bench.until(async () => (await bench.status()).actuators.miter === 'open' && bench.plugs.collector.on); });
    ctx.expect(bench.plugs.collector.on, 'the miter saw gets suction after the restart');
    await ctx.step('switch off the miter saw', async () => { bench.power('miterSaw', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
  },
};
