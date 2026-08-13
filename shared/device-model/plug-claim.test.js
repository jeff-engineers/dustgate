// plug-claim.test.js — pure unit tests for plug ownership (RFC §8).
//
// The C++ side (firmware/outlets/PlugClaim.h, exercised by test_plugclaim.cpp)
// must agree with this file case-for-case. The two engines can't share code, so
// the paired assertions ARE the anti-drift mechanism — same shape as
// nodelink.test.js / NodeLink.h.
//
// Run: `node plug-claim.test.js`, or `npm run plugclaim:test`.

'use strict';

const PC = require('./plug-claim');

const results = [];
const check = (name, cond, detail = '') => results.push({ name, ok: !!cond, detail });
const eq = (name, got, want) =>
  check(name, JSON.stringify(got) === JSON.stringify(want),
        `got ${JSON.stringify(got)} want ${JSON.stringify(want)}`);

// Our brain, as it appears in the URL plugs dial back to.
const US = { ourHost: '10.0.0.2', ourName: 'dustgate-shop' };
const ourUrl = 'ws://10.0.0.2:80/shelly-rpc';

// ── names carry the owner for HUMANS ───────────────────────────────────────
{
  eq('format appends the owner', PC.formatPlugName('Table Saw', 'dustgate-big'),
     'Table Saw · dustgate-big');
  eq('no owner leaves the label alone', PC.formatPlugName('Table Saw', null), 'Table Saw');
  eq('round trip', PC.parsePlugName(PC.formatPlugName('Table Saw', 'dustgate-big')),
     { label: 'Table Saw', owner: 'dustgate-big' });
  eq('an unsuffixed name has no owner', PC.parsePlugName('Table Saw'),
     { label: 'Table Saw', owner: null });
  // Splitting on the LAST separator: a label someone typed with a middle dot in
  // it is still a label, and our suffix is always last.
  eq('splits on the last separator', PC.parsePlugName('Saw · bench · dustgate-big'),
     { label: 'Saw · bench', owner: 'dustgate-big' });
  eq('empty name is empty label', PC.parsePlugName(''), { label: '', owner: null });
}

// ── URL parts ──────────────────────────────────────────────────────────────
{
  eq('host from a push URL', PC.wsHost(ourUrl), '10.0.0.2');
  eq('host without a port', PC.wsHost('ws://ha.local/api/shelly'), 'ha.local');
  eq('host of nothing', PC.wsHost(''), '');
  eq('path', PC.wsPath(ourUrl), '/shelly-rpc');
  eq('path of a foreign target', PC.wsPath('ws://ha.local/api/shelly/ws'), '/api/shelly/ws');
}

// ── the four states (RFC §8.3) ─────────────────────────────────────────────
{
  const c = PC.claimOf({ ...US, pushUrl: '', name: 'shellyplus1pm-abc' });
  eq('no push target → unclaimed', c.state, 'unclaimed');
  check('unclaimed is pickable', c.pickable);
  check('and we may point it at us', PC.mayRepoint(c));
}
{
  const c = PC.claimOf({ ...US, pushUrl: ourUrl, name: 'Table Saw · dustgate-shop' });
  eq('pushing at us → ours', c.state, 'ours');
  check('ours is pickable', c.pickable);
  check('ours may be repointed', PC.mayRepoint(c));
  eq('and the suffix is stripped for display', c.label, 'Table Saw');
}
{
  // The case the whole file exists for. Another brain owns it; pairing here
  // would silently stop the OTHER shop hearing that its saw started.
  const c = PC.claimOf({ ...US, pushUrl: 'ws://10.0.0.9:80/shelly-rpc',
                         name: 'Bandsaw · dustgate-bench' });
  eq('another DustGate → dustgate', c.state, 'dustgate');
  check('not pickable', !c.pickable);
  check('and NEVER repointed', !PC.mayRepoint(c));
  eq('the reason names the owner', c.reason, 'owned by dustgate-bench');
}
{
  // Name is a hint, not the authority: with no suffix we still refuse, and say
  // who by address rather than pretending not to know.
  const c = PC.claimOf({ ...US, pushUrl: 'ws://10.0.0.9:80/shelly-rpc', name: 'Bandsaw' });
  eq('an unnamed peer is still refused', c.state, 'dustgate');
  eq('named by address instead', c.reason, 'owned by 10.0.0.9');
}
{
  const c = PC.claimOf({ ...US, pushUrl: 'ws://ha.local/api/shelly/ws', name: 'Dust Collector' });
  eq('a non-DustGate target → foreign', c.state, 'foreign');
  check('still pickable — polling needs no permission', c.pickable);
  check('but its Ws config is left alone', !PC.mayRepoint(c));
  eq('and the UI says so plainly', c.reason, 'shared with ha.local — polled, not pushed');
}

// ── enable:false is a leftover, not an owner ───────────────────────────────
{
  const c = PC.claimOf({ ...US, pushUrl: 'ws://10.0.0.9:80/shelly-rpc', pushEnabled: false,
                         name: 'Bandsaw' });
  eq('a disabled push target is unclaimed', c.state, 'unclaimed');
  check('so it can be claimed normally', PC.mayRepoint(c));
}

// ── the authority is the URL, not the name ─────────────────────────────────
{
  // Someone renamed it in the Shelly app to claim it for another brain. The push
  // target still says us, so it is still ours — names are user-editable and this
  // is exactly why they can't be the decision.
  const c = PC.claimOf({ ...US, pushUrl: ourUrl, name: 'Table Saw · dustgate-bench' });
  eq('a misleading suffix does not hand the plug away', c.state, 'ours');
  check('still repointable', PC.mayRepoint(c));
}
{
  // ...and the reverse: our own name on it, pointed at a DIFFERENT DustGate
  // address. That is our own plug after a DHCP lease renewal — our IP moved and
  // every plug we own still dials the old one. On address alone we would refuse
  // to repair our entire shop, so the name breaks the tie here.
  const c = PC.claimOf({ ...US, pushUrl: 'ws://10.0.0.9:80/shelly-rpc',
                         name: 'Table Saw · dustgate-shop' });
  eq('our own suffix at a stale address is still ours', c.state, 'ours');
  check('flagged stale', c.stale === true);
  check('and repointable, which IS the repair', PC.mayRepoint(c));
  eq('the reason explains the address', c.reason,
     'pointed at a stale address (10.0.0.9) — will be repaired');
}
{
  // The tie-break is narrow ON PURPOSE. It never applies against a foreign app:
  // Home Assistant's plug stays Home Assistant's however it is named.
  const c = PC.claimOf({ ...US, pushUrl: 'ws://ha.local/api/shelly/ws',
                         name: 'Table Saw · dustgate-shop' });
  eq('our suffix cannot take a plug from a foreign app', c.state, 'foreign');
  check('still never repointed', !PC.mayRepoint(c));
}
{
  // ...nor for a suffix naming a DIFFERENT brain — the case it would be
  // dangerous to get wrong.
  const c = PC.claimOf({ ...US, pushUrl: 'ws://10.0.0.9:80/shelly-rpc',
                         name: 'Table Saw · dustgate-bench' });
  eq("another brain's suffix is refused", c.state, 'dustgate');
  check('not repointable', !PC.mayRepoint(c));
}
{
  // With no ourName supplied, the address is the only evidence there is.
  const c = PC.claimOf({ ourHost: '10.0.0.2', pushUrl: 'ws://10.0.0.9:80/shelly-rpc',
                         name: 'Table Saw · dustgate-shop' });
  eq('no ourName → no tie-break', c.state, 'dustgate');
}

// ── case and port differences are not ownership differences ────────────────
{
  const c = PC.claimOf({ ...US, pushUrl: 'ws://10.0.0.2:8080/shelly-rpc', name: 'Saw' });
  eq('a different port on our own host is still ours', c.state, 'ours');
  const d = PC.claimOf({ ourHost: 'DustGate-Shop.local', pushUrl: 'ws://dustgate-shop.local:80/shelly-rpc' });
  eq('host comparison is case-insensitive', d.state, 'ours');
}

// ── takeover: allowed, but only by a human who was told what breaks ────────
//
// The other half of "never steal a plug". Refusing outright would just mean
// doing it in the Shelly app instead, with no record of what changed — a shop
// gets rearranged, a brain gets replaced, a bench plug becomes a shop plug.
{
  const c = PC.claimOf({ ...US, pushUrl: 'ws://10.0.0.9:80/shelly-rpc',
                         name: 'Bandsaw · dustgate-bench' });
  check('a background pass may never take it', !PC.mayRepoint(c));
  check('...not even by asking twice', !PC.mayRepoint(c, false));
  check('a confirmed user may', PC.mayRepoint(c, true));
  check('it is offered as takeable', c.takeable === true);
  eq('and the holder is named', c.holder, 'dustgate-bench');
  check('the warning names what stops working',
    PC.takeoverWarning(c).includes('dustgate-bench') &&
    PC.takeoverWarning(c).includes('stop switching its collector'),
    PC.takeoverWarning(c));
}
{
  // A foreign app can be taken over too, with its own consequence: whatever the
  // user built over there goes quiet, which is a different sentence.
  const c = PC.claimOf({ ...US, pushUrl: 'ws://ha.local/api/shelly/ws', name: 'Saw' });
  check('foreign is takeable with confirmation', PC.mayRepoint(c, true));
  check('and the warning is about their automations',
    PC.takeoverWarning(c).includes('ha.local') &&
    PC.takeoverWarning(c).includes('automations'), PC.takeoverWarning(c));
}
{
  // Nothing to take: no dialog should ever appear for these.
  const mine = PC.claimOf({ ...US, pushUrl: ourUrl, name: 'Saw · dustgate-shop' });
  const free = PC.claimOf({ ...US, pushUrl: '', name: 'Saw' });
  check('ours is not takeable', mine.takeable === false);
  check('unclaimed is not takeable', free.takeable === false);
  eq('and neither has a warning', [PC.takeoverWarning(mine), PC.takeoverWarning(free)],
     [null, null]);
}

// ── report ──────────────────────────────────────────────────────────────────
let failed = 0;
for (const r of results) {
  if (!r.ok) failed++;
  console.log(`  ${r.ok ? '✓' : '✗'} ${r.name}${r.ok || !r.detail ? '' : `  — ${r.detail}`}`);
}
console.log(`\n${results.length - failed}/${results.length} passed${failed ? `, ${failed} FAILED` : ''}`);
process.exit(failed ? 1 : 0);
