import type { Topology } from '@topology';

// A pre-built v2 topology used to seed demo / Vercel mode so the Live view (and
// later the configurator) has a shop to show on a fresh page load. Mirrors the
// mockup shop: one sliding gate feeding four tools — Table saw + Bandsaw sense
// their own power (smart outlets), Router table + Drum sander are manual.
//
// Verified against validateTopology (shared/device-model/topology.js). Keep it
// valid if you edit — an invalid seed would leave demo mode with no topology.
export const DEMO_TOPOLOGY: Topology = {
  schemaVersion: 1,
  name: "Jeff's Shop",
  controllers: [{ id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc', link: { transport: 'wifi-ws', host: 'dustgate.local' } }],
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
        { id: 'b4', opensState: 's4', role: 'tool' },
      ],
      linear: { calibration: { stepsPerMm: 51.47, measuredSpanSteps: 4387, homeIsMaxEndstop: false, manifoldModel: 'rockler-2.5' } },
    },
    { id: 'saw',    type: 'tool', name: 'Table saw',    sensor: { outlet: { gen: 2, host: 'shellyplugus-tablesaw', ip: '192.168.87.30', thresholdW: 50 } } },
    { id: 'band',   type: 'tool', name: 'Bandsaw',      sensor: { outlet: { gen: 2, host: 'shellyplugus-bandsaw', ip: '192.168.87.27', thresholdW: 40 } } },
    { id: 'router', type: 'tool', name: 'Router table' },
    { id: 'sander', type: 'tool', name: 'Drum sander' },
  ],
  ducts: [
    { child: 'sel',    parent: 'dc' },
    { child: 'saw',    parent: 'sel', parentBranch: 'b1' },
    { child: 'band',   parent: 'sel', parentBranch: 'b2' },
    { child: 'router', parent: 'sel', parentBranch: 'b3' },
    { child: 'sander', parent: 'sel', parentBranch: 'b4' },
  ],
};
