/** Shared board metrics and the pure geometry helpers the router and the component
 *  both need. This is the single source of truth for how big a glyph is and where
 *  its edges sit — build.component.ts imports these rather than keeping its own
 *  copies, so a duct can never be drawn to a different box than the one A* avoided.
 *
 *  No Angular in here, by design: the router is testable as plain TypeScript. */

export interface Pt { x: number; y: number; }
export interface Box { x0: number; y0: number; x1: number; y1: number; }

/** Grid pitch. One cell = one place a fitting can stand. */
export const CELL = 108;
/** Board origin offset, so column 0 isn't flush against the canvas edge. */
export const PAD = 64;
/** A* lattice spacing — half a cell, which is fine enough to slip a duct between
 *  two devices in adjacent cells and coarse enough to stay fast. */
export const LATTICE = CELL / 2;
/** Clearance every device footprint is inflated by before the router treats it as
 *  an obstacle, so ducts don't grease past a corner. */
export const CLEARANCE = 15;
/** Height of a unit (sliding gate / manifold) bar. */
export const UNIT_H = 46;
/** How far a unit's body overhangs its first and last outlet. */
export const GATE_PAD = 0.42 * CELL;
/** Half-height of a tool body.
 *
 *  34, not 24, since 2026-08-15: the body carries TWO rows now — the machine's
 *  name and, under it, the smart plug (docs/mockups/outlet-docks-multi-system.html).
 *  The router reads this as the obstacle's half-height, so growing the drawing
 *  without growing this would route ducts straight through the plug row. */
export const TOOL_HALF = 34;
/** How far below a unit's body an outlet stub starts. */
export const OUTLET_STUB = UNIT_H / 2 + 12;
/** Where a unit's feed run stops, measured from its top edge. Matches the 12px the
 *  OUTLETS stub out of the bottom, because the unit now draws an inlet stub to meet
 *  it — so the trunk lands on a spigot rather than butting into the body.
 *
 *  5px was not enough: the duct is stroked 6 with a ROUND cap, so its ink runs 3px
 *  past the endpoint, leaving barely a pixel against the box outline. It still read
 *  as touching. */
export const INLET_GAP = 12;

/** `pickup` is a machine's SUPPLEMENTAL port — an overarm guard, a hood. It has no
 *  cell of its own: it rides on the top edge of the machine's box as a second inlet,
 *  which is what stops a shop with a two-port saw reading as a shop with two saws.
 *
 *  `board` is a controller. It lived on a rail above the grid until 2026-08-16 and
 *  is an ordinary piece on it now — which is why its size is HERE rather than with
 *  the rest of the wiring metrics: a board occupies a cell, so a duct has to steer
 *  around the same box the drawing puts there. */
export type Glyph = 'collector' | 'slidingGate' | 'ballvalve' | 'manifold' | 'junction' | 'tool' | 'pickup' | 'board';
/** Half-width of the hood a pickup draws, and the box the router steers around. */
export const PICKUP_HALF = 9;

/** The SPIGOT: the square inlet a machine's PRIMARY port draws on its top edge,
 *  opposite the tapered hood(s) of its pickups. Square = the main port, tapered =
 *  a pickup — the vocabulary docs/mockups/secondary-ports.html Option A settles on.
 *
 *  It appears only on a machine that HAS a pickup. The point of the pair is to say
 *  which inlet is which, and a lone duct landing on a lone box already says that,
 *  so a spigot on every tool in the shop would be decoration rather than
 *  information.
 *
 *  {@link SPIGOT_DX} is why the router needs {@link SceneNode.inletDx}: the duct has
 *  to LAND on the glyph, so the top inlet port moves left with it. The offsets are
 *  the mockup's proportions (-0.40 and +0.57 of half-width) rescaled to this
 *  canvas's narrower 76px tool body. */
export const SPIGOT_W = 14;
export const SPIGOT_H = 11;
export const SPIGOT_DX = -17;
/** A board's body. Deliberately narrower than a CELL so two boards on neighbouring
 *  cells have air between them instead of sharing an edge and reading as one module. */
export const BOARD_W = 96;
export const BOARD_H = 52;

/** The router's view of a placed device. `x`/`y` are the resolved centre — for a
 *  unit that is its FIRST outlet, not the middle of the bar. */
export interface SceneNode {
  id: string;
  glyph: Glyph;
  isUnit: boolean;
  span: number;
  x: number;
  y: number;
  /** Shifts this device's TOP inlet off its centreline, so a duct lands on the
   *  glyph that marks the entry rather than on the middle of the box. Set for a
   *  machine wearing a spigot ({@link SPIGOT_DX}); 0 or absent for everything
   *  else, which keeps every other device entering dead centre as before. */
  inletDx?: number;
}

export function cellX(col: number): number { return PAD + col * CELL; }
export function cellY(row: number): number { return PAD + row * CELL; }

/** Half-width of a glyph. A unit is measured from its centre, which is why the
 *  span term is halved here but not in {@link deviceBox}. */
export function halfW(n: SceneNode): number {
  if (n.isUnit) return (n.span - 1) * CELL / 2 + GATE_PAD;
  switch (n.glyph) {
    case 'collector': return 30;
    case 'ballvalve': return 22;
    case 'junction': return 8;
    case 'pickup': return PICKUP_HALF;
    case 'board': return BOARD_W / 2;
    default: return 38;
  }
}

export function halfH(n: SceneNode): number {
  if (n.isUnit) return UNIT_H / 2;
  switch (n.glyph) {
    case 'collector': return 30;
    case 'ballvalve': return 22;
    case 'junction': return 8;
    case 'pickup': return PICKUP_HALF;
    case 'board': return BOARD_H / 2;
    default: return TOOL_HALF;
  }
}

/** The glyph's footprint in board pixels, optionally inflated by `m`. A unit's
 *  origin is outlet 0, so its body runs rightward from there. */
export function deviceBox(n: SceneNode, m = 0): Box {
  const x0 = n.isUnit ? n.x - GATE_PAD : n.x - halfW(n);
  const x1 = n.isUnit ? n.x + (n.span - 1) * CELL + GATE_PAD : n.x + halfW(n);
  const hh = halfH(n);
  return { x0: x0 - m, y0: n.y - hh - m, x1: x1 + m, y1: n.y + hh + m };
}

/** Does an axis-aligned segment pass through a box? Touching an edge doesn't count —
 *  a duct is allowed to run flush along a clearance boundary. */
export function segBoxHit(a: Pt, b: Pt, box: Box): boolean {
  if (Math.abs(a.x - b.x) < 0.5) {
    const x = a.x, lo = Math.min(a.y, b.y), hi = Math.max(a.y, b.y);
    return x > box.x0 && x < box.x1 && hi > box.y0 && lo < box.y1;
  }
  const y = a.y, lo = Math.min(a.x, b.x), hi = Math.max(a.x, b.x);
  return y > box.y0 && y < box.y1 && hi > box.x0 && lo < box.x1;
}

export function firstHitBox(a: Pt, b: Pt, boxes: Box[]): Box | null {
  let best: Box | null = null, bestD = Infinity;
  for (const box of boxes) {
    if (!segBoxHit(a, b, box)) continue;
    const cx = (box.x0 + box.x1) / 2, cy = (box.y0 + box.y1) / 2;
    const d = Math.hypot(cx - a.x, cy - a.y);
    if (d < bestD) { bestD = d; best = box; }
  }
  return best;
}

export function ptInBox(p: Pt, box: Box): boolean {
  return p.x > box.x0 && p.x < box.x1 && p.y > box.y0 && p.y < box.y1;
}

/** Drop duplicate and collinear points, so a lattice path collapses to its corners.
 *  This is what turns "step, step, step" into the two-point straight drop the
 *  drawing code expects. */
export function simplifyPts(pts: Pt[]): Pt[] {
  const dedup: Pt[] = [];
  for (const p of pts) {
    const last = dedup[dedup.length - 1];
    if (last && Math.abs(last.x - p.x) < 0.5 && Math.abs(last.y - p.y) < 0.5) continue;
    dedup.push(p);
  }
  const res: Pt[] = [];
  for (let i = 0; i < dedup.length; i++) {
    const a = dedup[i - 1], b = dedup[i], c = dedup[i + 1];
    if (a && c && ((Math.abs(a.x - b.x) < 0.5 && Math.abs(b.x - c.x) < 0.5) ||
                   (Math.abs(a.y - b.y) < 0.5 && Math.abs(b.y - c.y) < 0.5))) continue;
    res.push(b);
  }
  return res;
}

/** Distance from a point to a segment — used for duct hit-testing. */
export function ptSegDist(px: number, py: number, a: Pt, b: Pt): number {
  const vx = b.x - a.x, vy = b.y - a.y;
  const len2 = vx * vx + vy * vy;
  if (len2 < 1e-6) return Math.hypot(px - a.x, py - a.y);
  let t = ((px - a.x) * vx + (py - a.y) * vy) / len2;
  t = Math.max(0, Math.min(1, t));
  return Math.hypot(px - (a.x + t * vx), py - (a.y + t * vy));
}
