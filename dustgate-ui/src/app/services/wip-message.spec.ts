/** wip-message — "system \"s2\": element \"p8\"…" said in the user's own words.
 *
 *  Plain TypeScript, no Angular, no browser. Run with `npm test`.
 *
 *  The messages here are the REAL ones: every `message` string below was produced
 *  by validateShop/validateTopology, not invented for the test. That is the whole
 *  value of this suite — the phrasings match on message text, so the thing worth
 *  catching is the model rewording a message out from under a phrasing. If one of
 *  these strings stops being what the model emits, this file is the thing that has
 *  drifted, and the fix is to re-copy it from shared/device-model, not to loosen
 *  the pattern.
 */

import { suite } from '../../test-harness';
import type { ShopDoc } from './shop-doc';
import { humaniseIssue, nameForId, wipSummary } from './wip-message';

const { check, eq, report } = suite();

/** Two systems, a machine with two ports, one unnamed junction. */
const shop = (): ShopDoc => JSON.parse(JSON.stringify({
  schemaVersion: 2,
  name: 'My Shop',
  controllers: [
    { id: 'primary', role: 'primary', name: 'Shop Brain', board: 'devkitc' },
    { id: 'wall', role: 'secondary', name: 'Back wall', link: { transport: 'wifi-ws', host: 'wall' } },
  ],
  systems: [
    {
      id: 's1', name: 'Main line',
      elements: [
        { id: 'dc', type: 'collector', name: 'Cyclone' },
        { id: 'g1', type: 'selector', name: 'Saw gate', kind: 'servoGate', controllerId: 'primary' },
        { id: 'p1', type: 'tool', name: 'Cabinet', machineId: 'saw' },
        { id: 'end0', type: 'junction', name: 'Open end' },
      ],
      ducts: [],
    },
    {
      // No name of its own — its collector is what it's known by.
      id: 's2',
      elements: [
        { id: 'dc2', type: 'collector', name: 'Back wall cyclone' },
        { id: 'p8', type: 'tool', name: 'Overarm', machineId: 'saw', supplemental: true },
        { id: 'lonely', type: 'tool', machineId: 'nobody' },
      ],
      ducts: [],
    },
  ],
  machines: [
    { id: 'saw', name: 'Table saw' },
    { id: 'jointer', name: 'Jointer' },
  ],
})) as ShopDoc;

// ── resolving an id to a name ───────────────────────────────────────────────
{
  const d = shop();
  eq('a named element resolves to its name', nameForId(d, 'dc'), 'Cyclone');
  // p1 and p8 both belong to "saw", so neither is the machine's only port and
  // each keeps its own name — displayName()'s rule, exercised through here.
  eq('one of two ports keeps its own name', nameForId(d, 'p8'), 'Overarm');
  eq('a machine resolves by name', nameForId(d, 'saw'), 'Table saw');
  eq('a controller resolves by name', nameForId(d, 'wall'), 'Back wall');
  eq('a named system resolves by name', nameForId(d, 's1'), 'Main line');
  eq('an unnamed system resolves to its collector', nameForId(d, 's2'), 'Back wall cyclone');
  eq('an unnamed port falls back to a kind word', nameForId(d, 'lonely'), 'a tool');
  check('an id that names nothing stays null', nameForId(d, 'ghost') === null);
}

// ── the reported bug, end to end ────────────────────────────────────────────
{
  const d = shop();
  const said = humaniseIssue(d, {
    code: 'tree',
    message: 'system "s2": element "p8" must have exactly one parent duct (has 0)',
    ref: 'p8',
  });
  eq('the reported message names the piece and the system',
     said, 'Back wall cyclone: Overarm isn’t piped to anything yet — run a duct to it.');
  check('and no id survives it', !/\bp8\b|\bs2\b/.test(said), said);
}

// ── the system prefix earns its place, or goes ──────────────────────────────
{
  const one = shop();
  one.systems = [one.systems[0]];
  const said = humaniseIssue(one, {
    message: 'system "s1": element "end0" must have exactly one parent duct (has 0)',
  });
  check('a one-system shop is not told which system', !said.startsWith('Main line'), said);
  eq('it just says the thing', said, 'Open end isn’t piped to anything yet — run a duct to it.');
}

// ── the phrasings ──────────────────────────────────────────────────────────
const says = (message: string) => humaniseIssue(shop(), { message });

{
  eq('two parents',
     says('system "s1": element "p1" must have exactly one parent duct (has 2)'),
     'Main line: Cabinet has 2 ducts running to it. A piece takes exactly one.');

  eq('orphaned from the collector',
     says('system "s1": element "g1" does not reach the collector (cycle or broken chain)'),
     'Main line: Saw gate doesn’t connect back to the dust collector.');

  eq('no collector',
     says('system "s1": exactly one collector required (found 0)'),
     'Main line: This system has no dust collector.');

  eq('two collectors in one system',
     says('system "s1": exactly one collector required (found 2)'),
     'Main line: This system has 2 dust collectors. Each system has exactly one.');

  eq('an outlet with nothing on it',
     says('system "s1": branch "b1" role tool but nothing wired to it'),
     'Main line: A gate has an outlet with nothing on it. Put a tool there, or cap it.');

  // Channels are 0-based in the document and 1-based everywhere a person sees
  // them — the picker says "Servo 1". Off by one here would be worse than the id.
  eq('a servo channel collision counts from 1',
     says('system "s1": servo channel 0 on host "primary" is already used by "g1"'),
     'Main line: Saw gate is already on servo 1 of Shop Brain. '
     + 'Two gates can’t share a channel — move one to a free one.');

  eq('too many valves on one board',
     says('host "primary" has 5 servo selectors (max 4 per host)'),
     'Shop Brain is driving 5 ball valves. A board drives at most 4 — pair another board.');

  eq('an unreachable secondary',
     says('secondary board "Back wall" has no link — the primary can\'t reach it'),
     'The primary has no address for “Back wall”, so it can’t drive anything on it.');

  eq('two tools on one plug',
     says('system "s1": smart outlet 192.168.1.9 is on two elements ("p1" and "g1")'),
     'Main line: Cabinet and Saw gate are both paired to the outlet at 192.168.1.9. '
     + 'An outlet belongs to one tool.');
}

// ── the fallback, which is what has to survive the model changing ───────────
{
  const d = shop();
  // A message with no phrasing: keeps the model's words, loses the ids.
  const said = humaniseIssue(d, { message: 'system "s2": element "p8" invented a brand new way to be wrong' });
  eq('an unknown message keeps its words and swaps its ids',
     said, 'Back wall cyclone: element “Overarm” invented a brand new way to be wrong');

  eq('an id that resolves to nothing is left exactly as it was',
     humaniseIssue(d, { message: 'duct child "ghost" does not resolve' }),
     'duct child "ghost" does not resolve');

  eq('an empty message still says something',
     humaniseIssue(d, { message: '' }), 'incomplete layout');
}

// ── the summary line ───────────────────────────────────────────────────────
{
  const d = shop();
  eq('no errors', wipSummary(d, []), 'incomplete layout');

  eq('one error is said alone',
     wipSummary(d, [{ message: 'system "s1": element "end0" must have exactly one parent duct (has 0)' }]),
     'Main line: Open end isn’t piped to anything yet — run a duct to it.');

  eq('more than one says how many',
     wipSummary(d, [
       { message: 'system "s1": element "end0" must have exactly one parent duct (has 0)' },
       { message: 'system "s1": exactly one collector required (found 0)' },
       { message: 'selector missing controllerId' },
     ]),
     'Main line: Open end isn’t piped to anything yet — run a duct to it. (and 2 more)');
}

report();
