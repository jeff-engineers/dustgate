// A board dies and a new one (a different name) takes its place: Replace board moves its gates in one step.
'use strict';
const { jeffsShop, BOARDS } = require('./_shop');
const { replaceBoard } = require('./_edit');

module.exports = {
  title: "09 The planer/saw board dies; replace it with a new board",
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    await ctx.step('the planer/saw board dies', async () => { bench.nodes[BOARDS.pwmA].stop(); await ctx.sleep(500); });
    await ctx.step('pair a new board, and replace the dead one with it on the canvas', async () => {
      await bench.pair('dg-new-board');
      bench.addNode('dg-new-board', 'pwm');
      await bench.waitOnline(['dg-new-board']);
      await ctx.save(replaceBoard(ctx.doc, BOARDS.pwmA, 'dg-new-board'));
      await bench.unpair(BOARDS.pwmA);
    });
    for (const [tool, gate] of [['saw', 'sawGate']]) {
      await ctx.step(`switch on the ${tool}`, async () => { bench.power(tool, 600); await bench.until(async () => bench.nodes['dg-new-board'].gateAt[gate] === 'open' && bench.plugs.collector.on); });
      ctx.expect(bench.nodes['dg-new-board'].gateAt[gate] === 'open', `the new board opens the ${tool}'s gate`);
      await ctx.step(`switch off the ${tool}`, async () => { bench.power(tool, 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    }
    await ctx.step('switch the planer on from the app', async () => { await bench.manual('planer', true); await bench.until(async () => bench.nodes['dg-new-board'].gateAt.planerGate === 'open' && bench.plugs.collector.on); });
    ctx.expect(bench.nodes['dg-new-board'].gateAt.planerGate === 'open', "the new board opens the planer's gate");
    await ctx.step('switch the planer off', async () => { await bench.manual('planer', false); await bench.until(() => !bench.plugs.collector.on, 15000); });
    ctx.expect(!((await bench.problems()).problems || []).some((x) => x.subject?.id === BOARDS.pwmA), 'nothing is left said about the dead board');
  },
};
