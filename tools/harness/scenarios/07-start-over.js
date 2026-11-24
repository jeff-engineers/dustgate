// "Reset everything", then set the same shop up again from the app, with the same boards and plugs.
'use strict';
const { jeffsShop, shop, BOARDS } = require('./_shop');

module.exports = {
  title: '07 Reset everything, then set the shop up again',
  async run(ctx) {
    const { bench } = ctx;
    const ports = await jeffsShop(ctx);
    await ctx.step('reset everything', async () => { await bench.resetAll(); ctx.forget(); await ctx.sleep(1500); });
    await ctx.step('pair every board again', async () => {
      for (const id of Object.values(BOARDS)) await bench.pair(id);
      await bench.until(() => Object.values(bench.nodes).every((n) => n.online), 10000);
    });
    ctx.expect(Object.values(bench.nodes).every((n) => n.online), 'every board pairs again after a reset');
    await ctx.step('save the same shop again', () => ctx.save(shop(ports)));
    await ctx.step('switch on the saw', async () => { bench.power('saw', 600); await bench.until(async () => bench.plugs.collector.on); });
    ctx.expect(bench.plugs.collector.on, 'the saw gets suction in the set-up-again shop');
    await ctx.step('switch off the saw', async () => { bench.power('saw', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
  },
};
