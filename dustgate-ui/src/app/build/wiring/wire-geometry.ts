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

import { CELL, PAD } from '../routing/geometry';

// ── board metrics ────────────────────────────────────────────────────────────
/** A board is drawn as the module it is: a body with a row of ports on its
 *  underside. One cell wide, so it occupies a single grid square like any piece. */
/** Deliberately narrower than a CELL (108) so two boards on neighbouring cells have
 *  air between them instead of sharing an edge and reading as one module. */
export const BOARD_W = 96;
export const BOARD_H = 52;
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

/** Height of the board rail — the band above the grid where every board lives.
 *  Drawn in NEGATIVE y, so the cell grid below keeps the coordinates it always had
 *  and no saved layout has to move. */
export const RAIL_H = 76;
/** Pitch of a board slot along the rail: BOARD_W plus air, so two boards can't
 *  share an edge however you order them. */
export const BOARD_SLOT = 124;
/** Width of the "+ Find boards" chip, which is pinned to the rail's right end. */
export const FIND_W = 128;

/** Left edge of slot 0, leaving the rail a caption column: the boards used to start
 *  at PAD and paint straight over the BOARDS label, which is drawn before them. */
export const RAIL_X0 = PAD + 98;

/** Centre of the nth slot in the rail. Boards are one-dimensional now — the rail
 *  is above every gate, so a cable can only ever leave a port downward. */
export function railSlot(slot: number): Pt {
  return { x: RAIL_X0 + slot * BOARD_SLOT, y: -RAIL_H / 2 };
}
/** Which slot a point falls in, for a drag. */
export function slotAt(x: number): number {
  return Math.max(0, Math.round((x - RAIL_X0) / BOARD_SLOT));
}

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
export function cableRun(from: Pt, to: Pt, rank: number, bias = 0, clear = 0): Pt[] {
  if (Math.abs(from.x - to.x) < 0.5) return [from, to];      // straight drop

  // Gate ABOVE the board. Unreachable while every board sits in the rail, which is
  // above the whole grid — kept because it is the correct answer if a board is ever
  // allowed off the rail, and because it is what makes the port strip's side an
  // invariant rather than a coincidence. The cable still leaves downward, drops
  // clear of the board, runs across, then climbs.
  if (to.y < from.y) {
    const lane = from.y + clear + LANE_GAP + rank * LANE_STEP + bias;
    return [from, { x: from.x, y: lane }, { x: to.x, y: lane }, to];
  }

  // Gate below: run high, then drop. The floor is far enough off the port that the
  // cable is clear of the board before it turns, so you can still see which port it
  // left from; the ceiling keeps it off the gate's body if the two are close.
  const floor = from.y + PORT_STUB;
  const ceil = to.y - clear - LANE_GAP;
  let lane = floor + rank * LANE_STEP + bias;
  // Squeezed: give up height rather than clearance — a lane that turns close to the
  // board is untidy, a lane inside the gate's box is the bug.
  if (lane > ceil) lane = ceil;
  // Unless there is no room at all, in which case take the plain dog-leg and let the
  // pieces sort themselves out.
  if (lane <= from.y) lane = (from.y + to.y) / 2;
  return [from, { x: from.x, y: lane }, { x: to.x, y: lane }, to];
}

/** Lane order for the cables off one board: rank 0 is the LONGEST horizontal
 *  travel, so it takes the highest lane and shorter runs nest below it. */
export function rankByTravel<T>(items: T[], span: (t: T) => number): Map<T, number> {
  const out = new Map<T, number>();
  [...items].sort((a, b) => span(b) - span(a)).forEach((t, i) => out.set(t, i));
  return out;
}

// ── crossings ────────────────────────────────────────────────────────────────
type Seg = readonly [Pt, Pt];
const isH = (s: Seg) => Math.abs(s[0].y - s[1].y) < 0.5;

export function segmentsOf(pts: readonly Pt[]): Seg[] {
  const out: Seg[] = [];
  for (let i = 0; i < pts.length - 1; i++) out.push([pts[i], pts[i + 1]] as const);
  return out;
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
 * board on the rail is on the same network by definition, so the curve drew a fact
 * that is never not true while adding a line that crossed the shop. The network's
 * name in the rail says the same thing in the space it already occupies. */
