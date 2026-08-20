/** Router conformance cases R1–R8, matching the validation mockups.
 *
 *  Plain TypeScript on purpose — no Angular, no browser. Run with:
 *      npm run test:routing
 */

import { type SceneNode, CELL, CLEARANCE, PAD, SPIGOT_DX, cellX, cellY, deviceBox, segBoxHit } from './geometry';
import { type Scene, type RoutedDuct, Router, routeAll, sceneBounds } from './router';

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
  eqPath('dc→sel is the straight drop', path(r, 'sel'), [[64, 94], [64, 137]]);
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
  eqPath('collector right port → tool left port, flat', path(r, 'planer'), [[94, 64], [134, 64]]);

  // Mirrored: the tool on the left uses its right port and is equally flat.
  const s2 = scene([collector('dc', 1, 0), tool('planer', 0, 0)], [{ childId: 'planer', parentId: 'dc' }]);
  eqPath('mirrored, tool right port', path(routeAll(s2), 'planer'), [[142, 64], [102, 64]]);

  // Directly below: top port still wins, because it costs no bends there.
  const s3 = scene([collector('dc', 0, 0), tool('saw', 0, 1)], [{ childId: 'saw', parentId: 'dc' }]);
  eqPath('directly below → top port, straight drop', path(routeAll(s3), 'saw'), [[64, 94], [64, 138]]);

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
    [[94, 64], [172, 64], [172, 138]]);

  // The bias is a tiebreaker, not a mandate — R3 already covers the case where a
  // side entry is CLEARLY shorter (same row, flat) and confirms it still wins
  // there; this just adds the near-tie this bias exists for.
}

// ── R3c · a pickup must not push its machine's own run off the top ──────────

group('R3c a machine keeps its straight drop after growing a pickup');
{
  const pickup = (id: string, on: SceneNode, dx: number): SceneNode =>
    ({ id, glyph: 'pickup', isUnit: false, span: 1, x: on.x + dx, y: on.y - 34 });

  // Gate directly above the tool: the run is a straight drop, and must stay one.
  const saw = tool('saw', 0, 1);
  const plain = scene([collector('dc', 0, 0), saw], [{ childId: 'saw', parentId: 'dc' }]);
  const before = path(routeAll(plain), 'saw');
  ok('baseline is a straight drop', before.length === 2, JSON.stringify(before));

  // Same shop, but the saw now has a hood on its top edge and its inlet has shifted
  // to make room. The hood is NOT an obstacle — inflated by CLEARANCE it would cover
  // the very lattice node a top entry arrives through, which used to make the router
  // throw the top port away and come in from the side instead.
  const withHood: SceneNode = { ...saw, inletDx: SPIGOT_DX };
  const hood = pickup('saw-aux', saw, 15);
  const after = scene([collector('dc', 0, 0), withHood, hood],
                      [{ childId: 'saw', parentId: 'dc' }, { childId: 'saw-aux', parentId: 'dc' }]);
  const p = path(routeAll(after), 'saw');
  const end = p[p.length - 1];
  ok('still enters from the TOP, not a side',
     Math.round(end.y) < Math.round(cellY(1)), `ended at ${JSON.stringify(end)}`);
  ok('and lands on the spigot, not the centreline',
     Math.round(end.x) === Math.round(cellX(0) + SPIGOT_DX),
     `ended at x=${Math.round(end.x)}, spigot at ${cellX(0) + SPIGOT_DX}`);
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

// ── summary ──────────────────────────────────────────────────────────────────

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures) process.exit(1);
