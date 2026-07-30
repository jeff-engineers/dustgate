// topology.fixtures.js — hand-built topologies for testing the v2 model.
//
// Reused by topology.test.js now and (later) by the device-model migrate +
// conformance. Kept minimal but exercising every shape: linear N-branch,
// servo gate, servo manifold, the `feed` role (multi-hop), blocked branches,
// and multi-selector fan-in.

'use strict';

const clone = (o) => JSON.parse(JSON.stringify(o));

const primary = { id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc' };

// ── star: one linear actuator, two tools on two branches ────────────────────
const star = {
  schemaVersion: 1,
  name: 'star',
  controllers: [primary],
  elements: [
    { id: 'dc', type: 'collector', name: 'Dust Collector' },
    {
      id: 'sel', type: 'selector', name: 'Main', controllerId: 'primary', kind: 'linear',
      states: [
        { id: 'home', isClosed: true, positionMm: 0 },
        { id: 's1', isClosed: false, positionMm: 12.5 },
        { id: 's2', isClosed: false, positionMm: 95.4 },
      ],
      branches: [
        { id: 'b1', opensState: 's1', role: 'tool' },
        { id: 'b2', opensState: 's2', role: 'tool' },
      ],
      linear: { calibration: { stepsPerMm: 51.47, measuredSpanSteps: 4387, homeIsMaxEndstop: false, manifoldModel: 'rockler-2.5' } },
    },
    { id: 'toolA', type: 'tool', name: 'Bandsaw',   sensor: { outlet: { gen: 2, ip: '192.168.87.27', thresholdW: 6 } } },
    { id: 'toolB', type: 'tool', name: 'Table Saw', sensor: { outlet: { gen: 2, ip: '192.168.87.30', thresholdW: 5 } } },
  ],
  ducts: [
    { child: 'sel', parent: 'dc' },
    { child: 'toolA', parent: 'sel', parentBranch: 'b1' },
    { child: 'toolB', parent: 'sel', parentBranch: 'b2' },
  ],
};

// ── feedChain: linear → (feed) → servo manifold; plus a blocked branch ──────
const feedChain = {
  schemaVersion: 1,
  name: 'feedChain',
  controllers: [primary],
  elements: [
    { id: 'dc', type: 'collector', name: 'Dust Collector' },
    {
      id: 'lin', type: 'selector', name: 'Main', controllerId: 'primary', kind: 'linear',
      states: [
        { id: 'home', isClosed: true, positionMm: 0 },
        { id: 's1', isClosed: false, positionMm: 12.5 },
        { id: 's2', isClosed: false, positionMm: 95.4 },
        { id: 's3', isClosed: false, positionMm: 178.3 },
      ],
      branches: [
        { id: 'b1', opensState: 's1', role: 'tool' },
        { id: 'b2', opensState: 's2', role: 'feed' },     // → manifold
        { id: 'b3', opensState: 's3', role: 'blocked' },  // capped, no child
      ],
      linear: { calibration: { stepsPerMm: 51.47, measuredSpanSteps: 4387, homeIsMaxEndstop: false, manifoldModel: 'rockler-2.5' } },
    },
    {
      id: 'man', type: 'selector', name: 'Manifold A', controllerId: 'primary', kind: 'servoManifold',
      // LEFT is the reference (offset 0); closed/right are the ball's port offsets.
      states: [
        { id: 'left', isClosed: false, offsetDeg: 0 },
        { id: 'closed', isClosed: true, offsetDeg: 80 },
        { id: 'right', isClosed: false, offsetDeg: 161 },
      ],
      branches: [
        { id: 'mL', opensState: 'left', role: 'tool' },
        { id: 'mR', opensState: 'right', role: 'tool' },
      ],
      servo: { channel: 0, referenceAngle: 5, moveMs: 600, holdAtRest: false, detented: true },
    },
    { id: 'toolA', type: 'tool', name: 'Bandsaw',      sensor: { outlet: { gen: 2, ip: '192.168.87.27', thresholdW: 6 } } },
    { id: 'toolL', type: 'tool', name: 'Router Table', sensor: { outlet: { gen: 2, ip: '192.168.87.31', thresholdW: 8 } } },
    { id: 'toolR', type: 'tool', name: 'Drum Sander',  sensor: { outlet: { gen: 2, ip: '192.168.87.32', thresholdW: 7 } } },
  ],
  ducts: [
    { child: 'lin', parent: 'dc' },
    { child: 'toolA', parent: 'lin', parentBranch: 'b1' },
    { child: 'man', parent: 'lin', parentBranch: 'b2' },     // feed
    { child: 'toolL', parent: 'man', parentBranch: 'mL' },
    { child: 'toolR', parent: 'man', parentBranch: 'mR' },
  ],
};

// ── twoGates: two independent binary servo gates → collector ────────────────
// The "two tools running at once is fine" case (no shared selector).
const twoGates = {
  schemaVersion: 1,
  name: 'twoGates',
  controllers: [primary],
  elements: [
    { id: 'dc', type: 'collector', name: 'Dust Collector' },
    {
      id: 'gate1', type: 'selector', name: 'Gate 1', controllerId: 'primary', kind: 'servoGate',
      // OPEN is the reference (offset 0); closed is the quarter-turn +90.
      states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
      branches: [{ id: 'g1', opensState: 'open', role: 'tool' }],
      servo: { channel: 0, referenceAngle: 10, moveMs: 600, holdAtRest: false, detented: true },
    },
    {
      id: 'gate2', type: 'selector', name: 'Gate 2', controllerId: 'primary', kind: 'servoGate',
      states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
      branches: [{ id: 'g2', opensState: 'open', role: 'tool' }],
      servo: { channel: 1, referenceAngle: 10, moveMs: 600, holdAtRest: false, detented: true },
    },
    { id: 'toolX', type: 'tool', name: 'Jointer', sensor: { outlet: { gen: 2, ip: '192.168.87.40', thresholdW: 5 } } },
    { id: 'toolY', type: 'tool', name: 'Planer',  sensor: { outlet: { gen: 2, ip: '192.168.87.41', thresholdW: 9 } } },
  ],
  ducts: [
    { child: 'gate1', parent: 'dc' },
    { child: 'gate2', parent: 'dc' },
    { child: 'toolX', parent: 'gate1', parentBranch: 'g1' },
    { child: 'toolY', parent: 'gate2', parentBranch: 'g2' },
  ],
};

module.exports = { clone, star, feedChain, twoGates };
