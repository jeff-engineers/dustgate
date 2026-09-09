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

const some = <T,>(xs: Iterable<T>, f: (x: T) => boolean): boolean => {
  for (const x of xs) if (f(x)) return true;
  return false;
};

/**
 * What one solve produces: the picture, and the lattice it was drawn from.
 *
 * `out` is what goes on screen — lanes nested apart by separateLanes(), so a run in
 * it can sit up to half a lane step off the lattice edge it actually routed on.
 * `lattice` is the same solve BEFORE that nudge, and it is what the next solve must
 * be fed back as `prior`. Nesting the picture and then re-nesting the nested picture
 * next frame is how a run nobody was dragging walked off its line half a lane step
 * at a time (2026-09-07).
 */
export interface Solved {
  out: Map<string, RoutedDuct>;
  lattice: Map<string, RoutedDuct>;
  shared: string[];
}

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
 * {@link routeAll}, plus WHICH ducts are still drawn over another one.
 *
 * The list used to be computed and thrown away — every caller took the map and the
 * fact that two runs are drawn on top of each other went nowhere, so the canvas
 * could produce an overlap and say nothing about it. Splitting the two lets a
 * caller ask "would this edit force an overlap?" before committing to it, which is
 * how the branch-dot menu greys a splice it can't draw (2026-09-07).
 *
 * An id in `shared` means that duct is drawn over some OTHER duct — which one is
 * not recorded, because the geometric check that finds them works a segment at a
 * time and has no notion of who was there first.
 */
export function routeAllShared(scene: Scene, opts: RouteAllOpts = {}): Solved {
  let order = trunkFirst(scene);
  let best: Solved | null = null;
  for (let attempt = 0; attempt < 3; attempt++) {
    const pass = routePass(scene, opts, order);
    // What comes BACK is what is drawn over something. What the pass reports —
    // `sharedLane` — is a different and weaker fact: that duct's path runs along a
    // lattice edge an earlier one had already taken. The lanes are nested apart
    // below, so most of those land somewhere clean, and reporting them as overlaps
    // named runs that are perfectly fine (2026-09-07: moving the demo's drum sander
    // down one cell said two runs were drawn over each other, and nothing was).
    //
    // It is still the right thing to REORDER on, which is why the pass keeps
    // reporting it: a duct that had to take someone else's lane is the one worth
    // routing first next time, whether or not this attempt ended up tidy.
    // Separate first, then judge: what comes back is what is STILL drawn over
    // something after the lanes are nested, which for most boards is nothing.
    const nested = separateLanes(pass.out);
    const drawn = drawnOverlaps(nested);
    if (!drawn.length) return { out: nested, lattice: pass.out, shared: [] };
    const result = { out: nested, lattice: pass.out, shared: drawn };
    // The TIDIEST attempt wins, not the first. Keeping the first was written when
    // this list meant "gave up a lane", where one attempt was as good as another;
    // now it counts runs actually drawn over something, so a reorder that halves it
    // is a better picture and was being thrown away.
    if (!best || result.shared.length < best.shared.length) best = result;
    const promote = pass.sharedLane[0] ?? drawn[0];
    if (order[0] === promote) break;            // already first; reordering can't help
    order = [promote, ...order.filter(id => id !== promote)];
  }
  // `best` is always set by here: the loop returns early when a pass is clean,
  // and assigns `best` on every pass that is not. There used to be a fallback
  // routePass() after this line, which could never run — and which reported
  // `shared: []` unconditionally, so if it ever HAD run it would have claimed a
  // board with overlaps was clean.
  return best!;
}

/**
 * The order ducts are solved in: down the tree from the collector, trunk first.
 *
 * It was alphabetical by child id, which is deterministic and nothing else. On the
 * demo shop that put every `sel*` and `tool*` ahead of every `wye*` — so the MAIN
 * TRUNK was the last thing routed, and until it was, the band along the top of the
 * board looked to every other run like empty highway. A manifold feed took it,
 * crossed the whole shop up there, and the trunk then had to nest itself around the
 * run that had taken its lane: three pieces of one straight line at three different
 * heights (reported 2026-09-07).
 *
 * Solving from the root outward matches how the shop is actually built and how the
 * costs are meant to work. An early run claims its lane and later runs pay USED and
 * CROSS to come near it, and the trunk is the one run that should never be the one
 * paying — everything downstream hangs off it.
 *
 * Depth first, id second, so the result is still fully determined: two ducts at the
 * same depth are solved in the same order on every board, every frame.
 */
function trunkFirst(scene: Scene): string[] {
  const parentOf = new Map(scene.ducts.map(d => [d.childId, d.outlet?.unitId ?? d.parentId]));
  const depth = new Map<string, number>();
  const depthOf = (id: string): number => {
    const seen = depth.get(id);
    if (seen !== undefined) return seen;
    depth.set(id, 0);                        // breaks a cycle in a half-edited doc
    const up = parentOf.get(id);
    const d = up === undefined || !parentOf.has(up) ? 0 : depthOf(up) + 1;
    depth.set(id, d);
    return d;
  };
  return [...scene.ducts]
    .map(d => d.childId)
    .sort((a, b) => depthOf(a) - depthOf(b) || (a < b ? -1 : a > b ? 1 : 0));
}

/** How far apart two runs sharing a lane are pulled. A duct is stroked at 6, so
 *  this is a clear gap rather than a graze — the same reasoning as LANE_STEP in the
 *  wiring layer, which has separated cables this way since boards went on the grid. */
export const LANE_STEP = 12;

/**
 * Pull runs that share a lane apart, the way the wiring layer nests its cables.
 *
 * Ducts and cables had opposite answers to the same problem and only one of them
 * worked. A cable that would share a corridor is given its own LANE and nested
 * beside its neighbours; a duct that would share one was sent round the shop to
 * find a lane of its own, and when there wasn't one, drawn on top of its neighbour
 * and reported. That detour is most of what makes a layout look like plumbing
 * nobody would build — bends that exist to satisfy a rule rather than to get
 * anywhere.
 *
 * So the router still solves on the lattice, and then the drawn line is nudged off
 * it. Both runs move, symmetrically about the lane they wanted, so neither is "the
 * one that got shifted" and a pair reads as a pair.
 *
 * This makes the picture inexact ON PURPOSE, and that is worth being explicit
 * about: the canvas is a representation of a shop, not a blueprint. Nobody measures
 * off it and nothing is cut to it, so a run drawn 6px from the lattice it routed on
 * costs nothing real and buys a picture where both runs can be seen.
 *
 * Only the SEGMENT that clashes moves, with its two endpoints — so the neighbouring
 * legs, being perpendicular, stay perpendicular and simply grow or shrink. An
 * endpoint on a port moves too: a duct leaving a junction dot 6px off centre still
 * visibly leaves that dot, and pinning it would need a dogleg that costs more
 * legibility than it buys.
 */
function separateLanes(lattice: ReadonlyMap<string, RoutedDuct>): Map<string, RoutedDuct> {
  // A COPY, always. The caller keeps the lattice-true solve to hand back as `prior`
  // next frame, and a frozen run is handed straight through from there — nudging the
  // original in place meant that run picked up another half lane step every frame of
  // a drag it was not part of.
  const out = new Map<string, RoutedDuct>();
  for (const [id, r] of lattice) out.set(id, { ...r, pts: r.pts.map(p => ({ ...p })) });

  type Ref = { id: string; i: number };            // segment i of run id
  const groups = new Map<string, Ref[]>();

  for (const [id, r] of out) {
    for (let i = 0; i < r.pts.length - 1; i++) {
      const a = r.pts[i], b = r.pts[i + 1];
      const horiz = Math.abs(a.y - b.y) < 0.5, vert = Math.abs(a.x - b.x) < 0.5;
      if (!horiz && !vert) continue;
      // Keyed on the LINE it sits on, so everything sharing that line is compared;
      // which of them actually overlap is settled below.
      const key = horiz ? `h${Math.round(a.y)}` : `v${Math.round(a.x)}`;
      const list = groups.get(key);
      if (list) list.push({ id, i }); else groups.set(key, [{ id, i }]);
    }
  }

  for (const refs of groups.values()) {
    if (refs.length < 2) continue;
    const span = (r: Ref): [number, number] => {
      const pts = out.get(r.id)!.pts, a = pts[r.i], b = pts[r.i + 1];
      return Math.abs(a.y - b.y) < 0.5
        ? [Math.min(a.x, b.x), Math.max(a.x, b.x)]
        : [Math.min(a.y, b.y), Math.max(a.y, b.y)];
    };
    // Only across DIFFERENT runs: one run doubling back on its own line is its own
    // business, and nudging half of it apart would just bend it.
    const clash = refs.filter(r => refs.some(o => {
      if (o.id === r.id) return false;
      const [a0, a1] = span(r), [b0, b1] = span(o);
      return Math.min(a1, b1) - Math.max(a0, b0) > 1;
    }));
    if (clash.length < 2) continue;

    // One lane per RUN, not per segment: a run clashing twice on the same line takes
    // the same lane both times or it zigzags between them. Ordered by id so a shop
    // draws the same way every time.
    const lanes = [...new Set(clash.map(r => r.id))].sort();
    const mid = (lanes.length - 1) / 2;
    for (const r of clash) {
      const delta = (lanes.indexOf(r.id) - mid) * LANE_STEP;
      if (!delta) continue;
      const pts = out.get(r.id)!.pts;
      const horiz = Math.abs(pts[r.i].y - pts[r.i + 1].y) < 0.5;
      if (horiz) {
        pts[r.i] = { x: pts[r.i].x, y: pts[r.i].y + delta };
        pts[r.i + 1] = { x: pts[r.i + 1].x, y: pts[r.i + 1].y + delta };
      } else {
        pts[r.i] = { x: pts[r.i].x + delta, y: pts[r.i].y };
        pts[r.i + 1] = { x: pts[r.i + 1].x + delta, y: pts[r.i + 1].y };
      }
    }
  }
  return out;
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

function routePass(scene: Scene, opts: RouteAllOpts, order: string[]): { out: Map<string, RoutedDuct>; sharedLane: string[] } {
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
  // Ducts whose path runs along an edge an earlier one already took. NOT a claim
  // that they overlap — see routeAllShared — only that they wanted the same lane.
  const sharedLane: string[] = [];

  const ceilings = ceilingsOf(scene.nodes);

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
      ceilingY: ceilingFor(child, ceilings),
      usedEdges: used,
      usedNodes: crossed,
      priorEdges: prior?.get(d.childId)?.edges,
    };
    const strict = obstaclesFor(scene.nodes, EMPTY);
    // THREE attempts, each relaxing one rule, so the picture degrades in the order a
    // person would accept: stay under the outlet; then climb; then cross your own
    // endpoints rather than fail.
    //
    // Sharing a lane is NOT a rung on this ladder any more, and taking it off is the
    // 2026-09-07 fix. It used to be the first one — a pass that walled off every edge
    // an earlier run had claimed — and a wall cannot weigh two pictures against each
    // other. On the shop that reported this, the manifold's feed would have shared two
    // lattice edges with the leg beside it for a cost of 360; walling them off sent it
    // straight up to the collector's own line, west across the entire shop and back
    // down, for 416. The search was right by its own rule and the drawing was absurd.
    //
    // So lane sharing is priced (USED) rather than forbidden, and the ONE thing that
    // makes that safe is separateLanes(), which nests whatever still shares a lane
    // 12px apart afterwards. Without that pass this would be trading a lasso for two
    // runs drawn on top of each other, which is the worse of the two.
    //
    // The ceiling stays a wall first and a cost second, because a pipe going up over
    // the shop to come back down is not a compromise anyone would accept — but
    // neither is a machine you cannot reach at all, so a tool parked above the
    // collector still gets its duct.
    let res: RouteResult = routeOne(from, to, { ...common, obstacles: strict, ceilingBlocks: true });
    if (!res.ok) {
      res = routeOne(from, to, { ...common, obstacles: strict });
      if (!res.ok) {
        res = routeOne(from, to, { ...common, obstacles: obstaclesFor(scene.nodes, new Set([child.id, parent.id])) });
      }
    }
    // Which runs ACTUALLY ended up sharing an edge with an earlier one — the reorder
    // signal routeAllShared works from. It used to be "the strict pass failed", which
    // was a claim about a rule that no longer exists; asking the finished path is
    // both simpler and closer to what the reorder is trying to fix.
    if (some(res.edges, e => used.has(e))) sharedLane.push(d.childId);

    for (const e of res.edges) used.add(e);
    for (const n of res.nodes) crossed.add(n);
    for (const k of portsOfPath(res.pts)) takenPorts.add(k);
    out.set(d.childId, { pts: res.pts, ok: res.ok, edges: res.edges, nodes: res.nodes });
  }

  return { out, sharedLane };
}

/**
 * The heights ducting stays below — one for the board, and one per system.
 *
 * A collector's side ports sit on its centreline, and that is where the trunk leaves:
 * everything downstream of it flows away and DOWN, so pipe drawn above that line is
 * going up only to come back. The router had no opinion about height at all, and the
 * empty band across the top of the board was therefore its favourite place to put a
 * run it could not fit anywhere else.
 *
 * TWO ceilings, because they answer different questions (jeff, 2026-09-07):
 *
 *   `global` — the topmost collector's outlet. The top of the page. NOTHING goes
 *     above it, an auxiliary run included.
 *   `bySystem` — each system's own collector outlet, which for every system below
 *     the first is lower down the page and therefore tighter. It keeps a system's
 *     ducting inside its own band instead of climbing into the shop above it.
 *
 * An AUXILIARY run is exempt from the per-system ceiling and bound only by the
 * global one. It is the one run allowed to cross the seam between two systems, so a
 * ceiling at its own system's collector would forbid the very thing it exists to
 * do — reach a machine in the system above.
 *
 * Undefined on a board with no collector yet, which is a real state while someone is
 * drawing: no collector, no outlet height, no opinion.
 */
export interface Ceilings {
  global?: number;
  bySystem: Map<string, number>;
}

export function ceilingsOf(nodes: SceneNode[]): Ceilings {
  let global = Infinity;
  const bySystem = new Map<string, number>();
  for (const n of nodes) {
    if (n.glyph !== 'collector') continue;
    global = Math.min(global, n.y);
    if (n.systemId === undefined) continue;
    const seen = bySystem.get(n.systemId);
    if (seen === undefined || n.y < seen) bySystem.set(n.systemId, n.y);
  }
  return { global: global === Infinity ? undefined : global, bySystem };
}

/** The topmost collector's outlet on its own — the global ceiling. */
export function ceilingOf(nodes: SceneNode[]): number | undefined {
  return ceilingsOf(nodes).global;
}

/** Which of the two applies to the run ending at `child`. */
export function ceilingFor(child: SceneNode, c: Ceilings): number | undefined {
  if (child.glyph === 'secondaryPort') return c.global;      // the seam-crossing run
  const own = child.systemId === undefined ? undefined : c.bySystem.get(child.systemId);
  if (own === undefined) return c.global;
  // A system's own ceiling can only ever be at or below the global one; taking the
  // lower of the two makes that a guarantee rather than an assumption about layout.
  return c.global === undefined ? own : Math.max(c.global, own);
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
    // The LATTICE solve, not the drawn one — see Solved. A frozen run is re-held from
    // here on every frame of a drag, so it has to be the same base every time.
    this.last = solved.lattice;
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
