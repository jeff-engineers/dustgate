// board-id.js — ONE rule for "which board is this id".
//
// The JS twin of firmware/control/BoardId.h, and the C++ is the reference. The same node
// is legitimately "node-1" (what you paired, what mDNS advertises) and "node-1.local" (what
// you must dial, and so what /api/nodes reports and what the UI writes back into link.host).
// Comparing those with === is a shop whose link is green while every gate on it is
// un-commandable. Three copies of the rule existed (here, tools/mock-api.js, the UI's
// board-setup); they now all import this one.
//
// `board-id.test.js` ↔ `firmware/test/test_boardid.cpp`: same cases, same order.
// PURE. No I/O.

'use strict';

/** The canonical spelling: case-folded, a trailing dot and the ".local" suffix removed. */
function bareHost(h) {
  let s = String(h == null ? '' : h);
  if (s.endsWith('.')) s = s.slice(0, -1);
  if (s.length > '.local'.length && s.slice(-'.local'.length).toLowerCase() === '.local') s = s.slice(0, -'.local'.length);
  return s.toLowerCase();
}

/** Does this controllerId mean THIS board? Absent means this board; so does its own id, in any
 *  spelling. A board that does not know its own id (`ownId` empty) claims no NAMED board. */
function isOwnBoard(controllerId, ownId) {
  return !controllerId || bareHost(controllerId) === bareHost(ownId);
}

/** Do two controllerIds name the same board? */
function sameBoard(a, b, ownId) {
  if (isOwnBoard(a, ownId) && isOwnBoard(b, ownId)) return true;
  return bareHost(a) === bareHost(b);
}

module.exports = { bareHost, isOwnBoard, sameBoard };
