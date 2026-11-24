// Rearranging: the saw's gate moves from the planer's board to the sander's board, then to the serial board.
'use strict';
const { jeffsShop, BOARDS } = require('./_shop');
const { moveGate } = require('./_edit');

module.exports = {
  title: "02 Move a gate to another board, then to the serial board, and the saw still gets suction",
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    const sawRuns = async (what) => {
      await ctx.step(`switch on the saw (${what})`, async () => { bench.power('saw', 600); await bench.until(async () => (await bench.status()).actuators.sawGate === 'open' && bench.plugs.collector.on); });
      ctx.expect(bench.plugs.collector.on && (await bench.status()).actuators.sawGate === 'open', `the saw gets suction (${what})`);
      await ctx.step('switch off the saw', async () => { bench.power('saw', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    };
    await ctx.step("move the saw's gate to the sander's board, channel 1", () => ctx.save(moveGate(ctx.doc, 'sawGate', BOARDS.pwmB, { channel: 1 })));
    await sawRuns('on the sander board');
    ctx.expect(bench.nodes[BOARDS.pwmB].gateAt.sawGate !== undefined, "the sander's board is the one that moved it");
    await ctx.step("move the saw's gate onto the serial board as servo 4", () => ctx.save(moveGate(ctx.doc, 'sawGate', BOARDS.bus, { bus: 4 })));
    await sawRuns('on the serial board');
    ctx.expect(bench.nodes[BOARDS.bus].gateAt.sawGate !== undefined, 'the serial board is the one that moved it');
  },
};
