// Suite for deep-link.ts — resolving `/tools?el=<id>` onto a row.
//
// Worth a suite because the failure is SILENT: an id that resolves to nothing
// just shows the plain list, which is indistinguishable from someone tapping
// "Tools" in the nav. The link would look built and be broken.

import { suite } from '../../test-harness';
import { resolveDeepLink, type LinkableRow } from './deep-link';

const { check, eq, report } = suite();

const row = (id: string, machineId: string, primary = true): LinkableRow => ({ id, machineId, primary });

// A one-system shop: a collector, a single-port jointer, and a saw with a
// cabinet port (primary) plus an overarm (supplemental).
const rows: LinkableRow[] = [
  row('dc1', ''),
  row('p-jointer', 'm-jointer'),
  row('p-saw-cab', 'm-saw'),
  row('p-saw-arm', 'm-saw', false),
];

const idOf = (el: string | null | undefined): string | null => resolveDeepLink(rows, el)?.id ?? null;

eq('a collector is found by its own id', idOf('dc1'), 'dc1');
eq('a port is found by its own id', idOf('p-jointer'), 'p-jointer');

// The shop page lists MACHINES, so this is the id every link from it carries.
eq('a machine id lands on its port', idOf('m-jointer'), 'p-jointer');
eq('...and on the PRIMARY port when there are two', idOf('m-saw'), 'p-saw-cab');

// Both rows edit the same outlet, so landing on either would "work" — and
// arriving at the overarm to pair the saw reads as the wrong screen.
check('the supplemental port is still reachable directly',
      idOf('p-saw-arm') === 'p-saw-arm');

eq('no param opens the list', idOf(null), null);
eq('an empty param opens the list', idOf(''), null);
// A piece deleted between the link being drawn and being followed. The list is
// the right answer — better than an editor for something that is gone.
eq('an unknown id opens the list', idOf('m-gone'), null);
// A collector carries no machine id; an empty one must not match everything.
eq('an empty machine id matches nothing', idOf('  ') , null);

report();
