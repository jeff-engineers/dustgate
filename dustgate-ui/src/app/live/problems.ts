/** Live view "Needs attention": turn the device's `problems` into rows.
 *  Pure — no Angular — so spec-runner can drive it. The DEVICE words the reason
 *  (`text`); this only adds a headline from the code and the name of the thing. */

import type { Problem } from '@topology-device';

export interface ProblemRow {
  key: string;
  severity: 'bad' | 'warn';
  what: string;
  why: string;
  /** "for 4m", or '' when the device gave no duration. */
  duration: string;
  code: string;
  subjectId: string;
}

/** system id → display name, board/plug/selector ids as the device sends them. */
export type NameOf = (type: string, id: string) => string;

export function duration(ms: number | undefined): string {
  if (ms === undefined || ms < 0) return '';
  const s = Math.round(ms / 1000);
  if (s < 90) return 'for ' + s + 's';
  const m = Math.round(s / 60);
  return m < 90 ? 'for ' + m + 'm' : 'for ' + Math.round(m / 60) + 'h';
}

export function headline(p: Problem, name: string): string {
  switch (p.code) {
    case 'collector-no-start': return name + " isn't starting";
    case 'collector-needs-start': return 'Turn on ' + name;
    case 'collector-blind':    return "Can't see " + name;
    case 'collector-wont-stop': return name + " won't stop";
    case 'rf-gave-up':         return name + ' did not respond';
    case 'rf-send-failed':     return "Couldn't send " + name + "'s remote press";
    case 'board-offline':      return 'Board ' + name + ' is offline';
    case 'plug-unreachable':   return 'Plug unreachable: ' + name;
    case 'move-failed':        return "A gate didn't move";
    default:                   return p.code.replace(/-/g, ' ');
  }
}

/** Reds first, then in the order the device listed them. */
export function problemRows(problems: Problem[] | undefined, nameOf: NameOf): ProblemRow[] {
  const rows = (problems ?? []).map((p, i) => ({
    i,
    row: {
      key: p.code + ':' + p.subject.id,
      severity: p.severity,
      what: headline(p, nameOf(p.subject.type, p.subject.id)),
      why: p.text,
      duration: duration(p.forMs),
      code: p.code,
      subjectId: p.subject.id,
    } as ProblemRow,
  }));
  rows.sort((a, b) => (a.row.severity === b.row.severity ? a.i - b.i : a.row.severity === 'bad' ? -1 : 1));
  return rows.map(r => r.row);
}

/** Why a request failed, in words. The device's own reason when it sent one
 *  (`{error: "..."}`), else what happened to the connection. Never empty. */
export function whyFailed(e: unknown): string {
  const err = e as { status?: number; error?: unknown; message?: string } | null;
  if (!err) return 'unknown error';
  if (err.status === 0 || err.status === undefined) return "the controller didn't answer";
  const body = err.error as { error?: unknown; message?: unknown } | string | null | undefined;
  const text = typeof body === 'string' ? body
    : typeof body?.error === 'string' ? body.error
    : typeof body?.message === 'string' ? body.message : '';
  return text ? text : 'the controller refused it (HTTP ' + err.status + ')';
}
