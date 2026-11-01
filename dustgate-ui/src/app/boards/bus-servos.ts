/**
 * A bus board's servos as the Boards page lists them (2026-11-01): by the GATE each one drives, never by number — the
 * number is the board's business, shown only in developer mode (jeff: "obscure the numbering, tie them to a valve").
 *
 * Built from two halves that can disagree, and the list says which: the layout (which gates are on this board, and which
 * servo number each holds) and the board's own BUS report (which numbers answer, and how hard each worked). A gate with no
 * number yet has no servo; a number that answers but no gate holds is a servo nobody has tied to a gate.
 *
 * Pure, so the spec drives it without Angular.
 */
import type { BusServoState } from '../services/bus-types';
import type { ConfigurableSelector } from '../gates/selector-types';

export interface BusServoLine {
  /** The gate it drives; absent for a servo no gate holds. */
  gateId?: string;
  name: string;
  /** 'valve' | 'manifold' | 'slider', in a woodworker's words. */
  kind: string;
  /** Its servo number; absent while the gate has none. */
  bus?: number;
  /** What the board last said about it; absent when it has not reported this number. */
  state?: BusServoState;
  /** The peak load of its last move, percent; null with no move reported. */
  last: number | null;
  /** What it usually needs, percent: the median of its earlier moves. null until there are enough to say. */
  usual: number | null;
}

/** Moves before a "usual" is worth showing: fewer, and one odd move is half the median. */
export const USUAL_AFTER_MOVES = 3;

/** The median of every move but the last — so the last is compared against what came before it, not against itself. */
export function usualLoad(history: number[]): number | null {
  const before = history.slice(0, -1);
  if (before.length < USUAL_AFTER_MOVES) return null;
  const s = [...before].sort((a, b) => a - b);
  const m = Math.floor(s.length / 2);
  return s.length % 2 ? s[m] : Math.round((s[m - 1] + s[m]) / 2);
}

export function busNumberOf(sel: ConfigurableSelector): number | undefined {
  return sel.kind === 'linear' ? sel.linear?.bus : sel.servo?.bus;
}

const kindWord = (sel: ConfigurableSelector) =>
  sel.kind === 'linear' ? 'slider' : sel.kind === 'servoManifold' ? 'manifold' : 'ball valve';

/** The gates on this board, each with its servo's report, then any reported servo no gate holds. Chain order first when
 *  the board has chains (as wired), then the rest in layout order. */
export function busServoLines(gates: ConfigurableSelector[], reported: BusServoState[] = [], chains: string[][] = []): BusServoLine[] {
  const order = new Map<string, number>();
  chains.flat().forEach((id, i) => order.set(id, i));
  const sorted = [...gates].sort((a, b) => (order.get(a.id) ?? 1e6) - (order.get(b.id) ?? 1e6));
  const byNumber = new Map(reported.map((r) => [r.id, r]));
  const held = new Set<number>();
  const out: BusServoLine[] = sorted.map((g) => {
    const bus = busNumberOf(g);
    if (bus !== undefined) held.add(bus);
    const state = bus !== undefined ? byNumber.get(bus) : undefined;
    return {
      gateId: g.id, name: g.name || g.id, kind: kindWord(g), bus, state,
      last: state && state.moves > 0 ? state.peak : null,
      usual: state ? usualLoad(state.history) : null,
    };
  });
  for (const r of reported) {
    if (held.has(r.id)) continue;
    out.push({ name: 'A servo not tied to a gate', kind: 'servo', bus: r.id, state: r,
               last: r.moves > 0 ? r.peak : null, usual: usualLoad(r.history) });
  }
  return out;
}
