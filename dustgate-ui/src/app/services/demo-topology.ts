import type { Topology } from '@topology';

// A pre-built v2 topology used to seed demo / Vercel mode so the Live view (and
// later the configurator) has a shop to show on a fresh page load. Mirrors the
// mockup shop: a sliding gate feeding three tools directly, and its fourth outlet
// feeding a MANIFOLD that splits to the Drum sander and — through an inline BALL
// VALVE — a Planer. Table saw + Bandsaw sense their own power (smart outlets);
// everything else is switched by hand.
//
// All three gate kinds on purpose: the demo is the only shop most people will see
// before they draw their own, and a layout with one sliding gate taught nothing
// about valves, manifolds, or what a branched run looks like. Every gate is
// calibrated so the shop stays READY and `/` still lands on the Live view.
//
// Verified against validateTopology (shared/device-model/topology.js). Keep it
// valid if you edit — an invalid seed would leave demo mode with no topology.
export const DEMO_TOPOLOGY: Topology = {
  schemaVersion: 1,
  name: "Jeff's Shop",
  // Two boards, both carrying gates — the primary drives the sliding gate's stepper
  // and the back-wall node drives both servos. A one-board example taught nothing
  // about why a second board exists or what the wiring view is for.
  controllers: [
    { id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc', link: { transport: 'wifi-ws', host: 'dustgate.local' } },
    // Host matches a node discoverNodes() reports, so /boards shows it already paired.
    { id: 'dustgate-node-1', role: 'secondary', name: 'Back wall', board: 'qtpy_c3', link: { transport: 'wifi-ws', host: 'dustgate-node-1' } },
  ],
  elements: [
    { id: 'dc', type: 'collector', name: 'Cyclone' },
    {
      id: 'sel', type: 'selector', name: 'Main gate', controllerId: 'primary', kind: 'linear',
      states: [
        { id: 'home', isClosed: true,  positionMm: 0 },
        { id: 's1',   isClosed: false, positionMm: 12.5 },
        { id: 's2',   isClosed: false, positionMm: 95.4 },
        { id: 's3',   isClosed: false, positionMm: 178.3 },
        { id: 's4',   isClosed: false, positionMm: 261.2 },
      ],
      branches: [
        { id: 'b1', opensState: 's1', role: 'tool' },
        { id: 'b2', opensState: 's2', role: 'tool' },
        { id: 'b3', opensState: 's3', role: 'tool' },
        { id: 'b4', opensState: 's4', role: 'feed' },
      ],
      linear: { calibration: { stepsPerMm: 51.47, measuredSpanSteps: 4387, homeIsMaxEndstop: false, manifoldModel: 'rockler-2.5' } },
    },
    { id: 'saw',    type: 'tool', name: 'Table saw',    sensor: { outlet: { gen: 2, host: 'shellyplugus-tablesaw', ip: '192.168.87.30', thresholdW: 50 } } },
    { id: 'band',   type: 'tool', name: 'Bandsaw',      sensor: { outlet: { gen: 2, host: 'shellyplugus-bandsaw', ip: '192.168.87.27', thresholdW: 40 } } },
    { id: 'router', type: 'tool', name: 'Router table' },
    // Two-way manifold on the sliding gate's last outlet, isolating two tools on
    // the back wall with one servo.
    {
      id: 'man', type: 'selector', name: 'Back-wall manifold', controllerId: 'dustgate-node-1', kind: 'servoManifold',
      states: [
        { id: 'left',   isClosed: false, offsetDeg: 0 },
        { id: 'closed', isClosed: true,  offsetDeg: 80 },
        { id: 'right',  isClosed: false, offsetDeg: 161 },
      ],
      branches: [
        { id: 'mL', opensState: 'left',  role: 'tool' },
        { id: 'mR', opensState: 'right', role: 'tool' },
      ],
      servo: { channel: 0, detented: true, referenceAngle: 90 },
    },
    { id: 'sander', type: 'tool', name: 'Drum sander' },
    // Ball valve on its OWN trunk straight off the cyclone — one in, one out.
    // Deliberately not in series behind another gate: a valve that only repeats
    // what an upstream gate already does gets flagged (redundant), and the example
    // shop shouldn't ship demonstrating the thing the app warns you about.
    {
      id: 'bv', type: 'selector', name: 'Jointer valve', controllerId: 'dustgate-node-1', kind: 'servoGate',
      states: [
        { id: 'open',   isClosed: false, offsetDeg: 0 },
        { id: 'closed', isClosed: true,  offsetDeg: 90 },
      ],
      branches: [{ id: 'v1', opensState: 'open', role: 'tool' }],
      servo: { channel: 1, detented: true, referenceAngle: 90 },
    },
    { id: 'planer', type: 'tool', name: 'Planer' },
    { id: 'jointer', type: 'tool', name: 'Jointer' },
  ],
  ducts: [
    { child: 'sel',    parent: 'dc' },
    { child: 'saw',    parent: 'sel', parentBranch: 'b1' },
    { child: 'band',   parent: 'sel', parentBranch: 'b2' },
    { child: 'router', parent: 'sel', parentBranch: 'b3' },
    { child: 'man',    parent: 'sel', parentBranch: 'b4' },
    { child: 'sander', parent: 'man', parentBranch: 'mL' },
    { child: 'planer', parent: 'man', parentBranch: 'mR' },
    { child: 'bv',     parent: 'dc' },
    { child: 'jointer', parent: 'bv', parentBranch: 'v1' },
  ],
  ui: {
    layout: {
      dc: { col: 0, row: 0 }, sel: { col: 0, row: 1 },
      saw: { col: 0, row: 2 }, band: { col: 1, row: 2 }, router: { col: 2, row: 2 },
      man: { col: 3, row: 2 }, sander: { col: 3, row: 3 }, planer: { col: 4, row: 3 },
      bv: { col: 5, row: 1 }, jointer: { col: 5, row: 2 },
    },
    // Order along the board rail, left to right. Placement is no longer a decision —
    // the rail sits above the whole grid, so a board can only be reordered.
    wiring: { boards: { primary: 0, 'dustgate-node-1': 1 } },
  },
};
