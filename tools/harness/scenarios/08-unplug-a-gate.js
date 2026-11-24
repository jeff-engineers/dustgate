// The way out of the 2026-11-23 trap without re-wiring at once: the sander's board was reflashed to serial, so its
// PWM gate is unplugged from the board and left in the shop. Save works, the rest of the shop works, the app says what
// the sander needs, and wiring it later as a serial servo brings it back.
'use strict';
const { jeffsShop, BOARDS } = require('./_shop');
const { unwireGate, moveGate, boardDrives } = require('./_edit');

module.exports = {
  title: "08 Unplug the sander's gate from its board, run the shop, then wire it again",
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    await ctx.step("reflash the sander's board as a serial board", async () => { bench.nodes[BOARDS.pwmB].reflash('bus'); await bench.waitOnline([BOARDS.pwmB]); });
    await ctx.step("unplug the sander's gate from it on the canvas, and save", async () => {
      const r = await ctx.save(unwireGate(boardDrives(ctx.doc, BOARDS.pwmB, 'bus'), 'sanderGate'));
      ctx.expect(r.status === 200, 'the layout with an unplugged gate saves');
    });
    await ctx.step('switch on the sander', async () => { bench.power('sander', 600); await ctx.sleep(2500); });
    const p = (await bench.problems()).problems || [];
    ctx.expect(p.some((x) => x.subject?.id === 'sanderGate' && /isn't wired to a board/.test(x.text)), "the app says the sander's gate isn't wired to a board");
    await ctx.step('switch off the sander', async () => { bench.power('sander', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    await ctx.step('the saw works meanwhile', async () => { bench.power('saw', 600); await bench.until(async () => (await bench.status()).actuators.sawGate === 'open' && bench.plugs.collector.on); });
    ctx.expect(bench.plugs.collector.on, 'the saw gets suction');
    await ctx.step('switch off the saw', async () => { bench.power('saw', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    await ctx.step("wire the sander's gate to the serial board as servo 5", () => ctx.save(moveGate(ctx.doc, 'sanderGate', BOARDS.pwmB, { bus: 5 })));
    await ctx.step('switch on the sander', async () => { bench.power('sander', 600); await bench.until(async () => (await bench.status()).actuators.sanderGate === 'open' && bench.plugs.collector.on); });
    ctx.expect(bench.nodes[BOARDS.pwmB].gateAt.sanderGate === 'open', 'the sander gets suction again');
    await ctx.step('switch off the sander', async () => { bench.power('sander', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    ctx.expect(!((await bench.problems()).problems || []).some((x) => x.subject?.id === 'sanderGate'), "nothing is left said about the sander's gate");
  },
};
