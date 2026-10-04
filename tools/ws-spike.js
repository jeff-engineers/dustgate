#!/usr/bin/env node
// =============================================================================
// ws-spike.js — what does ONE more WebSocket client cost the brain's internal RAM?
//
// Step 0 of docs/nodes-dial-the-brain-plan.md. The plan saves memory by replacing a
// task-per-node link (~10 KB each) with an inbound WebSocket (event-driven, on the
// existing async_tcp task). Whether that saves ~7 KB per node or ~2 is a fact about
// AsyncWebSocketClient on this board, and it can be measured before any firmware is
// written: open N clients to the brain's EXISTING /ws and read heap.internal.free
// from /api/info after each one.
//
//   node tools/ws-spike.js [--brain 192.168.86.46] [--count 9] [--settle-ms 1500]
//                          [--hold-ms 10000]
//
// Prints: the baseline (and its noise), the free-heap delta per connection, the total,
// the per-connection mean, and what comes back after they all close (a leak shows
// here). Run it on a QUIET brain — no browsers open, nodes linked or not but the same
// before and after — or the baseline moves under you; it samples the baseline five
// times so you can see how much it moves on its own.
//
// CAVEATS, stated rather than hidden:
//   * /ws is the APP's socket, and the brain pushes a status frame to every client on
//     it, so each connection also carries a queue the node link will not. This
//     OVERSTATES the cost of a node link a little. It is the right direction to be
//     wrong in for a go / no-go.
//   * Reading /api/info opens a short HTTP connection of its own; the script takes
//     each sample after the previous one has closed, and notes the baseline noise.
//   * It measures free internal heap, not what AsyncTCP's buffers cost when a node
//     is sending SENSE every few seconds. The first inbound-link build re-measures.
// =============================================================================
'use strict';

const http = require('http');
const { WebSocket } = require('ws');

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : d; };
const BRAIN  = arg('brain', '192.168.86.46');
const COUNT  = +arg('count', 9);
const SETTLE = +arg('settle-ms', 1500);
const HOLD   = +arg('hold-ms', 10000);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function info() {
  return new Promise((resolve, reject) => {
    const req = http.get({ host: BRAIN, port: 80, path: '/api/info', timeout: 8000, agent: false,
                           insecureHTTPParser: true }, (res) => {
      let b = ''; res.on('data', (c) => (b += c));
      res.on('end', () => { try { resolve(JSON.parse(b)); } catch (e) { reject(e); } });
    });
    req.on('timeout', () => req.destroy(new Error('the brain did not answer /api/info in 8 s')));
    req.on('error', reject);
  });
}

const free = (d) => (d.heap ? d.heap.internal.free : d.heapFree);
const largest = (d) => (d.heap ? d.heap.internal.largest : d.heapBlock);
const kb = (n) => (n / 1024).toFixed(1).padStart(6) + ' KB';

async function sample() { await sleep(SETTLE); const d = await info(); return { free: free(d), largest: largest(d), d }; }

(async () => {
  console.log(`brain ${BRAIN}: ${COUNT} WebSocket clients to /ws, ${SETTLE} ms settle between each\n`);

  const base = [];
  for (let i = 0; i < 5; i++) { base.push((await sample()).free); }
  const baseline = Math.round(base.reduce((a, b) => a + b, 0) / base.length);
  const noise = Math.max(...base) - Math.min(...base);
  const first = await info();
  console.log(`build ${first.build}, uptime ${first.uptimeSec} s`);
  console.log(`baseline free ${kb(baseline)}   (5 samples, spread ${noise} B — deltas under that are noise)\n`);

  const socks = [];
  let prev = baseline;
  const deltas = [];
  for (let i = 1; i <= COUNT; i++) {
    const ws = new WebSocket(`ws://${BRAIN}/ws`);
    ws.on('error', () => {});
    await new Promise((res) => { ws.once('open', res); ws.once('error', res); ws.once('close', res); });
    if (ws.readyState !== 1) { console.log(`  #${i}: could not connect — the brain refused or ran out`); break; }
    socks.push(ws);
    const s = await sample();
    const delta = prev - s.free;
    deltas.push(delta);
    console.log(`  #${String(i).padStart(2)}  free ${kb(s.free)}   this one cost ${String(delta).padStart(6)} B   largest block ${kb(s.largest)}`);
    prev = s.free;
  }

  if (socks.length) {
    console.log(`\nholding ${socks.length} open for ${HOLD / 1000} s (status pushes arrive on each)…`);
    await sleep(HOLD);
    const held = await sample();
    const total = baseline - held.free;
    const mean = total / socks.length;
    console.log(`after hold: free ${kb(held.free)}, ${total} B for ${socks.length} clients = ${Math.round(mean)} B each (${(mean / 1024).toFixed(2)} KB)`);
    console.log(`            largest block ${kb(held.largest)}   brain-reported min ever ${kb(held.d.heap ? held.d.heap.internal.min : held.d.heapMin)}`);
  }

  for (const ws of socks) ws.close();
  await sleep(4000);
  const after = await sample();
  console.log(`\nafter closing all: free ${kb(after.free)}   (${after.free - baseline >= 0 ? '+' : ''}${after.free - baseline} B vs baseline — a large negative number is a leak)\n`);

  if (socks.length) {
    const mean = (baseline - (prev)) / socks.length;
    console.log('READ IT AS:');
    console.log(`  per client ≈ ${(mean / 1024).toFixed(1)} KB   (the plan assumed ~2.5–3 KB per node; a node link today costs ~10 KB)`);
    console.log(mean <= 4096
      ? '  → at or under ~4 KB: the plan holds, go on to step 1.'
      : mean <= 6144
        ? '  → 4–6 KB: it still saves, but about half of what the plan says; re-read the memory table.'
        : '  → over 6 KB: the saving is small. Re-open option A (one shared link task) or a bigger board.');
  }
})().catch((e) => { console.error('✗', e.message); process.exit(1); });
