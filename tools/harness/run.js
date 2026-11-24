#!/usr/bin/env node
// run.js — run the shop scenarios: `node harness/run.js` for all of them, `node harness/run.js rearrange` for those whose
// file name contains a word. Each scenario gets a fresh brain, fresh boards and fresh plugs (bench.js).
//
// Every step is followed by the invariants (invariants.js), given a few seconds to come true — a gate takes time to
// move and make-before-break opens before it closes — and then held to them. Whatever the brain raises as a problem is
// printed as the app would show it, because the words are half of what a scenario tests: a problem with no plain
// "what to do" is a dead end for a woodworker even when the shop is safe.
'use strict';
const fs = require('fs');
const path = require('path');
const { Bench, sleep } = require('./bench');
const { check } = require('./invariants');

const SETTLE_MS = Number(process.env.HARNESS_SETTLE_MS || 6000);

async function runOne(file) {
  const scenario = require(file);
  const bench = new Bench();
  const failures = [];
  let doc = null;
  let allowRefused = [];
  const seenProblems = new Set();

  const ctx = {
    bench,
    get doc() { return doc; },
    /** Save `d` as the app would, and make it what the invariants hold the shop to. Returns the brain's answer. */
    async save(d) {
      doc = JSON.parse(JSON.stringify(d));
      const r = await bench.save(doc);
      if (r.status >= 400) failures.push(`  save refused (${r.status}): ${r.text}`);
      return r;
    },
    /** Gates a step EXPECTS a board to refuse (a valve left on a board that can't drive it). */
    allowRefused(ids) { allowRefused = ids; },
    expect(cond, sentence) {
      if (!cond) { failures.push(`  expected: ${sentence}`); process.stdout.write(`      FAIL expected: ${sentence}\n`); }
      return !!cond;
    },
    /** The brain was told to forget the shop (a reset): nothing to hold it to until the next save. */
    forget() { doc = null; },
    async step(name, fn) {
      process.stdout.write(`  · ${name}\n`);
      const before = failures.length;
      await fn();
      let broken = [];
      const ok = await bench.until(async () => { broken = doc ? await check(bench, doc, { allowRefused }) : []; return !broken.length; }, SETTLE_MS, 200);
      if (!ok) for (const b of broken) failures.push(`  [${name}] ${b}`);
      const shown = doc ? ((await bench.problems()).problems || []) : [];
      for (const p of shown) {
        const k = `${p.code}|${p.text}`;
        if (seenProblems.has(k)) continue;
        seenProblems.add(k);
        process.stdout.write(`      the app says: ${p.severity === 'bad' ? '⛔' : '⚠'} ${p.text} [${p.code}]\n`);
      }
      if (failures.length > before) for (const f of failures.slice(before)) process.stdout.write(`      FAIL ${f.trim()}\n`);
    },
    sleep,
  };

  process.stdout.write(`\n${scenario.title}\n`);
  try {
    await scenario.run(ctx);
  } catch (e) {
    failures.push(`  crashed: ${e.stack || e.message}`);
    process.stdout.write(`      FAIL crashed: ${e.message}\n`);
  } finally {
    if (failures.length && process.env.HARNESS_VERBOSE) process.stdout.write(bench.log.slice(-40).map((l) => `      | ${l}`).join('\n') + '\n');
    await bench.close();
  }
  return failures;
}

(async () => {
  const words = process.argv.slice(2);
  const dir = path.join(__dirname, 'scenarios');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.js') && !f.startsWith('_')).sort()
    .filter((f) => !words.length || words.some((w) => f.includes(w)))
    .map((f) => path.join(dir, f));
  // A scenario that shows a bug not fixed yet says so (`knownBroken`, naming the TODO): it still runs and prints every
  // failure, but does not turn CI red. Once it passes, the runner says to take the flag off, so a fix can't go unnoticed.
  let failed = 0, known = 0, healed = 0;
  for (const f of files) {
    const fails = await runOne(f);
    const kb = require(f).knownBroken;
    if (fails.length && kb) { known++; process.stdout.write(`  ✗ ${fails.length} failure(s) — known: ${kb}\n`); }
    else if (fails.length) { failed++; process.stdout.write(`  ✗ ${fails.length} failure(s)\n`); }
    else if (kb) { healed++; process.stdout.write(`  ✓ — and it was marked known-broken: take the knownBroken flag off\n`); }
    else process.stdout.write('  ✓\n');
  }
  process.stdout.write(`\n${files.length - failed - known}/${files.length} scenarios passed` +
    (known ? `, ${known} known broken` : '') + (healed ? `, ${healed} fixed but still flagged` : '') + '\n');
  process.exitCode = failed || healed ? 1 : 0;
})();
