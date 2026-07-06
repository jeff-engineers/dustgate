#!/usr/bin/env node
// eval.js — DustGate setup agent evaluation harness.
//
// Runs scripted user-turn sequences against the real Anthropic API, executes
// tool calls in-process, then checks assertions about which tools were called,
// in what order, and the resulting device state.
//
// Usage:
//   cd tools && node eval/eval.js
//   node eval/eval.js --scenario 01-happy-path   # run one scenario by name prefix
//   node eval/eval.js --verbose                   # show full tool call trace
//
// Requires ANTHROPIC_KEY in tools/.env

'use strict';

try { require('dotenv').config({ path: require('path').join(__dirname, '../.env') }); }
catch { /* dotenv not installed — run: cd tools && npm install */ }

const https    = require('https');
const fs       = require('fs');
const path     = require('path');
const { MockState }               = require('./mock-state');
const { SYSTEM_PROMPT, TOOLS }    = require('./agent-schema');

const ANT_KEY = process.env.ANTHROPIC_KEY;
if (!ANT_KEY) {
  console.error('\n  ✗  ANTHROPIC_KEY not set. Add it to tools/.env\n');
  process.exit(1);
}

// ── CLI args ──────────────────────────────────────────────────────────────────
const args     = process.argv.slice(2);
const verbose  = args.includes('--verbose') || args.includes('-v');
const filterArg = (() => {
  const i = args.indexOf('--scenario');
  return i >= 0 ? args[i + 1] : null;
})();

const MODEL = 'claude-haiku-4-5-20251001'; // fast + cheap for evals; swap to sonnet for thoroughness

// ── Anthropic API call ────────────────────────────────────────────────────────

function callAnthropic(body) {
  return new Promise((resolve, reject) => {
    const payload = JSON.stringify(body);
    const options = {
      hostname: 'api.anthropic.com',
      path:     '/v1/messages',
      method:   'POST',
      headers: {
        'Content-Type':      'application/json',
        'x-api-key':         ANT_KEY,
        'anthropic-version': '2023-06-01',
        'Content-Length':    Buffer.byteLength(payload),
      },
    };
    const req = https.request(options, res => {
      let data = '';
      res.on('data', d => data += d);
      res.on('end', () => {
        try {
          const parsed = JSON.parse(data);
          if (res.statusCode !== 200) {
            reject(new Error(`Anthropic ${res.statusCode}: ${parsed.error?.message ?? data}`));
          } else {
            resolve(parsed);
          }
        } catch {
          reject(new Error(`Failed to parse Anthropic response: ${data}`));
        }
      });
    });
    req.on('error', reject);
    req.write(payload);
    req.end();
  });
}

// ── Single-turn runner ────────────────────────────────────────────────────────
// Sends one user message, runs the agentic tool-use loop until end_turn,
// returns the list of tool calls made during this turn.

async function runTurn(history, userText, mockState) {
  history.push({ role: 'user', content: userText });

  const toolsCalled = [];
  let continueLoop  = true;

  while (continueLoop) {
    const response = await callAnthropic({
      model:      MODEL,
      max_tokens: 1024,
      system:     SYSTEM_PROMPT,
      tools:      TOOLS,
      messages:   history,
    });

    const stopReason = response.stop_reason;
    const content    = response.content ?? [];

    history.push({ role: 'assistant', content });

    const toolUseBlocks = content.filter(b => b.type === 'tool_use');

    if (stopReason === 'tool_use' && toolUseBlocks.length > 0) {
      const toolResults = [];

      for (const block of toolUseBlocks) {
        let result;
        let isError = false;
        try {
          result = mockState.execute(block.name, block.input ?? {});
          toolsCalled.push({ name: block.name, input: block.input ?? {} });
        } catch (e) {
          isError = true;
          result  = { error: e.message };
          toolsCalled.push({ name: block.name, input: block.input ?? {}, error: e.message });
        }

        if (verbose) {
          const inputStr = JSON.stringify(block.input ?? {});
          const resultStr = JSON.stringify(result).slice(0, 80);
          console.log(`    ⚙  ${block.name}(${inputStr}) → ${resultStr}${isError ? ' [ERROR]' : ''}`);
        }

        toolResults.push({
          type:        'tool_result',
          tool_use_id: block.id,
          content:     JSON.stringify(result),
          is_error:    isError,
        });
      }

      history.push({ role: 'user', content: toolResults });
      continueLoop = true;
    } else {
      continueLoop = false;
    }
  }

  return toolsCalled;
}

// ── Assertion checker ─────────────────────────────────────────────────────────

function checkAssertions(assertions, allToolsCalled, mockState) {
  const failures = [];
  const toolNames = allToolsCalled.map(t => t.name);

  // Required tools — each must appear at least once
  for (const required of (assertions.required_tools ?? [])) {
    if (!toolNames.includes(required)) {
      failures.push(`missing required tool: ${required}`);
    }
  }

  // Forbidden tools — none may appear
  for (const forbidden of (assertions.forbidden_tools ?? [])) {
    if (toolNames.includes(forbidden)) {
      failures.push(`forbidden tool was called: ${forbidden}`);
    }
  }

  // Ordering — [before, after]: before must appear before the last occurrence of after
  for (const [before, after] of (assertions.must_precede ?? [])) {
    const beforeIdx = toolNames.indexOf(before);
    const afterIdx  = toolNames.lastIndexOf(after);
    if (beforeIdx === -1 || afterIdx === -1) continue; // absence caught by required_tools
    if (beforeIdx > afterIdx) {
      failures.push(`ordering: ${before} must precede ${after}`);
    }
  }

  // Final device state
  const fs_check = assertions.final_state ?? {};
  if ('num_active_stops' in fs_check && mockState.numActiveStops !== fs_check.num_active_stops) {
    failures.push(`state: expected numActiveStops=${fs_check.num_active_stops}, got ${mockState.numActiveStops}`);
  }
  if ('homed' in fs_check && mockState.homed !== fs_check.homed) {
    failures.push(`state: expected homed=${fs_check.homed}, got ${mockState.homed}`);
  }
  if ('outlets_configured' in fs_check && mockState.outlets.length !== fs_check.outlets_configured) {
    failures.push(`state: expected ${fs_check.outlets_configured} outlet(s), got ${mockState.outlets.length}`);
  }

  return failures;
}

// ── Scenario runner ───────────────────────────────────────────────────────────

async function runScenario(scenario) {
  const history     = [];
  const mockState   = new MockState();
  const allToolsCalled = [];
  let totalTurnTime = 0;

  if (verbose) console.log(`\n  Turns:`);

  for (let i = 0; i < scenario.turns.length; i++) {
    const userText = scenario.turns[i];
    if (verbose) console.log(`\n  [${i + 1}] User: "${userText}"`);

    if (allToolsCalled.length > (scenario.assertions?.max_turns ?? 50)) {
      return {
        pass:    false,
        failures: [`exceeded max tool calls (${scenario.assertions?.max_turns ?? 50})`],
        toolsCalled:  allToolsCalled.map(t => t.name),
        finalState:   summariseState(mockState),
        durationMs:   totalTurnTime,
      };
    }

    const t0    = Date.now();
    const tools = await runTurn(history, userText, mockState);
    totalTurnTime += Date.now() - t0;
    allToolsCalled.push(...tools);
  }

  const failures = checkAssertions(scenario.assertions ?? {}, allToolsCalled, mockState);

  return {
    pass:       failures.length === 0,
    failures,
    toolsCalled:  allToolsCalled.map(t => t.name),
    finalState:   summariseState(mockState),
    durationMs:   totalTurnTime,
  };
}

function summariseState(s) {
  return {
    homed:           s.homed,
    numActiveStops:  s.numActiveStops,
    outletsConfigured: s.outlets.length,
    motorInverted:   s.motorInverted,
  };
}

// ── Load scenarios ────────────────────────────────────────────────────────────

function loadScenarios() {
  const dir = path.join(__dirname, 'scenarios');
  return fs.readdirSync(dir)
    .filter(f => f.endsWith('.json'))
    .filter(f => !filterArg || f.startsWith(filterArg))
    .sort()
    .map(f => {
      const scenario = JSON.parse(fs.readFileSync(path.join(dir, f), 'utf8'));
      scenario._file = f;
      return scenario;
    });
}

// ── Reporting ─────────────────────────────────────────────────────────────────

const PASS = '\x1b[32m✓ PASS\x1b[0m';
const FAIL = '\x1b[31m✗ FAIL\x1b[0m';

function printResult(scenario, result) {
  const icon  = result.pass ? PASS : FAIL;
  const secs  = (result.durationMs / 1000).toFixed(1);
  console.log(`\n${icon}  ${scenario.name}  (${secs}s)`);

  if (!result.pass) {
    for (const f of result.failures) console.log(`       ↳ ${f}`);
  }

  if (verbose || !result.pass) {
    console.log(`       tools: ${result.toolsCalled.join(' → ')}`);
    console.log(`       state: ${JSON.stringify(result.finalState)}`);
  }
}

function printSummary(results) {
  const total   = results.length;
  const passed  = results.filter(r => r.pass).length;
  const failed  = total - passed;
  const elapsed = results.reduce((s, r) => s + r.durationMs, 0);
  console.log('\n' + '─'.repeat(50));
  console.log(`  ${passed}/${total} passed  (${(elapsed / 1000).toFixed(1)}s total)`);
  if (failed > 0) console.log(`\x1b[31m  ${failed} failed\x1b[0m`);
  console.log('─'.repeat(50) + '\n');
}

// ── Main ──────────────────────────────────────────────────────────────────────

async function main() {
  const scenarios = loadScenarios();

  if (scenarios.length === 0) {
    console.error('No scenarios found' + (filterArg ? ` matching "${filterArg}"` : ''));
    process.exit(1);
  }

  console.log(`\nDustGate eval — model: ${MODEL} — ${scenarios.length} scenario(s)\n`);

  const results = [];
  for (const scenario of scenarios) {
    process.stdout.write(`  Running: ${scenario.name} …`);
    try {
      const result = await runScenario(scenario);
      process.stdout.write('\r');
      printResult(scenario, result);
      results.push(result);
    } catch (e) {
      process.stdout.write('\r');
      console.log(`${FAIL}  ${scenario.name}\n       ↳ exception: ${e.message}`);
      results.push({ pass: false, failures: [e.message], toolsCalled: [], finalState: {}, durationMs: 0 });
    }
  }

  printSummary(results);
  process.exit(results.every(r => r.pass) ? 0 : 1);
}

main().catch(e => { console.error(e); process.exit(1); });
