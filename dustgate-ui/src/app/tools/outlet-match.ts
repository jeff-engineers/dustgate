// ── Guessing which plug belongs to which machine, by name ────────────────────
//
// A Shelly arrives on the network calling itself whatever the person who set it
// up typed into the Shelly app — "TableSaw", "table saw", "Table-Saw", or
// "shellyplug-s-8f21c4" if they typed nothing. The canvas already knows the
// machine is called "Table Saw". Matching those two strings is worth doing
// because the alternative is the user pairing six plugs by hand, one at a time,
// walking to each machine to switch it on.
//
// It is a SUGGESTION, never an action. Every match this file produces is shown
// with the plug's name next to the machine's and has to be accepted — see the
// dock's `suggest` state in build.component.ts. That is the whole reason a fuzzy
// score is acceptable here: being wrong costs a glance, not a mis-wired shop.
//
// Extracted from the component so it can be tested without a DOM. The design is
// docs/mockups/outlet-dock.html; this is that file's scorer, made typed.

/** Case, spaces, dashes and underscores are noise here — nobody means them. */
function norm(s: string): string {
  return String(s || '').toLowerCase().replace(/[^a-z0-9]+/g, '');
}

function bigrams(s: string): Map<string, number> {
  const out = new Map<string, number>();
  for (let i = 0; i < s.length - 1; i++) {
    const g = s.slice(i, i + 2);
    out.set(g, (out.get(g) ?? 0) + 1);
  }
  return out;
}

/** Dice coefficient over character bigrams: 0…1, symmetric, no tuning knobs. */
function dice(a: string, b: string): number {
  if (a.length < 2 || b.length < 2) return a === b ? 1 : 0;
  const A = bigrams(a), B = bigrams(b);
  let hits = 0, total = 0;
  for (const [g, n] of A) { total += n; const m = B.get(g); if (m) hits += Math.min(n, m); }
  for (const [, n] of B) total += n;
  return (2 * hits) / total;
}

/**
 * How much `outletName` looks like `machineName`, 0…1.
 *
 * The tiers above `dice` exist because the common cases are not fuzzy at all and
 * shouldn't be scored as if they were: "TableSaw" vs "Table Saw" is the same
 * string once normalised, and "Table Saw 240v" starts with it. Bigrams then
 * catch the rest ("tablesaw" ↔ "tabel saw").
 */
export function nameScore(outletName: string, machineName: string): number {
  const a = norm(outletName), b = norm(machineName);
  if (!a || !b) return 0;
  if (a === b) return 1;
  if (a.startsWith(b) || b.startsWith(a)) return 0.94;
  if (a.includes(b) || b.includes(a)) return 0.88;
  return dice(a, b);
}

/**
 * Below this, don't offer it at all.
 *
 * Set from measurement, not taste. Scoring realistic shop pairs leaves a wide
 * empty band, and 0.45 sits in the middle of it:
 *
 *   TRUE PAIRS      Jointer/Jointer 1.00 · Router/Router Table 0.94 ·
 *                   Sander/Drum Sander 0.88 · Table Sw/Table Saw 0.77 ·
 *                   planr/Planer 0.67 · tabel saw/Table Saw 0.57  ← worst
 *   FALSE PAIRS     Bandsaw/Table Saw 0.31 · Miter Saw/Table Saw 0.29  ← best
 *                   Lathe/Planer 0.22 · Planer/Jointer 0.18 ·
 *                   Drill Press/Drum Sander 0.11 · a bare shellyplug id 0.00
 *
 * The mockup used 0.62, which is inside the true-pair range: it rejects a single
 * transposed letter ("tabel saw"), the most common way a name is mistyped, for
 * no gain — the nearest false pair is still 0.28 away below.
 *
 * "Miter Saw" vs "Table Saw" is the pair that matters, because both are real
 * machines that can sit in one shop. It scores 0.29 and stays well clear.
 */
export const MATCH_MIN = 0.45;

export interface Nameable { id: string; name: string; }

/** The best unclaimed machine for one plug, or null if nothing is close enough. */
export function bestMatch<T extends Nameable>(
  outletName: string, candidates: readonly T[], taken: ReadonlySet<string> = new Set(),
): { item: T; score: number } | null {
  let best: T | null = null, bs = 0;
  for (const c of candidates) {
    if (taken.has(c.id)) continue;
    const s = nameScore(outletName, c.name);
    if (s > bs) { bs = s; best = c; }
  }
  return best && bs >= MATCH_MIN ? { item: best, score: bs } : null;
}

export interface PairInput { id: string; name: string; }

/**
 * Pair every free plug with a free machine in one pass — the "Match by name"
 * button.
 *
 * STRONGEST FIRST, globally, rather than walking the plugs in order. Greedy in
 * list order lets a 0.65 match consume a machine that a 0.98 match wanted, and
 * the user sees one obviously-right pairing silently lose to one they'd have
 * rejected. Sorting first costs nothing at these sizes and makes the result
 * independent of how the two lists happen to be ordered.
 *
 * Returns only the pairs it is confident about; anything unmatched is left for
 * the user to drag.
 */
export function matchAll(
  outlets: readonly PairInput[], machines: readonly PairInput[], takenMachines: ReadonlySet<string> = new Set(),
): { outletId: string; machineId: string; score: number }[] {
  const pairs: { outletId: string; machineId: string; score: number }[] = [];
  for (const o of outlets) {
    for (const m of machines) {
      if (takenMachines.has(m.id)) continue;
      const score = nameScore(o.name, m.name);
      if (score >= MATCH_MIN) pairs.push({ outletId: o.id, machineId: m.id, score });
    }
  }
  pairs.sort((a, b) => b.score - a.score);

  const usedO = new Set<string>(), usedM = new Set<string>(takenMachines);
  const out: { outletId: string; machineId: string; score: number }[] = [];
  for (const p of pairs) {
    if (usedO.has(p.outletId) || usedM.has(p.machineId)) continue;
    usedO.add(p.outletId); usedM.add(p.machineId);
    out.push(p);
  }
  return out;
}
