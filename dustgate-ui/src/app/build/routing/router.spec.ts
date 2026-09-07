/** Router conformance cases R1–R8, matching the validation mockups.
 *
 *  Plain TypeScript on purpose — no Angular, no browser. Run with:
 *      npm run test:routing
 */

import { type SceneNode, CAP_W, CELL, CLEARANCE, PAD, PRIMARY_PORT_DX, SECONDARY_PORT_DX, TOOL_HALF,
         cellX, cellY, deviceBox, segBoxHit } from './geometry';
import { type Scene, type RoutedDuct, Router, routeAll, routeAllShared, sceneBounds } from './router';
import { outPorts } from './route-grid';

// ── harness ──────────────────────────────────────────────────────────────────

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
function eqPath(name: string, got: readonly { x: number; y: number }[], want: number[][]): void {
  const g = got.map(p => [Math.round(p.x), Math.round(p.y)]);
  ok(name, JSON.stringify(g) === JSON.stringify(want), `got  ${JSON.stringify(g)}\n       want ${JSON.stringify(want)}`);
}
function group(name: string): void { console.log(`\n${name}`); }

// ── scene builders ───────────────────────────────────────────────────────────

const at = (col: number, row: number) => ({ x: PAD + col * CELL, y: PAD + row * CELL });
// A collector's half-extent is COLLECTOR_HALF = 38 — it grew from 30 on
// 2026-08-25, when the glyph became a barrel carrying its own name and plug row.
// That is why every run that starts on a collector's edge below starts 8 further
// out than it used to.
const collector = (id: string, col: number, row: number): SceneNode => ({ id, glyph: 'collector', isUnit: false, span: 1, ...at(col, row) });
const tool = (id: string, col: number, row: number): SceneNode => ({ id, glyph: 'tool', isUnit: false, span: 1, ...at(col, row) });
const unit = (id: string, col: number, row: number, span: number): SceneNode => ({ id, glyph: 'slidingGate', isUnit: true, span, ...at(col, row) });
/** A controller board. It carries no duct — it is only ever an obstacle, which is
 *  the whole meaning of a board owning its cell. */
const board = (id: string, col: number, row: number): SceneNode => ({ id, glyph: 'board', isUnit: false, span: 1, ...at(col, row) });

function scene(nodes: SceneNode[], ducts: Scene['ducts']): Scene {
  return { nodes, ducts, bounds: sceneBounds(nodes) };
}

/** Every path segment must stay out of every device box it isn't attached to. */
function crossesADevice(s: Scene, childId: string, pts: readonly { x: number; y: number }[]): string | null {
  const duct = s.ducts.find(d => d.childId === childId);
  const parentId = duct?.outlet?.unitId ?? duct?.parentId;
  for (const n of s.nodes) {
    if (n.id === childId || n.id === parentId || n.glyph === 'junction') continue;
    const box = deviceBox(n, CLEARANCE);
    for (let i = 0; i < pts.length - 1; i++) {
      if (segBoxHit(pts[i], pts[i + 1], box)) return `${childId} crosses ${n.id} on segment ${i}`;
    }
  }
  return null;
}

/** "No lasso", as something a test can actually check: the run never reverses
 *  along an axis it has already committed to. */
function reverses(pts: readonly { x: number; y: number }[]): boolean {
  let sx = 0, sy = 0;
  for (let i = 0; i < pts.length - 1; i++) {
    const dx = Math.sign(Math.round(pts[i + 1].x - pts[i].x));
    const dy = Math.sign(Math.round(pts[i + 1].y - pts[i].y));
    if (dx && sx && dx !== sx) return true;
    if (dy && sy && dy !== sy) return true;
    if (dx) sx = dx;
    if (dy) sy = dy;
  }
  return false;
}

const path = (r: ReadonlyMap<string, RoutedDuct>, id: string) => r.get(id)?.pts ?? [];

// ── R1 · demo layout stays a straight drop ───────────────────────────────────

group('R1  demo layout stays a straight drop');
{
  const s = scene(
    [collector('dc', 0, 0), unit('sel', 0, 1, 4),
     tool('saw', 0, 2), tool('band', 1, 2), tool('router', 2, 2), tool('sander', 3, 2)],
    [{ childId: 'sel', parentId: 'dc' },
     { childId: 'saw', outlet: { unitId: 'sel', index: 0 } },
     { childId: 'band', outlet: { unitId: 'sel', index: 1 } },
     { childId: 'router', outlet: { unitId: 'sel', index: 2 } },
     { childId: 'sander', outlet: { unitId: 'sel', index: 3 } }],
  );
  const r = routeAll(s);
  ok('routes all 5 ducts', r.size === 5);
  eqPath('dc→sel is the straight drop', path(r, 'sel'), [[64, 102], [64, 137]]);
  eqPath('sel.b1→saw', path(r, 'saw'), [[64, 207], [64, 246]]);
  eqPath('sel.b2→band', path(r, 'band'), [[172, 207], [172, 246]]);
  eqPath('sel.b3→router', path(r, 'router'), [[280, 207], [280, 246]]);
  eqPath('sel.b4→sander', path(r, 'sander'), [[388, 207], [388, 246]]);
  ok('every path is 2 points, 0 bends', [...r.values()].every(v => v.pts.length === 2));
  ok('every path is vertical', [...r.values()].every(v => Math.abs(v.pts[0].x - v.pts[1].x) < 0.5));
  ok('nothing crosses a device', [...r.keys()].every(id => !crossesADevice(s, id, path(r, id))));
  ok('all routes solved (no fallback)', [...r.values()].every(v => v.ok));
}

// ── R3 · sideways run enters the tool from the side ──────────────────────────

group('R3  sideways runs enter the tool from the side');
{
  const s = scene([collector('dc', 0, 0), tool('planer', 1, 0)], [{ childId: 'planer', parentId: 'dc' }]);
  const r = routeAll(s);
  eqPath('collector right port → tool left port, flat', path(r, 'planer'), [[102, 64], [134, 64]]);

  // A collector may leave from its TOP. Without that port, a collector standing
  // directly beside a wide unit had to reach the unit's top inlet by turning up
  // out of a SIDE — and beside an adjacent unit the two clearance boxes overlap,
  // so there is no column to turn on and the run wrapped the long way round.
  {
    const c = { id: 'dc', glyph: 'collector' as const, isUnit: false, span: 1, x: 200, y: 200 };
    const tops = outPorts(c).filter(p => p.dir === 3);
    ok('a collector offers a top port', tops.length === 1);
    ok('...at the top edge, on its centreline',
       tops[0].pt.x === c.x && tops[0].pt.y < c.y);
    ok('and still offers all three of the others', outPorts(c).length === 4);
  }

  // Mirrored: the tool on the left uses its right port and is equally flat.
  const s2 = scene([collector('dc', 1, 0), tool('planer', 0, 0)], [{ childId: 'planer', parentId: 'dc' }]);
  eqPath('mirrored, tool right port', path(routeAll(s2), 'planer'), [[134, 64], [102, 64]]);

  // Directly below: top port still wins, because it costs no bends there.
  const s3 = scene([collector('dc', 0, 0), tool('saw', 0, 1)], [{ childId: 'saw', parentId: 'dc' }]);
  eqPath('directly below → top port, straight drop', path(routeAll(s3), 'saw'), [[64, 102], [64, 138]]);

  // No route may ever enter a tool from underneath.
  const s4 = scene([collector('dc', 0, 2), tool('saw', 0, 0)], [{ childId: 'saw', parentId: 'dc' }]);
  const p4 = path(routeAll(s4), 'saw');
  const last = p4[p4.length - 1];
  ok('tool exposes no bottom port', Math.round(last.y) !== Math.round(PAD + 0 * CELL + 24),
     `terminated at ${JSON.stringify(last)}`);
}

// ── R3b · a near-tie between top and side entry favours the top ─────────────

group('R3b top entry is preferred when it is roughly as cheap as a side one');
{
  // Diagonal offset, one cell each way: the tool's left port and its top port are
  // almost exactly as far from the collector's right port (152px, one bend,
  // either way) — the kind of near-tie R3's flat and directly-below cases don't
  // reach, and where the router used to have no reason to prefer one over the
  // other. TOP_ENTRY_BIAS (route-grid.ts) is what decides it now.
  const s = scene([collector('dc', 0, 0), tool('t', 1, 1)], [{ childId: 't', parentId: 'dc' }]);
  eqPath('diagonal down-right → still enters from the top', path(routeAll(s), 't'),
    [[102, 64], [172, 64], [172, 138]]);

  // The bias is a tiebreaker, not a mandate — R3 already covers the case where a
  // side entry is CLEARLY shorter (same row, flat) and confirms it still wins
  // there; this just adds the near-tie this bias exists for.
}

// ── R3c · a secondary port must not push its machine's own run off the top ──────────

group('R3c a machine keeps its straight drop after growing a secondary port');
{
  const secondaryPort = (id: string, on: SceneNode, dx: number): SceneNode =>
    ({ id, glyph: 'secondaryPort', isUnit: false, span: 1, x: on.x + dx, y: on.y - 34 });

  // Gate directly above the tool: the run is a straight drop, and must stay one.
  const saw = tool('saw', 0, 1);
  const plain = scene([collector('dc', 0, 0), saw], [{ childId: 'saw', parentId: 'dc' }]);
  const before = path(routeAll(plain), 'saw');
  ok('baseline is a straight drop', before.length === 2, JSON.stringify(before));

  // Same shop, but the saw now has a secondary port on its top edge and its inlet has
  // shifted to make room. That port is NOT an obstacle — inflated by CLEARANCE it would
  // cover the very lattice node a top entry arrives through, which used to make the
  // router throw the top port away and come in from the side instead.
  const grown: SceneNode = { ...saw, inletDx: PRIMARY_PORT_DX };
  const aux = secondaryPort('saw-aux', saw, 15);
  const after = scene([collector('dc', 0, 0), grown, aux],
                      [{ childId: 'saw', parentId: 'dc' }, { childId: 'saw-aux', parentId: 'dc' }]);
  const p = path(routeAll(after), 'saw');
  const end = p[p.length - 1];
  ok('still enters from the TOP, not a side',
     Math.round(end.y) < Math.round(cellY(1)), `ended at ${JSON.stringify(end)}`);
  ok('and lands on the primary port, not the centreline',
     Math.round(end.x) === Math.round(cellX(0) + PRIMARY_PORT_DX),
     `ended at x=${Math.round(end.x)}, primary port at ${cellX(0) + PRIMARY_PORT_DX}`);
}

// ── R3d · a secondary port fed from below comes in the side, not over the shop ──

group('R3d a secondary port takes a side entry rather than looping over the shop');
{
  // The shape that found this: a machine high on the canvas, a second collector
  // BELOW and to the right of it. A secondary port could only be entered from
  // directly above, so the only legal route climbed to the top of the board and
  // came back down over everything in between.
  const saw = tool('saw', 0, 1);
  const dc2 = collector('dc2', 2, 3);
  const aux: SceneNode = {
    id: 'saw-aux', glyph: 'secondaryPort', isUnit: false, span: 1,
    x: saw.x + SECONDARY_PORT_DX, y: saw.y - 34, hostBox: deviceBox(saw),
  };
  const s = scene([collector('dc', 0, 0), { ...saw, inletDx: PRIMARY_PORT_DX }, dc2, aux],
                  [{ childId: 'saw', parentId: 'dc' }, { childId: 'saw-aux', parentId: 'dc2' }]);
  const p = path(routeAll(s), 'saw-aux');
  const end = p[p.length - 1];
  const topOfSaw = saw.y - TOOL_HALF;

  ok('it lands on the machine, not on the 9px glyph',
     Math.abs(end.x - saw.x) <= 38 + 1 && Math.abs(end.y - saw.y) <= TOOL_HALF + 1,
     `ended at ${JSON.stringify(end)}`);
  ok('from the RIGHT side, the side its collector is on',
     Math.round(end.x) === Math.round(saw.x + 38), `ended at x=${Math.round(end.x)}`);
  // Changed 2026-08-20. It used to be offset below the midline unconditionally, to
  // guard a stack that only happens when the machine's OTHER run lands on the same
  // side — and here the primary comes down from the top, so the side is this port's
  // alone and it takes the middle of it. The offset is now handed in as portDy by
  // whoever can see both landings; the case it exists for is the next block.
  ok('and dead centre on that side, because nothing else landed there',
     Math.round(end.y) === Math.round(saw.y), `ended at y=${Math.round(end.y)}, midline ${saw.y}`);
  // The whole point: nothing in the run goes ABOVE the machine any more.
  ok('and never climbs over the top of the shop to get there',
     p.every(pt => pt.y > topOfSaw - 20), JSON.stringify(p));

  // The case the offset DOES exist for: portDy set, and the run lands off-midline.
  const shared = scene([collector('dc', 0, 0), { ...saw, inletDx: PRIMARY_PORT_DX }, dc2,
                        { ...aux, portDy: SECONDARY_PORT_DX }],
                       [{ childId: 'saw', parentId: 'dc' }, { childId: 'saw-aux', parentId: 'dc2' }]);
  const sp = path(routeAll(shared), 'saw-aux');
  ok('portDy steps it off the midline so it cannot stack on a port sharing that side',
     Math.round(sp[sp.length - 1].y) === Math.round(saw.y + SECONDARY_PORT_DX),
     `ended at y=${Math.round(sp[sp.length - 1].y)}`);
  // …and the approach to that off-lattice point stays square.
  const lastTwo = sp.slice(-2);
  ok('and the last segment is level, not a diagonal jab at the port',
     Math.abs(lastTwo[0].y - lastTwo[1].y) < 0.5, JSON.stringify(lastTwo));

  // Without a hostBox it is still top-entry only — the pre-D-41 behaviour, kept as
  // the fallback for any caller that hasn't got a machine to hand.
  const orphan = scene([collector('dc', 0, 0), saw, dc2, { ...aux, hostBox: undefined }],
                       [{ childId: 'saw', parentId: 'dc' }, { childId: 'saw-aux', parentId: 'dc2' }]);
  const op = path(routeAll(orphan), 'saw-aux');
  ok('no host box → still comes down from above',
     op[op.length - 1].y < topOfSaw, JSON.stringify(op.at(-1)));
}

// ── R4 · obstacle in the span ────────────────────────────────────────────────

// The y of every tool endpoint moved 256 → 246 (and 148 → 138) on 2026-08-15,
// when TOOL_HALF went 24 → 34: a tool body now carries a second row for its smart
// plug, so its top edge — where a duct lands — is 10 higher. Shapes, bends and
// lanes are all unchanged; only where the drop stops.
group('R4  obstacle in the span — one detour, no lasso');
{
  const s = scene(
    [unit('gate', 0, 1, 2), tool('band', 2, 2), tool('sander', 3, 2)],
    [{ childId: 'sander', outlet: { unitId: 'gate', index: 1 } }],
  );
  const r = routeAll(s);
  const p = path(r, 'sander');
  eqPath('lane above the obstacle, 2 bends', p, [[172, 207], [172, 226], [388, 226], [388, 246]]);
  ok('no reversal (no lasso)', !reverses(p));
  ok('clears the obstacle box', !crossesADevice(s, 'sander', p));

  // With the obstacle gone the side port is reachable, and a 1-bend approach into
  // the tool's left side beats going over the top. The detour above is therefore
  // driven purely by the obstacle, not by a hardcoded preference for entering tops.
  const s2 = scene([unit('gate', 0, 1, 2), tool('sander', 3, 2)], [{ childId: 'sander', outlet: { unitId: 'gate', index: 1 } }]);
  const p2 = path(routeAll(s2), 'sander');
  eqPath('obstacle removed → 1-bend side entry', p2, [[172, 207], [172, 280], [350, 280]]);
  ok('the detour is caused by the obstacle, not by the port table', p2.length < p.length);
}

// ── R4b · a board is an obstacle like any other ──────────────────────────────

// Boards came back onto the canvas on 2026-08-16 (docs/boards-on-canvas-plan.md).
// A board owns its cell exclusively, and this is what that has to mean for the
// router: a duct steers around the hardware rather than being drawn through it.
group('R4b a duct routes around a board');
{
  // The same shape as R4, with a BOARD standing where the obstacle tool stood.
  const s = scene(
    [unit('gate', 0, 1, 2), board('brain', 2, 2), tool('sander', 3, 2)],
    [{ childId: 'sander', outlet: { unitId: 'gate', index: 1 } }],
  );
  const p = path(routeAll(s), 'sander');
  ok('clears the board box', !crossesADevice(s, 'sander', p), JSON.stringify(p));
  ok('no reversal (no lasso)', !reverses(p));
  // Take the board away and the straight side entry comes back, so the detour is
  // the board's doing and not a preference baked into the port table.
  const s2 = scene([unit('gate', 0, 1, 2), tool('sander', 3, 2)], [{ childId: 'sander', outlet: { unitId: 'gate', index: 1 } }]);
  const p2 = path(routeAll(s2), 'sander');
  ok('the detour is caused by the board', p2.length < p.length,
     `with ${JSON.stringify(p)} without ${JSON.stringify(p2)}`);
}

// ── R5 · drag stability ──────────────────────────────────────────────────────

group('R5  drag stability — the prefix must not move');
{
  const build = (sanderCol: number) => scene(
    [unit('gate', 0, 1, 2), tool('saw', 0, 2), tool('sander', sanderCol, 2)],
    [{ childId: 'saw', outlet: { unitId: 'gate', index: 0 } },
     { childId: 'sander', outlet: { unitId: 'gate', index: 1 } }],
  );
  const router = new Router();
  const r0 = router.routes(build(2));
  const saw0 = JSON.stringify(path(r0, 'saw'));
  const prefixes: string[] = [];
  for (const col of [2, 3, 4]) {
    const r = router.routes(build(col), new Set(['saw']));
    prefixes.push(JSON.stringify(path(r, 'sander').slice(0, 2)));
    ok(`col ${col}: frozen duct is byte-identical`, JSON.stringify(path(r, 'saw')) === saw0);
  }
  ok('shared prefix identical across the drag', new Set(prefixes).size === 1, prefixes.join(' / '));

  // Memoization: the same scene twice must not re-solve.
  const s = build(3);
  const a = router.routes(s), b = router.routes(s);
  ok('identical scene returns the memoized map', a === b);
}

// ── R7 · parallel runs separate ──────────────────────────────────────────────

group('R7  parallel runs separate on their own');
{
  const s = scene(
    [unit('gate', 0, 1, 2), tool('band', 3, 2), tool('planer', 3, 3)],
    [{ childId: 'band', outlet: { unitId: 'gate', index: 0 } },
     { childId: 'planer', outlet: { unitId: 'gate', index: 1 } }],
  );
  const r = routeAll(s);
  console.log(`       band   ${JSON.stringify(path(r, 'band').map(p => [p.x, p.y]))}`);
  console.log(`       planer ${JSON.stringify(path(r, 'planer').map(p => [p.x, p.y]))}`);
  const eb = r.get('band')!.edges, ep = r.get('planer')!.edges;
  ok('the two runs share no lattice edge', ![...ep].some(e => eb.has(e)));
  ok('neither crosses a device', !crossesADevice(s, 'band', path(r, 'band')) && !crossesADevice(s, 'planer', path(r, 'planer')));
  ok('deterministic across solves', JSON.stringify([...routeAll(s)]) === JSON.stringify([...routeAll(s)]));
}

// ── R9 · a gate never emits from its top ─────────────────────────────────────

group('R9  no duct leaves a gate via the top');
{
  const valve = (id: string, col: number, row: number): SceneNode =>
    ({ id, glyph: 'ballvalve', isUnit: false, span: 1, ...at(col, row) });

  // Child below, beside, and ABOVE the valve — the last is the one that used to
  // tempt the router into leaving through the top.
  for (const [name, tcol, trow] of [['below', 1, 2], ['beside', 3, 1], ['above', 3, 0]] as const) {
    const s = scene([valve('bv', 1, 1), tool('drill', tcol, trow)], [{ childId: 'drill', parentId: 'bv' }]);
    const p = path(routeAll(s), 'drill');
    const bv = s.nodes[0];
    ok(`child ${name}: leaves at or below the valve centre`, p.length > 0 && p[0].y >= bv.y - 0.5,
       `first point ${JSON.stringify(p[0])}, valve y ${bv.y}`);
  }

  // And is never entered from underneath.
  const s2 = scene([collector('dc', 1, 2), valve('bv', 1, 0)], [{ childId: 'bv', parentId: 'dc' }]);
  const p2 = path(routeAll(s2), 'bv');
  const bv2 = s2.nodes[1];
  ok('valve is not entered from below', p2.length > 0 && p2[p2.length - 1].y <= bv2.y + 0.5,
     `last point ${JSON.stringify(p2[p2.length - 1])}`);
}

// ── R10 · a duct never crosses its own parent's body ─────────────────────────

group('R10 a duct never crosses its own endpoints');
{
  // Tool parked ABOVE the gate row: the only way there is around the gate, and the
  // parent used to be exempt from its own obstacle set, so the run went straight
  // through the bar.
  const s = scene(
    [unit('gate', 0, 1, 4), tool('saw', 3, 0)],
    [{ childId: 'saw', outlet: { unitId: 'gate', index: 0 } }],
  );
  const p = path(routeAll(s), 'saw');
  const gate = s.nodes[0];
  const box = deviceBox(gate);
  let through = false;
  for (let i = 0; i < p.length - 1; i++) if (segBoxHit(p[i], p[i + 1], box)) through = true;
  ok('does not cross the gate it hangs off', !through, JSON.stringify(p.map(q => [q.x, q.y])));
  ok('still finds a route', routeAll(s).get('saw')!.ok);

  // The same must hold for the child's own body.
  const cbox = deviceBox(s.nodes[1]);
  let throughChild = false;
  for (let i = 0; i < p.length - 1; i++) if (segBoxHit(p[i], p[i + 1], cbox)) throughChild = true;
  ok('does not cross the tool it feeds', !throughChild);
}

// ── R11 · crossings are costed ───────────────────────────────────────────────

group('R11 crossings are avoided when there is a way round');
{
  // Two gate drops, and a third run that has to get past both. With crossings free
  // it cut straight across them; with the node cost it takes the lane underneath.
  const s = scene(
    [unit('gate', 0, 0, 3), tool('a', 0, 2), tool('b', 1, 2), tool('far', 3, 1)],
    [{ childId: 'a', outlet: { unitId: 'gate', index: 0 } },
     { childId: 'b', outlet: { unitId: 'gate', index: 1 } },
     { childId: 'far', outlet: { unitId: 'gate', index: 2 } }],
  );
  const r = routeAll(s);
  const nodesOf = (id: string) => r.get(id)!.nodes;
  const shared = [...nodesOf('far')].filter(n => nodesOf('a').has(n) || nodesOf('b').has(n));
  console.log(`       far ${JSON.stringify(path(r, 'far').map(q => [q.x, q.y]))}`);
  ok('the long run crosses neither drop', shared.length === 0, `shared nodes: ${shared.join(' ')}`);
  ok('every route still solved', [...r.values()].every(v => v.ok));
}

// ── R12 · no two ducts ever share a lane ─────────────────────────────────────

group('R12 settled layouts have no overlapping duct');
{
  // Randomised, fixed seed: 400 shops of a gate feeding scattered tools. Two ducts
  // running along the same lattice edge is what "the ducts overlap" looks like on
  // screen, and it used to happen whenever a detour cost more than the sharing
  // penalty — no cost setting fixes that, so sharing is now a hard constraint with
  // a reorder-and-retry behind it.
  let seed = 12345;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const pickN = (n: number) => Math.floor(rnd() * n);

  let bad = 0, worstScene = '';
  for (let t = 0; t < 400; t++) {
    const span = 2 + pickN(3);
    const nodes: SceneNode[] = [collector('dc', 0, 0), unit('gate', 0, 2, span)];
    const ducts: Scene['ducts'] = [{ childId: 'gate', parentId: 'dc' }];
    const taken = new Set(['0,0']);
    for (let i = 0; i < span; i++) {
      let c = 0, r = 0, k = 0;
      do { c = pickN(9); r = 4 + pickN(6); k++; } while (taken.has(`${c},${r}`) && k < 40);
      taken.add(`${c},${r}`);
      nodes.push(tool(`t${i}`, c, r));
      ducts.push({ childId: `t${i}`, outlet: { unitId: 'gate', index: i } });
    }
    const r = routeAll(scene(nodes, ducts));
    const sets = [...r.values()].map(v => v.edges);
    let clash = false;
    for (let i = 0; i < sets.length && !clash; i++)
      for (let j = i + 1; j < sets.length && !clash; j++)
        for (const e of sets[j]) if (sets[i].has(e)) { clash = true; break; }
    if (clash) { bad++; if (!worstScene) worstScene = JSON.stringify(nodes.map(n => [n.id, n.x, n.y])); }
  }
  ok('400 random layouts, none share a lattice edge', bad === 0, `${bad} bad; first: ${worstScene}`);
}

// ── R13 · the overlap that survives is REPORTED ──────────────────────────────

group('R13 routeAllShared names the ducts that had to share');
{
  // R12 says a settled layout doesn't overlap; this says what happens when one
  // genuinely can't be settled. The relaxation in routePass ends with an
  // overlapping picture rather than a failure, which is right — a shop dragged
  // into a corner still has to draw — but the fact was computed and thrown away,
  // so nothing upstream could mention it. Found by search over cramped boards:
  // five tools packed round a 5-outlet gate, one of which has nowhere clean left.
  const s = scene(
    [collector('dc', 0, 0), unit('g', 0, 1, 5),
     tool('t0', 3, 3), tool('t1', 0, 2), tool('t2', 0, 4), tool('t3', 2, 2), tool('t4', 3, 2)],
    [{ childId: 'g', parentId: 'dc' },
     { childId: 't0', outlet: { unitId: 'g', index: 0 } },
     { childId: 't1', outlet: { unitId: 'g', index: 1 } },
     { childId: 't2', outlet: { unitId: 'g', index: 2 } },
     { childId: 't3', outlet: { unitId: 'g', index: 3 } },
     { childId: 't4', outlet: { unitId: 'g', index: 4 } }],
  );
  const { out, shared } = routeAllShared(s);
  ok('the board still routes — sharing is a fallback, not a failure', out.size === 6);
  ok('and the ducts that had to share are named', shared.length > 0, JSON.stringify(shared));
  // The named duct is really the one drawn over something else.
  const sets = new Map([...out].map(([id, v]) => [id, v.edges]));
  const clashes = (id: string) => [...sets].some(([other, e]) =>
    other !== id && [...sets.get(id)!].some(k => e.has(k)));
  ok('...and each named duct really is drawn over another', shared.every(clashes),
     JSON.stringify(shared));

  // A clean board reports nothing, and routeAll is still the same answer without
  // the extra return — every other caller reads it that way.
  const clean = scene([collector('dc', 0, 0), tool('saw', 0, 2)],
                      [{ childId: 'saw', parentId: 'dc' }]);
  ok('a clean board shares nothing', routeAllShared(clean).shared.length === 0);
  ok('routeAll is routeAllShared without the list',
     JSON.stringify([...routeAll(clean)]) === JSON.stringify([...routeAllShared(clean).out]));
}

// ── R14 · a capped end is as wide as the bar it draws ────────────────────────

group('R14 a capped end claims the width of its stopper bar');
{
  // An open end is a dot; a capped one draws a 28px bar across the run. Both were
  // routed as a half-8 junction, so the bar overhung its own clearance by 6px each
  // side — and a duct is deliberately allowed to run flush along a clearance
  // boundary, which put a run under the bar with nothing in the router objecting.
  // Reported 2026-09-07 on a secondary run, which is the kind most likely to be
  // routed past an end belonging to something else.
  const open: SceneNode = { id: 'e', glyph: 'junction', isUnit: false, span: 1, ...at(2, 2) };
  const capped: SceneNode = { ...open, capped: true };

  ok('an open end stays a dot', deviceBox(open).x1 - deviceBox(open).x0 === 16);
  ok('a capped end is the bar it draws', deviceBox(capped).x1 - deviceBox(capped).x0 === CAP_W);
  ok('...and no taller, since the bar is shorter than the dot it replaces',
     deviceBox(capped).y1 - deviceBox(capped).y0 === 16);

  // The gap a flush run used to take: 8px off centre is inside the bar, 14 is not.
  const box = deviceBox(capped);
  const a = { x: capped.x + 8, y: capped.y - 40 }, b = { x: capped.x + 8, y: capped.y + 40 };
  ok('a run 8px off the centreline is now inside the capped end', segBoxHit(a, b, box));
  ok('and one at the bar\'s own edge is not',
     !segBoxHit({ ...a, x: capped.x + CAP_W / 2 }, { ...b, x: capped.x + CAP_W / 2 }, box));
}

// ── R15 · overlap is judged on what is DRAWN ─────────────────────────────────

group('R15 overlaps the lattice cannot see are still reported');
{
  // The edge bookkeeping is the router's own currency and misses two shapes that
  // reach the screen anyway. Both were found on the demo layout, one of them
  // present since the day it was drawn.

  // 1 · A shared origin. Two ducts off one tee, both leaving downward, are one
  //     line until they separate. No edge is claimed twice — they are the SAME
  //     edge, taken by two runs that both legitimately start there.
  const s1 = scene(
    [collector('dc', 0, 0), unit('g', 0, 1, 2), tool('a', 0, 3), tool('b', 0, 4)],
    [{ childId: 'g', parentId: 'dc' },
     { childId: 'a', outlet: { unitId: 'g', index: 0 } },
     { childId: 'b', outlet: { unitId: 'g', index: 0 } }],
  );
  const r1 = routeAllShared(s1);
  ok('two runs off one outlet, drawn one over the other, are reported',
     r1.shared.length > 0, JSON.stringify([...r1.out].map(([k, v]) => [k, v.pts])));

  // 2 · A clean board still reports nothing. The check is geometric now, so this
  //     is the one that would catch it crying wolf on ordinary layouts.
  const s2 = scene(
    [collector('dc', 0, 0), unit('g', 0, 1, 2), tool('a', 0, 3), tool('b', 1, 3)],
    [{ childId: 'g', parentId: 'dc' },
     { childId: 'a', outlet: { unitId: 'g', index: 0 } },
     { childId: 'b', outlet: { unitId: 'g', index: 1 } }],
  );
  ok('an ordinary board is still clean', routeAllShared(s2).shared.length === 0,
     JSON.stringify(routeAllShared(s2).shared));

  // A tee where the legs leave in DIFFERENT directions shares only the point they
  // meet at, which is construction, not overlap.
  const s3 = scene(
    [collector('dc', 1, 0), unit('g', 0, 2, 3), tool('a', 0, 4), tool('b', 2, 4)],
    [{ childId: 'g', parentId: 'dc' },
     { childId: 'a', outlet: { unitId: 'g', index: 0 } },
     { childId: 'b', outlet: { unitId: 'g', index: 2 } }],
  );
  ok('meeting at a point is not overlapping', routeAllShared(s3).shared.length === 0,
     JSON.stringify(routeAllShared(s3).shared));
}

// ── summary ──────────────────────────────────────────────────────────────────

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exit(1);
