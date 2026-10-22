#!/usr/bin/env node
// check-brain-parity.js — the two brains must run the same policy.
//
// There are two brains: the ESP32 primary (firmware/firmware.ino and the files it compiles) and the native brain
// (native/src/). They share the decision code in firmware/control/ and firmware/outlets/, and each has a SHELL — a main
// loop that feeds that code readings and applies what it decides. Drift happens in the shells: on 2026-10-18 the Pi brain
// was found never to have run plug relocation at all, because the loop that drove control/OutletRelocate.h lived only in
// firmware.ino. The shared header was host-tested, so nothing failed.
//
// THE RULE THIS ENFORCES: every shared header the ESP32 brain reaches (control/, outlets/, sensing/, utils/ — followed
// through includes) is reached by the native brain too, unless it is listed in ESP32_ONLY below with the reason it cannot
// apply to a brain with no pads. A new shared header used by one brain fails here until the other uses it or the list
// says why not. The reverse is checked too (NATIVE_ONLY).
//
// Including is necessary, not sufficient: a header can be included and its policy never ticked. So a policy is written as a
// class or function both shells CALL, and ENTRY_POINTS lists the names each shell must mention.
//
// THE ESP32 BRAIN IS FROZEN (jeff, 2026-10-18; root CLAUDE.md): the Pi is the brain. So the check is one-sided where it
// matters: the native brain missing something the ESP32 has FAILS (the Pi lacks a feature); the ESP32 missing something the
// native brain has is LISTED, not failed — new work goes into the shared code and the native shell only. The policies in
// ENTRY_POINTS that both shells already call stay required in both, so the frozen shell cannot silently lose one either.
//
// Run: `npm run brain:parity` (tools/), also run by `npm run firmware:test`.

'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const FW = path.join(ROOT, 'firmware');
const SHARED_DIRS = ['control', 'outlets', 'sensing', 'utils', 'api'];

// Shared headers only an ESP32 brain can use, and why. A brain with no pads cannot read an ADC, key a pin or light a pixel;
// anything else here is drift.
const ESP32_ONLY = {
  'api/HttpApiServer.h': 'the ESP32\'s web server (ESPAsyncWebServer); its routes are the drift listed under KNOWN_DRIFT api/ApiCore.h',
  'control/ControlInput.h': 'the board\'s own buttons',
  'control/FaultPolicy.h': 'which begin() failure of THIS board\'s hardware costs which capability',
  'control/LocalActuatorBus.h': 'servos on this board\'s own pads',
  'control/NodeLinkPlan.h': 'which ESP32 WebSocket clients to stop and start; the native NodeHub keeps one session per node',
  'control/NodeRegistry.h': 'pairings in NVS; the native brain keeps nodes.json',
  'control/RemoteActuatorBus.h': 'the ESP32 transport around NodeSession (shared by both); the native one is SessionBus.h',
  'control/OutletSweep.h': 'the ESP32 sweep (one address per loop pass); the native brain has its own threaded Sweep.h',
  'control/RfCollectorPresser.h': 'keys this board\'s own transmitter pad',
  'control/RfAddressGuess.h': 'tries DIP addresses on this board\'s own transmitter pad, supervised at the bench',
  'control/SerialDebugControl.h': 'the USB serial console',
  'control/SmartOutletControl.h': 'the ESP32 plug poll task; the native brain\'s is PlugPoller.h, around the same drivers',
  'control/TopologyStore.h': 'the layout in LittleFS; the native brain writes a file',
  'sensing/ClampJack.h': 'reads this board\'s own jack',
  'sensing/CtSensor.h': 'reads this board\'s own ADC',
  'sensing/CtTrip.h': 'decides on this board\'s own clamp',
  'utils/BinSensor.h': 'reads this board\'s own bin pad',
  'outlets/OutletConfig.h': 'plug slots in NVS',
  'utils/Diag.h': 'ESP32 heap and CPU figures',
  'utils/LinkLog.h': 'the link log on flash; the native brain writes the same JSON Lines from Log.h',
  'utils/MdnsLock.h': 'ESP-IDF\'s single mDNS querier',
  'utils/MotionMath.h': 'stop positions of this board\'s own rack',
  'utils/ResetReason.h': 'why this chip reset',
  'utils/SerialCapture.h': 'tees the ESP32 Serial into its log ring',
  'utils/StatusScreenModel.h': 'what this board\'s OLED says',
  'utils/StatusVocab.h': 'this board\'s pixel and OLED vocabulary',
  'utils/WiFiConfig.h': 'ESP32 WiFi credentials in NVS',
  'utils/BuildStamp.h': 'the ESP32 build stamp; the native brain stamps from git at make time',
  'utils/MdnsQuery.h': 'ESP-IDF mDNS; the native brain has no browser (its sweep finds Shellys too)',
  'utils/OtaGuard.h': 'ESP32 OTA probation and rollback; the Pi\'s update.sh rolls back',
  'utils/SerialLog.h': 'the USB serial ring; the native brain has Log.h',
  'utils/StatusLed.h': 'this board\'s pixel',
  'utils/StatusScreen.h': 'this board\'s OLED',
  'utils/WakeButton.h': 'this board\'s wake button',
  'utils/Watchdog.h': 'the ESP32 task watchdog; systemd watches the Pi brain',
  'utils/WiFiProvisioner.h': 'ESP32 WiFi credentials; the Pi joins WiFi through the OS',
};
// KNOWN drift, written down so it is not forgotten: allowed to pass, printed on every run, each with its TODO.md entry.
// Shrink this list; never grow it to make a new failure go away.
const KNOWN_DRIFT = {
  'api/ApiCore.h': 'the API\'s request handling: the native brain serves ~20 routes through it (tested, test_apicore.cpp); the ESP32\'s '
    + 'HttpApiServer.cpp still has its OWN copy of every one — parked with the frozen ESP32 brain, TODO.md "ESP32 routes onto api/ApiCore.h"',
  'outlets/Provision.h': 'claiming a plug: the native brain uses the shared sequence, the ESP32 still has its own copy in '
    + 'SmartOutletControl::provisionPushOutlets() (same plugclaim rule, its own steps) — parked with the frozen ESP32 brain, TODO.md "Move the ESP32\'s plug provisioning loop onto outlets/Provision.h"',
};
const NATIVE_ONLY = {};

// Policies each shell must CALL, not only include: [name, why, esp32]. `esp32: false` for a policy added after the freeze —
// the native shell must call it, the frozen ESP32 shell need not.
const ENTRY_POINTS = [
  ['DeviceProblems', 'board and plug problems'],
  ['driveCollectorPress', 'the collector press retry policy'],
  ['PlugRelocator', 'finding a plug again, and noticing a different plug at its address'],
  ['beaconEveryMs', 'the beacon\'s pace'],
  ['hourlyNodesField', 'the link log\'s hourly line'],
  ['outletPollDue', 'whether a plug is asked for its power now (the push-quiet fallback)'],
  ['boardLight', 'what each board\'s lights say about the shop (D-86)', false],
];

const ESP32_ROOTS = ['firmware.ino', 'api/HttpApiServer.cpp'].map((f) => path.join(FW, f));
const NATIVE_DIR = path.join(ROOT, 'native', 'src');
// What the brain binary is compiled from (native/Makefile SRCS), followed through includes — not every file in the
// directory, or a header nothing includes any more would count as used.
const NATIVE_ROOTS = (fs.readFileSync(path.join(ROOT, 'native', 'Makefile'), 'utf8').match(/^SRCS\s*:=\s*(.*)$/m) || ['', ''])[1]
  .split(/\s+/).filter((f) => /^src\/.*\.cpp$/.test(f)).map((f) => path.join(ROOT, 'native', f));
if (!NATIVE_ROOTS.length) { console.error('could not read SRCS from native/Makefile'); process.exit(1); }

// Where a bare `#include "X.h"` resolves for the native build (its -I list), and for the ESP32 (relative + firmware/).
const NATIVE_INC = [NATIVE_DIR, ...SHARED_DIRS.map((d) => path.join(FW, d)), path.join(FW, 'api'), FW];
function resolve(from, name, native) {
  const cands = [path.join(path.dirname(from), name), path.join(FW, name)];
  if (native) for (const d of NATIVE_INC) cands.push(path.join(d, name));
  return cands.find((c) => fs.existsSync(c));
}

function reach(roots, native) {
  const seen = new Set();
  const stack = [...roots];
  while (stack.length) {
    const f = stack.pop();
    if (seen.has(f)) continue;
    seen.add(f);
    const text = fs.readFileSync(f, 'utf8');
    for (const m of text.matchAll(/^\s*#\s*include\s+"([^"]+)"/gm)) {
      const r = resolve(f, m[1], native);
      if (r && r.startsWith(ROOT)) stack.push(r);
      const cpp = r && r.replace(/\.h$/, '.cpp');
      if (cpp && cpp !== r && fs.existsSync(cpp)) stack.push(cpp);
    }
  }
  return seen;
}
const sharedOf = (set) => new Set([...set].map((f) => path.relative(FW, f).split(path.sep).join('/'))
  .filter((r) => SHARED_DIRS.some((d) => r.startsWith(d + '/')) && r.endsWith('.h')));

const esp = reach(ESP32_ROOTS, false);
const nat = reach(NATIVE_ROOTS, true);
const espShared = sharedOf(esp);
const natShared = sharedOf(nat);
const problems = [];

for (const h of [...espShared].sort()) {
  if (natShared.has(h) || ESP32_ONLY[h] || KNOWN_DRIFT[h]) continue;
  problems.push(`${h}: the ESP32 brain uses it and the native brain does not — use it there, or list it in ESP32_ONLY with why it cannot apply`);
}
const frozenLacks = [];
for (const h of [...natShared].sort()) {
  if (espShared.has(h) || NATIVE_ONLY[h] || KNOWN_DRIFT[h]) continue;
  frozenLacks.push(h);   // the frozen ESP32 brain does not have it: listed, not failed
}
for (const h of Object.keys(ESP32_ONLY)) {
  if (natShared.has(h)) problems.push(`${h}: listed ESP32_ONLY but the native brain uses it now — take it off the list`);
  if (!fs.existsSync(path.join(FW, h))) problems.push(`${h}: listed ESP32_ONLY but no longer exists — take it off the list`);
}

// A brain's SHELL is its own code: everything outside the shared directories, plus the platform-only files inside them
// (ESP32_ONLY — SmartOutletControl.cpp is the ESP32's plug poller as much as PlugPoller.h is the native one's).
const rel = (f) => path.relative(FW, f).split(path.sep).join('/');
const isShell = (f, only) => {
  const r = rel(f);
  if (!SHARED_DIRS.some((d) => r.startsWith(d + '/'))) return true;
  return Boolean(only[r] || only[r.replace(/\.cpp$/, '.h')]);
};
// Code only: a policy named in a comment is not a policy called.
const stripComments = (t) => t.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/.*$/gm, '');
const textOf = (set, only) => [...set].filter((f) => isShell(f, only)).map((f) => stripComments(fs.readFileSync(f, 'utf8'))).join('\n');
const espShell = textOf(esp, ESP32_ONLY);
const natShell = textOf(nat, NATIVE_ONLY);
for (const [name, why, esp32 = true] of ENTRY_POINTS) {
  const re = new RegExp(`\\b(${name})\\b`);
  if (esp32 && !re.test(espShell)) problems.push(`${name} (${why}): the ESP32 brain's shell never calls it`);
  else if (!esp32 && !re.test(espShell)) frozenLacks.push(`${name} (${why})`);
  if (!re.test(natShell)) problems.push(`${name} (${why}): the native brain's shell never calls it`);
}

for (const [h, why] of Object.entries(KNOWN_DRIFT)) {
  if (espShared.has(h) && natShared.has(h)) problems.push(`${h}: listed KNOWN_DRIFT but both brains use it now — take it off the list`);
  else console.warn(`known drift: ${h} — ${why}`);
}

for (const h of frozenLacks) console.warn(`frozen ESP32 brain lacks: ${h}`);

if (problems.length) {
  console.error(problems.join('\n'));
  console.error(`\n${problems.length} brain-parity problem(s). See the header of tools/check-brain-parity.js.`);
  process.exit(1);
}
console.log(`brains ok — ${[...natShared].filter((h) => espShared.has(h)).length} shared headers in both, ${frozenLacks.length} thing(s) the frozen ESP32 brain lacks, ${Object.keys(ESP32_ONLY).length} ESP32-only, ${ENTRY_POINTS.length} policies checked`);
