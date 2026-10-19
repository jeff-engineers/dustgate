// plug-identity.js — is the plug at a saved address still the plug the layout means?
//
// The JS twin of samePlug() in firmware/control/OutletRelocate.h, and the C++ is the reference: the brain decides, and
// the app must agree, or it hides a plug the brain no longer counts as anyone's (2026-10-18 — a new Tasmota leased the
// address the Table Saw's Shelly had left, and the app called it the Table Saw's). The reasoning is in that header.
//
// `plug-identity.test.js` ↔ the "S" block of `firmware/test/test_relocate.cpp`: same cases, same order.
// PURE. No I/O.

'use strict';

/** "aa:bb:cc:dd:ee:ff", "AA-BB-..." and "AABBCCDDEEFF" are one plug; '' for anything that is not six bytes of hex. */
function normMac(m) {
  const hex = String(m == null ? '' : m).replace(/[:\-. ]/g, '').toUpperCase();
  if (!/^[0-9A-F]{12}$/.test(hex)) return '';
  return hex.match(/../g).join(':');
}

/** The layout's default when it does not say: a Shelly. */
function normKind(k) { return k === 'tasmota' ? 'tasmota' : 'shelly'; }

/** 'yes' | 'no' | 'unknown'. `saved` is what the layout recorded ({kind, mac}); `seen` what answered there ({reachable, kind, mac}). */
function samePlug(saved, seen) {
  if (!seen || !seen.reachable) return 'unknown';
  const a = normMac(saved && saved.mac), b = normMac(seen.mac);
  if (a && b) return a === b ? 'yes' : 'no';
  if (normKind(saved && saved.kind) !== normKind(seen.kind)) return 'no';
  return 'unknown';
}

module.exports = { normMac, normKind, samePlug };
