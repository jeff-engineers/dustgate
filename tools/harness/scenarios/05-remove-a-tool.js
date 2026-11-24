// The drum sander leaves the shop: its tool, its gate, its plug, and then its board.
'use strict';
const { jeffsShop, BOARDS } = require('./_shop');
const { removeToolAndGate, removeBoard } = require('./_edit');

module.exports = {
  title: '05 Take the drum sander out of the shop, then its board',
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    await ctx.step('delete the drum sander and its gate', () => ctx.save(removeToolAndGate(ctx.doc, 'sander', 'sanderGate')));
    await ctx.step('the saw still works', async () => { bench.power('saw', 600); await bench.until(async () => bench.plugs.collector.on); });
    await ctx.step('switch off the saw', async () => { bench.power('saw', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    await ctx.step("unpair the sander's board and delete it", async () => {
      await bench.unpair(BOARDS.pwmB);
      bench.nodes[BOARDS.pwmB].stop();
      await ctx.save(removeBoard(ctx.doc, BOARDS.pwmB));
    });
    ctx.expect(!((await bench.problems()).problems || []).length, 'no problem is left standing');
  },
};
