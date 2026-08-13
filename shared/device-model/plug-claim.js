// plug-claim.js — who owns a smart plug, and what we're allowed to do to it.
//
// docs/shop-schema-rfc.md §8. The LAN is not ours alone: Home Assistant, the
// Shelly app, a bench brain and a shop brain all see every plug on it. Pairing
// repoints a plug's push target via `Ws.SetConfig`, which SILENTLY steals it —
// the previous owner simply stops hearing that the tool started, with no error
// on either side. That is the failure this file exists to prevent.
//
// TWO SIGNALS, DIFFERENT JOBS:
//
//   The push target is the AUTHORITY, for the machine. Read `Ws.GetConfig` and
//   compare its server URL against ours. It says who owns the plug regardless of
//   what anyone renamed it to.
//
//   The friendly name carries the owner, for HUMANS. "Table Saw · dustgate-big"
//   is legible in the Shelly app, where no DustGate UI is present to explain
//   anything. It is a hint and never the decision: names are user-editable, and
//   renaming one in the Shelly app is already a documented breakage
//   (docs/outlet-discovery-provisioning.md).
//
// THE RULE THAT FALLS OUT: a plug already pointed at another controller is
// pairable READ-ONLY, by polling. We never rewrite its `Ws` config. Push is an
// optimization (latency, less traffic), not a capability — polling already gets
// us the one thing we need from a sensor plug, and the per-outlet polling
// fallback exists today for when a push connection is down. So there is no role
// that REQUIRES the theft, and "should we take it?" stops being a question the
// UI has to ask on every pairing.
//
// PURE. No state, no I/O. Mirrored in firmware/outlets/PlugClaim.h.

'use strict';

// U+00B7 MIDDLE DOT, spaced. Chosen over a hyphen because tool names contain
// hyphens ("Table Saw - shop") and the parse has to be unambiguous.
const OWNER_SEP = ' · ';

// The path a DustGate brain's push URL always ends in (ws://<ip>:80/shelly-rpc).
// It is what distinguishes "another DustGate has this" from "Home Assistant has
// this" — the two cases get opposite answers, so the distinction is load-bearing.
const DUSTGATE_WS_PATH = '/shelly-rpc';

/** "Table Saw" + "dustgate-big" → "Table Saw · dustgate-big". */
function formatPlugName(label, owner) {
  const base = String(label || '').trim();
  if (!owner) return base;
  return base ? `${base}${OWNER_SEP}${owner}` : String(owner);
}

/**
 * Split a plug's friendly name back into what to SHOW and who claims it.
 *
 * Splits on the LAST separator: a tool called "Saw · bench" that someone typed
 * by hand is still a label, and the suffix we appended is always last.
 */
function parsePlugName(name) {
  const s = String(name == null ? '' : name);
  const i = s.lastIndexOf(OWNER_SEP);
  if (i < 0) return { label: s, owner: null };
  return { label: s.slice(0, i), owner: s.slice(i + OWNER_SEP.length) || null };
}

/**
 * Host out of a push URL: "ws://10.0.0.5:80/shelly-rpc" → "10.0.0.5".
 * Deliberately hand-rolled rather than `new URL()` — this file is mirrored into
 * C++ and the shapes must stay identical.
 */
function wsHost(url) {
  const s = String(url || '');
  const scheme = s.indexOf('://');
  if (scheme < 0) return '';
  const rest = s.slice(scheme + 3);
  const end = rest.search(/[/:?]/);
  return (end < 0 ? rest : rest.slice(0, end)).toLowerCase();
}

/** Path of a push URL: "ws://10.0.0.5:80/shelly-rpc" → "/shelly-rpc". */
function wsPath(url) {
  const s = String(url || '');
  const scheme = s.indexOf('://');
  const rest = scheme < 0 ? s : s.slice(scheme + 3);
  const i = rest.indexOf('/');
  return i < 0 ? '' : rest.slice(i);
}

/**
 * Decide what a plug is, and what we may do to it.
 *
 * @param {object} p
 * @param {string} p.pushUrl  the plug's configured `Ws.server`, "" if unset
 * @param {boolean} [p.pushEnabled]  `Ws.enable`. A server with enable:false is
 *        a leftover, not an owner — treat it as unclaimed.
 * @param {string} p.ourHost  our own IP or host, as it appears in OUR push URL
 * @param {string} [p.ourName] our mDNS hostname, the owner we write into names.
 *        Used ONLY to recognise our own plug at a stale address — see below.
 * @param {string} [p.name]   the plug's current friendly name
 *
 * @returns {{state:string, owner:string|null, pickable:boolean, mode:string,
 *            reason:string|null, label:string}}
 *   state    ours | unclaimed | dustgate | foreign
 *   mode     push (we may rewrite Ws) | poll (we must not)
 *   reason   why it isn't pickable, or how it will be paired — UI text
 *   label    the name with any owner suffix stripped, for display
 */
function claimOf(p) {
  const cfg = p || {};
  const parsed = parsePlugName(cfg.name);
  const enabled = cfg.pushEnabled !== false;
  const url = enabled ? String(cfg.pushUrl || '') : '';
  const host = wsHost(url);
  const ours = wsHost(`ws://${cfg.ourHost || ''}`);

  const base = { owner: parsed.owner, label: parsed.label };

  if (!host) {
    // Nothing is pushing anywhere. The ordinary case for a plug out of the box,
    // and the one where drag-and-drop needs no dialog at all.
    return { ...base, state: 'unclaimed', pickable: true, mode: 'push',
             takeable: false, holder: null, reason: null };
  }
  if (host === ours) {
    return { ...base, state: 'ours', pickable: true, mode: 'push',
             takeable: false, holder: null, reason: null };
  }
  if (wsPath(url) === DUSTGATE_WS_PATH) {
    // A DustGate brain — but WHICH one? Our own address moves: a DHCP lease
    // renewal leaves every plug we own pointing at an IP we no longer have.
    // Compared on address alone, our entire shop would read as another brain's
    // property and we would refuse to repair it, exactly when repair is the
    // thing that has to happen (SmartOutletControl::checkLocalIpChange exists
    // for this).
    //
    // So the NAME breaks the tie, under two conditions that keep it honest:
    // only among targets already known to be DustGate brains, and only for our
    // own exact hostname. It can never hand us a plug that Home Assistant owns,
    // and it can never take one whose suffix names a different brain.
    if (cfg.ourName && parsed.owner === cfg.ourName) {
      return {
        ...base, state: 'ours', stale: true, pickable: true, mode: 'push',
        takeable: false, holder: null,
        reason: `pointed at a stale address (${host}) — will be repaired`,
      };
    }
    // Another brain. Not pickable, and NAMED — "not on offer" without a reason
    // is indistinguishable from a plug that failed to answer discovery.
    const who = parsed.owner || host;
    return {
      ...base, state: 'dustgate', pickable: false, mode: 'poll',
      // TAKEABLE, but only by a human who has been told what breaks. The poll
      // task can never do this; see mayRepoint().
      takeable: true, holder: who,
      reason: `owned by ${who}`,
    };
  }
  // Something else on the LAN is listening — Home Assistant, Node-RED, a script.
  // Pairable, by POLLING, and we leave its Ws config alone. Both owners keep
  // working and neither is told a lie.
  return {
    ...base, state: 'foreign', pickable: true, mode: 'poll',
    takeable: true, holder: host,
    reason: `shared with ${host} — polled, not pushed`,
  };
}

/**
 * May we write this plug's Ws config?
 *
 * @param {object} claim     from claimOf()
 * @param {boolean} [confirmed]  the user explicitly approved a takeover
 *
 * Two different questions, deliberately answered by one function:
 *
 *   Unconfirmed — what the poll task asks on every provisioning pass. The answer
 *   for anyone else's plug is NO, forever, without asking a human. That is what
 *   makes silent theft impossible: a background task can never take a plug.
 *
 *   Confirmed — what the config sheet asks after the user has been told, in
 *   words, what stops working. A person may absolutely repoint a plug they own
 *   in real life; a shop rearranges, a brain gets replaced, a bench plug becomes
 *   a shop plug. Refusing that outright would just mean doing it in the Shelly
 *   app with no record of what changed.
 */
const mayRepoint = (claim, confirmed = false) => {
  if (!claim) return false;
  if (claim.mode === 'push') return true;
  return !!confirmed && !!claim.takeable;
};

/**
 * The sentence to put in front of the user before a takeover, naming what
 * breaks. Never "are you sure?" — that asks nothing. It says which controller
 * stops hearing from this plug, because that is the thing they can't see.
 */
function takeoverWarning(claim) {
  if (!claim || !claim.takeable) return null;
  const who = claim.owner || claim.holder || 'the current owner';
  return claim.state === 'dustgate'
    ? `${who} will stop receiving updates from this plug, and any tool it senses there will stop switching its collector.`
    : `${who} will stop receiving push updates from this plug. Anything you have built there — automations, dashboards — goes quiet until you point it back.`;
}

module.exports = {
  OWNER_SEP, DUSTGATE_WS_PATH,
  formatPlugName, parsePlugName, wsHost, wsPath,
  claimOf, mayRepoint, takeoverWarning,
};
