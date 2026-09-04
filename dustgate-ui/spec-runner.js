#!/usr/bin/env node
// spec-runner.js — runs the UI's compiled test suites under plain node.
//
// Two jobs, both small:
//
//   1. Resolve the tsconfig path aliases at RUNTIME. tsc uses `paths` for type
//      resolution but does not rewrite import specifiers on emit, so the compiled
//      output still says require('@shop'). Rather than add a bundler or a
//      module-alias dependency for three names, we patch the resolver directly —
//      this is the whole of what those packages do.
//
//   2. Run every suite in one process and report a combined result, so `npm test`
//      is one command with one exit code.
//
// Suites are plain modules that throw on failure and print their own tally, the
// same contract as shared/device-model/*.test.js.

'use strict';

const Module = require('module');
const path = require('path');
const { execFileSync } = require('child_process');

const ROOT = __dirname;
const OUT = path.join(ROOT, '.spec-test');

// ── 1. alias resolution ─────────────────────────────────────────────────────
// Mirrors the "paths" block in tsconfig.json. Keep the two in step: a name here
// that isn't there type-checks and then fails at run time, and the reverse
// compiles and then fails at run time — both loud, neither silent.
const ALIASES = {
  '@topology': '../shared/device-model/topology.js',
  '@topology-device': '../shared/device-model/topology-device.js',
  '@shop': '../shared/device-model/shop.js',
  '@device-model': '../shared/device-model/device-model.js',
};

const origResolve = Module._resolveFilename;
Module._resolveFilename = function (request, ...rest) {
  const hit = ALIASES[request];
  if (hit) return origResolve.call(this, path.resolve(ROOT, hit), ...rest);
  return origResolve.call(this, request, ...rest);
};

// ── 2. compile, then run ────────────────────────────────────────────────────
const SUITES = [
  ['services/shop-doc.spec.js', 'shop-doc'],
  ['services/shop-ready.spec.js', 'shop-ready'],
  ['services/wip-message.spec.js', 'wip-message'],
  ['gates/selector-types.spec.js', 'selector-types'],
  ['tools/outlet-match.spec.js', 'outlet-match'],
  ['tools/deep-link.spec.js', 'deep-link'],
  ['build/plug-label.spec.js', 'plug-label'],
  ['boards/board-drives.spec.js', 'board-drives'],
  ['build-stamp.spec.js', 'build-stamp'],
];

try {
  execFileSync(path.join(ROOT, 'node_modules/.bin/tsc'), ['-p', 'tsconfig.spec.json'],
    { cwd: ROOT, stdio: 'inherit' });
} catch {
  console.error('\nspec build FAILED — nothing ran.');
  process.exit(2);
}

let failed = 0;
for (const [file, name] of SUITES) {
  console.log(`\n── ${name} ${'─'.repeat(Math.max(0, 60 - name.length))}`);
  try {
    require(path.join(OUT, 'app', file));
  } catch (e) {
    failed++;
    console.error(`  suite threw: ${e && e.message ? e.message : e}`);
  }
}

// Each suite calls process.exitCode on its own failures; this only adds the
// "a whole suite blew up" case on top.
if (failed) process.exitCode = 1;
console.log(failed ? `\n${failed} suite(s) failed to run.` : '');
