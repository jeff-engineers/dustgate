/** Wiring-layer geometry: where a board sits, where its ports are, and how the
 *  cable between a port and a gate is drawn.
 *
 *  Deliberately separate from routing/ — a cable is not a duct and the two want
 *  opposite things. Ducts pay heavily to share a lane or cross, because two ducts
 *  in one place is a physical impossibility; cable bundles along a wall and crosses
 *  ductwork all day. So rather than a second cost profile for A*, wires get their
 *  own small router: one lane per cable, nested so they don't cross each other.
 *
 *  Pure functions, no Angular, no DOM — same discipline as routing/geometry.ts, so
 *  the whole thing is testable with `npm run test:wiring`.
 */

import { type Box, type Pt as GeomPt, BOARD_H, BOARD_W, CELL, PAD, segBoxHit } from '../routing/geometry';

// ── board metrics ────────────────────────────────────────────────────────────
/** A board is drawn as the module it is: a body with a row of ports on its
 *  underside. One cell wide, so it occupies a single grid square like any piece.
 *
 *  Its BOX lives in routing/geometry with every other glyph's, because a board on
 *  the grid is something ducts have to steer around — re-exported here so the
 *  wiring layer still has one import for everything about a board. */
export { BOARD_H, BOARD_W };
/** Servo channels + stepper drivers one ESP32 can drive. Mirrors MAX_SERVOS_PER_HOST
 *  and MAX_LINEAR_PER_HOST in shared/device-model/topology.js — the PWM bank and the
 *  single stepper driver. The port strip IS this budget, drawn. */
export const SERVO_PORTS = 4;
export const PORT_PITCH = 18;
export const PORT_W = 12;
export const PORT_H = 7;
export const STEPPER_W = 16;
/** Port strip straddles the board's bottom edge. */
export const PORT_DY = 22;

// ── cable metrics ────────────────────────────────────────────────────────────
export const CORNER_R = 7;      // matches the duct's round linejoin at its own weight
export const HOP_R = 4.5;       // the schematic "these two don't touch" bump
/** A straight run out of the port before the cable is allowed to turn. Without it a
 *  cable whose lane happens to sit close to the board turns while still touching the
 *  board edge, and you can't tell which of five ports it left from. */
export const PORT_STUB = 18;
/** Minimum clearance between a lane and the gate it serves — the last resort when
 *  a high lane won't fit — then one lane per additional cable. */
export const LANE_GAP = 24;
export const LANE_STEP = 14;
/** How far off a gate's side its servo tab sits. */
export const TAB_DX = 30;
export const TAB_W = 16;
export const TAB_H = 14;

export interface Pt { x: number; y: number; }
export interface Cell { col: number; row: number; }

/* The board rail — a band in NEGATIVE y above the grid, with a slot pitch, a pin
 * translate and a scrim under it — was deleted on 2026-08-16. It existed because a
 * brain placed BELOW the gates it drives routes its cables badly, and it paid for
 * that with the other end of every cable sitting somewhere you can't see the moment
 * you scroll. Boards are ordinary pieces on the grid now and the fix for the
 * original problem is a better DEFAULT placement — top-right of the system you're
 * working in. See docs/boards-on-canvas-plan.md. */

/** Where port `ch` sits on a board centred at `c`. Channel 0..3 are servo; passing
 *  SERVO_PORTS gives the stepper port. Returns the port's CENTRE, and its bottom
 *  edge is where a cable leaves. */
export function portPos(c: Pt, ch: number): Pt {
  const first = -((SERVO_PORTS) * PORT_PITCH) / 2;         // centre the 5-port strip
  return { x: c.x + first + ch * PORT_PITCH, y: c.y + PORT_DY };
}
export function portWidth(ch: number): number { return ch >= SERVO_PORTS ? STEPPER_W : PORT_W; }
/**
 * Where a cable leaves the board: ALWAYS the underside of its port.
 *
 * An earlier version flipped to the top edge when the gate was above, to stop the
 * cable crossing its own board. It did stop that, but a cable appearing at the top
 * of a board whose ports are all on the bottom gives you nothing to trace it back
 * to — you could see the wire and not the port. Cables now always emerge from the
 * strip itself and cableRun detours them clear instead.
 */
export function portExit(c: Pt, ch: number): Pt {
  const p = portPos(c, ch);
  return { x: p.x, y: p.y + PORT_H / 2 };
}

/** A gate's servo tab: the electrical end of a blast gate, on its right-hand side. */
export function tabPos(gate: Pt, halfW: number): Pt {
  return { x: gate.x + Math.max(halfW, 0) + TAB_DX - 22, y: gate.y };
}

// ── the cable run ────────────────────────────────────────────────────────────
/**
 * One cable, port → tab, as a 2-bend orthogonal run: out of the port, along a
 * lane, into the tab.
 *
 * The lane sits HIGH — a short stub below the port and no further. Cables used to
 * hug the gate row instead, which meant every run crossed the shop at the height
 * where all the ductwork and every gate box already is. Up near the rail the band
 * is empty, so the cables share it, travel there, and drop straight down onto
 * their tabs. It also keeps the descent clear of the gate bodies: the tab is off
 * the gate's right edge, so a drop that starts high still only ever comes down
 * beside the piece it serves.
 *
 * `rank` is what stops cables crossing each other: 0 is the LONGEST run and takes
 * the highest lane, with shorter runs nested beneath it. Nesting has to go this
 * way round — a long cable on a lower lane would be crossed by every shorter cable
 * dropping past it. (With low lanes the sense was the opposite, and getting it
 * backwards was not cosmetic: it put two of the five cables in the reference shop
 * through each other.)
 *
 * `clear` is the gate's half-height. The tab sits at the gate's CENTRE, so a lane
 * measured only from the tab lands inside the body; it only bites now in the
 * cramped case, where the lane is pushed back down toward the gate.
 */
export type SegCost = (a: Pt, b: Pt) => number;

/** What a cable pays to cross each kind of thing in its way, for pickDrop()'s
 *  choice of column. Exported so the weighting is asserted directly rather than
 *  only observed through whichever path a full route happens to settle on.
 *
 *  A device body is expensive: a wire drawn over a gate or through a tool reads as
 *  going INTO it, and the piece it actually serves is somewhere else entirely. A
 *  duct is cheap — cable crosses ductwork all day in a real shop, and the crossing
 *  is drawn plainly rather than hopped (see cablePath) — so it costs just enough to
 *  break a tie, never enough to send a wire round the shop to dodge one. A
 *  previously-drawn CABLE costs more than a duct: the per-board lane nesting in
 *  cableRun already keeps one board's own runs apart, this is for the crossings it
 *  can't reach — two boards' bundles, or two runs to opposite corners of the
 *  canvas — which used to get a cosmetic hop and nothing else, so the router had no
 *  reason to prefer the tidier column when one was available.
 *
 *  `share` is the dearest of the three, and it is not a crossing at all: it is two
 *  cables running down the SAME corridor, one drawn over the other. Crossings are
 *  legible — the hop says the two don't touch — but a shared corridor draws two
 *  cables as one line, and the second one has simply disappeared until it peels
 *  off somewhere further down. That is worse than either cable taking a longer way
 *  round, so it is priced above a crossing and well clear of the per-cell penalty
 *  that keeps a drop near its own tab. Wires only: a cable sharing a duct's column
 *  is a different (and much rarer) kind of untidy, and pricing it here would send
 *  cables round the shop to dodge the trunk they are meant to run beside. */
export const CROSSING_COST = { box: 100, duct: 1, wire: 8, share: 40 } as const;

/** A SegCost that charges CROSSING_COST for each kind of thing `[a, b]` crosses.
 *  `ducts` is a set of polylines (their own individual segments are checked);
 *  `drawn` is already-flattened wire segments. */
export function crossingCost(boxes: readonly Box[], ducts: readonly GeomPt[][], drawn: readonly Seg[] = []): SegCost {
  return (a, b) => {
    let c = 0;
    for (const box of boxes) if (segBoxHit(a, b, box)) c += CROSSING_COST.box;
    for (const pts of ducts) {
      for (let i = 0; i < pts.length - 1; i++) {
        if (crossing([a, b] as const, [pts[i], pts[i + 1]] as const)) { c += CROSSING_COST.duct; break; }
      }
    }
    for (const seg of drawn) {
      if (crossing([a, b] as const, seg)) c += CROSSING_COST.wire;
      else if (sharesLane([a, b] as const, seg)) c += CROSSING_COST.share;
    }
    return c;
  };
}

/** Which vertical corridor a cable comes down in.
 *
 *  It used to be "the tab's own x, always", which is right until something stands
 *  between the port and the tab. Stacking the shop down the page made that the
 *  common case rather than the rare one: a gate three bands down is a long drop, and
 *  every piece in the bands above it is in the way. The reference shop had a cable
 *  descending straight through the main gate and the table saw.
 *
 *  So the drop is chosen instead of assumed. Candidates are the tab's own column
 *  first, then the GUTTERS between grid columns working outward — a gutter is the
 *  one vertical line a piece can't occupy, since a unit's body stops GATE_PAD past
 *  its last outlet and a tool is narrower still. Each is scored by what the caller
 *  says it costs to cross, plus a small penalty for straying from the tab, so a
 *  cable only leaves its own column when something is actually in it and comes back
 *  as soon as it can. */
function pickDrop(to: Pt, lane: number, cost: SegCost): number {
  const k = Math.floor((to.x - PAD) / CELL);
  const gutter = PAD + k * CELL + CELL / 2;
  const cands = [to.x];
  for (let i = 0; i < 8; i++) {
    cands.push(gutter + i * CELL);
    const left = gutter - (i + 1) * CELL;
    if (left > 0) cands.push(left);
  }
  let best = to.x, bestScore = Infinity;
  for (const x of cands) {
    const score = cost({ x, y: lane }, { x, y: to.y })      // the descent
                + cost({ x, y: to.y }, { x: to.x, y: to.y }) // the jog back to the tab
                + Math.abs(x - to.x) / CELL;                 // …and stay near it
    if (score < bestScore) { bestScore = score; best = x; }
  }
  return best;
}

/**
 * @param rank   0 is the longest run and takes the highest lane; see above.
 * @param lanes  how many cables are nesting in this band — the caller knows, and
 *               without it the spacing can only assume there is room for another
 *               LANE_STEP and collapse everything onto the ceiling when there
 *               isn't. Defaults to "this one is the last", which is the old
 *               behaviour for any caller that doesn't care.
 */
export function cableRun(from: Pt, to: Pt, rank: number, bias = 0, clear = 0, cost?: SegCost,
                         lanes = rank + 1): Pt[] {
  const price = cost ?? (() => 0);
  // Straight drop — but only if the column is actually empty; otherwise fall through
  // and let the lane logic route around whatever is standing in it.
  if (Math.abs(from.x - to.x) < 0.5 && price(from, to) === 0) return [from, to];

  // Gate ABOVE the board. Written while every board still sat in a rail over the
  // whole grid, where it was unreachable; boards stand on the grid now, so a brain
  // dragged below the gates it drives comes through here. The cable still leaves
  // downward — the port strip's side is an invariant, not a coincidence — drops
  // clear of the board, runs across, then climbs.
  if (to.y < from.y) {
    const lane = from.y + clear + LANE_GAP + rank * LANE_STEP + bias;
    return [from, { x: from.x, y: lane }, { x: to.x, y: lane }, to];
  }

  // Gate below: run high, then drop. The floor is far enough off the port that the
  // cable is clear of the board before it turns, so you can still see which port it
  // left from; the ceiling keeps it off the gate's body if the two are close.
  const floor = from.y + PORT_STUB + bias;
  const ceil = to.y - clear - LANE_GAP;
  // Nesting has to fit in the gap there IS, not the gap LANE_STEP assumes.
  //
  // This used to be `floor + rank * LANE_STEP`, clamped to the ceiling — and a
  // clamp is not a fallback, it is a collision: with the gates one row under
  // their board, every rank past the first computed a lane below the ceiling and
  // was pushed onto exactly the same line, so three cables were drawn as one
  // (the shop, 2026-08-22 — two of the six runs shared 378 units of lane). The
  // spacing compresses instead. It only ever compresses: a roomy drop still
  // steps by LANE_STEP, which is what keeps the lanes readable in the normal
  // case that this whole scheme was tuned for.
  const span = Math.max(0, ceil - floor);
  const step = lanes > 1 ? Math.min(LANE_STEP, span / (lanes - 1)) : LANE_STEP;
  let lane = floor + rank * step;
  // Still no room — the board is sitting on top of its gate. Give up height
  // rather than clearance: a lane that turns close to the board is untidy, a lane
  // inside the gate's box is the bug.
  if (lane > ceil) lane = ceil;
  // Unless there is no room at all, in which case take the plain dog-leg and let the
  // pieces sort themselves out.
  if (lane <= from.y) lane = (from.y + to.y) / 2;
  const drop = pickDrop(to, lane, price);
  if (Math.abs(drop - to.x) < 0.5) return [from, { x: from.x, y: lane }, { x: to.x, y: lane }, to];
  // Detoured: down a clear gutter, then in along the tab's own row.
  return [from, { x: from.x, y: lane }, { x: drop, y: lane }, { x: drop, y: to.y }, to];
}

/** Lane order for the cables off one board: rank 0 is the LONGEST horizontal
 *  travel, so it takes the highest lane and shorter runs nest below it. */
export function rankByTravel<T>(items: T[], span: (t: T) => number): Map<T, number> {
  const out = new Map<T, number>();
  [...items].sort((a, b) => span(b) - span(a)).forEach((t, i) => out.set(t, i));
  return out;
}

// ── crossings ────────────────────────────────────────────────────────────────
export type Seg = readonly [Pt, Pt];
const isH = (s: Seg) => Math.abs(s[0].y - s[1].y) < 0.5;

export function segmentsOf(pts: readonly Pt[]): Seg[] {
  const out: Seg[] = [];
  for (let i = 0; i < pts.length - 1; i++) out.push([pts[i], pts[i + 1]] as const);
  return out;
}

/**
 * Do two axis-aligned segments run down the SAME line and overlap along it?
 *
 * The failure this exists to catch: two cables off one board, both routed into the
 * same clear gutter, drawn one exactly over the other. Nothing in crossing() sees
 * it — they never intersect, they coincide — so it cost the router nothing and it
 * picked that column twice. On screen the second cable is invisible for the whole
 * shared stretch and then appears out of nowhere where it peels off.
 *
 * Meeting end-to-end is NOT overlapping: consecutive segments of one run share a
 * point by construction, and a cable that continues past where another one stopped
 * is drawing exactly what is there. So the shared stretch has to have length.
 */
export function sharesLane(a: Seg, b: Seg, eps = 0.5): boolean {
  const ah = isH(a), bh = isH(b);
  if (ah !== bh) return false;                                  // perpendicular
  const axis = (s: Seg) => (isH(s) ? s[0].y : s[0].x);          // the line it sits on
  if (Math.abs(axis(a) - axis(b)) > eps) return false;          // parallel, not collinear
  const span = (s: Seg) => (isH(s)
    ? [Math.min(s[0].x, s[1].x), Math.max(s[0].x, s[1].x)]
    : [Math.min(s[0].y, s[1].y), Math.max(s[0].y, s[1].y)]);
  const [a0, a1] = span(a), [b0, b1] = span(b);
  return Math.min(a1, b1) - Math.max(a0, b0) > eps;
}

/** Where two axis-aligned segments cross, or null. Touching endpoints don't count —
 *  a hop belongs where two cables pass, not where they meet. */
export function crossing(a: Seg, b: Seg): Pt | null {
  const h = isH(a) ? a : (isH(b) ? b : null);
  const v = isH(a) ? b : a;
  if (!h || isH(v)) return null;                              // parallel
  const y = h[0].y, x = v[0].x;
  const [x1, x2] = [h[0].x, h[1].x].sort((p, q) => p - q);
  const [y1, y2] = [v[0].y, v[1].y].sort((p, q) => p - q);
  if (x > x1 && x < x2 && y > y1 && y < y2) return { x, y };
  return null;
}

// ── path emitter ─────────────────────────────────────────────────────────────
const f = (n: number) => Math.round(n * 100) / 100;

/**
 * Path data for one cable: rounded corners, plus a hop wherever it crosses one of
 * `under` (the cables already drawn).
 *
 * Hops are for cable-over-CABLE only. A hop is the schematic mark for "these two
 * conductors do not connect", so it earns its place where a short would otherwise
 * be implied; a cable crossing a duct implies nothing electrical, and bumping every
 * one of those turns an ordinary run into a washboard.
 */
export function cablePath(pts: readonly Pt[], under: readonly Seg[] = []): string {
  const S = segmentsOf(pts);
  if (!S.length) return '';
  let d = '';

  for (let i = 0; i < S.length; i++) {
    const [a, b] = S[i];
    const dx = Math.sign(b.x - a.x), dy = Math.sign(b.y - a.y);
    // Corners eat into both neighbours, so a short segment gets a smaller radius
    // rather than a corner that overshoots its own line.
    const len = Math.abs(b.x - a.x) + Math.abs(b.y - a.y);
    const rIn  = i > 0 ? Math.min(CORNER_R, len / 2) : 0;
    const rOut = i < S.length - 1 ? Math.min(CORNER_R, len / 2) : 0;
    const start = { x: a.x + dx * rIn, y: a.y + dy * rIn };
    const end   = { x: b.x - dx * rOut, y: b.y - dy * rOut };
    if (i === 0) d += `M ${f(start.x)} ${f(start.y)}`;

    const along = (p: Pt) => (dx ? (p.x - start.x) * dx : (p.y - start.y) * dy);
    const runLen = along(end);
    const hops = under
      .map(u => crossing([a, b] as const, u))
      .filter((p): p is Pt => !!p)
      .filter(p => along(p) > HOP_R && along(p) < runLen - HOP_R)
      .sort((p, q) => along(p) - along(q));

    for (const h of hops) {
      const p1 = { x: h.x - dx * HOP_R, y: h.y - dy * HOP_R };
      const p2 = { x: h.x + dx * HOP_R, y: h.y + dy * HOP_R };
      // Horizontal runs bulge up, vertical runs bulge right — consistent enough
      // that the convention stops being noticed.
      const sweep = dx ? (dx > 0 ? 1 : 0) : (dy > 0 ? 1 : 0);
      d += ` L ${f(p1.x)} ${f(p1.y)} A ${HOP_R} ${HOP_R} 0 0 ${sweep} ${f(p2.x)} ${f(p2.y)}`;
    }
    d += ` L ${f(end.x)} ${f(end.y)}`;

    if (i < S.length - 1) {
      const [c, e] = S[i + 1];
      const ex = Math.sign(e.x - c.x), ey = Math.sign(e.y - c.y);
      const nextLen = Math.abs(e.x - c.x) + Math.abs(e.y - c.y);
      const r = Math.min(CORNER_R, len / 2, nextLen / 2);
      const after = { x: c.x + ex * r, y: c.y + ey * r };
      const turn = dx * ey - dy * ex;                        // screen coords, y down
      d += ` A ${f(r)} ${f(r)} 0 0 ${turn > 0 ? 1 : 0} ${f(after.x)} ${f(after.y)}`;
    }
  }
  return d;
}

/* The wireless hop between the primary and a secondary used to be drawn here, as a
 * dotted curve with a transport badge on it. It was removed deliberately: every
 * paired board is on the same network by definition, so the curve drew a fact that
 * is never not true while adding a line that crossed the shop. */
