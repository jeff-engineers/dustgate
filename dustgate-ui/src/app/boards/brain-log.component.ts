import { AfterViewChecked, Component, ElementRef, OnDestroy, OnInit, ViewChild } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { ApiService } from '../services/api.service';
import { SerialLog, clock, copyText, matches, type LogEntry, type LogFilter } from './serial-log';

// ── Brain log ────────────────────────────────────────────────────────────────
// The brain's serial output, read over WiFi: the same text a USB monitor shows,
// for a board in the shop with no laptop on it. A bench and support tool, so it
// opens from the brain's row in Your boards and nowhere else.
// Design: docs/mockups/brain-log.html (approved 2026-10-03).
//
// Polls GET /api/serial once a second with the cursor the last reply handed
// back (firmware/utils/SerialLog.h). All the bookkeeping — lines from bytes,
// missed-bytes and restart marks, which chip a line belongs to — is in
// serial-log.ts, where the specs can reach it.

const POLL_MS = 1000;
const RETRY_MS = 3000;

@Component({
  selector: 'app-brain-log',
  standalone: true,
  imports: [CommonModule, FormsModule],
  styles: [`
    :host { display: flex; flex-direction: column; gap: 12px; max-width: 460px; margin: 0 auto;
            padding: 16px 14px 40px; }
    .head { display: flex; align-items: center; gap: 10px; }
    .back { background: none; border: none; color: var(--muted); font-size: 13px; padding: 4px 0; }
    .head h1 { font-size: 17px; font-weight: 600; margin: 0; flex: 1; }
    .head h1 small { display: block; font-size: 12px; font-weight: 400; color: var(--muted); }

    .status { display: flex; align-items: center; gap: 8px; font-size: 12.5px; color: var(--muted); flex-wrap: wrap; }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--success); flex: none; }
    .dot.paused { background: var(--muted); }
    .dot.down { background: var(--danger); }
    .status .sep { opacity: .5; }

    .tools { display: flex; gap: 8px; flex-wrap: wrap; }
    .chip { border-radius: 20px; padding: 5px 12px; font-size: 12.5px;
            background: var(--bg); border: 1px solid var(--border); color: var(--text); }
    .chip.on { background: var(--accent); border-color: var(--accent); color: #1a1200; font-weight: 600; }
    .find { flex: 1 1 140px; min-width: 0; background: var(--bg); border: 1px solid var(--border);
            color: var(--text); border-radius: 8px; padding: 6px 10px; font-size: 13px; }

    .down { font-size: 12.5px; color: var(--danger); background: rgba(217,68,68,.1); border-radius: 10px; padding: 8px 12px; }

    /* Taller than the mockup's 420px: on a phone this IS the screen, and the
       controls above and below are all that need to stay in view. */
    .log { background: #0a0a0a; border: 1px solid var(--border); border-radius: var(--radius);
           height: min(62vh, 640px); overflow-y: auto; overflow-x: hidden; padding: 10px 12px;
           font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
           font-size: 11.5px; line-height: 1.55; }
    .ln { display: grid; grid-template-columns: 5.4em 1fr; gap: 8px; white-space: pre-wrap; word-break: break-word; }
    .ln .t { color: #555; font-variant-numeric: tabular-nums; text-align: right; }
    .ln.err .m { color: var(--danger); }
    .ln.warn .m { color: var(--warn, #d29922); }
    .tag { font-weight: 600; color: var(--muted); }
    .tag.boards { color: #6cb6ff; } .tag.network { color: #b392f0; }
    .mk { margin: 8px 0; text-align: center; font-family: system-ui, sans-serif; font-size: 11.5px; padding: 4px 0; }
    .mk.gap  { color: var(--warn, #d29922); border-top: 1px dashed rgba(210,153,34,.5); border-bottom: 1px dashed rgba(210,153,34,.5); }
    .mk.restart { color: var(--accent); border-top: 1px solid rgba(240,165,0,.4); }
    .mk.down, .mk.up { color: var(--muted); }
    .empty { color: var(--muted); font-family: system-ui, sans-serif; font-size: 12.5px; text-align: center; padding: 30px 10px; }
    .jump { position: sticky; bottom: 0; margin: 6px auto 0; display: block; border-radius: 20px;
            padding: 6px 14px; font-size: 12.5px; background: var(--accent); border: none; color: #1a1200;
            font-weight: 600; font-family: system-ui, sans-serif; }

    /* The command line. Monospace like the log it feeds, so what you type looks
       like what comes back. */
    .cmd { display: flex; gap: 8px; }
    .cmd input { flex: 1; min-width: 0; background: var(--bg); border: 1px solid var(--border); color: var(--text);
                 border-radius: 8px; padding: 9px 10px; font-size: 13px;
                 font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace; }
    .cmd button { border-radius: 8px; padding: 9px 16px; font-size: 13px; border: none;
                  background: var(--accent); color: #1a1200; font-weight: 600; }
    .cmd button:disabled { opacity: .45; }
    .cmd-err { font-size: 12.5px; color: var(--danger); margin: -4px 0 0; }

    .foot { display: flex; gap: 8px; }
    .foot button { flex: 1; border-radius: 8px; padding: 10px 12px; font-size: 13px;
                   background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .note { font-size: 12px; color: var(--muted); line-height: 1.55; margin: 0; }
  `],
  template: `
    <div class="head">
      <button class="back" type="button" (click)="back()" title="Back to Your boards">‹ Boards</button>
      <h1>Brain log<small>{{ host }} · same text as the USB serial monitor</small></h1>
    </div>

    <div class="status" aria-live="polite">
      <span class="dot" [class.paused]="paused && !down" [class.down]="down"></span>
      <span>{{ down ? 'Not reachable' : paused ? 'Paused' : 'Live' }}</span>
      <span class="sep">·</span>
      <span title="The brain keeps about 32 KB of recent output in memory. Older lines roll off, and a restart clears it.">
        {{ lineCount }} {{ lineCount === 1 ? 'line' : 'lines' }} since this screen opened
      </span>
    </div>

    <div class="tools" role="group" aria-label="Show">
      <button type="button" class="chip" [class.on]="filter === 'all'" [attr.aria-pressed]="filter === 'all'"
              (click)="setFilter('all')">All</button>
      <button type="button" class="chip" [class.on]="filter === 'boards'" [attr.aria-pressed]="filter === 'boards'"
              (click)="setFilter('boards')" title="Lines about the other boards: links, moves, acknowledgements">Boards</button>
      <button type="button" class="chip" [class.on]="filter === 'network'" [attr.aria-pressed]="filter === 'network'"
              (click)="setFilter('network')" title="WiFi, name lookups and finding plugs">Network</button>
      <button type="button" class="chip" [class.on]="filter === 'problems'" [attr.aria-pressed]="filter === 'problems'"
              (click)="setFilter('problems')" title="Errors and warnings only">Problems</button>
      <input class="find" type="search" placeholder="Find text…" aria-label="Find text in the log"
             [(ngModel)]="find" (ngModelChange)="refilter()">
    </div>

    <div class="down" *ngIf="down">
      Can't reach the brain. Retrying every {{ retrySec }} s. The lines below are what arrived before it went quiet.
    </div>

    <div class="log" #logBox tabindex="0" aria-label="Log output" (scroll)="onScroll()">
      <ng-container *ngFor="let e of shown; trackBy: bySeq">
        <div *ngIf="e.kind === 'line'; else markTpl" class="ln" [class.err]="e.level === 'err'" [class.warn]="e.level === 'warn'">
          <span class="t">{{ time(e.at) }}</span><span class="m"><span *ngIf="e.tag" class="tag" [ngClass]="e.group">{{ e.tag }}</span>{{ e.rest }}</span>
        </div>
        <ng-template #markTpl><div class="mk" [ngClass]="e.kind">{{ e.text }}</div></ng-template>
      </ng-container>
      <div class="empty" *ngIf="!shown.length">
        {{ log.entries.length ? 'Nothing matches that filter yet.' : (down ? 'Nothing received yet.' : 'Waiting for the brain…') }}
      </div>
      <button *ngIf="paused && held" type="button" class="jump" (click)="togglePause()">
        {{ held }} new {{ held === 1 ? 'line' : 'lines' }} · Resume
      </button>
    </div>

    <form class="cmd" (submit)="send($event)">
      <input type="text" name="cmd" [(ngModel)]="command" (keydown)="history($event)"
             placeholder="Run a command, e.g. help" aria-label="Serial command"
             autocomplete="off" autocapitalize="off" spellcheck="false" maxlength="120"
             title="Runs on the brain exactly as if typed at the USB serial monitor. Type help for the list.">
      <button type="submit" [disabled]="!command.trim() || sending || down">Run</button>
    </form>
    <p class="cmd-err" *ngIf="cmdError">{{ cmdError }}</p>

    <div class="foot">
      <button type="button" (click)="togglePause()">{{ paused ? 'Resume' : 'Pause' }}</button>
      <button type="button" (click)="copy()" title="Copies every line currently shown, with its time">{{ copyLabel }}</button>
    </div>
    <p class="note">
      Times are when each line reached this screen, not when the brain printed it.
      Pausing stops the screen scrolling; lines keep arriving underneath and appear when you resume.
      Commands run on the brain exactly as if typed at the USB serial monitor, including
      <b>wifireset</b> and <b>clearcal</b>.
    </p>
  `,
})
export class BrainLogComponent implements OnInit, OnDestroy, AfterViewChecked {
  @ViewChild('logBox') logBox?: ElementRef<HTMLDivElement>;

  readonly log = new SerialLog();
  /** What is drawn. Frozen while paused — new lines keep landing in `log`. */
  shown: LogEntry[] = [];
  filter: LogFilter = 'all';
  find = '';
  paused = false;
  held = 0;
  down = false;
  lineCount = 0;
  host = 'dustgate';
  copyLabel = 'Copy shown lines';
  readonly retrySec = RETRY_MS / 1000;
  command = '';
  cmdError = '';
  sending = false;
  /** Commands run this visit, newest last — ↑/↓ walk it like a shell. */
  private sent: string[] = [];
  private histAt = -1;

  private timer: ReturnType<typeof setTimeout> | null = null;
  private alive = true;
  /** Follow the bottom only while the reader is AT the bottom: scrolling up to
   *  read something must not be yanked back down by the next line. */
  private stick = true;
  private scrollPending = false;

  constructor(private api: ApiService, private router: Router) {}

  async ngOnInit(): Promise<void> {
    await this.api.whenReady();
    this.host = this.api.deviceInfo?.owner || 'dustgate';
    void this.pollOnce();
  }

  ngOnDestroy(): void {
    this.alive = false;
    if (this.timer) clearTimeout(this.timer);
  }

  ngAfterViewChecked(): void {
    if (!this.scrollPending) return;
    this.scrollPending = false;
    const el = this.logBox?.nativeElement;
    if (el) el.scrollTop = el.scrollHeight;
  }

  /** Bumped whenever the schedule is replaced, so a poll still in flight from
   *  the old schedule finishes without starting a second loop. */
  private gen = 0;

  private schedule(ms: number): void {
    if (this.timer) clearTimeout(this.timer);
    const g = ++this.gen;
    this.timer = setTimeout(() => void this.pollOnce(g), ms);
  }

  private async pollOnce(g = this.gen): Promise<void> {
    if (!this.alive || g !== this.gen) return;
    let wait = POLL_MS;
    try {
      const c = await this.api.readSerial(this.log.cursor);
      const now = Date.now();
      if (this.down) { this.log.mark('up', `Reachable again at ${clock(now)}.`, now); this.down = false; }
      const added = this.log.apply(c, now);
      if (added) this.changed(added);
      // A full chunk means more is waiting (opening on a board that has been
      // printing for a while): drain it now rather than a second at a time.
      // And straight after a restart, whose reply was thrown away (cursor back
      // to 0 while the board has plainly printed something).
      if (c.text.length >= 8000 || (this.log.cursor === 0 && c.next > 0)) wait = 50;
    } catch {
      if (!this.down) {
        const now = Date.now();
        this.log.mark('down', `Lost contact at ${clock(now)}.`, now);
        this.down = true;
        this.changed(1);
      }
      wait = RETRY_MS;
    }
    if (this.alive && g === this.gen) this.schedule(wait);
  }

  private changed(added: number): void {
    this.lineCount = this.log.entries.filter(e => e.kind === 'line').length;
    if (this.paused) { this.held += added; return; }
    this.refilter();
  }

  refilter(): void {
    this.shown = this.log.entries.filter(e => matches(e, this.filter, this.find.trim()));
    if (this.stick && !this.paused) this.scrollPending = true;
  }

  setFilter(f: LogFilter): void {
    this.filter = f;
    this.stick = true;
    this.refilter();
  }

  togglePause(): void {
    this.paused = !this.paused;
    if (!this.paused) { this.held = 0; this.stick = true; this.refilter(); }
  }

  onScroll(): void {
    const el = this.logBox?.nativeElement;
    if (!el) return;
    this.stick = el.scrollHeight - el.scrollTop - el.clientHeight < 30;
  }

  async send(ev: Event): Promise<void> {
    ev.preventDefault();
    const line = this.command.trim();
    if (!line || this.sending) return;
    this.sending = true;
    this.cmdError = '';
    try {
      await this.api.sendSerial(line);
      if (this.sent[this.sent.length - 1] !== line) this.sent.push(line);
      this.histAt = -1;
      this.command = '';
      // You asked for output: show it, and fetch it now rather than in a second.
      if (this.paused) this.togglePause();
      this.stick = true;
      this.schedule(250);
    } catch (e) {
      this.cmdError = e instanceof Error ? e.message : "The brain didn't take that command.";
    } finally {
      this.sending = false;
    }
  }

  history(ev: KeyboardEvent): void {
    if (ev.key !== 'ArrowUp' && ev.key !== 'ArrowDown') return;
    if (!this.sent.length) return;
    ev.preventDefault();
    if (ev.key === 'ArrowUp') this.histAt = this.histAt < 0 ? this.sent.length - 1 : Math.max(0, this.histAt - 1);
    else this.histAt = this.histAt < 0 ? -1 : this.histAt + 1;
    if (this.histAt >= this.sent.length) this.histAt = -1;
    this.command = this.histAt < 0 ? '' : this.sent[this.histAt];
  }

  async copy(): Promise<void> {
    try {
      await navigator.clipboard.writeText(copyText(this.shown));
      this.copyLabel = 'Copied';
    } catch {
      // Clipboard access needs a secure origin, and http://dustgate.local is not
      // one — so on the real board this is the path. Select the text instead,
      // which the phone's own Copy then works on.
      const el = this.logBox?.nativeElement;
      const sel = window.getSelection();
      if (el && sel) { const r = document.createRange(); r.selectNodeContents(el); sel.removeAllRanges(); sel.addRange(r); }
      this.copyLabel = 'Selected — use Copy';
    }
    setTimeout(() => (this.copyLabel = 'Copy shown lines'), 2000);
  }

  time(ms: number): string { return clock(ms); }
  bySeq(_: number, e: LogEntry): number { return e.seq; }
  back(): void { void this.router.navigate(['/boards']); }
}
