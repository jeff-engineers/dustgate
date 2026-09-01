/** The pathfinder: a half-cell lattice over the board, and an A* that walks it.
 *
 *  This replaces the old stack of local guesses (baseDuctPoints / laneOffset /
 *  clearLaneY / avoidDevices / detourSeg). Those each made a defensible decision in
 *  isolation, which is exactly why they fought each other — a lane chosen to clear
 *  one box put the next leg through another, and the skirting pass then lassoed
 *  around it. One search over the whole run can't do that. */

import {
  type Box, type Pt, type SceneNode,
  CELL, CLEARANCE, GATE_PAD, INLET_GAP, LATTICE, OUTLET_STUB, PAD,
  SECONDARY_PORT_HALF, UNIT_H,
  deviceBox, halfH, halfW, ptInBox, segBoxHit, simplifyPts,
} from './geometry';

/** 0=east, 1=south, 2=west, 3=north. */
export type Dir = 0 | 1 | 2 | 3;
const DX = [1, 0, -1, 0];
const DY = [0, 1, 0, -1];
const opposite = (d: Dir): Dir => ((d + 2) % 4) as Dir;

/** A place a duct may attach, and the direction it leaves the device in.
 *
 *  `bias` is a soft extra cost charged ONLY if the search settles on this port as
 *  the goal — it never affects the search while a cheaper pairing is still live, so
 *  a genuinely shorter run to a biased port still wins. Used to prefer one entrance
 *  over another that's otherwise equally valid, without closing the door on it. */
export interface Port { pt: Pt; dir: Dir; bias?: number; }

/** Costs, in units where one lattice step is 4. The plan specifies 1 / +8 / +6 /
 *  +2 / −3; everything is scaled by 4 so the reuse discount can't make an edge
 *  cheaper than zero, which would break A*'s optimality guarantee. */
const STEP = 4;
const TURN = 32;      // strongly prefer few bends
const USED = 24;      // an edge another duct already took — soft, so parallel runs separate
const CROSS = 40;     // a NODE another duct passes through: this is what a crossing costs.
                      // Two orthogonal runs on a lattice can only meet at a node, so
                      // charging for the node is charging for the crossing — worth more
                      // than a bend, because the tidiest crossing is the one not drawn.
const HUG = 8;        // an edge running right alongside a device
const REUSE = 3;      // discount for an edge this duct used last frame — keeps a good prefix
const TOP_ENTRY_BIAS = STEP * 3;   // see the Port.bias doc — enough to win a near-tie,
                                    // not enough to out-price a meaningfully shorter side run
/** Cheapest an edge can ever be; the A* heuristic must not exceed this per step. */
const MIN_STEP = STEP - REUSE;

export interface GridOpts {
  /** Device footprints to route around, already inflated. */
  obstacles: Box[];
  /** Board bounds in pixels; the lattice extends a cell past these so a run can
   *  go around the outside of everything. */
  bounds: Box;
  /** Edge keys another duct has already claimed this pass. */
  usedEdges?: ReadonlySet<string>;
  /** Treat `usedEdges` as walls rather than as expensive. Two ducts sharing a lattice
   *  edge is exactly what "the ducts overlap" looks like on screen, and no cost
   *  setting makes that reliably not happen — a long enough detour always eventually
   *  costs more than sharing. The caller tries this first and falls back. */
  blockUsed?: boolean;
  /** Node keys another duct passes through — entering one is a crossing. */
  usedNodes?: ReadonlySet<string>;
  /** Edge keys this duct used on the previous solve. */
  priorEdges?: ReadonlySet<string>;
}

/** Canonical key for a lattice node. */
export function nodeKey(ax: number, ay: number): string { return `${ax},${ay}`; }

/** Canonical key for the undirected lattice edge between two nodes. */
export function edgeKey(ax: number, ay: number, bx: number, by: number): string {
  return (ax < bx || (ax === bx && ay <= by))
    ? `${ax},${ay}|${bx},${by}`
    : `${bx},${by}|${ax},${ay}`;
}

export interface RouteResult {
  pts: Pt[];
  /** Lattice edges the winning path used, for the caller's soft-cost bookkeeping. */
  edges: Set<string>;
  /** Lattice nodes the winning path passes through, for the crossing cost. */
  nodes: Set<string>;
  /** False when A* found nothing and `pts` is the straight-dogleg fallback. */
  ok: boolean;
}

// ── ports ────────────────────────────────────────────────────────────────────

/** Where a duct may LEAVE this device. `outlet` picks the stub on a unit. */
export function outPorts(n: SceneNode, outlet?: number): Port[] {
  if (n.isUnit) {
    const i = Math.max(0, Math.min(n.span - 1, outlet ?? 0));
    return [{ pt: { x: n.x + i * CELL, y: n.y + OUTLET_STUB }, dir: 1 }];
  }
  const hw = halfW(n), hh = halfH(n);
  if (n.glyph === 'collector') {
    // ALL FOUR SIDES, top included (2026-08-28). It was bottom/left/right, and
    // the missing top made a collector standing directly beside a wide unit take
    // a long way round: to reach a sliding gate's inlet — which is at the top of
    // its LEFTMOST cell — the run has to get above the gate, and with no top port
    // the only way up is out of a side and then a turn.
    //
    // That turn needs a free lattice column, and beside an adjacent unit there
    // isn't one. At CELL 108 the collector's obstacle reaches x+60 and the gate's
    // starts at x+39, so the two clearance boxes OVERLAP: the ~40px of daylight
    // you can see between the drawn glyphs is narrower than the router is allowed
    // to squeeze through. So the run went out the far side and wrapped all the way
    // around — correct for the ports it had, and obviously silly to look at.
    //
    // Rising straight out of the top needs no gap at all. Nothing about airflow
    // objects: a collector is the ROOT of the duct tree and everything drawn from
    // it flows inward, so no side of it reads as backwards the way a gate's top
    // does.
    return [
      { pt: { x: n.x, y: n.y + hh }, dir: 1 },
      { pt: { x: n.x - hw, y: n.y }, dir: 2 },
      { pt: { x: n.x + hw, y: n.y }, dir: 0 },
      { pt: { x: n.x, y: n.y - hh }, dir: 3 },
    ];
  }
  // A gate never emits from its top — air leaves a gate going downstream, and a duct
  // drawn out of the top of one reads as flowing backwards. Ball valves are inline so
  // they may run sideways, but the top stays an inlet only.
  if (n.glyph === 'tool' || n.glyph === 'ballvalve') return sidePorts(n, 1);
  return allSidePorts(n);   // junction: a bare tee on a run, so any direction
}

/** Where a duct may ENTER this device. */
export function inPorts(n: SceneNode): Port[] {
  // Stop the trunk just SHORT of a unit's top edge. Landing exactly on it put the
  // duct's 6px stroke half on top of the gate's own outline, so the run and the box
  // read as one shape and you couldn't see where the pipe ended.
  if (n.isUnit) return [{ pt: { x: n.x, y: n.y - UNIT_H / 2 - INLET_GAP }, dir: 3 }];
  // A trunk really can reach a tool from either side, and closing that off would
  // be wrong — but a duct dropping in from directly above reads as "this is what
  // feeds it" at a glance, where a side entry can look like it's skirting past on
  // its way somewhere else. The bias breaks a near-tie toward the top without
  // sending the router miles out of its way when a side entry is genuinely the
  // shorter path — see TOP_ENTRY_BIAS.
  //
  // The top port carries `inletDx`, so on a machine wearing a primary port the duct lands
  // ON that glyph. It costs the router nothing: entry() rounds a top port to the
  // nearest lattice COLUMN, and a ±17px shift rounds to the same one — only the
  // final drawn segment jogs across to the glyph.
  if (n.glyph === 'tool' || n.glyph === 'ballvalve')
    return sidePorts(n, 3).map(p => p.dir === 3
      ? { ...p, pt: { x: p.pt.x + (n.inletDx ?? 0), y: p.pt.y } }
      : { ...p, bias: TOP_ENTRY_BIAS });
  // A SECONDARY PORT is entered on ITS MACHINE'S edges, not on the 9px glyph's own —
  // the same three sides, biased the same way, because a port that behaved differently
  // depending on its shape would be a second rule to hold in your head (D-41). The
  // glyph is then drawn wherever the run actually landed.
  //
  // It had only a downward top port until 2026-08-20, and the cost was a detour rather
  // than a wrong picture: a run fed from a collector BELOW the machine had to climb
  // over the whole shop to come down on top of it — [202,496] → [226,64] → [187,117]
  // in the shop that found it.
  //
  // A side port sits on the machine's midline, and only steps off it (portDy) when
  // another of that machine's runs actually landed on the same side — otherwise a
  // lone secondary port hung below the middle of the edge for no visible reason.
  // It costs the router nothing either way: entry() rounds a side port to the
  // nearest lattice ROW, and an offset this small rounds to the same one.
  if (n.glyph === 'secondaryPort') {
    const top: Port = { pt: { x: n.x, y: n.y - SECONDARY_PORT_HALF - INLET_GAP }, dir: 3 };
    const b = n.hostBox;
    if (!b) return [top];
    const midY = (b.y0 + b.y1) / 2 + (n.portDy ?? 0);
    return [
      top,
      { pt: { x: b.x0, y: midY }, dir: 2, bias: TOP_ENTRY_BIAS },
      { pt: { x: b.x1, y: midY }, dir: 0, bias: TOP_ENTRY_BIAS },
    ];
  }
  return allSidePorts(n);
}

/** Three of the four sides: `keep` (top for an inlet, bottom for an outlet) plus
 *  left and right. Tools take top, left and right — a trunk really does reach a
 *  machine from whichever side it runs down, but approaching from underneath reads
 *  wrong on a top-down shop plan, so the fourth side stays closed. */
function sidePorts(n: SceneNode, keep: Dir): Port[] {
  return allSidePorts(n).filter(p => p.dir !== opposite(keep));
}

function allSidePorts(n: SceneNode): Port[] {
  const hw = halfW(n), hh = halfH(n);
  return [
    { pt: { x: n.x + hw, y: n.y }, dir: 0 },
    { pt: { x: n.x, y: n.y + hh }, dir: 1 },
    { pt: { x: n.x - hw, y: n.y }, dir: 2 },
    { pt: { x: n.x, y: n.y - hh }, dir: 3 },
  ];
}

// ── the lattice ──────────────────────────────────────────────────────────────

class Lattice {
  readonly gx0: number; readonly gy0: number;
  readonly w: number; readonly h: number;
  private readonly blocked: Uint8Array;
  private readonly hugs: Uint8Array;

  constructor(opts: GridOpts) {
    const { bounds, obstacles } = opts;
    this.gx0 = Math.floor((bounds.x0 - CELL - PAD) / LATTICE);
    this.gy0 = Math.floor((bounds.y0 - CELL - PAD) / LATTICE);
    this.w = Math.ceil((bounds.x1 + CELL - PAD) / LATTICE) - this.gx0 + 1;
    this.h = Math.ceil((bounds.y1 + CELL - PAD) / LATTICE) - this.gy0 + 1;
    this.blocked = new Uint8Array(this.w * this.h);
    this.hugs = new Uint8Array(this.w * this.h);

    for (let iy = 0; iy < this.h; iy++) {
      for (let ix = 0; ix < this.w; ix++) {
        const p = this.pt(ix, iy);
        if (obstacles.some(b => ptInBox(p, b))) this.blocked[iy * this.w + ix] = 1;
      }
    }
    // A node "hugs" if it sits next to a blocked one. Costing these slightly keeps
    // runs off the clearance boundary when there's an equally short lane further out.
    for (let iy = 0; iy < this.h; iy++) {
      for (let ix = 0; ix < this.w; ix++) {
        if (this.blocked[iy * this.w + ix]) continue;
        for (let d = 0 as Dir; d < 4; d++) {
          const nx = ix + DX[d], ny = iy + DY[d];
          if (nx < 0 || ny < 0 || nx >= this.w || ny >= this.h) continue;
          if (this.blocked[ny * this.w + nx]) { this.hugs[iy * this.w + ix] = 1; break; }
        }
      }
    }
  }

  idx(ix: number, iy: number): number { return iy * this.w + ix; }
  inside(ix: number, iy: number): boolean { return ix >= 0 && iy >= 0 && ix < this.w && iy < this.h; }
  isBlocked(ix: number, iy: number): boolean { return !this.inside(ix, iy) || this.blocked[this.idx(ix, iy)] === 1; }
  isHug(ix: number, iy: number): boolean { return this.inside(ix, iy) && this.hugs[this.idx(ix, iy)] === 1; }

  /** Lattice index → board pixels. */
  pt(ix: number, iy: number): Pt {
    return { x: PAD + (this.gx0 + ix) * LATTICE, y: PAD + (this.gy0 + iy) * LATTICE };
  }
  /** Absolute lattice coordinate, for stable edge keys across solves. */
  abs(ix: number, iy: number): [number, number] { return [this.gx0 + ix, this.gy0 + iy]; }

  /** The first lattice node a duct reaches after leaving `port`, stepping outward.
   *  The port's transverse coordinate is always already on the lattice — every cell
   *  centre and every outlet is — so only the along-direction coordinate snaps. */
  entry(port: Port): { ix: number; iy: number } | null {
    const gxf = (port.pt.x - PAD) / LATTICE, gyf = (port.pt.y - PAD) / LATTICE;
    let gx: number, gy: number;
    const EPS = 1e-6;
    switch (port.dir) {
      case 0: gx = Math.ceil(gxf + EPS); gy = Math.round(gyf); break;
      case 2: gx = Math.floor(gxf - EPS); gy = Math.round(gyf); break;
      case 1: gy = Math.ceil(gyf + EPS); gx = Math.round(gxf); break;
      default: gy = Math.floor(gyf - EPS); gx = Math.round(gxf); break;
    }
    const ix = gx - this.gx0, iy = gy - this.gy0;
    return this.inside(ix, iy) ? { ix, iy } : null;
  }
}

// ── A* ───────────────────────────────────────────────────────────────────────

/** Min-heap over (priority, state). Small boards, but this runs on every drag
 *  frame, so no sorting the open list. */
class Heap {
  private a: number[] = [];   // flat [prio, state, prio, state, …]
  get size(): number { return this.a.length >> 1; }
  push(prio: number, state: number): void {
    let i = this.a.length >> 1;
    this.a.push(prio, state);
    while (i > 0) {
      const p = (i - 1) >> 1;
      if (this.a[p << 1] <= this.a[i << 1]) break;
      this.swap(i, p); i = p;
    }
  }
  pop(): { prio: number; state: number } {
    const prio = this.a[0], state = this.a[1];
    const lastPrio = this.a[this.a.length - 2], lastState = this.a[this.a.length - 1];
    this.a.length -= 2;
    if (this.a.length) {
      this.a[0] = lastPrio; this.a[1] = lastState;
      let i = 0;
      for (;;) {
        const l = i * 2 + 1, r = l + 1;
        let m = i;
        if (l < this.size && this.a[l << 1] < this.a[m << 1]) m = l;
        if (r < this.size && this.a[r << 1] < this.a[m << 1]) m = r;
        if (m === i) break;
        this.swap(i, m); i = m;
      }
    }
    return { prio, state };
  }
  private swap(i: number, j: number): void {
    const pi = i << 1, pj = j << 1;
    [this.a[pi], this.a[pj]] = [this.a[pj], this.a[pi]];
    [this.a[pi + 1], this.a[pj + 1]] = [this.a[pj + 1], this.a[pi + 1]];
  }
}

/**
 * Route one duct from any of `from` to any of `to`.
 *
 * Both endpoints offer several ports; rather than trying each pair in turn, every
 * start port seeds the open set at cost 0 and every target port is an accepted
 * goal — one search picks the cheapest pairing, which is what lets a run leave the
 * collector sideways when that is genuinely the shorter way round.
 */
export function routeOne(from: Port[], to: Port[], opts: GridOpts): RouteResult {
  const grid = new Lattice(opts);
  const used = opts.usedEdges ?? new Set<string>();
  const crossed = opts.usedNodes ?? new Set<string>();
  const prior = opts.priorEdges ?? new Set<string>();

  // Goal ports, keyed by the lattice node they're reached from.
  const goals = new Map<number, { port: Port; approach: Dir }[]>();
  for (const port of to) {
    const e = grid.entry(port);
    if (!e || grid.isBlocked(e.ix, e.iy)) continue;
    const cell = grid.idx(e.ix, e.iy);
    const list = goals.get(cell) ?? [];
    list.push({ port, approach: opposite(port.dir) });
    goals.set(cell, list);
  }

  const stateOf = (ix: number, iy: number, d: Dir): number => (grid.idx(ix, iy) << 2) | d;
  const nStates = grid.w * grid.h * 4;
  const g = new Float64Array(nStates).fill(Infinity);
  const parent = new Int32Array(nStates).fill(-1);
  const open = new Heap();

  // Heuristic: manhattan lattice distance at the cheapest an edge can ever be, so
  // it never overestimates even where the prior-route discount applies.
  const goalCells = [...goals.keys()].map(c => ({ ix: c % grid.w, iy: Math.floor(c / grid.w) }));
  const h = (ix: number, iy: number): number => {
    let best = Infinity;
    for (const gc of goalCells) best = Math.min(best, Math.abs(gc.ix - ix) + Math.abs(gc.iy - iy));
    return best === Infinity ? 0 : best * MIN_STEP;
  };

  const starts = new Map<number, Port>();
  for (const port of from) {
    const e = grid.entry(port);
    if (!e || grid.isBlocked(e.ix, e.iy)) continue;
    const s = stateOf(e.ix, e.iy, port.dir);
    if (g[s] === 0) continue;
    g[s] = 0;
    starts.set(s, port);
    open.push(h(e.ix, e.iy), s);
  }

  let bestGoal = -1, bestGoalCost = Infinity, bestGoalPort: Port | null = null;
  if (goalCells.length && starts.size) {
    while (open.size) {
      const { prio, state } = open.pop();
      const cell = state >> 2, d = (state & 3) as Dir;
      const ix = cell % grid.w, iy = Math.floor(cell / grid.w);
      if (prio > g[state] + h(ix, iy) + 1e-9) continue;   // stale heap entry

      const here = goals.get(cell);
      if (here) {
        for (const goal of here) {
          const cost = g[state] + (d === goal.approach ? 0 : TURN) + (goal.port.bias ?? 0);
          if (cost < bestGoalCost) { bestGoalCost = cost; bestGoal = state; bestGoalPort = goal.port; }
        }
      }
      // Everything still in the queue costs at least g[state]; once that alone
      // exceeds the best goal we have, nothing can improve on it.
      if (g[state] >= bestGoalCost) break;

      for (let nd = 0 as Dir; nd < 4; nd = (nd + 1) as Dir) {
        const nx = ix + DX[nd], ny = iy + DY[nd];
        if (grid.isBlocked(nx, ny)) continue;
        const [ax, ay] = grid.abs(ix, iy), [bx, by] = grid.abs(nx, ny);
        const key = edgeKey(ax, ay, bx, by);
        if (opts.blockUsed && used.has(key)) continue;   // no shared lanes on this pass
        let cost = STEP;
        if (nd !== d) cost += TURN;
        if (used.has(key)) cost += USED;
        if (crossed.has(nodeKey(bx, by))) cost += CROSS;
        if (grid.isHug(nx, ny)) cost += HUG;
        if (prior.has(key)) cost -= REUSE;
        const ns = stateOf(nx, ny, nd);
        const ng = g[state] + cost;
        if (ng < g[ns]) { g[ns] = ng; parent[ns] = state; open.push(ng + h(nx, ny), ns); }
      }
    }
  }

  if (bestGoal < 0 || !bestGoalPort) return fallback(from, to, opts);

  // Walk back to a start state, then out through both ports.
  const chain: number[] = [];
  for (let s = bestGoal; s !== -1; s = parent[s]) {
    chain.push(s);
    if (starts.has(s)) break;
  }
  chain.reverse();
  const startPort = starts.get(chain[0]);
  if (!startPort) return fallback(from, to, opts);

  const pts: Pt[] = [startPort.pt];
  const edges = new Set<string>();
  const nodes = new Set<string>();
  let prevCell = -1;
  for (const s of chain) {
    const cell = s >> 2;
    const ix = cell % grid.w, iy = Math.floor(cell / grid.w);
    if (cell !== prevCell) {
      const [bx, by] = grid.abs(ix, iy);
      if (prevCell >= 0) {
        const px = prevCell % grid.w, py = Math.floor(prevCell / grid.w);
        const [ax, ay] = grid.abs(px, py);
        edges.add(edgeKey(ax, ay, bx, by));
      }
      nodes.add(nodeKey(bx, by));
      pts.push(grid.pt(ix, iy));
      prevCell = cell;
    }
  }
  pts.push(bestGoalPort.pt);
  return { pts: simplifyPts(squareOffEnds(pts, startPort, bestGoalPort)), edges, nodes, ok: true };
}

/**
 * Keep the first and last segments square.
 *
 * The lattice is a grid, but a PORT need not sit on it — a port offset along its
 * edge (to clear another port on the same edge) is off-lattice by design. Joining
 * the lattice straight to it drew a diagonal for the last few pixels, which is the
 * one place ductwork stopped looking like ductwork: real pipe turns at 90°, and a
 * short jog across says "it enters here" where a slanted line says "something is
 * slightly wrong".
 *
 * The elbow goes in along the port's OWN axis: a top or bottom port is approached
 * vertically, so the jog runs along the lattice row and drops in; a side port is
 * approached horizontally, so it runs down the lattice column and comes in level.
 * The corner therefore sits on the lattice line the run already occupies and never
 * cuts a new way across anything.
 *
 * simplifyPts() drops it again whenever the port was on-lattice after all, so an
 * ordinary run is untouched.
 */
function squareOffEnds(pts: Pt[], start: Port, goal: Port): Pt[] {
  if (pts.length < 2) return pts;
  const out = pts.slice();
  const vertical = (d: Dir) => d === 1 || d === 3;
  const corner = (port: Port, next: Pt): Pt =>
    vertical(port.dir) ? { x: port.pt.x, y: next.y } : { x: next.x, y: port.pt.y };

  const goalNext = out[out.length - 2];
  const g = corner(goal, goalNext);
  if (Math.abs(g.x - goal.pt.x) > 0.5 || Math.abs(g.y - goal.pt.y) > 0.5) {
    out.splice(out.length - 1, 0, g);
  }
  const startNext = out[1];
  const st = corner(start, startNext);
  if (Math.abs(st.x - start.pt.x) > 0.5 || Math.abs(st.y - start.pt.y) > 0.5) {
    out.splice(1, 0, st);
  }
  return out;
}

/** Boxed in: emit the straight dogleg so there's still something to draw, and let
 *  the caller flag the route. */
function fallback(from: Port[], to: Port[], opts: GridOpts): RouteResult {
  const a = from[0]?.pt, b = to[to.length - 1]?.pt;
  if (!a || !b) return { pts: [], edges: new Set(), nodes: new Set(), ok: false };
  const mid = { x: a.x, y: (a.y + b.y) / 2 };
  const pts = Math.abs(a.x - b.x) < 0.5
    ? [a, b]
    : [a, mid, { x: b.x, y: mid.y }, b];
  return { pts: simplifyPts(pts), edges: new Set(), nodes: new Set(), ok: false };
}

/** Obstacle set for one duct: every device except its own endpoints. Junctions are
 *  tee points on a run, not bodies, so they never block. */
export function obstaclesFor(nodes: SceneNode[], exempt: ReadonlySet<string>): Box[] {
  const out: Box[] = [];
  for (const n of nodes) {
    if (exempt.has(n.id) || n.glyph === 'junction') continue;
    // A SECONDARY PORT is not an obstacle. It is a 9px glyph riding the top edge of a machine
    // whose own box is already in this list, so it adds nothing to steer around —
    // but inflated by CLEARANCE it becomes a ~48px box straddling that edge, and the
    // lattice node a top entry arrives through sits 20px above the edge, INSIDE it.
    // That made isBlocked() throw the machine's own top port away as a goal, so a
    // machine grew a secondary port and its primary run promptly fled to a side entry.
    if (n.glyph === 'secondaryPort') continue;
    out.push(deviceBox(n, CLEARANCE));
  }
  return out;
}

export { CLEARANCE, GATE_PAD, segBoxHit };
