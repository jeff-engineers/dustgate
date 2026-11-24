// jeffs-shop.js — the harness's starting shop, shaped like the real one (2026-11-23) with fake addresses:
//
//   collector ── wye ─┬─ planer gate (PWM board A, ch 0) ── Planer       (no plug: switched on by hand in the app)
//                     ├─ saw gate    (PWM board A, ch 1) ── Table saw    (Tasmota; 4 ft of servo extension in the shop)
//                     ├─ sander gate (PWM board B, ch 0) ── Drum sander  (Tasmota)
//                     ├─ miter valve (bus board, servo 2) ── Miter saw   (Tasmota; 4 ft of extension in the shop)
//                     └─ manifold    (bus board, servo 3) ─┬─ Router table (Tasmota)
//                                                          └─ Jointer      (Tasmota)
//
// The collector is started by a Shelly switching its contactor. Plugs are `127.0.0.1:<port>`; `plugs` says which port
// is which so the runner can stand a fake plug on each. Returns a fresh copy every call: scenarios edit it.
'use strict';

const BOARDS = { pwmA: 'dg-planer-saw', pwmB: 'dg-sander', bus: 'dg-serial-1' };

function shop(ports) {
  const tas = (port, w = 50) => ({ gen: 0, kind: 'tasmota', ip: `127.0.0.1:${port}`, thresholdW: w });
  const valve = (id, name, board, servo) => ({
    id, type: 'selector', name, controllerId: board, kind: 'servoGate',
    states: [{ id: 'open', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 90 }],
    branches: [{ id: 'b1', opensState: 'open', role: 'tool' }],
    servo: { ...servo, referenceAngle: 10, detented: true },
  });
  const tool = (id, name) => ({ id, type: 'tool', name, machineId: id });
  return {
    schemaVersion: 2,
    name: "Jeff's shop (harness)",
    controllers: [
      { id: 'brain', role: 'primary', name: 'Brain', board: 'pi' },
      { id: BOARDS.pwmA, role: 'secondary', name: 'Planer & saw', board: 'xiao_c5', link: { transport: 'wifi-ws', host: BOARDS.pwmA } },
      { id: BOARDS.pwmB, role: 'secondary', name: 'Sander', board: 'xiao_c5', link: { transport: 'wifi-ws', host: BOARDS.pwmB } },
      { id: BOARDS.bus, role: 'secondary', name: 'Serial 1', board: 'xiao_c5', drives: 'bus',
        chains: [['miter', 'manifold']], link: { transport: 'wifi-ws', host: BOARDS.bus } },
    ],
    systems: [{
      id: 'system-1', name: 'Dust collection',
      elements: [
        { id: 'dc', type: 'collector', name: 'Dust collector',
          control: { outlet: { gen: 2, kind: 'shelly', ip: `127.0.0.1:${ports.collector}` } } },
        { id: 'wye', type: 'junction', name: 'Wye' },
        valve('planerGate', 'Planer gate', BOARDS.pwmA, { channel: 0 }),
        valve('sawGate', 'Saw gate', BOARDS.pwmA, { channel: 1 }),
        valve('sanderGate', 'Sander gate', BOARDS.pwmB, { channel: 0 }),
        valve('miter', 'Miter valve', BOARDS.bus, { bus: 2 }),
        { id: 'manifold', type: 'selector', name: 'Router/jointer manifold', controllerId: BOARDS.bus, kind: 'servoManifold',
          states: [{ id: 'left', isClosed: false, offsetDeg: 0 }, { id: 'closed', isClosed: true, offsetDeg: 80 },
                   { id: 'right', isClosed: false, offsetDeg: 161 }],
          branches: [{ id: 'mL', opensState: 'left', role: 'tool' }, { id: 'mR', opensState: 'right', role: 'tool' }],
          servo: { bus: 3, referenceAngle: 90, detented: true } },
        tool('planer', 'Planer'), tool('saw', 'Table saw'), tool('sander', 'Drum sander'),
        tool('miterSaw', 'Miter saw'), tool('router', 'Router table'), tool('jointer', 'Jointer'),
      ],
      ducts: [
        { child: 'wye', parent: 'dc' },
        { child: 'planerGate', parent: 'wye' }, { child: 'sawGate', parent: 'wye' }, { child: 'sanderGate', parent: 'wye' },
        { child: 'miter', parent: 'wye' }, { child: 'manifold', parent: 'wye' },
        { child: 'planer', parent: 'planerGate', parentBranch: 'b1' },
        { child: 'saw', parent: 'sawGate', parentBranch: 'b1' },
        { child: 'sander', parent: 'sanderGate', parentBranch: 'b1' },
        { child: 'miterSaw', parent: 'miter', parentBranch: 'b1' },
        { child: 'router', parent: 'manifold', parentBranch: 'mL' },
        { child: 'jointer', parent: 'manifold', parentBranch: 'mR' },
      ],
    }],
    machines: [
      { id: 'planer', name: 'Planer' },
      { id: 'saw', name: 'Table saw', sensor: { outlet: tas(ports.saw) } },
      { id: 'sander', name: 'Drum sander', sensor: { outlet: tas(ports.sander) } },
      { id: 'miterSaw', name: 'Miter saw', sensor: { outlet: tas(ports.miterSaw) } },
      { id: 'router', name: 'Router table', sensor: { outlet: tas(ports.router) } },
      { id: 'jointer', name: 'Jointer', sensor: { outlet: tas(ports.jointer) } },
    ],
    devices: [],
  };
}

/** The plugs this shop names: which fake to stand where. */
const PLUGS = [
  { key: 'collector', kind: 'shelly', mac: 'AA:00:00:00:00:01', blowerW: 1800 },
  { key: 'saw', kind: 'tasmota', mac: 'AA:00:00:00:00:02' },
  { key: 'sander', kind: 'tasmota', mac: 'AA:00:00:00:00:03' },
  { key: 'miterSaw', kind: 'tasmota', mac: 'AA:00:00:00:00:04' },
  { key: 'router', kind: 'tasmota', mac: 'AA:00:00:00:00:05' },
  { key: 'jointer', kind: 'tasmota', mac: 'AA:00:00:00:00:06' },
];

module.exports = { shop, BOARDS, PLUGS };
