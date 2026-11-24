// The trap from 2026-11-23: a board reflashed from PWM to serial, with its gate still drawn as a PWM servo on it.
// What must hold: the rest of the shop keeps working, the app says in plain words what's wrong and what to do, and
// re-wiring the gate as a serial servo fixes it — no reset, no second reflash.
'use strict';
const { jeffsShop, BOARDS } = require('./_shop');
const { moveGate, boardDrives } = require('./_edit');

module.exports = {
  title: "03 Reflash the sander's board from PWM to serial, then re-wire its gate",
  async run(ctx) {
    const { bench } = ctx;
    await jeffsShop(ctx);
    await ctx.step("reflash the sander's board as a serial board (same name)", async () => {
      ctx.allowRefused(['sanderGate']);
      bench.nodes[BOARDS.pwmB].reflash('bus');
      await bench.waitOnline([BOARDS.pwmB]);
    });
    await ctx.step('switch on the sander: its gate is still drawn as a PWM servo', async () => {
      bench.power('sander', 600);
      await ctx.sleep(3000);
    });
    const p = (await bench.problems()).problems || [];
    ctx.expect(p.some((x) => x.subject?.id === 'sanderGate' && /Re-wire it on the canvas/.test(x.text)), 'the app says the sander gate needs re-wiring, and how');
    await ctx.step('switch off the sander', async () => { bench.power('sander', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    await ctx.step('the saw still works meanwhile', async () => { bench.power('saw', 600); await bench.until(async () => (await bench.status()).actuators.sawGate === 'open' && bench.plugs.collector.on); });
    ctx.expect(bench.plugs.collector.on, 'the saw still gets suction');
    await ctx.step('switch off the saw', async () => { bench.power('saw', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
    await ctx.step("re-wire the sander's gate as serial servo 5 on that board", async () => {
      ctx.allowRefused([]);
      bench.nodes[BOARDS.pwmB].refused.length = 0;
      await ctx.save(moveGate(boardDrives(ctx.doc, BOARDS.pwmB, 'bus'), 'sanderGate', BOARDS.pwmB, { bus: 5 }));
    });
    await ctx.step('switch on the sander', async () => { bench.power('sander', 600); await bench.until(async () => (await bench.status()).actuators.sanderGate === 'open' && bench.plugs.collector.on); });
    ctx.expect(bench.nodes[BOARDS.pwmB].gateAt.sanderGate === 'open', 'the sander gets suction from its new serial servo');
    await ctx.step('switch off the sander', async () => { bench.power('sander', 0); await bench.until(() => !bench.plugs.collector.on, 15000); });
  },
};
