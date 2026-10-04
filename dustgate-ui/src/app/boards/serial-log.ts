/** The Brain log screen's bookkeeping, kept out of the component so it can be
 *  tested under plain node (spec-runner.js). See docs/mockups/brain-log.html.
 *
 *  The board serves raw bytes by a running cursor (GET /api/serial,
 *  firmware/utils/SerialLog.h); this turns those into lines, says out loud when
 *  bytes were missed or the brain restarted, and sorts each line into the chips
 *  the screen filters by. No Angular, no browser. */

/** One reply from GET /api/serial. */
export interface SerialChunk {
  text: string;
  /** First byte returned. Greater than the `from` asked for = bytes missed. */
  start: number;
  /** Ask for this next time. */
  next: number;
  /** Changes when the brain restarts. */
  boot: string;
}

export type LogGroup = 'boards' | 'network' | 'other';
export type LogLevel = 'err' | 'warn' | '';
export type LogFilter = 'all' | 'boards' | 'network' | 'problems';

export interface LogLine {
  kind: 'line';
  seq: number;
  /** When the board printed it, when the board stamped it; otherwise when it reached this screen. */
  at: number;
  text: string;
  /** The leading [TAG], when there is one; drawn in its group's colour. */
  tag: string;
  rest: string;
  group: LogGroup;
  level: LogLevel;
}

export interface LogMark {
  kind: 'gap' | 'restart' | 'down' | 'up';
  seq: number;
  at: number;
  text: string;
}

export type LogEntry = LogLine | LogMark;

// Tags as the firmware prints them (grep '"\[' firmware). Boards = anything about
// the link to another board; Network = WiFi, name lookup and finding plugs.
const BOARD_TAGS = /^(NODE|NODE←|NODE→|NODES|LINKLOG|PING|TAKEOVER)$/;
const NETWORK_TAGS = /^(WiFi|MDNS|mDNS|MDNSPROBE|DISCOVER|PROVISION|Outlets|SWEEP|RETRY)$/;

// "[839711][E][NetworkClient.cpp:435] ..." — the core's own log_e/log_w format.
const CORE_ERR = /^\[\d+\]\[E\]/;
const CORE_WARN = /^\[\d+\]\[W\]/;
const ERR_WORDS = /\[ERROR\]|\berror\b|\bfail(ed|s|ure)?\b|\blost\b|reset by peer|\bpanic\b|brownout|pwr_glitch/i;
const WARN_WORDS = /\bwarn(ing)?\b|⚠|\bdark:|backing off|\brefused\b|\bstale\b|\btimed? ?out\b/i;

/** Sort one line. `prev` is the line before it: an indented continuation (the
 *  firmware wraps long messages that way) belongs to whatever it continues. */
export function classify(text: string, prev?: LogLine): Pick<LogLine, 'tag' | 'rest' | 'group' | 'level'> {
  let level: LogLevel = CORE_ERR.test(text) || ERR_WORDS.test(text) ? 'err'
    : CORE_WARN.test(text) || WARN_WORDS.test(text) ? 'warn' : '';
  if (/^\s{2,}\S/.test(text) && prev) {
    return { tag: '', rest: text, group: prev.group, level: level || prev.level };
  }
  const m = text.match(/^\[([^\]\d][^\]]*)\]/);
  const tag = m ? m[0] : '';
  const name = m ? m[1] : '';
  const group: LogGroup = BOARD_TAGS.test(name) ? 'boards' : NETWORK_TAGS.test(name) ? 'network' : 'other';
  return { tag, rest: text.slice(tag.length), group, level };
}

// The board stamps each line it prints (firmware/utils/SerialLog.cpp): "15:35:45.123Z "
// in UTC once it has NTP, "+63.512s " (uptime) before that. Lines from the core's
// own log_e() carry none. The stamp is taken off the text, so the [TAG] after it
// still sorts the line, and a UTC one becomes the line's real time; `now` is when
// the reply reached us, which only decides which UTC day the clock means.
const STAMP_WALL = /^(\d\d):(\d\d):(\d\d)\.(\d{3})Z /;
const STAMP_UP = /^\+\d+\.\d{3}s /;

export function unstamp(text: string, now: number): { at: number; text: string } {
  const m = text.match(STAMP_WALL);
  if (m) {
    const d = new Date(now);
    let at = Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), +m[1], +m[2], +m[3], +m[4]);
    const half = 12 * 3600 * 1000;
    if (at - now > half) at -= 2 * half;          // just past UTC midnight
    else if (now - at > half) at += 2 * half;
    return { at, text: text.slice(m[0].length) };
  }
  const u = text.match(STAMP_UP);
  return u ? { at: now, text: text.slice(u[0].length) } : { at: now, text };
}

export function matches(e: LogEntry, filter: LogFilter, find: string): boolean {
  if (e.kind !== 'line') return true;          // gaps and restarts always show
  if (filter === 'boards' && e.group !== 'boards') return false;
  if (filter === 'network' && e.group !== 'network') return false;
  if (filter === 'problems' && !e.level) return false;
  return !find || e.text.toLowerCase().includes(find.toLowerCase());
}

export function clock(ms: number): string {
  const d = new Date(ms);
  const p = (n: number) => String(n).padStart(2, '0');
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

/** What "Copy shown lines" puts on the clipboard. */
export function copyText(entries: LogEntry[]): string {
  return entries.map(e => e.kind === 'line' ? `${clock(e.at)}  ${e.text}` : `--- ${e.text} ---`).join('\n');
}

export class SerialLog {
  /** Lines and marks, oldest first, capped at `max`. */
  readonly entries: LogEntry[] = [];
  /** Where to ask from next. */
  cursor = 0;
  private boot: string | null = null;
  private partial = '';
  private seq = 0;
  private lastLine: LogLine | undefined;

  constructor(private readonly max = 2000) {}

  /** Take one reply. Returns how many entries it added. */
  apply(c: SerialChunk, now: number): number {
    const before = this.seq;
    if (this.boot !== null && c.boot !== this.boot) {
      // A restart. The text in THIS reply was read from our old cursor, which
      // means nothing in the new boot — so drop it and ask again from byte 0.
      this.flushPartial(now);
      this.mark('restart', 'The brain restarted. Lines above are from before it.', now);
      this.boot = c.boot;
      this.cursor = 0;
      return this.seq - before;
    }
    if (this.boot !== null && c.start > this.cursor) {
      const n = c.start - this.cursor;
      this.flushPartial(now);
      this.mark('gap', `${n.toLocaleString('en-US')} characters missed here. The brain's buffer wrapped before this screen caught up.`, now);
    }
    this.boot = c.boot;
    this.cursor = c.next;
    if (c.text) {
      const parts = (this.partial + c.text).split('\n');
      // The last piece has no newline yet: hold it until the rest arrives, so a
      // line is never drawn in two halves.
      this.partial = parts.pop() ?? '';
      for (const p of parts) this.line(p.replace(/\r$/, ''), now);
    }
    return this.seq - before;
  }

  mark(kind: LogMark['kind'], text: string, now: number): void {
    this.push({ kind, seq: ++this.seq, at: now, text });
  }

  private flushPartial(now: number): void {
    if (this.partial) this.line(this.partial, now);
    this.partial = '';
  }

  private line(text: string, now: number): void {
    const u = unstamp(text, now);
    const l: LogLine = { kind: 'line', seq: ++this.seq, at: u.at, text: u.text, ...classify(u.text, this.lastLine) };
    this.lastLine = l;
    this.push(l);
  }

  private push(e: LogEntry): void {
    this.entries.push(e);
    if (this.entries.length > this.max) this.entries.splice(0, this.entries.length - this.max);
  }
}
