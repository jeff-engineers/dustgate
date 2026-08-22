// wip-message.ts — turning a validation error into something a woodworker can act on.
//
// validateShop() speaks the schema's language, and it should: it is the contract
// firmware is certified against, its messages name the field that broke, and every
// one of them is an id because ids are what the document is made of. That is right
// for a log and wrong for the guide bar, where it surfaced as
//
//     Work in progress — saved here, but the controller won't take it yet:
//     system "s2": element "p8" must have exactly one parent duct (has 0).
//
// Nobody named anything "s2" or "p8" — the editor did, silently, and the user is
// left holding an id for a thing they know as the overarm guard on the table saw.
//
// So the translation lives HERE rather than in shared/device-model: the model is
// the anti-drift spec and its message strings are part of what the conformance
// suites pin down, while what a person should be told is a UI question and changes
// on UI grounds. Nothing below reaches back into the model — it reads the same
// document the user is editing and rewrites the sentence.
//
// TWO LAYERS, and the second one matters more than it looks:
//
//   1. A phrasing for each failure the canvas can actually produce, written as the
//      sentence you would say out loud. These carry the jargon away as well as the
//      ids — "must have exactly one parent duct (has 0)" is not more useful with a
//      name in it.
//   2. A fallback that keeps the model's own words but swaps every quoted id for
//      the name of the thing. It is deliberately dumb and deliberately total: new
//      checks land in the model without this file knowing, and the failure mode of
//      an unknown message must be "correct but stiff", never a raw id.
//
// Angular-free on purpose — see tsconfig.spec.json. Covered by wip-message.spec.ts.

import type { ShopDoc, RawEl } from './shop-doc';
import { displayName, machinesOf, systemsOf } from './shop-doc';

export interface ValidationIssue {
  code?: string;
  message: string;
  /** The id the check was about. Present on most, absent on shape-level ones. */
  ref?: string;
}

/** `system "s2": ` — how validateShop tags a per-system failure. */
const SYSTEM_PREFIX = /^system "([^"]+)": /;

/** Every quoted id in a message body. */
const QUOTED = /"([^"]+)"/g;

/**
 * What to call a piece that was never named.
 *
 * Only reachable for a document that arrived from somewhere other than this
 * editor — everything the canvas creates is born with a name — so these are a
 * backstop, not the common path.
 */
function kindWord(el: RawEl): string {
  const type = el['type'];
  if (type === 'collector') return 'the dust collector';
  if (type === 'junction') return 'an open end';
  if (type === 'tool') return 'a tool';
  if (type === 'selector') {
    const kind = el['kind'];
    return kind === 'servoGate' ? 'a ball valve'
      : kind === 'servoManifold' ? 'a manifold'
      : kind === 'linear' ? 'a sliding gate'
      : 'a gate';
  }
  return 'a piece';
}

/**
 * The name a person knows an id by, or null when the id resolves to nothing.
 *
 * Null is a real answer and the caller has to respect it: an id that names
 * nothing is usually the whole point of the error (a duct pointing at a deleted
 * piece), and inventing a label for it would hide exactly what went wrong.
 */
export function nameForId(doc: ShopDoc | null, id: string): string | null {
  if (!doc || !id) return null;

  for (const sys of systemsOf(doc)) {
    for (const el of sys.elements ?? []) {
      if (el['id'] !== id) continue;
      // displayName() carries the machine indirection: a machine's only port is
      // known by the MACHINE's name, which is the name on the canvas. It falls back
      // to the bare id when there is nothing better, and an id is precisely what we
      // are here to get rid of — so that answer counts as "unnamed".
      const shown = displayName(doc, el);
      return shown && shown !== id ? shown : kindWord(el);
    }
  }
  for (const m of machinesOf(doc)) if (m.id === id) return m.name || null;
  for (const c of (doc['controllers'] as RawEl[] | undefined) ?? []) {
    if (c['id'] === id) return (c['name'] as string) || null;
  }
  const sys = systemsOf(doc).find((s) => s.id === id);
  if (sys) return systemName(doc, sys.id);
  return null;
}

/**
 * What to call a system. Its own name if it has one, otherwise its collector's —
 * which is the right answer twice over: a system's identity IS its collector
 * (that is why the grey ground is unlabelled), and the collector is the thing
 * sitting on screen with a name under it.
 */
function systemName(doc: ShopDoc | null, systemId: string): string | null {
  const sys = systemsOf(doc).find((s) => s.id === systemId);
  if (!sys) return null;
  if (sys.name) return sys.name;
  const collector = (sys.elements ?? []).find((e) => e['type'] === 'collector');
  return (collector?.['name'] as string) || null;
}

/** `"p8"` → `“Overarm guard”`, leaving an id that resolves to nothing alone. */
function substituteIds(doc: ShopDoc | null, body: string): string {
  return body.replace(QUOTED, (whole, id: string) => {
    const name = nameForId(doc, id);
    return name ? `“${name}”` : whole;
  });
}

/** A quoted id's name, or the bare id when it resolves to nothing. */
function label(doc: ShopDoc | null, id: string | undefined): string {
  if (!id) return 'something';
  return nameForId(doc, id) ?? `“${id}”`;
}

/**
 * The hand-written phrasings, matched on the message BODY (system prefix already
 * stripped). Ordered: first match wins.
 *
 * Matching on text rather than on `code` is the deliberate choice. `code` is
 * coarse — a dozen different failures all carry `code: 'tree'` — so it can't pick
 * a sentence on its own, and pattern plus captured ids is what actually says which
 * one happened. The cost is that rewording a message in the model drops that case
 * back to the fallback, which is why the fallback has to stay good.
 */
const PHRASINGS: Array<{
  re: RegExp;
  say: (doc: ShopDoc | null, m: RegExpMatchArray) => string;
}> = [
  // ── the tree: what is and isn't piped up ──
  {
    re: /^element "([^"]+)" must have exactly one parent duct \(has 0\)$/,
    say: (d, m) => `${label(d, m[1])} isn’t piped to anything yet — run a duct to it.`,
  },
  {
    re: /^element "([^"]+)" must have exactly one parent duct \(has (\d+)\)$/,
    say: (d, m) => `${label(d, m[1])} has ${m[2]} ducts running to it. A piece takes exactly one.`,
  },
  {
    re: /^element "([^"]+)" does not reach the collector/,
    say: (d, m) => `${label(d, m[1])} doesn’t connect back to the dust collector.`,
  },
  {
    re: /^collector must have no parent duct$/,
    say: () => `The dust collector has a duct running INTO it. It’s the root — everything runs from it.`,
  },
  {
    re: /^exactly one collector required \(found (\d+)\)$/,
    say: (_d, m) => m[1] === '0'
      ? `This system has no dust collector.`
      : `This system has ${m[1]} dust collectors. Each system has exactly one.`,
  },

  // ── outlets on a gate ──
  {
    re: /^branch "[^"]+" role tool but nothing wired to it$/,
    say: () => `A gate has an outlet with nothing on it. Put a tool there, or cap it.`,
  },
  {
    re: /^branch "[^"]+" role feed but nothing wired to it$/,
    say: () => `A gate has an outlet meant to feed more ductwork, with nothing on it. Run a duct from it, or cap it.`,
  },
  {
    re: /^duct to selector "([^"]+)" needs parentBranch$/,
    say: (d, m) => `A duct leaves ${label(d, m[1])} without saying which outlet it comes off.`,
  },

  // ── boards ──
  {
    re: /^servo channel (\d+) on host "([^"]+)" is already used by "([^"]+)"$/,
    say: (d, m) => `${label(d, m[3])} is already on servo ${Number(m[1]) + 1} of ${label(d, m[2])}. `
                 + `Two gates can’t share a channel — move one to a free one.`,
  },
  {
    re: /^host "([^"]+)" has (\d+) servo selectors \(max (\d+) per host\)$/,
    say: (d, m) => `${label(d, m[1])} is driving ${m[2]} ball valves. A board drives at most ${m[3]} — pair another board.`,
  },
  {
    re: /^host "([^"]+)" has (\d+) linear selectors \(max 1 per host\)$/,
    say: (d, m) => `${label(d, m[1])} is driving ${m[2]} sliding gates. A board has one stepper, so it drives one.`,
  },
  {
    re: /^selector missing controllerId$/,
    say: () => `A gate has no board driving it.`,
  },
  {
    re: /^controllerId "([^"]+)" does not resolve$/,
    say: (_d, m) => `A gate names a board (“${m[1]}”) this shop doesn’t have. Pair it, or move the gate.`,
  },
  {
    re: /^secondary board "([^"]+)" has no link(\.host)? — the primary can't reach it$/,
    say: (_d, m) => `The primary has no address for “${m[1]}”, so it can’t drive anything on it.`,
  },
  {
    re: /^exactly one primary controller required \(found (\d+)\)$/,
    say: (_d, m) => m[1] === '0'
      ? `No board is marked as the primary — one has to be the brain.`
      : `${m[1]} boards are marked primary. Exactly one is the brain.`,
  },

  // ── plugs ──
  {
    re: /^smart outlet ([^ ]+) is on two elements \("([^"]+)" and "([^"]+)"\)$/,
    say: (d, m) => `${label(d, m[2])} and ${label(d, m[3])} are both paired to the plug at ${m[1]}. A plug belongs to one tool.`,
  },
];

/**
 * One validation error, said the way you'd say it to the person building the shop.
 *
 * The system is named only when there are two or more: with a single system the
 * prefix is true and useless, and the guide bar is one line.
 */
export function humaniseIssue(doc: ShopDoc | null, issue: ValidationIssue): string {
  const raw = (issue?.message ?? '').trim();
  if (!raw) return 'incomplete layout';

  const prefixed = raw.match(SYSTEM_PREFIX);
  const body = prefixed ? raw.slice(prefixed[0].length) : raw;

  let said = '';
  for (const p of PHRASINGS) {
    const m = body.match(p.re);
    if (m) { said = p.say(doc, m); break; }
  }
  if (!said) said = substituteIds(doc, body);

  // Only worth naming the system when the shop has more than one to confuse.
  const many = systemsOf(doc).length > 1;
  const sys = prefixed && many ? systemName(doc, prefixed[1]) : null;
  return sys ? `${sys}: ${said}` : said;
}

/**
 * The whole guide-bar line for an unfinished shop.
 *
 * Only the FIRST error is shown, which is unchanged behaviour: the list is often
 * one cause reported several ways, and a bar that scrolls is a bar nobody reads.
 * The count is worth saying, though — "and 3 more" is the difference between "I'm
 * nearly done" and "I have some work to do".
 */
export function wipSummary(doc: ShopDoc | null, issues: ValidationIssue[] | undefined): string {
  const list = issues ?? [];
  if (!list.length) return 'incomplete layout';
  const first = humaniseIssue(doc, list[0]);
  return list.length > 1 ? `${first} (and ${list.length - 1} more)` : first;
}
