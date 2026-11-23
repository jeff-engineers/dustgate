// layout-warning.ts — what a saved layout's problems mean for the shop: which tools, gates, plugs, clamps and boards may
// not work (D-88). Each is named by its own word (root CLAUDE.md's glossary) — never lumped as "devices".
//
// A save is never refused for a problem the shop can run around: the brain keeps the layout and drives what the problem
// does not touch. So what a person needs is not "your layout is invalid" but WHICH of their things may misbehave — the
// tools, gates, plugs, clamps and boards each problem names — and the first problem said plainly (wip-message.ts).
// The validator's own words go to the console (reportNote at the call site).
//
// Built on each issue's `ref` (the element, machine, board or system the check was about); an issue without one falls
// back to the quoted ids in its message. An id that names nothing is skipped, never shown: it is usually the whole fault
// (a chain naming a deleted gate) and is not a thing in the shop.
//
// Angular-free on purpose — see tsconfig.spec.json. Covered by layout-warning.spec.ts.

import type { ShopDoc, RawEl } from './shop-doc';
import { displayName, machinesOf, systemsOf } from './shop-doc';
import { wipSummary, type ValidationIssue } from './wip-message';

/** How many names the sentence lists before "and N more" — the guide bar is one line. */
const MAX_NAMED = 4;

const QUOTED = /"([^"]+)"/g;
/** `system "s2": ` — how validateShop tags a per-system failure. */
const SYSTEM_PREFIX = /^system "[^"]+": /;

function elementById(doc: ShopDoc | null, id: string): RawEl | null {
  for (const sys of systemsOf(doc)) for (const el of sys.elements ?? []) if (el['id'] === id) return el;
  return null;
}

/** What an id names, by its own word (a tool by its name, "Saw gate", "Planer’s plug", "board Serial 1"), or null when
 *  it names nothing in the shop (an open end, or nothing at all). */
function nameFor(doc: ShopDoc | null, id: string, message: string): string | null {
  // A one-port tool's port shares its machine's id, so a problem with a plug or a clamp asks for the machine first.
  const m = machinesOf(doc).find((x) => x.id === id);
  if (m) {
    const name = m.name || 'a tool';
    // The plug or the clamp belongs to the tool (D-83), so a problem with one is named as the tool's.
    if (/outlet|plug/i.test(message)) return `${name}’s plug`;
    if (/sensor\.ct|clamp/i.test(message)) return `${name}’s clamp`;
  }
  const el = elementById(doc, id);
  if (el) {
    const type = el['type'];
    if (type === 'junction') return null;                                  // a fitting, not a thing that can fail
    if (type === 'collector') return (el['name'] as string) || 'the dust collector';
    const name = displayName(doc, el);
    // A gate is called a gate: one named after its tool ("Bandsaw") would otherwise read as the tool.
    if (type === 'selector') return !name || name === id ? 'a gate' : /gate|valve|manifold|slider/i.test(name) ? name : `${name} gate`;
    return name && name !== id ? name : 'a tool';
  }
  if (m) return m.name || 'a tool';
  const c = ((doc?.['controllers'] as RawEl[] | undefined) ?? []).find((x) => x['id'] === id);
  if (c) return c['name'] ? `board ${c['name'] as string}` : 'a board';
  const sys = systemsOf(doc).find((s) => s.id === id);
  if (sys) {
    const dc = (sys.elements ?? []).find((e) => e['type'] === 'collector');
    return (dc?.['name'] as string) || sys.name || 'the dust collector';
  }
  return null;
}

/** Everything the issues name, each once, in the order the checks found them. */
export function affectedNames(doc: ShopDoc | null, issues: ValidationIssue[] | undefined): string[] {
  const out: string[] = [];
  for (const issue of issues ?? []) {
    // The system prefix is where the problem is, not what it affects: naming the collector for every problem in its
    // system would accuse the one thing that is fine.
    const message = (issue?.message ?? '').replace(SYSTEM_PREFIX, '');
    // The ref first, then whatever else the message names: two gates on one servo channel carry one gate as the ref and
    // the other in the words, and the brain moves neither.
    const ids = [...(issue?.ref ? [issue.ref] : []), ...[...message.matchAll(QUOTED)].map((m) => m[1])];
    for (const id of ids) {
      const d = nameFor(doc, id, message);
      if (d && !out.includes(d)) out.push(d);
    }
  }
  return out;
}

/** "A, B and C" — and past MAX_NAMED, "A, B, C, D and 3 more". */
function listOf(names: string[]): string {
  if (names.length <= 1) return names.join('');
  if (names.length > MAX_NAMED) return `${names.slice(0, MAX_NAMED).join(', ')} and ${names.length - MAX_NAMED} more`;
  return `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

/** The sentence for a layout saved with problems: what may not work, then the first problem. `justSaved` leads with
 *  "Saved — but", for the save that just happened; elsewhere (the Live view, opening the canvas) it is simply true. */
export function layoutWarning(doc: ShopDoc | null, issues: ValidationIssue[] | undefined, justSaved = false): string {
  const names = affectedNames(doc, issues);
  const why = wipSummary(doc, issues);
  const what = names.length ? listOf(names) : 'part of the shop';
  if (justSaved) return `Saved — but ${what} may not work properly until this is fixed: ${why}`;
  return `${what[0].toUpperCase()}${what.slice(1)} may not work properly until the layout is fixed: ${why}`;
}
