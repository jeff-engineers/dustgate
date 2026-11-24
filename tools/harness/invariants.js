// invariants.js — what must be true of the shop after EVERY step a person takes, whatever the step was. A scenario that
// breaks one fails at the step that broke it, with the sentence below saying what a woodworker would have seen.
//
// Checked against the boards themselves (where the fake nodes last put each gate) and the plugs (whether the collector's
// switch is on), not only against what the brain says, so "the brain thinks it's open" is not mistaken for "it's open".
'use strict';

function selectors(doc) {
  const out = [];
  for (const s of doc.systems || []) for (const e of s.elements || []) if (e.type === 'selector') out.push({ sys: s.id, e });
  return out;
}
function openState(e, stateId) {
  const st = (e.states || []).find((x) => x.id === stateId);
  return !!st && !st.isClosed;
}

/**
 * @param {import('./bench').Bench} bench
 * @param {object} doc  the layout the scenario last saved (what the person believes the shop is)
 * @param {{allowRefused?: string[]}} opts  gates a scenario EXPECTS a board to refuse (a gate on the wrong kind of board)
 * @returns {Promise<string[]>} one sentence per broken rule; empty when all hold
 */
async function check(bench, doc, opts = {}) {
  const broken = [];
  const status = await bench.status();
  const running = await bench.layout();

  // 1. The layout saved is the layout the brain runs.
  //    The brain writes back what it learns about a plug (its MAC, pinning its identity) and spells a Shelly by leaving
  //    `kind` out; neither is a change to the shop, so both are set aside before comparing.
  if (JSON.stringify(comparable(running)) !== JSON.stringify(comparable(doc)))
    broken.push('the brain is running a different layout from the one that was saved');

  // 2. Never dead-head: a running collector has at least one gate open in its system, at the boards.
  const collectorPlug = bench.plugs.collector;
  const sels = selectors(doc);
  for (const sys of doc.systems || []) {
    const sysOn = collectorPlug ? collectorPlug.on : !!(status.systems?.[sys.id]?.collectorOn);
    if (!sysOn) continue;
    const gates = sels.filter((s) => s.sys === sys.id);
    if (!gates.length) continue;
    const anyOpen = gates.some(({ e }) => {
      const n = bench.nodes[e.controllerId];
      return n && n.online && openState(e, n.gateAt[e.id]);
    });
    if (!anyOpen) broken.push(`${sys.name || sys.id}: the collector is running with every gate closed (dead-headed)`);
  }

  // 3. Every tool that's running has its path open.
  for (const [tool, t] of Object.entries(status.tools || {})) {
    if (!t.active) continue;
    const m = status.machines?.[tool];
    if (!m || m.status !== 'routed') broken.push(`${tool} is running but its path is not open (${m ? m.status : 'not routed'})`);
  }

  // 4. Where the brain says each gate is, the board agrees (for boards that are on).
  for (const { e } of sels) {
    const n = bench.nodes[e.controllerId];
    if (!n || !n.online) continue;
    if ((opts.allowRefused || []).includes(e.id)) continue;
    const said = status.actuators?.[e.id];
    if (said !== undefined && n.gateAt[e.id] !== undefined && said !== n.gateAt[e.id])
      broken.push(`${e.name}: the brain says ${said} but the board put it ${n.gateAt[e.id]}`);
  }

  // 5. No board refused a move, unless the scenario set that up on purpose.
  for (const n of Object.values(bench.nodes)) {
    for (const r of n.refused) if (!(opts.allowRefused || []).includes(r.selectorId))
      broken.push(`board ${n.id} refused to move ${r.selectorId}: ${r.why}`);
  }

  // 6. Every problem the brain raises is about something in the layout (nothing left over from before an edit).
  const ids = new Set();
  for (const c of doc.controllers || []) ids.add(c.id);
  for (const s of doc.systems || []) { ids.add(s.id); for (const e of s.elements || []) ids.add(e.id); }
  for (const m of doc.machines || []) { ids.add(m.id); const o = m.sensor?.outlet; if (o?.ip) ids.add(o.ip); }
  for (const s of doc.systems || []) for (const e of s.elements || []) for (const o of [e.control?.outlet, e.sensor?.outlet]) if (o?.ip) ids.add(o.ip);
  for (const p of (await bench.problems()).problems || []) {
    const subject = p.subject?.id;
    if (subject && !ids.has(subject)) broken.push(`a problem (${p.code}) is about "${subject}", which the layout no longer has`);
  }
  return broken;
}

function comparable(doc) {
  const d = JSON.parse(JSON.stringify(doc));
  const plug = (o) => { if (!o) return; delete o.mac; if (o.kind === 'shelly') delete o.kind; };
  for (const m of d.machines || []) plug(m.sensor?.outlet);
  for (const s of d.systems || []) for (const e of s.elements || []) { plug(e.control?.outlet); plug(e.sensor?.outlet); }
  return sortKeys(d);
}

function sortKeys(v) {
  if (Array.isArray(v)) return v.map(sortKeys);
  if (v && typeof v === 'object') return Object.fromEntries(Object.keys(v).sort().map((k) => [k, sortKeys(v[k])]));
  return v;
}

module.exports = { check, selectors, openState };
