/**
 * Sample layouts to tune the routing costs against — several, not one.
 *
 * Tuning on the demo layout alone is how a constant gets set to whatever suits one
 * picture, and on 2026-09-07 that is exactly what happened: TURN was measured at 48
 * because the reference scene stopped improving there. It stops improving at 32.
 * Every named scene below is flat from 32 upward and would have agreed with any
 * number at all; only the fuzz set could tell 48 from 64, which is the answer that
 * shipped. Add a scene here before trusting a number.
 *
 *     cd dustgate-ui && npm run bench:routing
 *
 * It prints a table and asserts nothing — a bench, not a test. The invariants that
 * came OUT of it (nothing above the ceiling, nothing drawn over anything) are in
 * router.spec.ts, where a regression fails a build instead of needing to be noticed
 * in a column of numbers.
 *
 * To sweep a constant, `./routing-sweep.sh` builds a patched copy of this directory
 * rather than editing it — see that script's header.
 *
 * WHAT THE COLUMNS MEAN. `bends` is what makes a layout look like plumbing nobody
 * would build, and is the number most of these costs exist to move. `above` is
 * horizontal pipe drawn higher than the collector's outlet — pipe going up only to
 * come back — measured with a LANE_STEP tolerance, since a nested lane sits half a
 * step off the line it was nudged from and is not a climb. `unsolved` and `ov` are
 * failures rather than trade-offs: a run the router could not place at all, and one
 * still drawn over another after the lanes are nested. Both should stay zero.
 */
import { type SceneNode, CELL, PAD, deviceBox } from './geometry';
import { type Scene, LANE_STEP, routeAllShared, sceneBounds } from './router';

const at = (c: number, r: number) => ({ x: PAD + c * CELL, y: PAD + r * CELL });
const N = (id: string, glyph: SceneNode['glyph'], c: number, r: number, span = 1, isUnit = false): SceneNode =>
  ({ id, glyph, isUnit, span, ...at(c, r) });

type Named = { name: string; scene: Scene };

/** The demo shop. `manCol` and `sanderRow` are the two nudges that broke it. */
function demo(manCol = 2, sanderRow = 2): Scene {
  const saw = N('saw', 'tool', 3, 2);
  const nodes: SceneNode[] = [
    N('dc', 'collector', 0, 0),
    N('j1', 'junction', 1, 0), N('j2', 'junction', 3, 0), N('j3', 'junction', 5, 0), N('j4', 'junction', 7, 0),
    N('v1', 'ballvalve', 1, 1), N('v2', 'ballvalve', 3, 1), N('v3', 'ballvalve', 5, 1),
    N('planer', 'tool', 1, 2), saw, N('sander', 'tool', 5, sanderRow),
    N('man', 'manifold', manCol, 4, 3, true),
    N('jointer', 'tool', manCol, 5), N('router', 'tool', manCol + 2, 5),
    N('v4', 'ballvalve', 6, 4), N('miter', 'tool', 6, 5),
    { id: 'guard', glyph: 'secondaryPort', isUnit: false, span: 1,
      x: saw.x + 15, y: saw.y - 34, hostBox: deviceBox(saw) },
  ];
  const ducts: Scene['ducts'] = [
    { childId: 'j1', parentId: 'dc' }, { childId: 'j2', parentId: 'j1' },
    { childId: 'j3', parentId: 'j2' }, { childId: 'j4', parentId: 'j3' },
    { childId: 'v1', parentId: 'j1' }, { childId: 'v2', parentId: 'j2' }, { childId: 'v3', parentId: 'j3' },
    { childId: 'planer', parentId: 'v1' }, { childId: 'saw', parentId: 'v2' }, { childId: 'sander', parentId: 'v3' },
    { childId: 'man', parentId: 'j4' },
    { childId: 'jointer', outlet: { unitId: 'man', index: 0 } },
    { childId: 'router', outlet: { unitId: 'man', index: 1 } },
    { childId: 'v4', parentId: 'j4' }, { childId: 'miter', parentId: 'v4' },
    { childId: 'guard', parentId: 'j4' },
  ];
  return { nodes, ducts, bounds: sceneBounds(nodes) };
}

/** Two systems stacked, the second one lower — the shop-vacuum band on the demo. */
function twoSystems(): Scene {
  const nodes: SceneNode[] = [
    N('dc', 'collector', 0, 0), N('j1', 'junction', 2, 0), N('j2', 'junction', 4, 0),
    N('v1', 'ballvalve', 2, 1), N('v2', 'ballvalve', 4, 1),
    N('planer', 'tool', 2, 2), N('saw', 'tool', 4, 2),
    N('vac', 'collector', 0, 4), N('g', 'slidingGate', 2, 4, 2, true),
    N('sander', 'tool', 2, 5), N('router', 'tool', 3, 5),
  ];
  const ducts: Scene['ducts'] = [
    { childId: 'j1', parentId: 'dc' }, { childId: 'j2', parentId: 'j1' },
    { childId: 'v1', parentId: 'j1' }, { childId: 'v2', parentId: 'j2' },
    { childId: 'planer', parentId: 'v1' }, { childId: 'saw', parentId: 'v2' },
    { childId: 'g', parentId: 'vac' },
    { childId: 'sander', outlet: { unitId: 'g', index: 0 } },
    { childId: 'router', outlet: { unitId: 'g', index: 1 } },
  ];
  return { nodes, ducts, bounds: sceneBounds(nodes) };
}

/** A long trunk with everything hanging below it: the shape a ceiling should not
 *  change at all, because nothing wants to be above the collector anyway. */
function longRow(): Scene {
  const nodes: SceneNode[] = [N('dc', 'collector', 0, 0)];
  const ducts: Scene['ducts'] = [];
  let prev = 'dc';
  for (let i = 1; i <= 6; i++) {
    nodes.push(N(`j${i}`, 'junction', i, 0), N(`t${i}`, 'tool', i, 2));
    ducts.push({ childId: `j${i}`, parentId: prev }, { childId: `t${i}`, parentId: `j${i}` });
    prev = `j${i}`;
  }
  return { nodes, ducts, bounds: sceneBounds(nodes) };
}

/** A tool parked ABOVE the collector. The ceiling is a cost, not a wall, and this
 *  is the scene that proves it: the run has to get up there somehow. */
function aboveTheCollector(): Scene {
  const nodes: SceneNode[] = [
    N('dc', 'collector', 0, 3), N('j1', 'junction', 2, 3),
    N('high', 'tool', 2, 0), N('low', 'tool', 4, 4),
  ];
  const ducts: Scene['ducts'] = [
    { childId: 'j1', parentId: 'dc' },
    { childId: 'high', parentId: 'j1' }, { childId: 'low', parentId: 'j1' },
  ];
  return { nodes, ducts, bounds: sceneBounds(nodes) };
}

/** A sliding gate on the collector's OWN row. A unit is entered at the top of its
 *  leftmost cell, which sits above the collector's centreline — so this is the scene
 *  where a hard ceiling and the top-entry rule actually collide. */
function gateOnCollectorRow(): Scene {
  const nodes: SceneNode[] = [
    N('dc', 'collector', 0, 1), N('g', 'slidingGate', 2, 1, 2, true),
    N('a', 'tool', 2, 3), N('b', 'tool', 3, 3),
  ];
  const ducts: Scene['ducts'] = [
    { childId: 'g', parentId: 'dc' },
    { childId: 'a', outlet: { unitId: 'g', index: 0 } },
    { childId: 'b', outlet: { unitId: 'g', index: 1 } },
  ];
  return { nodes, ducts, bounds: sceneBounds(nodes) };
}

/** The same gate a whole row ABOVE the collector — nothing about it is reachable
 *  without going up, so it is the scene that proves the ceiling still yields. */
function gateAboveCollector(): Scene {
  const nodes: SceneNode[] = [
    N('dc', 'collector', 0, 2), N('g', 'slidingGate', 2, 0, 2, true),
    N('a', 'tool', 2, 3), N('b', 'tool', 4, 3),
  ];
  const ducts: Scene['ducts'] = [
    { childId: 'g', parentId: 'dc' },
    { childId: 'a', outlet: { unitId: 'g', index: 0 } },
    { childId: 'b', outlet: { unitId: 'g', index: 1 } },
  ];
  return { nodes, ducts, bounds: sceneBounds(nodes) };
}

const scenes: Named[] = [
  { name: 'demo', scene: demo() },
  { name: 'demo+manifold-right', scene: demo(3) },
  { name: 'demo+sander-down', scene: demo(2, 3) },
  { name: 'two-systems', scene: twoSystems() },
  { name: 'long-row', scene: longRow() },
  { name: 'above-collector', scene: aboveTheCollector() },
  { name: 'gate-on-dc-row', scene: gateOnCollectorRow() },
  { name: 'gate-above-dc', scene: gateAboveCollector() },
];

/** Ceiling for measurement: the topmost collector's outlet height. */
function ceiling(s: Scene): number {
  let y = Infinity;
  for (const n of s.nodes) if (n.glyph === 'collector') y = Math.min(y, n.y);
  return y;
}

function measure(s: Scene) {
  const { out, shared } = routeAllShared(s);
  const cy = ceiling(s);
  let bends = 0, len = 0, above = 0, minY = Infinity;
  const paths: Record<string, number[][]> = {};
  for (const [id, r] of out) {
    paths[id] = r.pts.map(p => [Math.round(p.x), Math.round(p.y)]);
    bends += Math.max(0, r.pts.length - 2);
    for (const p of r.pts) minY = Math.min(minY, p.y);
    for (let i = 0; i < r.pts.length - 1; i++) {
      const a = r.pts[i], b = r.pts[i + 1];
      const d = Math.abs(a.x - b.x) + Math.abs(a.y - b.y);
      len += d;
      // Horizontal pipe drawn above the ceiling is the thing being counted; a
      // vertical leg passing through is just how you get to something up there.
      // Tolerance of a full LANE_STEP: a nested lane sits half a step off the
      // lattice line by design, and counting that as "climbing" would report 55px
      // of trespass on a board whose highest pipe IS the outlet.
      if (Math.abs(a.y - b.y) < 0.5 && a.y < cy - LANE_STEP) above += d;
    }
  }
  let unsolved = 0;
  for (const r of out.values()) if (!r.ok) unsolved++;
  return {
    unsolved, bends, shared: shared.length, len: Math.round(len), above: Math.round(above),
    minY: Math.round(minY), offBoard: minY < s.bounds.y0 - CELL, paths,
  };
}

// A seeded fuzz set, for the aggregate rather than any one picture.
function fuzz(n: number) {
  let seed = 4242;
  const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  const pick = (k: number) => Math.floor(rnd() * k);
  let bends = 0, shared = 0, len = 0, above = 0, off = 0;
  for (let t = 0; t < n; t++) {
    const span = 2 + pick(3);
    const nodes: SceneNode[] = [N('dc', 'collector', 0, 0), N('gate', 'slidingGate', 0, 2, span, true)];
    const ducts: Scene['ducts'] = [{ childId: 'gate', parentId: 'dc' }];
    const taken = new Set(['0,0']);
    for (let i = 0; i < span; i++) {
      let c = 0, r = 0, k = 0;
      do { c = pick(7); r = 4 + pick(4); k++; } while (taken.has(`${c},${r}`) && k < 40);
      taken.add(`${c},${r}`);
      nodes.push(N(`t${i}`, 'tool', c, r));
      ducts.push({ childId: `t${i}`, outlet: { unitId: 'gate', index: i } });
    }
    const m = measure({ nodes, ducts, bounds: sceneBounds(nodes) });
    bends += m.bends; shared += m.shared; len += m.len; above += m.above; off += m.offBoard ? 1 : 0;
  }
  return { n, bends, shared, len, above, off };
}

const pad = (v: unknown, n: number) => String(v).padStart(n);
console.log(`routing bench · CELL ${CELL}\n`);
console.log('scene                 bends    ov  unsolved   length   above');
for (const { name, scene: sc } of scenes) {
  const m = measure(sc);
  console.log(name.padEnd(20) + pad(m.bends, 6) + pad(m.shared, 6) + pad(m.unsolved, 10)
            + pad(m.len, 9) + pad(m.above, 8) + (m.offBoard ? '  OFF-BOARD' : ''));
}
const f = fuzz(200);
console.log('-'.repeat(58));
console.log(`${f.n} random shops`.padEnd(20) + pad(f.bends, 6) + pad(f.shared, 6)
          + pad('', 10) + pad(f.len, 9) + pad(f.above, 8) + (f.off ? `  ${f.off} OFF-BOARD` : ''));
