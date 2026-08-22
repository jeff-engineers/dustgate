// test-harness.ts — the tiny assertion harness the UI suites share.
//
// Same shape and same output as shared/device-model/*.test.js, on purpose: one
// reporting style across the repo means a CI log reads the same whether the
// failure came from the model, the firmware or here.
//
// It does NOT call process.exit() the way the standalone model suites do —
// spec-runner.js loads several suites into one process, and an exit in the first
// one would silently skip the rest. It sets process.exitCode instead, which is
// the same outcome for CI and lets every suite have its say.

export interface Result { name: string; ok: boolean; detail: string }

export function suite() {
  const results: Result[] = [];

  const check = (name: string, cond: unknown, detail = ''): void => {
    results.push({ name, ok: !!cond, detail });
  };

  /** Deep-equality by JSON. Enough for plain data, and it prints the diff for free. */
  const eq = (name: string, got: unknown, want: unknown): void =>
    check(name, JSON.stringify(got) === JSON.stringify(want),
      `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

  const report = (): void => {
    let failed = 0;
    for (const r of results) {
      if (!r.ok) failed++;
      console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok ? '' : `  — ${r.detail}`}`);
    }
    console.log(`\n  ${results.length - failed}/${results.length} passed`);
    if (failed) process.exitCode = 1;
  };

  return { check, eq, report };
}
