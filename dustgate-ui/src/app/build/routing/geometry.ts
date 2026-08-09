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
/** Half-height of a tool body. */
export const TOOL_HALF = 24;
/** How far below a unit's body an outlet stub starts. */
export const OUTLET_STUB = UNIT_H / 2 + 12;

export type Glyph = 'collector' | 'slidingGate' | 'ballvalve' | 'manifold' | 'junction' | 'tool';

/** The router's view of a placed device. `x`/`y` are the resolved centre — for a
 *  unit that is its FIRST outlet, not the middle of the bar. */
export interface SceneNode {
  id: string;
  glyph: Glyph;
  isUnit: boolean;
  span: number;
  x: number;
  y: number;
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
    default: return 38;
  }
}

export function halfH(n: SceneNode): number {
  if (n.isUnit) return UNIT_H / 2;
  switch (n.glyph) {
    case 'collector': return 30;
    case 'ballvalve': return 22;
    case 'junction': return 8;
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
