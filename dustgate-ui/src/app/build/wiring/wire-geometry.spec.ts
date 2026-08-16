/** Wiring-layer geometry cases W1–W8.
 *
 *  Plain TypeScript on purpose — no Angular, no browser. Run with:
 *      npm run test:wiring
 */

import {
  type Pt, BOARD_H, BOARD_W, CORNER_R, HOP_R, LANE_GAP, LANE_STEP, PORT_H, SERVO_PORTS,
  cablePath, cableRun, crossing, portExit, portPos, PORT_STUB,
  rankByTravel, segmentsOf,
} from './wire-geometry';
import { CELL, cellX, cellY, halfH, halfW } from '../routing/geometry';

let failures = 0, checks = 0;
function ok(name: string, cond: boolean, detail?: string): void {
  checks++;
  if (cond) { console.log(`  ok   ${name}`); return; }
  failures++;
  console.log(`  FAIL ${name}${detail ? `\n       ${detail}` : ''}`);
}
function group(name: string): void { console.log(`\n${name}`); }
const P = (x: number, y: number): Pt => ({ x, y });
const near = (a: number, b: number, eps = 0.01) => Math.abs(a - b) < eps;

// ── W1 · ports ───────────────────────────────────────────────────────────────
group('W1 the port strip is the hardware budget');
{
  // A board stands on an ordinary grid cell. It sat in a rail above the whole grid
  // until 2026-08-16, in negative y with its own slot pitch — see
  // docs/boards-on-canvas-plan.md for why that came out.
  const c = { x: cellX(3), y: cellY(0) };
  const xs = [0, 1, 2, 3, 4].map(ch => portPos(c, ch).x);
  ok('five ports, evenly pitched', xs.every((x, i) => i === 0 || x - xs[i - 1] === 18), xs.join());
  ok('strip is centred on the board', near((xs[0] + xs[4]) / 2, c.x));
  ok('ports straddle the underside', portPos(c, 0).y > c.y && portPos(c, 0).y < c.y + BOARD_H / 2);
  ok('cable leaves the port underside', near(portExit(c, 0).y, portPos(c, 0).y + PORT_H / 2));
  ok('channel 4 is the stepper, past the servo bank', SERVO_PORTS === 4);
}

// ── W1b · a board is a glyph on the grid ─────────────────────────────────────
group('W1b a board owns its cell, so ducts have a box to steer around');
{
  const b = { id: 'primary', glyph: 'board' as const, isUnit: false, span: 1, x: cellX(2), y: cellY(1) };
  ok('the router sizes it from the drawn body', halfW(b) === BOARD_W / 2 && halfH(b) === BOARD_H / 2);
  // Narrower than a cell, or two boards side by side share an edge and read as one
  // module — and a duct could never slip between them.
  ok('a board is narrower than its cell', BOARD_W < CELL, `${BOARD_W} vs ${CELL}`);
  ok('and shorter than one', BOARD_H < CELL, `${BOARD_H} vs ${CELL}`);
  ok('the whole strip fits inside the body',
     portPos({ x: b.x, y: b.y }, 0).x > b.x - BOARD_W / 2
     && portPos({ x: b.x, y: b.y }, SERVO_PORTS).x < b.x + BOARD_W / 2);
}

// ── W2 · the lane ────────────────────────────────────────────────────────────
group('W2 a cable is a two-bend run through its own lane, run HIGH');
{
  const run = cableRun(P(136, 93), P(94, 280), 0);
  ok('four points', run.length === 4, JSON.stringify(run));
  ok('leaves the port straight down', run[0].x === run[1].x);
  ok('arrives at the tab straight', run[2].x === run[3].x && run[3].x === 94);
  // The whole point of running high: travel happens near the rail, not down in the
  // ductwork, and the cable drops onto its tab from above.
  ok('turns as soon as it is clear of the board', near(run[1].y, 93 + PORT_STUB));
  ok('the horizontal run sits well above the gate row', run[1].y < (93 + 280) / 2, JSON.stringify(run));
  ok('lane is shared by the two middle points', run[1].y === run[2].y);
}
{
  // A gate is a BOX, not a point. With a high lane the clearance normally has
  // nothing to do, but when board and gate are close it pushes the lane back down
  // to just above the body rather than through it.
  const tight = cableRun(P(136, 180), P(94, 280), 5, 0, 23);
  ok('a squeezed lane gives up height, not clearance', tight[1].y <= 280 - 23 - LANE_GAP,
     JSON.stringify(tight));
  const roomy = cableRun(P(136, 93), P(94, 280), 0, 0, 23);
  ok('a roomy run ignores the clearance and stays high', near(roomy[1].y, 93 + PORT_STUB));
}
{
  const straight = cableRun(P(136, 93), P(136, 280), 0);
  ok('a port directly over its tab is one straight drop', straight.length === 2);
}
{
  // Board parked right on top of its gate: no room for a lane, so it dog-legs
  // rather than emitting a lane outside the two ends.
  const tight = cableRun(P(136, 270), P(94, 280), 3);
  ok('a tight gap still yields a lane between the ends',
     tight[1].y > 270 && tight[1].y < 280, JSON.stringify(tight));
}

// ── W3 · nesting ─────────────────────────────────────────────────────────────
group('W3 lanes nest, so cables do not cross each other');
{
  // The real five-gate shop: three gates on the primary at (1,0).
  const c = P(172, 64);
  const targets = [P(94, 280), P(202, 280), P(310, 280)];     // g1, g2, g3 tabs
  const chans = [0, 1, 3];
  const legs = targets.map((t, i) => ({ from: portExit(c, chans[i]), to: t }));
  const rank = rankByTravel(legs, l => Math.abs(l.to.x - l.from.x));

  ok('the shortest traveller ranks last, nested lowest', rank.get(legs[0]) === 2);
  ok('the furthest ranks first, taking the highest lane', rank.get(legs[2]) === 0);

  const runs = legs.map(l => cableRun(l.from, l.to, rank.get(l)!));
  ok('furthest run takes the highest lane',
     runs[2][1].y < runs[1][1].y && runs[1][1].y < runs[0][1].y,
     runs.map(r => r[1].y).join());

  // The whole point: no cable may cross another.
  let hits = 0;
  for (let i = 0; i < runs.length; i++)
    for (let j = i + 1; j < runs.length; j++)
      for (const a of segmentsOf(runs[i]))
        for (const b of segmentsOf(runs[j]))
          if (crossing(a, b)) hits++;
  ok('no cable crosses another', hits === 0, `${hits} crossings`);
}

// ── W4 · crossings ───────────────────────────────────────────────────────────
group('W4 crossing detection');
{
  const h = [P(100, 80), P(300, 80)] as const;
  const v = [P(200, 20), P(200, 140)] as const;
  const x = crossing(h, v);
  ok('finds an honest crossing', !!x && x.x === 200 && x.y === 80);
  ok('order does not matter', JSON.stringify(crossing(v, h)) === JSON.stringify(x));
  ok('parallel segments never cross', crossing(h, [P(100, 90), P(300, 90)] as const) === null);
  ok('a shared endpoint is a join, not a crossing',
     crossing([P(100, 80), P(200, 80)] as const, v) === null);
  ok('segments that miss do not cross',
     crossing(h, [P(200, 100), P(200, 140)] as const) === null);
}

// ── W5 · path emitter ────────────────────────────────────────────────────────
group('W5 corners are rounded, and only cable-over-cable hops');
{
  const run = cableRun(P(136, 93), P(94, 280), 0);
  const plain = cablePath(run);
  ok('two corners, two arcs', (plain.match(/A /g) ?? []).length === 2, plain);
  ok('no hop without another cable under it', !plain.includes(`A ${HOP_R}`), plain);
  ok('starts at the port', plain.startsWith('M 136 93'), plain);
  ok('ends at the tab', plain.trim().endsWith('L 94 280'), plain);
}
{
  // A duct passed as `under` would hop — so the component must not pass ducts. This
  // pins the emitter's contract: it hops over whatever it is given, no more.
  const run = cableRun(P(136, 93), P(94, 280), 0);
  const other = segmentsOf([P(60, 150), P(400, 150)]);        // a cable across its drop
  const hopped = cablePath(run, other);
  ok('a cable crossing a cable gets a hop', hopped.includes(`A ${HOP_R} ${HOP_R}`), hopped);
  // The run turns high now, so it is the DESCENT that meets the other cable.
  ok('the hop lands on the crossing', hopped.includes('94 145.5') && hopped.includes('94 154.5'), hopped);
}
{
  // Short segment: the corner radius must not overrun the line it belongs to.
  const tiny = cablePath([P(0, 0), P(0, 8), P(8, 8)]);
  const radii = [...tiny.matchAll(/A ([\d.]+) /g)].map(m => Number(m[1]));
  ok('corner radius shrinks to fit a short segment',
     radii.every(r => r <= CORNER_R && r <= 4), JSON.stringify(radii));
}
{
  // Bulge direction, so the convention is stable: horizontal → up, vertical → right.
  const horiz = cablePath([P(0, 50), P(200, 50)], segmentsOf([P(100, 0), P(100, 100)]));
  ok('a rightward run bulges up (sweep 1)', /A 4\.5 4\.5 0 0 1 104\.5 50/.test(horiz), horiz);
  const left = cablePath([P(200, 50), P(0, 50)], segmentsOf([P(100, 0), P(100, 100)]));
  ok('a leftward run still bulges up (sweep 0)', /A 4\.5 4\.5 0 0 0 95\.5 50/.test(left), left);
  const down = cablePath([P(50, 0), P(50, 200)], segmentsOf([P(0, 100), P(100, 100)]));
  ok('a downward run bulges right (sweep 1)', /A 4\.5 4\.5 0 0 1 50 104\.5/.test(down), down);
}

// ── W6 · the real shop, end to end ───────────────────────────────────────────
group('W6 the five-gate shop draws without a single cable-over-cable hop');
{
  const primary = P(172, 64);
  const back = P(604, 172);
  const legs = [
    { from: portExit(primary, 0), to: P(94, 280) },
    { from: portExit(primary, 1), to: P(202, 280) },
    { from: portExit(primary, 3), to: P(310, 280) },
    { from: portExit(back, 0), to: P(418, 280) },
    { from: portExit(back, 1), to: P(526, 280) },
  ];
  // Ranked per board, which is how the component does it.
  const byBoard = [legs.slice(0, 3), legs.slice(3)];
  const runs: Pt[][] = [];
  for (const grp of byBoard) {
    const rank = rankByTravel(grp, l => Math.abs(l.to.x - l.from.x));
    for (const l of grp) runs.push(cableRun(l.from, l.to, rank.get(l)!));
  }
  let hops = 0;
  const drawn: ReturnType<typeof segmentsOf> = [];
  for (const r of runs) {
    for (const a of segmentsOf(r)) for (const b of drawn) if (crossing(a, b)) hops++;
    drawn.push(...segmentsOf(r));
  }
  ok('five cables, no cable-over-cable crossing', hops === 0, `${hops} crossings`);
  ok('every cable reaches its tab', runs.every(r => r[r.length - 1].y === 280));
}

// ── W7 · the gate row is left alone ──────────────────────────────────────────
group('W7 cables travel in the empty band, not through the ductwork');
{
  // Five cables to gates spread across the shop, all off one board in the top row.
  const board = P(172, 64);
  const targets = [94, 202, 310, 418, 526].map(x => P(x, 280));
  const legs = targets.map((t, i) => ({ from: portExit(board, i % 5), to: t }));
  const rank = rankByTravel(legs, l => Math.abs(l.to.x - l.from.x));
  const runs = legs.map(l => cableRun(l.from, l.to, rank.get(l)!));
  const gateRow = 280 - 40;                      // anywhere near the gates' boxes
  ok('no horizontal run travels down at gate level',
     runs.every(r => r[1].y < gateRow), runs.map(r => r[1].y).join());
  // This read "the top THIRD" while boards lived on a rail two bands above the shop,
  // where the drop was 318 units and five nested lanes used a sixth of it. A board
  // stands on the grid now — usually one row above the gates it drives — so the same
  // five lanes occupy a real share of a much shorter drop. The half is what the claim
  // was always about: the cables bunch at the TOP of the gap and come down onto their
  // tabs, rather than crossing the shop at the height the ductwork runs at.
  ok('every lane sits in the upper half of the drop',
     runs.every(r => r[1].y < (board.y + 280) / 2), runs.map(r => r[1].y).join());
}

// ── W8 · lane spacing ────────────────────────────────────────────────────────
group('W8 lane spacing');
{
  const runs = [0, 1, 2, 3].map(r => cableRun(P(136, 60), P(400, 280), r));
  const lanes = runs.map(r => r[1].y);
  ok('each rank gets its own lane', new Set(lanes).size === 4, lanes.join());
  ok('lanes step by LANE_STEP', near(lanes[1] - lanes[0], LANE_STEP), lanes.join());
  ok('rank 0 turns one stub below the port', near(lanes[0], 60 + PORT_STUB));
  ok('lanes descend from the highest rank', lanes[0] < lanes[3]);
}

// ── W9 · a gate above the board ──────────────────────────────────────────────
group('W9 a board below its gate still leaves from the underside');
{
  // Reachable now that boards stand on the grid: drag one under the gates it drives
  // and this is the run you get. Nothing refuses the drop — see the plan's "what not
  // to re-buy" — so the geometry has to hold up rather than be unreachable.
  const board = P(172, 496);
  const from = portExit(board, 0);
  const to = P(400, 280);                                      // a tab well above it
  ok('the cable leaves the port underside, not the top edge', from.y > board.y);
  const run = cableRun(from, to, 0);
  ok('it drops further BELOW the board before turning', run[1].y > from.y, JSON.stringify(run));
  ok('the turn clears the board body', run[1].y > board.y + BOARD_H / 2, JSON.stringify(run));
  ok('it still reaches the tab', run[3].x === to.x && run[3].y === to.y);
  // Two cables off one board must not share the detour lane.
  const other = cableRun(portExit(board, 1), P(300, 280), 1);
  ok('ranked detours get their own lane', run[1].y !== other[1].y, `${run[1].y} vs ${other[1].y}`);
}

// ── W10 · the drop goes round what's standing in it ─────────────────────────
group('W10 a cable pays to cross a piece, so it comes down beside one instead');
{
  const from = P(136, 60), to = P(496, 604);      // a tab five bands down
  // A tool sitting in the tab's own column, halfway down the drop.
  const box = { x0: 496 - 38, y0: 246, x1: 496 + 38, y1: 314 };
  const cost = (a: Pt, b: Pt): number => {
    const vert = Math.abs(a.x - b.x) < 0.5;
    if (!vert) return 0;
    return a.x > box.x0 && a.x < box.x1
        && Math.max(a.y, b.y) > box.y0 && Math.min(a.y, b.y) < box.y1 ? 100 : 0;
  };
  const plain = cableRun(from, to, 0);
  ok('with no cost model it still drops straight down the tab column',
     plain.length === 4 && near(plain[2].x, to.x), JSON.stringify(plain));
  const run = cableRun(from, to, 0, 0, 0, cost);
  ok('a blocked column adds a bend', run.length === 5, JSON.stringify(run));
  const dropX = run[2].x;
  ok('the descent leaves the tool alone', cost(P(dropX, 100), P(dropX, 604)) === 0, String(dropX));
  ok('and comes down in the gutter, not halfway across the shop',
     Math.abs(dropX - to.x) < CELL, String(dropX));
  ok('it still lands on the tab', near(run[4].x, to.x) && near(run[4].y, to.y));
  ok('the jog back in runs along the tab row', near(run[3].y, to.y));
  // Nothing in the way: no detour, no extra bend.
  const clean = cableRun(from, P(496, 200), 0, 0, 0, cost);
  ok('an empty column is left alone', clean.length === 4, JSON.stringify(clean));
}

console.log(`\n${checks - failures}/${checks} checks passed`);
process.exit(failures ? 1 : 0);
