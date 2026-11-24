// A board loses power while its tool is running, then comes back. Never dead-head: if the only open gate is on a board
// that went away, the collector must not run against closed gates (the board shuts its gates when it loses the brain).
'use strict';
const { jeffsShop, BOARDS } = require('./_shop');

module.exports = {
  title: "04 The saw's board loses power mid-cut, then comes back",
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    await ctx.step('switch on the saw', async () => { bench.power('saw', 600); await bench.until(async () => (await bench.status()).actuators.sawGate === 'open' && bench.plugs.collector.on); });
    await ctx.step("the saw's board loses power", async () => { bench.nodes[BOARDS.pwmA].stop();
      await bench.until(async () => ((await bench.problems()).problems || []).some((x) => x.code === 'board-offline'), 30000); });
    const p = (await bench.problems()).problems || [];
    ctx.expect(p.some((x) => x.code === 'board-offline'), 'the app says the board is offline');
    await ctx.step("the board's power comes back", async () => { bench.nodes[BOARDS.pwmA].start(); await bench.waitOnline([BOARDS.pwmA]); await ctx.sleep(2000); });
    ctx.expect(bench.plugs.collector.on && bench.nodes[BOARDS.pwmA].gateAt.sawGate === 'open', 'the saw has suction again once the board is back');
    await ctx.step('switch off the saw', async () => { bench.power('saw', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    ctx.expect(!((await bench.problems()).problems || []).some((x) => x.code === 'board-offline'), 'the offline problem clears');
  },
};
