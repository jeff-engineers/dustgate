import type { Topology } from '@topology';

// A pre-built topology used to seed demo / Vercel mode so the Live view (and
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
  schemaVersion: 2,
  name: "Jeff's Shop",
  // Two boards, both carrying gates — the primary drives the sliding gate's stepper
  // and the back-wall node drives both servos. A one-board example taught nothing
  // about why a second board exists or what the wiring view is for.
  controllers: [
    { id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc', link: { transport: 'wifi-ws', host: 'dustgate.local' } },
    // Host matches a node discoverNodes() reports, so /boards shows it already paired.
    { id: 'dustgate-node-1', role: 'secondary', name: 'Back wall', board: 'qtpy_s3', link: { transport: 'wifi-ws', host: 'dustgate-node-1' } },
  ],
  // ONE system. The demo shop has one blower, which is what nearly every shop
  // has — a second system in the seed would teach the container at the cost of
  // making the first thing anyone sees more complicated than their own shop.
  systems: [{
    id: 'system-1',
    name: 'Dust collection',
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
    { id: 'saw',    type: 'tool', name: 'Table saw',    machineId: 'saw' },
    { id: 'band',   type: 'tool', name: 'Bandsaw',      machineId: 'band' },
    { id: 'router', type: 'tool', name: 'Router table', machineId: 'router' },
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
    { id: 'sander', type: 'tool', name: 'Drum sander', machineId: 'sander' },
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
    { id: 'planer', type: 'tool', name: 'Planer', machineId: 'planer' },
    { id: 'jointer', type: 'tool', name: 'Jointer', machineId: 'jointer' },
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
  }],
  // The things you switch on. Each owns its display name and its plug: the table
  // saw and bandsaw sense their own draw, the rest are switched by hand. One port
  // apiece here — a multi-port machine is a thing the canvas can build, not
  // something to bury in the first shop anyone sees.
  machines: [
    { id: 'saw',     name: 'Table saw', sensor: { outlet: { gen: 2, host: 'shellyplugus-tablesaw', ip: '192.168.87.30', thresholdW: 50 } } },
    { id: 'band',    name: 'Bandsaw',   sensor: { outlet: { gen: 2, host: 'shellyplugus-bandsaw',  ip: '192.168.87.27', thresholdW: 40 } } },
    { id: 'router',  name: 'Router table' },
    { id: 'sander',  name: 'Drum sander' },
    { id: 'planer',  name: 'Planer' },
    { id: 'jointer', name: 'Jointer' },
  ],
  devices: [],
  ui: {
    // No `layout` on purpose. This used to carry a hand-arranged one from the days
    // when the canvas fanned out sideways, which meant demo mode showed an
    // arrangement no new shop would ever get. Leaving it out hands the demo to
    // autoLayoutInto(), so what you see here is exactly what your own shop starts as.
    // Order along the board rail, left to right. Placement is no longer a decision —
    // the rail sits above the whole grid, so a board can only be reordered.
    wiring: { boards: { primary: 0, 'dustgate-node-1': 1 } },
  },
};
