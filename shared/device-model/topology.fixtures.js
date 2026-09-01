// topology.fixtures.js — hand-built topologies for testing the topology model.
//
// Reused by topology.test.js now and (later) by the device-model migrate +
// conformance. Kept minimal but exercising every shape: linear N-branch,
// servo gate, servo manifold, the `feed` role (multi-hop), blocked branches,
// and multi-selector fan-in.

'use strict';

const clone = (o) => JSON.parse(JSON.stringify(o));

// The default brain drives the PWM bank. `drives` is omitted rather than set to
// 'servo' on purpose: absent means servo, and leaving it out keeps these
// fixtures exercising that default the way a topology saved before the field
// existed does.
const primary = { id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc' };

// The same brain, flashed for the rack. A board is one or the other from the
// moment it is flashed — see Controller.drives in topology.js.
const sliderPrimary = { ...primary, drives: 'linear' };

// The second board the feed chain needs. A slider host drives a serial bus and a
// servo host drives PWM, and one ESP32 cannot do both — the two personalities
// contend for the same pads, and firmware/config.h #errors on a pin map claiming
// both. So "sliding gate feeds a servo manifold" is a TWO-BOARD topology, and it
// always was in hardware; the model just used to let it be drawn on one.
const servoNode = {
  id: 'node1', role: 'secondary', name: 'Manifold Node', board: 'xiao_c5',
  link: { transport: 'wifi-ws', host: 'dustgate-node-1.local' },
};

// A slider at the far end of a NodeLink socket: one rack, its own 12V supply at
// the gate, and no PWM channels at all.
const sliderNode = {
  id: 'slider1', role: 'secondary', name: 'Slider Node', board: 'xiao_c5',
  drives: 'linear',
  link: { transport: 'wifi-ws', host: 'dustgate-slider-1.local' },
};

// ── star: one linear actuator, two tools on two branches ────────────────────
const star = {
  schemaVersion: 1,
  name: 'star',
  controllers: [sliderPrimary],
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
  controllers: [sliderPrimary, servoNode],
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
      // On the NODE, not the primary: the primary here is the slider board.
      id: 'man', type: 'selector', name: 'Manifold A', controllerId: 'node1', kind: 'servoManifold',
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

// ── twoSystemShop: the shape the shop RFC was written for ───────────────────
// A 4" system on ball valves and a 2.5" system on a Rockler manifold, sharing a
// room, a network and ONE brain — but no duct. Built by migrating the two
// single-system fixtures above and merging them, which is also a live check that
// migrateToShop produces something the shop validator accepts.
//
// The table saw is the case the RFC exists for: ONE machine with a cabinet port
// on the 4" system and an overarm port on the 2.5" one. Its overarm is marked
// supplemental, so losing it is `partial` rather than `stripped`.
//
// TWO BOARDS, NOT ONE — corrected 2026-08-28. The RFC said "ONE brain", and one
// brain still owns the routing for both systems; what changed is that the brain
// cannot also be the hands for both. The 4" ball valves are PWM and the 2.5"
// Rockler manifold is a serial-bus slider, and those two builds contend for the
// same pads (firmware/config.h #errors on a pin map claiming both). So the
// slider hangs off its own node, which is what the hardware plan always assumed:
// the slider gets dedicated hardware riding with it and a 12V supply at the gate.
const twoSystemShop = {
  schemaVersion: 2,
  name: "Jeff's Shop",
  controllers: [primary, sliderNode],
  systems: [
    {
      id: 'big', name: '4" system',
      elements: [
        { id: 'dc-big', type: 'collector', name: 'Cyclone' },
        {
          id: 'bv-cab', type: 'selector', name: 'Cabinet valve', controllerId: 'primary', kind: 'servoGate',
          states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
          branches: [{ id: 'c1', opensState: 'open', role: 'tool' }],
          servo: { channel: 0, referenceAngle: 10, moveMs: 600, holdAtRest: false },
        },
        {
          id: 'bv-jnt', type: 'selector', name: 'Jointer valve', controllerId: 'primary', kind: 'servoGate',
          states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
          branches: [{ id: 'j1', opensState: 'open', role: 'tool' }],
          servo: { channel: 1, referenceAngle: 10, moveMs: 600, holdAtRest: false },
        },
        { id: 'ts-cabinet', type: 'tool', name: 'Cabinet · 4"', machineId: 'table-saw' },
        { id: 'jnt-port',   type: 'tool', name: 'Jointer · 4"', machineId: 'jointer' },
      ],
      ducts: [
        { child: 'bv-cab', parent: 'dc-big' },
        { child: 'bv-jnt', parent: 'dc-big' },
        { child: 'ts-cabinet', parent: 'bv-cab', parentBranch: 'c1' },
        { child: 'jnt-port',   parent: 'bv-jnt', parentBranch: 'j1' },
      ],
    },
    {
      id: 'small', name: '2.5" system',
      elements: [
        { id: 'dc-small', type: 'collector', name: 'Shop vac' },
        {
          // On its own board — see the two-boards note above.
          id: 'man', type: 'selector', name: 'Manifold', controllerId: 'slider1', kind: 'linear',
          states: [
            { id: 'home', isClosed: true,  positionMm: 0 },
            { id: 'm1',   isClosed: false, positionMm: 12.5 },
            { id: 'm2',   isClosed: false, positionMm: 95.4 },
          ],
          branches: [
            { id: 'p1', opensState: 'm1', role: 'tool' },
            { id: 'p2', opensState: 'm2', role: 'tool' },
          ],
          linear: { calibration: { stepsPerMm: 51.47, measuredSpanSteps: 4387, homeIsMaxEndstop: false, manifoldModel: 'rockler-2.5' } },
        },
        { id: 'ts-overarm', type: 'tool', name: 'Overarm · 2.5"', machineId: 'table-saw', supplemental: true },
        { id: 'drill-port', type: 'tool', name: 'Drill Press',    machineId: 'drill-press' },
      ],
      ducts: [
        { child: 'man', parent: 'dc-small' },
        { child: 'ts-overarm', parent: 'man', parentBranch: 'p1' },
        { child: 'drill-port', parent: 'man', parentBranch: 'p2' },
      ],
    },
  ],
  machines: [
    { id: 'table-saw',   name: 'Table Saw',   sensor: { outlet: { gen: 2, ip: '192.168.87.30', thresholdW: 100 } } },
    { id: 'jointer',     name: 'Jointer',     sensor: { outlet: { gen: 2, ip: '192.168.87.40', thresholdW: 40 } } },
    { id: 'drill-press', name: 'Drill Press', sensor: { outlet: { gen: 2, ip: '192.168.87.41', thresholdW: 20 } } },
  ],
  devices: [],
};

module.exports = { clone, star, primary, sliderPrimary, servoNode, sliderNode, feedChain, twoGates, twoSystemShop };
