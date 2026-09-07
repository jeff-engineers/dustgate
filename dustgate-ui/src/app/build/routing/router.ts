/** The single entry point: solve every duct on the board in one deterministic pass.
 *
 *  Routing all of them together is the point. Each solved run adds its lattice edges
 *  to a shared soft-cost map, so later runs steer off them by themselves — that's
 *  what replaces the old hand-rolled lane stagger — and because the order is fixed
 *  (sorted by child id) the same board always produces the same picture. */

import { type Box, type Pt, type SceneNode, deviceBox } from './geometry';
import { type Port, type RouteResult, PORT_REUSE, inPorts, obstaclesFor, outPorts, routeOne } from './route-grid';

export interface SceneDuct {
  childId: string;
  parentId?: string;
  /** Set when the duct hangs off a specific outlet of a unit. */
  outlet?: { unitId: string; index: number };
}

export interface Scene {
  nodes: SceneNode[];
  ducts: SceneDuct[];
  bounds: Box;
}

export interface RoutedDuct {
  pts: Pt[];
  /** False when the duct is boxed in and `pts` is the straight-dogleg fallback —
   *  the drag feedback reads this to explain why a drop won't work. */
  ok: boolean;
  edges: ReadonlySet<string>;
  nodes: ReadonlySet<string>;
}

/** No device exempt — the strict obstacle pass. */
const EMPTY: ReadonlySet<string> = new Set<string>();

export interface RouteAllOpts {
  /** Last solve's result, for the prior-route discount. */
  prior?: ReadonlyMap<string, RoutedDuct>;
  /** Ducts to hold at their prior paths instead of re-solving — everything not
   *  attached to the node being dragged. */
  frozen?: ReadonlySet<string>;
}

/**
 * Solve every duct, retrying if any of them had to share a lane.
 *
 * Sharing is nearly always an ordering accident rather than a real necessity: an
 * early, unconstrained duct takes the lane straight under a gate, and a later one
 * whose only way out was that lane has nowhere left to go. Routing the squeezed
 * duct FIRST usually gives everyone a clean path, so on failure we reorder and try
 * again rather than accept the overlap. Bounded and deterministic — a fixed number
 * of attempts, each with a fully determined order.
 */
export function routeAll(scene: Scene, opts: RouteAllOpts = {}): Map<string, RoutedDuct> {
  return routeAllShared(scene, opts).out;
}

/**
 * {@link routeAll}, plus WHICH ducts had to give up their exclusive lane.
 *
 * The list used to be computed and thrown away — every caller took the map and the
 * fact that two runs are drawn on top of each other went nowhere, so the canvas
 * could produce an overlap and say nothing about it. Splitting the two lets a
 * caller ask "would this edit force an overlap?" before committing to it, which is
 * how the branch-dot menu greys a splice it can't draw (2026-09-07).
 *
 * An id in `shared` means that duct is drawn over some OTHER duct — which one is
 * not recorded, because the pass that gave up doesn't know who it lost to.
 */
export function routeAllShared(scene: Scene, opts: RouteAllOpts = {}): { out: Map<string, RoutedDuct>; shared: string[] } {
  let order = [...scene.ducts]
    .map(d => d.childId)
    .sort((a, b) => a < b ? -1 : a > b ? 1 : 0);
  let best: { out: Map<string, RoutedDuct>; shared: string[] } | null = null;
  for (let attempt = 0; attempt < 3; attempt++) {
    const pass = routePass(scene, opts, order);
    pass.shared = union(pass.shared, drawnOverlaps(pass.out));
    if (!pass.shared.length) return pass;
    if (!best) best = pass;                     // keep the first result as the floor
    const promote = pass.shared[0];
    if (order[0] === promote) break;            // already first; reordering can't help
    order = [promote, ...order.filter(id => id !== promote)];
  }
  return best ?? routePass(scene, opts, order);
}

/**
 * Ducts whose DRAWN lines lie on top of each other, found geometrically rather
 * than from the lattice.
 *
 * The lane bookkeeping in routePass is the router's own currency — the edges of the
 * lattice it searches — and it is blind to two things that reach the screen anyway:
 *
 *  • **Sub-cell stubs.** Two glyphs standing in adjacent cells are joined by a line
 *    shorter than a lattice edge, so the run traverses no edge at all and claims
 *    nothing. Drag a gate next to the tee that feeds it and two runs share that gap
 *    with nothing objecting (found 2026-09-07).
 *  • **A shared origin.** Two ducts off one port, both leaving in the same
 *    direction, are one line until they separate. The demo's own manifold has done
 *    this from the start: 34px of two runs drawn as one, and every edge-based check
 *    ever run over it said the board was clean.
 *
 * So overlap is judged on what is actually drawn. Touching endpoints don't count —
 * ducts meet at a tee by construction — only a stretch two runs genuinely share.
 */
function drawnOverlaps(out: Map<string, RoutedDuct>): string[] {
  type Seg = { id: string; a: Pt; b: Pt };
  const segs: Seg[] = [];
  for (const [id, r] of out) {
    for (let i = 0; i < r.pts.length - 1; i++) segs.push({ id, a: r.pts[i], b: r.pts[i + 1] });
  }
  const hit = new Set<string>();
  for (let i = 0; i < segs.length; i++) {
    for (let j = i + 1; j < segs.length; j++) {
      const s = segs[i], t = segs[j];
      if (s.id === t.id) continue;
      if (hit.has(s.id) && hit.has(t.id)) continue;
      if (!collinearOverlap(s.a, s.b, t.a, t.b)) continue;
      hit.add(s.id); hit.add(t.id);
    }
  }
  return [...hit];
}

/** Do two axis-aligned segments lie on the same line and share more than a point?
 *  EPS is a hair over float noise, not a tolerance: the two runs are stroked at the
 *  same width, so any shared length at all is one line hiding another. */
function collinearOverlap(a0: Pt, a1: Pt, b0: Pt, b1: Pt): boolean {
  const EPS = 1;
  const horiz = (p: Pt, q: Pt) => Math.abs(p.y - q.y) < 0.5;
  const vert = (p: Pt, q: Pt) => Math.abs(p.x - q.x) < 0.5;
  const aH = horiz(a0, a1), bH = horiz(b0, b1);
  const aV = vert(a0, a1), bV = vert(b0, b1);
  let axis: number, bxis: number;
  if (aH && bH) { axis = a0.y; bxis = b0.y; }
  else if (aV && bV) { axis = a0.x; bxis = b0.x; }
  else return false;                                   // perpendicular, or a diagonal
  if (Math.abs(axis - bxis) > 0.5) return false;       // parallel, not collinear
  const span = (p: Pt, q: Pt): [number, number] => (aH
    ? [Math.min(p.x, q.x), Math.max(p.x, q.x)]
    : [Math.min(p.y, q.y), Math.max(p.y, q.y)]);
  const [s0, s1] = span(a0, a1), [t0, t1] = span(b0, b1);
  return Math.min(s1, t1) - Math.max(s0, t0) > EPS;
}

/** Both lists, no duplicates, order preserved — the first entry is what the retry
 *  promotes, so the edge-based finding stays in front where it was. */
function union(a: readonly string[], b: readonly string[]): string[] {
  const out = [...a];
  for (const id of b) if (!out.includes(id)) out.push(id);
  return out;
}

/** A port's identity for reuse: where it is, and which way the run goes through it.
 *  Rounded, because a port sits on a device edge in float pixels and the same port
 *  must key identically for every duct that reaches it. */
function portKey(p: Pt, dir: number): string {
  return `${Math.round(p.x)},${Math.round(p.y)},${dir}`;
}

/** Which direction the run leaves `a` in, as route-grid numbers them (0=E 1=S 2=W 3=N). */
function dirOf(a: Pt, b: Pt): number {
  if (Math.abs(a.x - b.x) < 0.5) return b.y > a.y ? 1 : 3;
  return b.x > a.x ? 0 : 2;
}

/** The two ports a finished path actually used — the one it left by and the one it
 *  arrived at, each named by the direction the run travels through it. */
function portsOfPath(pts: readonly Pt[]): string[] {
  if (pts.length < 2) return [];
  const n = pts.length;
  return [
    portKey(pts[0], dirOf(pts[0], pts[1])),
    portKey(pts[n - 1], dirOf(pts[n - 1], pts[n - 2])),
  ];
}

/** Bias every port an earlier run already used, leaving the rest untouched. Soft:
 *  a device whose only sane exit is taken still routes through it. */
function charge(ports: Port[], taken: ReadonlySet<string>): Port[] {
  if (!taken.size) return ports;
  return ports.map(p => (taken.has(portKey(p.pt, p.dir))
    ? { ...p, bias: (p.bias ?? 0) + PORT_REUSE }
    : p));
}

function routePass(scene: Scene, opts: RouteAllOpts, order: string[]): { out: Map<string, RoutedDuct>; shared: string[] } {
  const byId = new Map(scene.nodes.map(n => [n.id, n]));
  const prior = opts.prior;
  const frozen = opts.frozen;
  const out = new Map<string, RoutedDuct>();
  const used = new Set<string>();
  const crossed = new Set<string>();
  // Ports an earlier run in this pass has already left (or arrived) by. Two runs
  // through one port are drawn as one line until they separate, and the stub they
  // share is usually shorter than a lattice edge, so `used` never sees it — this is
  // the bookkeeping that does. Keyed on the point AND the direction: two runs may
  // meet at a tee from opposite sides all day, and that is a junction, not an
  // overlap.
  const takenPorts = new Set<string>();

  const rank = new Map(order.map((id, i) => [id, i]));
  const ducts = [...scene.ducts].sort((a, b) => (rank.get(a.childId) ?? 0) - (rank.get(b.childId) ?? 0));
  const shared: string[] = [];

  // Frozen runs claim their edges first, so the duct actually being dragged routes
  // around where the others already are rather than the other way round.
  for (const d of ducts) {
    if (!frozen?.has(d.childId)) continue;
    const held = prior?.get(d.childId);
    if (!held) continue;
    out.set(d.childId, held);
    for (const e of held.edges) used.add(e);
    for (const n of held.nodes) crossed.add(n);
    for (const k of portsOfPath(held.pts)) takenPorts.add(k);
  }

  for (const d of ducts) {
    if (out.has(d.childId)) continue;
    const child = byId.get(d.childId);
    if (!child) continue;

    const parent = d.outlet ? byId.get(d.outlet.unitId) : (d.parentId ? byId.get(d.parentId) : undefined);
    if (!parent) { out.set(d.childId, { pts: [], ok: false, edges: new Set(), nodes: new Set() }); continue; }

    const from: Port[] = charge(outPorts(parent, d.outlet?.index), takenPorts);
    const to: Port[] = charge(inPorts(child), takenPorts);

    // Two passes. The strict one treats EVERY device as an obstacle, including this
    // duct's own parent and child — otherwise a run is free to cut straight across
    // the gate it hangs off, which is what put a duct through the middle of the main
    // gate's body. The ports themselves sit outside their own inflated box, so the
    // strict pass can still reach them. Only if that genuinely fails do the endpoints
    // become passable again, so no route we could previously find is lost.
    const common = {
      bounds: scene.bounds,
      usedEdges: used,
      usedNodes: crossed,
      priorEdges: prior?.get(d.childId)?.edges,
    };
    // Three attempts, each relaxing one rule, so the picture degrades in the order a
    // person would accept: never share a lane; then share one rather than cross a
    // device; then cross your own endpoints rather than fail outright.
    const strict = obstaclesFor(scene.nodes, EMPTY);
    let res: RouteResult = routeOne(from, to, { ...common, obstacles: strict, blockUsed: true });
    if (!res.ok) {
      shared.push(d.childId);                   // had to give up its exclusive lane
      res = routeOne(from, to, { ...common, obstacles: strict });
      if (!res.ok) {
        res = routeOne(from, to, { ...common, obstacles: obstaclesFor(scene.nodes, new Set([child.id, parent.id])) });
      }
    }

    for (const e of res.edges) used.add(e);
    for (const n of res.nodes) crossed.add(n);
    for (const k of portsOfPath(res.pts)) takenPorts.add(k);
    out.set(d.childId, { pts: res.pts, ok: res.ok, edges: res.edges, nodes: res.nodes });
  }

  return { out, shared };
}

/** Board bounds wide enough to hold every glyph, before the lattice adds its own
 *  margin. Taking it from the devices rather than the viewport keeps the routing
 *  result independent of window size, so a resize can't move a duct. */
export function sceneBounds(nodes: SceneNode[]): Box {
  if (!nodes.length) return { x0: 0, y0: 0, x1: 400, y1: 300 };
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const n of nodes) {
    const b = deviceBox(n);
    x0 = Math.min(x0, b.x0); y0 = Math.min(y0, b.y0);
    x1 = Math.max(x1, b.x1); y1 = Math.max(y1, b.y1);
  }
  return { x0, y0, x1, y1 };
}

/** Memoizes {@link routeAll} on a hash of everything it reads.
 *
 *  Without this the component re-solved every duct for every other duct on every
 *  change-detection pass — O(n²) full re-routes just to draw a frame. */
export class Router {
  private hash = '';
  private cache = new Map<string, RoutedDuct>();
  private last = new Map<string, RoutedDuct>();
  private sharedIds: string[] = [];

  routes(scene: Scene, frozen?: ReadonlySet<string>): ReadonlyMap<string, RoutedDuct> {
    const h = sceneHash(scene, frozen);
    if (h === this.hash) return this.cache;
    const solved = routeAllShared(scene, { prior: this.last, frozen });
    this.cache = solved.out;
    this.sharedIds = solved.shared;
    this.last = this.cache;
    this.hash = h;
    return this.cache;
  }

  /** Ducts drawn over another duct as of the last solve — empty when the picture is
   *  clean, which is the normal case. Kept beside the routes so a caller can say so
   *  rather than leaving someone to spot it on the canvas. */
  shared(): readonly string[] { return this.sharedIds; }

  /** Drop the memo — call when the board changes shape in a way the hash can't see. */
  invalidate(): void { this.hash = ''; }

  /** The paths as of the last solve, for freezing during a drag. */
  committed(): ReadonlyMap<string, RoutedDuct> { return this.last; }
}

function sceneHash(scene: Scene, frozen?: ReadonlySet<string>): string {
  const parts: string[] = [];
  // `capped` is in the hash because it changes a junction's FOOTPRINT, so capping an
  // end has to re-solve the runs that were steering around the dot it used to be.
  for (const n of scene.nodes) parts.push(`${n.id}:${n.glyph}:${n.span}:${Math.round(n.x)}:${Math.round(n.y)}:${n.capped ? 'c' : ''}`);
  parts.push('|');
  for (const d of scene.ducts) parts.push(`${d.childId}<${d.parentId ?? ''}<${d.outlet?.unitId ?? ''}:${d.outlet?.index ?? ''}`);
  parts.push('|', `${Math.round(scene.bounds.x1)}x${Math.round(scene.bounds.y1)}`);
  if (frozen?.size) parts.push('|f:', [...frozen].sort().join(','));
  return parts.join(';');
}
