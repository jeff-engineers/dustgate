/** failure — "didn't answer" only when nothing answered; otherwise the brain's own reason (2026-10-17).
 *
 *  Plain TypeScript, no Angular, no browser. Run with `npm test`.
 */

import { suite } from '../../test-harness';
import { failureReason, noAnswer, reportFailure, QuietPoll } from './failure';

const { check, eq, report } = suite();

// The console is part of the contract here, so capture it.
const warned: unknown[][] = [];
const infos: unknown[][] = [];
const realWarn = console.warn, realInfo = console.info;
console.warn = (...a: unknown[]) => { warned.push(a); };
console.info = (...a: unknown[]) => { infos.push(a); };

{
  check('status 0 is no answer', noAnswer({ status: 0 }));
  check('a refusal is an answer', !noAnswer({ status: 409, error: { error: 'busy' } }));
  check('our own Error is not "no answer"', !noAnswer(new Error('too close to Port 3')));

  eq('no answer says so', failureReason({ status: 0 }), "the shop brain didn't answer");
  eq('...naming what was asked', failureReason({ status: 0 }, 'the plug'), "the plug didn't answer");
  eq('the brain\'s own reason', failureReason({ status: 409, error: { error: 'a tool is running - wait until the shop is quiet' } }),
     'a tool is running - wait until the shop is quiet');
  eq('a refused layout names its first issue', failureReason({ status: 400, error: { error: 'invalid topology', errors: [{ message: 'systems not an array' }] } }),
     'systems not an array');
  eq('...in the guide bar words, with how many more', failureReason({ status: 400, error: { error: 'invalid topology',
     errors: [{ message: 'system "s2": systems not an array' }, { message: 'second' }] } }), 'systems not an array (and 1 more)');
  eq('a string answer', failureReason({ status: 400, error: 'bad tool' }), 'bad tool');
  eq('a gateway error is no answer', failureReason({ status: 502, error: '<html>Bad Gateway</html>' }), "the shop brain didn't answer");
  eq('a 500 with no reason is no answer (the dev proxy, the brain down)', failureReason({ status: 500, error: null }), "the shop brain didn't answer");
  check('...and counts as no answer for the offline states', noAnswer({ status: 500, error: null }));
  eq('a bare 4xx is turned down, with no code in the words', failureReason({ status: 409, error: null }), 'the shop brain turned it down');
  eq('a 500 that carries a reason passes the reason on', failureReason({ status: 500, error: { error: 'disk full' } }), 'disk full');
  eq('our own Error keeps its message', failureReason(new Error('too close to Port 3')), 'too close to Port 3');
  check('never empty', failureReason(null).length > 0 && failureReason(undefined).length > 0);
}

{
  warned.length = 0;
  const shown = reportFailure('Tools · save a tool', { status: 409, url: '/api/topology', error: { error: 'busy' } }, 'Couldn\'t save — busy.');
  eq('it hands back what to show', shown, 'Couldn\'t save — busy.');
  check('and logs one line, prefixed and labelled', warned.length === 1 && warned[0][0] === '[DustGate] Tools · save a tool: refused (HTTP 409)');
  const d = warned[0][1] as Record<string, unknown>;
  check('with the request, the status and the answer', d['request'] === '/api/topology' && d['status'] === 409 && !!d['answer']);
  check('...and never a header', !('headers' in d));
}

{
  warned.length = 0; infos.length = 0;
  const p = new QuietPoll('Boards · refresh');
  p.fail({ status: 0 }); p.fail({ status: 0 }); p.fail({ status: 0 });
  check('a failing poll logs once, not every tick', warned.length === 1);
  p.ok();
  check('...and once when it recovers', infos.length === 1 && String(infos[0][0]).includes('answering again after 3'));
  p.ok();
  check('...and nothing while it stays well', infos.length === 1);
}

console.warn = realWarn; console.info = realInfo;
report();
