/** The single entry point: solve every duct on the board in one deterministic pass.
 *
 *  Routing all of them together is the point. Each solved run adds its lattice edges
 *  to a shared soft-cost map, so later runs steer off them by themselves — that's
 *  what replaces the old hand-rolled lane stagger — and because the order is fixed
 *  (sorted by child id) the same board always produces the same picture. */

import { type Box, type Pt, type SceneNode, deviceBox } from './geometry';
import { type Port, type RouteResult, inPorts, obstaclesFor, outPorts, routeOne } from './route-grid';

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
  let order = [...scene.ducts]
    .map(d => d.childId)
    .sort((a, b) => a < b ? -1 : a > b ? 1 : 0);
  let best: Map<string, RoutedDuct> | null = null;
  for (let attempt = 0; attempt < 3; attempt++) {
    const { out, shared } = routePass(scene, opts, order);
    if (!shared.length) return out;
    if (!best) best = out;                      // keep the first result as the floor
    const promote = shared[0];
    if (order[0] === promote) break;            // already first; reordering can't help
    order = [promote, ...order.filter(id => id !== promote)];
  }
  return best ?? routePass(scene, opts, order).out;
}

function routePass(scene: Scene, opts: RouteAllOpts, order: string[]): { out: Map<string, RoutedDuct>; shared: string[] } {
  const byId = new Map(scene.nodes.map(n => [n.id, n]));
  const prior = opts.prior;
  const frozen = opts.frozen;
  const out = new Map<string, RoutedDuct>();
  const used = new Set<string>();
  const crossed = new Set<string>();

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
  }

  for (const d of ducts) {
    if (out.has(d.childId)) continue;
    const child = byId.get(d.childId);
    if (!child) continue;

    const parent = d.outlet ? byId.get(d.outlet.unitId) : (d.parentId ? byId.get(d.parentId) : undefined);
    if (!parent) { out.set(d.childId, { pts: [], ok: false, edges: new Set(), nodes: new Set() }); continue; }

    const from: Port[] = outPorts(parent, d.outlet?.index);
    const to: Port[] = inPorts(child);

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

  routes(scene: Scene, frozen?: ReadonlySet<string>): ReadonlyMap<string, RoutedDuct> {
    const h = sceneHash(scene, frozen);
    if (h === this.hash) return this.cache;
    this.cache = routeAll(scene, { prior: this.last, frozen });
    this.last = this.cache;
    this.hash = h;
    return this.cache;
  }

  /** Drop the memo — call when the board changes shape in a way the hash can't see. */
  invalidate(): void { this.hash = ''; }

  /** The paths as of the last solve, for freezing during a drag. */
  committed(): ReadonlyMap<string, RoutedDuct> { return this.last; }
}

function sceneHash(scene: Scene, frozen?: ReadonlySet<string>): string {
  const parts: string[] = [];
  for (const n of scene.nodes) parts.push(`${n.id}:${n.glyph}:${n.span}:${Math.round(n.x)}:${Math.round(n.y)}`);
  parts.push('|');
  for (const d of scene.ducts) parts.push(`${d.childId}<${d.parentId ?? ''}<${d.outlet?.unitId ?? ''}:${d.outlet?.index ?? ''}`);
  parts.push('|', `${Math.round(scene.bounds.x1)}x${Math.round(scene.bounds.y1)}`);
  if (frozen?.size) parts.push('|f:', [...frozen].sort().join(','));
  return parts.join(';');
}
