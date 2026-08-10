/** Is the saved layout good enough to just USE, or does it still need work?
 *
 *  This is the question `/` asks on every app open: land the person on the Live
 *  tool list if their shop is finished, or drop them straight into the layout tool
 *  if it isn't. Kept as one function so the answer can't drift between the entry
 *  redirect and anything else that wants to ask.
 *
 *  "Good" here means the shop can actually be driven, which is a higher bar than
 *  "the document is valid": a layout can be perfectly well-formed and still have a
 *  gate nobody has measured yet.
 */

import { airflowIssues, validateTopology, type Topology } from '@topology';
import {
  elementsOf, isCalibrated, isLinearSelector, isServoSelector,
  type ConfigurableSelector,
} from '../gates/selector-types';

export interface ShopReadiness {
  ready: boolean;
  /** Why not, in the user's language — shown if we send them to the layout tool. */
  reason: string;
}

export function shopReadiness(topo: Topology | null | undefined): ShopReadiness {
  if (!topo) return { ready: false, reason: 'No layout saved yet.' };

  const valid = validateTopology(topo);
  if (!valid.ok) {
    const first = valid.errors?.[0]?.message;
    return { ready: false, reason: first ? `The layout has a problem: ${first}` : 'The layout has a problem.' };
  }

  const elements = elementsOf(topo);
  const tools = elements.filter(e => e.type === 'tool');
  if (!tools.length) return { ready: false, reason: 'No tools on the layout yet.' };

  // Leaks: a tool that can't be selected without pulling air somewhere else too.
  const leaks = airflowIssues(topo);
  if (leaks.length) {
    const names = leaks.map(l => l.name).join(', ');
    return { ready: false, reason: `Suction leaks to ${names} — that needs a gate before the shop can run.` };
  }

  // A gate nobody has measured can't be driven, so the shop isn't ready even though
  // the document is fine. This is the common "almost done" case.
  const gates = elements.filter(e => isLinearSelector(e) || isServoSelector(e)) as unknown as ConfigurableSelector[];
  const unmeasured = gates.filter(g => !isCalibrated(g));
  if (unmeasured.length) {
    const names = unmeasured.map(g => g.name || g.id).join(', ');
    return {
      ready: false,
      reason: unmeasured.length === 1
        ? `${names} still needs setting up.`
        : `${unmeasured.length} gates still need setting up: ${names}.`,
    };
  }

  return { ready: true, reason: '' };
}
