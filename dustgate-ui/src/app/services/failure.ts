// failure.ts — what to tell a person when a request fails, and what to tell the console.
//
// Written 2026-10-17 after an audit of every error handler in the app (docs/mockups/save-and-errors.html). The same
// two mistakes ran through most of them:
//
//   • "Couldn't reach the controller" for EVERY failure. The brain usually did answer — refusing, with a reason — and
//     the reason was thrown away, so a person went checking WiFi for a tool that was running or a layout it would not
//     take.
//   • Nothing in the console. A handler that showed a friendly line kept nothing to debug with, and about thirty
//     showed nothing at all.
//
// So: failureReason() says "didn't answer" only when nothing answered, and otherwise passes on the brain's own words;
// reportFailure() writes the whole story to the console — context, what the screen said, the status and the answer —
// and hands the friendly line back for the screen. Every line starts "[DustGate]", so one console filter shows them.
// Headers are never logged, so the API key never is.
//
// Angular-free on purpose (see tsconfig.spec.json): an HttpErrorResponse is read by its shape, not imported.

/** The parts of a failed request this reads. An HttpErrorResponse has all of them; a local Error has `message`. */
interface FailureShape { status?: number; error?: unknown; message?: string; url?: string | null }

/** Nothing answered: no HTTP status at all (status 0 is how a dropped connection reads). A local Error thrown by our
 *  own code carries its own message and is NOT "no answer". */
export function noAnswer(e: unknown): boolean {
  if (!e || typeof e !== 'object') return true;
  const f = e as FailureShape;
  if (typeof f.status === 'number') return f.status === 0;
  return !(e instanceof Error && f.message);
}

/** The brain's own words, from whichever shape its answer took: a string, {error}, {error, errors:[{message}]}, or
 *  {message}. A layout it refused names its first issue rather than the generic "invalid topology". */
function answerText(body: unknown): string {
  if (typeof body === 'string') return body.trim().startsWith('<') ? '' : body;
  if (!body || typeof body !== 'object') return '';
  const b = body as { error?: unknown; errors?: { message?: unknown }[]; message?: unknown };
  const first = Array.isArray(b.errors) && typeof b.errors[0]?.message === 'string' ? b.errors[0].message as string : '';
  if (first) return first;
  if (typeof b.error === 'string') return b.error;
  if (typeof b.message === 'string') return b.message;
  return '';
}

/**
 * Why it failed, in a few words that fit after a dash: "the shop brain didn't answer", or the brain's own reason, or
 * "the shop brain refused it (HTTP 409)" when it gave none. `who` names what was asked — a plug, rather than the brain,
 * for the plug screens. Never empty.
 */
export function failureReason(e: unknown, who = 'the shop brain'): string {
  if (!e || typeof e !== 'object') return 'something went wrong';
  const f = e as FailureShape;
  if (typeof f.status !== 'number') return e instanceof Error && f.message ? f.message : `${who} didn't answer`;
  if (f.status === 0) return `${who} didn't answer`;
  const said = answerText(f.error);
  return said || `${who} refused it (HTTP ${f.status})`;
}

/** The short label for the console line: what happened to the request. */
function kindOf(e: unknown): string {
  if (!e || typeof e !== 'object') return 'failed';
  const f = e as FailureShape;
  if (typeof f.status !== 'number') return e instanceof Error ? 'failed here' : 'no answer';
  if (f.status === 0) return 'no answer';
  return `refused (HTTP ${f.status})`;
}

/**
 * Log a failure for whoever is debugging, and return what the screen should say.
 *
 *     this.error = reportFailure('Tools · save a tool', e, `Couldn't save that tool — ${failureReason(e)}.`);
 *
 * `extra` is anything else worth seeing next to it (the validator's issues, the ids involved).
 */
export function reportFailure(context: string, e: unknown, shown: string, extra?: Record<string, unknown>): string {
  const f = (e && typeof e === 'object' ? e : {}) as FailureShape;
  const detail: Record<string, unknown> = { shown };
  if (f.url) detail['request'] = f.url;
  if (typeof f.status === 'number') detail['status'] = f.status;
  if (f.error !== undefined) detail['answer'] = f.error;
  else if (e instanceof Error) detail['error'] = e.message;
  if (extra) Object.assign(detail, extra);
  try { console.warn(`[DustGate] ${context}: ${kindOf(e)}`, detail); } catch { /* no console: nothing to do */ }
  return shown;
}

/** Log something that is not a failed request but leaves the screen saying less than it knows — a save the layout
 *  check refused, before anything was sent. */
export function reportNote(context: string, shown: string, extra?: Record<string, unknown>): string {
  try { console.warn(`[DustGate] ${context}`, { shown, ...(extra ?? {}) }); } catch { /* nothing to do */ }
  return shown;
}

/**
 * A background poll's failures, logged without flooding: the FIRST failure of a run and the recovery, never every tick.
 * A poll that fails quietly on screen is right — the next one usually works — but one that leaves nothing in the
 * console is a board that "just stopped" with nothing to show for it.
 */
export class QuietPoll {
  private failing = 0;
  constructor(private readonly context: string) {}
  fail(e: unknown): void {
    if (this.failing++ === 0) reportFailure(this.context, e, '(nothing shown — a background refresh; the last reading stays up)');
  }
  ok(): void {
    if (this.failing > 0) {
      try { console.info(`[DustGate] ${this.context}: answering again after ${this.failing} failed refresh${this.failing === 1 ? '' : 'es'}`); }
      catch { /* nothing to do */ }
    }
    this.failing = 0;
  }
}
