// ── Guessing which plug belongs to which machine, by name ────────────────────
//
// A Shelly arrives on the network calling itself whatever the person who set it
// up typed into the Shelly app — "TableSaw", "table saw", "Table-Saw", or
// "shellyplug-s-8f21c4" if they typed nothing. The canvas already knows the
// machine is called "Table Saw". Matching those two strings is worth doing
// because the alternative is the user pairing six plugs by hand, one at a time,
// walking to each machine to switch it on.
//
// IT ASSIGNS. "Match by name" writes the pairing straight onto the machine — it
// no longer offers a per-row suggestion waiting for a tick. That inverts which
// error is cheap: a wrong guess puts a wrong plug on a real machine and will be
// found when a gate doesn't open, while a missed guess costs one drag. Every
// tuning decision below follows from that, and none of it would be right for a
// version that suggested.
//
// Extracted from the component so it can be tested without a DOM. The design is
// docs/mockups/outlet-docks-multi-system.html.

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
 * 0.62 — and the interesting fact is that no threshold is SAFE, because the two
 * populations overlap. Measured over same-shop pairs:
 *
 *   TRUE   TableSaw/Table Saw 1.00 · Router/Router Table 0.94 ·
 *          Table Sw/Table Saw 0.77 · Jointr/Jointer 0.73 · planr/Planer 0.67 ·
 *          tabel saw/Table Saw 0.57                                    ← worst
 *   FALSE  Drum Sander/Disc Sander 0.56 · Belt Sander/Drum Sander 0.56  ← best
 *          Table Saw/Router table 0.47 · Bandsaw/Table Saw 0.31 ·
 *          Table Saw/Miter Saw 0.29 · Drill Press/Drum Sander 0.11
 *
 * 0.571 against 0.556 is not a gap. An earlier version of this file set the bar
 * at 0.45 claiming a wide empty band — measured against a false set that was too
 * easy (unrelated names, which are obviously far apart). Re-measured against
 * pairs from ONE shop, the hazard is plain: "Table Saw" scores 0.471 against
 * "Router table", so a 0.45 bar would hand the saw's plug to the router table
 * whenever the saw's own machine was already paired. Two machines sharing a word
 * is the normal case in a workshop, not the exotic one.
 *
 * So the bar sits ABOVE every false pair we can measure, and the cost is paid on
 * the other side: "tabel saw" (0.571) no longer matches and gets dragged across
 * by hand. That trade is only right because "Match by name" ASSIGNS rather than
 * suggesting — a wrong guess is a wrong plug on a real machine, a missed guess is
 * one drag. Back when it suggested and waited for a tick, the cheaper error was
 * the other one.
 */
export const MATCH_MIN = 0.62;

/**
 * How much better the best candidate has to be than the runner-up.
 *
 * A threshold alone cannot separate "Drum Sander" from "Disc Sander": a plug
 * called "Sander" scores 0.88 against BOTH, by containment. Confidence about a
 * name says nothing about which of two similar machines owns it, so an ambiguous
 * best answer is not an answer — it stays in the tray to be dragged.
 */
export const MATCH_MARGIN = 0.08;

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
 * Then two filters, because this ASSIGNS rather than suggesting: the score has to
 * clear MATCH_MIN, and it has to beat the plug's own runner-up by MATCH_MARGIN.
 * Anything left over stays in the tray to be dragged.
 */
export function matchAll(
  outlets: readonly PairInput[], machines: readonly PairInput[], takenMachines: ReadonlySet<string> = new Set(),
): { outletId: string; machineId: string; score: number }[] {
  const pairs: { outletId: string; machineId: string; score: number }[] = [];
  for (const o of outlets) {
    // Every candidate for THIS plug, so the runner-up is knowable.
    const scored = machines
      .filter(m => !takenMachines.has(m.id))
      .map(m => ({ machineId: m.id, score: nameScore(o.name, m.name) }))
      .sort((a, b) => b.score - a.score);
    const best = scored[0];
    if (!best || best.score < MATCH_MIN) continue;
    const runnerUp = scored[1]?.score ?? 0;
    if (best.score - runnerUp < MATCH_MARGIN) continue;   // ambiguous — leave it
    pairs.push({ outletId: o.id, machineId: best.machineId, score: best.score });
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
