#!/usr/bin/env node
// check-docs.js — the cheap documentation checks, so the rules in CLAUDE.md ("Keeping it current") fail a build
// instead of waiting for an audit. Written 2026-10-18, after one.
//
//   1. Every relative link in a CURRENT doc (.md / .html outside the two archive folders) points at a file that exists.
//   2. Every archived doc says so: a markdown file in docs/archive/ opens with "> **Archived", and an archived page
//      (docs/archive/*.html, docs/mockups/archived/*.html) carries an "archived" banner. A history page that looks
//      current is the trap the archive exists to avoid.
//   3. CLAUDE.md stays small: the root under ROOT_BUDGET lines, each directory's under DIR_BUDGET. Every session loads
//      them, and detail that matters in one directory belongs in that directory's file.
//
// Links inside archived pages are not checked: they are history, and some point at files deleted long ago.
// Run: `npm run docs:check` (tools/). Exits non-zero with one line per problem.

'use strict';
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const ROOT_BUDGET = 200;
const DIR_BUDGET = 100;
const ARCHIVES = ['docs/archive/', 'docs/mockups/archived/'];

const files = execFileSync('git', ['ls-files', '-co', '--exclude-standard'], { cwd: ROOT, encoding: 'utf8' })
  .split('\n').filter((f) => /\.(md|html)$/.test(f) && !f.includes('node_modules/') && fs.existsSync(path.join(ROOT, f)));
const archived = (f) => ARCHIVES.some((a) => f.startsWith(a));
const problems = [];

// 1. links
const LINK = /(?:\]\(|href="|src=")([^)"#\s]+)/g;
for (const f of files.filter((f) => !archived(f))) {
  const text = fs.readFileSync(path.join(ROOT, f), 'utf8');
  for (const m of text.matchAll(LINK)) {
    const target = m[1];
    if (/^[a-z]+:/i.test(target) || target.startsWith('/') || target.includes('${') || target.startsWith('{')) continue;
    if (!fs.existsSync(path.resolve(ROOT, path.dirname(f), target))) problems.push(`${f}: link to a missing file: ${target}`);
  }
}

// 2. archive banners
for (const f of files.filter(archived)) {
  if (path.basename(f) === 'README.md') continue;
  const text = fs.readFileSync(path.join(ROOT, f), 'utf8');
  if (f.endsWith('.md') && !text.startsWith('> **Archived')) problems.push(`${f}: archived, but does not open with "> **Archived <date>"`);
  if (f.endsWith('.html') && !/archived/i.test(text)) problems.push(`${f}: archived, but carries no "archived" banner`);
}

// 3. CLAUDE.md budgets
for (const f of files.filter((f) => path.basename(f) === 'CLAUDE.md')) {
  const lines = fs.readFileSync(path.join(ROOT, f), 'utf8').split('\n').length;
  const budget = f === 'CLAUDE.md' ? ROOT_BUDGET : DIR_BUDGET;
  if (lines > budget) problems.push(`${f}: ${lines} lines, over its budget of ${budget} — move detail to a directory's CLAUDE.md, or history to TODO/DONE.md`);
}

if (problems.length) {
  console.error(problems.join('\n'));
  console.error(`\n${problems.length} documentation problem(s).`);
  process.exit(1);
}
console.log(`docs ok — ${files.filter((f) => !archived(f)).length} current docs, ${files.filter(archived).length} archived`);
