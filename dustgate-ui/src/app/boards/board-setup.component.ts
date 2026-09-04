import { Component, OnDestroy, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { Subscription } from 'rxjs';
import { ApiService, DiscoveredNode, NodeLinkState } from '../services/api.service';
import type { Topology } from '@topology';
import {
  type Drives, DEFAULT_DRIVES, applyDrivesCache, drivesFromCaps, drivesFromHasLinear, resolveDrives,
} from './board-drives';
import {
  Controller, SERVO_CHANNELS_PER_BOARD,
  controllersOf, selectorsOnController,
} from '../gates/selector-types';

// ── Board setup pass ─────────────────────────────────────────────────────────
// A shop bigger than one board's worth of gates needs somewhere to say "there's
// another ESP32 on the back wall". This is that place — the first surface in the
// app that can ADD a controller; until now `controllers[]` was a hardcoded
// single primary and the per-gate board picker had exactly one option.
//
// STANDS ALONE. This screen needs no topology and never creates one: pairing
// lives on the device (control/NodeRegistry.h, NVS), including each board's
// friendly name. Set up every board before drawing a duct, or draw first and add
// boards later — neither order is privileged, and a layout wipe doesn't cost you
// your boards. The layout's `controllers[]` is kept in step when a layout exists,
// but it is a consequence of pairing, never a prerequisite for it.
//
// TWO-STEP BY DESIGN — discovery only POPULATES this list; adding a board writes
// an explicit `link.host` into the topology. A board is never adopted just
// because it answered an mDNS sweep: a spare on the bench, or a neighbour's
// system on the same LAN, would otherwise wander into a live layout. The mDNS
// hostname (not the IP) is what gets saved, so DHCP moving a board doesn't break
// the binding.

/** A board as this screen sees it. Built from the device's PAIRING list, not from
 *  the topology — `gates` is the only field a layout contributes, and it's 0 when
 *  there isn't one. */
interface BoardRow {
  id: string;                   // controllerId: 'primary', or the node's host
  name: string;
  host: string;
  board: string;
  primary: boolean;
  link: NodeLinkState | null;   // null for the primary (it's local) or before the first poll
  gates: number;
  /** What the board is FLASHED to drive. Not a preference — the PWM bank and the
   *  serial bus use the same pads, so a board is one or the other from the
   *  moment it is flashed (`dev.sh flash --slider`). This screen is where you
   *  tell the app which one you flashed. */
  drives: Drives;
}

@Component({
  selector: 'app-board-setup',
  standalone: true,
  imports: [CommonModule, FormsModule],
  styles: [`
    :host { display: block; max-width: 460px; margin: 0 auto; padding: 16px 14px 40px; }
    .head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
    .head .step { font-size: 12.5px; color: var(--muted); }

    /* The network name. It used to be captioned in the board rail above the
       canvas; the rail is gone, and this is the screen where the answer to
       "same WiFi as what?" actually matters. */
    .net { display: flex; align-items: center; gap: 7px; margin: -8px 0 16px 2px;
           font-size: 12.5px; color: var(--muted); }
    .net svg { width: 14px; height: 14px; flex: none; }
    .net b { color: var(--text); font-weight: 600; }

    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; padding: 6px 16px; margin-bottom: 14px; }
    .sec-title { font-size: 12.5px; color: var(--muted); margin: 0 0 6px 2px; }

    /* Wraps to two lines on a narrow phone rather than crushing the subtitle into
       a one-word column — a board name plus two buttons doesn't fit at 375px. */
    .row { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 10px;
           padding: 12px 0; border-bottom: 1px solid var(--border); }
    .row:last-child { border-bottom: none; }
    .info { flex: 1 1 190px; min-width: 0; }
    .actions { display: flex; gap: 8px; margin-left: auto; flex-shrink: 0; }
    .nm { font-size: 14px; display: flex; align-items: center; gap: 7px; flex-wrap: wrap; }
    .sub { font-size: 11.5px; color: var(--muted); margin-top: 2px; line-height: 1.45; }

    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); flex-shrink: 0; }
    .dot.on  { background: var(--success); }
    .dot.off { background: var(--danger); }

    .badge { font-size: 11.5px; padding: 3px 10px; border-radius: 20px; flex-shrink: 0; }
    .badge.ok   { color: var(--success); background: rgba(60,190,110,0.12); }
    .badge.warn { color: var(--danger);  background: rgba(220,70,70,0.12); }

    button.act { border-radius: 8px; padding: 7px 12px; font-size: 12.5px; flex-shrink: 0;
                 background: var(--bg); border: 1px solid var(--border); color: var(--text); }
    button.act.add { background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
    /* Deliberately NOT the accent fill "Add" gets. Taking a board off another
       shop is a real consequence, so it reads as the secondary action it is —
       outlined, in the warning colour, never the thing your thumb lands on by
       default. */
    button.act.take { border-color: var(--danger); color: var(--danger); background: transparent; }
    button.act:disabled { opacity: 0.45; }

    /* Sits under the board's own line, in the row, so the claim travels with the
       board it describes rather than becoming a legend somewhere else. */
    .claimed { font-size: 12px; color: var(--danger); line-height: 1.5; margin-top: 4px; }
    .claimed b { font-weight: 600; }

    input.rename { background: var(--bg); border: 1px solid var(--border); color: var(--text);
                   border-radius: 8px; padding: 7px 9px; font-size: 13.5px; width: 100%; }

    .scan-btn { width: 100%; border-radius: 8px; padding: 11px 14px; font-size: 13.5px; margin-top: 4px;
                background: var(--bg); border: 1px solid var(--border); color: var(--text); }
    .scan-btn:disabled { opacity: 0.6; }
    .hint  { font-size: 12px; color: var(--muted); line-height: 1.6; margin: 0 0 10px; }
    .empty { text-align: center; color: var(--muted); font-size: 13px; padding: 18px 10px; line-height: 1.6; }
    .err   { font-size: 12.5px; color: var(--danger); margin-top: 12px; }
    .note  { font-size: 12px; color: var(--muted); margin-top: 10px; line-height: 1.55; }

    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .next { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
  `],
  template: `
    <div class="head">
      <span class="step">Your boards</span>
      <span class="step">{{ rows.length }} {{ rows.length === 1 ? 'board' : 'boards' }}</span>
    </div>

    <!-- Absent while the device is running its own setup AP, and on firmware old
         enough not to report it — in both cases saying nothing beats guessing. -->
    <div class="net" *ngIf="ssid">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9"
           stroke-linecap="round" stroke-linejoin="round">
        <path d="M2.5 8.5a15 15 0 0 1 19 0"/><path d="M5.5 12a10.5 10.5 0 0 1 13 0"/>
        <path d="M8.5 15.5a6 6 0 0 1 7 0"/><circle cx="12" cy="19" r="1" fill="currentColor" stroke="none"/>
      </svg>
      <span>Everything here is on <b>{{ ssid }}</b></span>
    </div>

    <!-- Boards already in the layout -->
    <div class="card" *ngIf="rows.length">
      <div class="row" *ngFor="let r of rows">
        <div class="info">
          <ng-container *ngIf="renaming !== r.id; else renameBox">
            <div class="nm">
              <span class="dot" [class.on]="isOnline(r)" [class.off]="isOffline(r)"></span>
              {{ r.name }}
              <!-- Inside the name line, not a flex sibling: as a sibling it stole
                   width and wrapped the subtitle one word per line at 375px. -->
              <span class="badge warn" *ngIf="isOffline(r)">Not answering</span>
            </div>
            <div class="sub">{{ subtitle(r) }}</div>
          </ng-container>
          <ng-template #renameBox>
            <input class="rename" [(ngModel)]="renameText" (keyup.enter)="commitRename()"
                   placeholder="e.g. Back wall" autofocus/>
          </ng-template>
        </div>

        <div class="actions">
          <ng-container *ngIf="renaming !== r.id; else renameActions">
            <button class="act" (click)="startRename(r)">Rename</button>
            <button class="act" *ngIf="!r.primary"
                    [disabled]="r.gates > 0" (click)="remove(r)">Remove</button>
          </ng-container>
          <ng-template #renameActions>
            <button class="act add" (click)="commitRename()">Save</button>
            <button class="act" (click)="renaming = null">Cancel</button>
          </ng-template>
        </div>
      </div>
    </div>

    <p class="note" *ngIf="blockedRemoval">
      {{ blockedRemoval }}
    </p>

    <!-- Discovery -->
    <p class="sec-title">Add another board</p>
    <div class="card">
      <!-- "the same WiFi" is only useful if you know which one that is, and the
           person holding the new board is usually not the person who set it up. -->
      <p class="hint">
        Power up the board and put it on
        <ng-container *ngIf="ssid; else sameWifi"><b>{{ ssid }}</b></ng-container>
        <ng-template #sameWifi>the same WiFi</ng-template>.
        It announces itself on the network — tap Scan and it'll show up here.
      </p>

      <div class="row" *ngFor="let n of unadded()">
        <div style="flex:1; min-width:0">
          <div class="nm">{{ n.host }}</div>
          <div class="sub">{{ n.board }} · {{ n.ip }} · {{ n.servos }} servo channels</div>
          <!-- Shown, not hidden: a board that is powered and answering but absent
               from this list looks exactly like one that never announced itself,
               and those two have opposite fixes. Naming the owner is what turns
               "why isn't it here" into a decision. -->
          <div class="claimed" *ngIf="n.claimedBy">
            Already claimed by <b>{{ n.claimedBy }}</b> — taking it over will stop
            that shop driving this board.
          </div>
        </div>
        <button class="act add" *ngIf="!n.claimedBy" (click)="add(n)">Add</button>
        <button class="act take" *ngIf="n.claimedBy" (click)="add(n)">Take over</button>
      </div>

      <div class="empty" *ngIf="scanned && !unadded().length">
        {{ scanning ? 'Scanning…'
                    : (found.length ? 'Every board found is already paired.'
                                    : 'No boards found. Check it\\'s powered on and on the same WiFi.') }}
      </div>

      <button class="scan-btn" [disabled]="scanning" (click)="scan()">
        {{ scanning ? 'Scanning…' : (scanned ? '↻ Scan again' : 'Scan for boards') }}
      </button>
    </div>

    <p class="err" *ngIf="error">{{ error }}</p>

    <div class="nav">
      <button class="next" (click)="backToWiring()">Shop layout →</button>
    </div>
  `,
})
export class BoardSetupComponent implements OnInit, OnDestroy {
  rows: BoardRow[] = [];
  found: DiscoveredNode[] = [];
  links: NodeLinkState[] = [];
  scanning = false;
  scanned = false;
  error = '';
  blockedRemoval = '';
  renaming: string | null = null;
  renameText = '';
  /** The WiFi network the controller is joined to. Empty while it's running its
   *  own setup AP, or on firmware too old to report it. */
  ssid = '';

  private topo: Topology | null = null;
  private poll: ReturnType<typeof setInterval> | null = null;
  private statusSub: Subscription | null = null;

  constructor(private api: ApiService, private router: Router) {}

  async ngOnInit(): Promise<void> {
    await this.api.whenReady();
    // A missing layout is NORMAL here, not an error. This is often the first
    // screen someone opens: boards exist as soon as they're on the WiFi, long
    // before anyone has decided what the ductwork looks like.
    try {
      this.topo = JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology;
    } catch {
      this.topo = null;
    }
    await this.refreshLinks();
    // On LOAD as well as on add/rename: a board paired in an earlier session (or
    // before this layout existed) has no controllers[] entry, and without one the
    // per-gate "Driven by" picker never offers it.
    this.syncLayoutControllers();
    this.syncDrivesFromHardware();
    this.rebuild();
    // Link state is the point of this screen, so keep it live rather than
    // showing a snapshot that goes stale while someone walks to the shop.
    //
    // syncLayoutControllers() runs on EVERY tick, not just on load and add():
    // POST /api/nodes/pair returns before the device has done the pairing (the
    // registry write and the dial happen on the main loop, and /api/nodes is
    // served from a cache the loop republishes on its own schedule). So the
    // refreshLinks() inside add() usually reads a list that does NOT yet contain
    // the board just paired — nothing gets added to controllers[], and the board
    // shows up in this list a few seconds later, on a tick that used to only
    // rebuild rows. That is the bug where a board pairs fine, appears here, and
    // never appears on the build canvas: the canvas draws controllers[]. The
    // call is additive and only writes when something actually changed, so
    // running it repeatedly costs nothing.
    this.poll = setInterval(
      () => void this.refreshLinks().then(() => { this.syncLayoutControllers(); this.syncDrivesFromHardware(); this.rebuild(); }),
      3000,
    );

    // The network name rides the ordinary status push rather than a fetch of its
    // own: it can change under us (the device rejoining a different AP) and this
    // screen is already long-lived.
    this.statusSub = this.api.status$.subscribe(s => { this.ssid = s?.ssid ?? ''; });
  }

  ngOnDestroy(): void {
    if (this.poll) clearInterval(this.poll);
    this.statusSub?.unsubscribe();
  }

  // ── discovery ─────────────────────────────────────────────────────────────
  async scan(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    this.error = '';
    try {
      this.found = await this.api.discoverNodes();
    } catch {
      this.found = [];
      this.error = 'Scan failed — check the controller is connected.';
    } finally {
      this.scanning = false;
      this.scanned = true;
    }
  }

  /** Discovered boards not already PAIRED, matched by host. Compared against the
   *  pairing list rather than the layout — a board can be paired with no layout at
   *  all, and offering to add it again would just be confusing. */
  unadded(): DiscoveredNode[] {
    const paired = new Set(this.links.map((l) => this.bareHost(l.host)));
    return this.found.filter((n) => !paired.has(this.bareHost(n.host)));
  }

  /** Hosts round-trip as either "node-1" or "node-1.local" depending on whether
   *  the device has qualified them yet; compare on the bare label. */
  private bareHost(h: string): string {
    return (h || '').toLowerCase().replace(/\.local\.?$/, '');
  }

  /** Bind a discovered board into the layout by its STABLE mDNS host.
   *
   *  PAIRS FIRST, then binds. Pairing is what actually opens the link and it's
   *  persisted on the device independently of the layout, so it has to happen even
   *  if the topology write below fails — and it means the board goes green at the
   *  node before any gate has been assigned to it. Binding just tells the layout
   *  which board drives what. */
  async add(n: DiscoveredNode): Promise<void> {
    // Taking a board off another shop is not an undo-able mistake to make by
    // mistap: the previous owner simply stops being able to drive those gates,
    // with no error on its side. So the claim gets a confirmation, and the label
    // says what actually happens rather than "are you sure?".
    if (n.claimedBy &&
        !confirm(`${n.host} belongs to ${n.claimedBy}.\n\n` +
                 `Taking it over stops that shop driving this board. Continue?`)) {
      return;
    }
    try {
      await this.api.pairNode(n.host, undefined, !!n.claimedBy);
    } catch (e: unknown) {
      this.error = this.message(e);
      return;
    }
    await this.refreshLinks();
    this.syncLayoutControllers();   // no-op when there's no layout yet
    this.rebuild();
  }

  /** Unbind a board. Blocked while gates still name it — removing it would leave
   *  them pointing at a controller that doesn't exist, which the schema rejects
   *  ("controllerId does not resolve") and reads as a mysterious save failure
   *  rather than a clear reason.
   *
   *  The count is recomputed HERE rather than trusted from the row: the row is a
   *  render-time snapshot, and acting on a stale one silently unbinds a board
   *  that still has gates. The disabled button is the affordance; this is the
   *  actual guard. */
  async remove(r: BoardRow): Promise<void> {
    // Recomputed here rather than trusted from the row: the row is a render-time
    // snapshot, and acting on a stale one unpairs a board that still drives gates.
    // The disabled button is the affordance; this is the guard.
    const gates = this.topo ? selectorsOnController(this.topo, r.id).length : 0;
    if (gates > 0) {
      this.blockedRemoval =
        `${r.name} still drives ${gates} ${gates === 1 ? 'gate' : 'gates'}. ` +
        `Move ${gates === 1 ? 'it' : 'them'} to another board first.`;
      this.rebuild();
      return;
    }
    this.blockedRemoval = '';
    try {
      await this.api.unpairNode(r.host);
    } catch (e: unknown) {
      this.error = this.message(e);
      return;
    }
    await this.refreshLinks();
    if (this.topo) {
      const controllers = controllersOf(this.topo);
      const i = controllers.findIndex((c) => c.id === r.id);
      if (i >= 0) { controllers.splice(i, 1); await this.persist(); }
    }
    this.rebuild();
  }

  // ── rename ────────────────────────────────────────────────────────────────
  startRename(r: BoardRow): void { this.renaming = r.id; this.renameText = r.name; }

  /** Names live on the DEVICE, beside the pairing — so a board keeps its name
   *  through a layout wipe, and can be named before any layout exists. */
  async commitRename(): Promise<void> {
    const id = this.renaming;
    this.renaming = null;
    if (!id) return;
    const row = this.rows.find((r) => r.id === id);
    const next = this.renameText.trim();
    if (!row || !next || next === row.name) return;

    if (row.primary) {
      // The primary isn't in the pairing registry — it's this board. Its name is
      // only ever a layout label, so it needs one to be renamed at all.
      if (!this.topo) { this.error = 'Draw a layout first to name the primary.'; return; }
      const c = controllersOf(this.topo).find((x) => x.id === id);
      if (c) { c.name = next; await this.persist(); }
      return;
    }

    try { await this.api.pairNode(row.host, next); }
    catch (e: unknown) { this.error = this.message(e); return; }
    await this.refreshLinks();
    this.syncLayoutControllers();
    this.rebuild();
  }

  // ── display ───────────────────────────────────────────────────────────────
  isOnline(r: BoardRow): boolean { return r.primary || !!r.link?.online; }
  isOffline(r: BoardRow): boolean { return !r.primary && !r.link?.online; }

  subtitle(r: BoardRow): string {
    const bits: string[] = [];
    // What it drives, in the words the canvas uses for its ports. Read-only: the
    // board reported this, nobody chose it.
    bits.push(r.drives === 'linear' ? 'sliding gate' : 'servo valves');
    bits.push(r.primary ? 'primary — runs the app' : (r.host || 'no address'));
    if (r.board) bits.push(r.board);
    // A slider board's whole capacity is one rack, so "of 4" would be a lie on it.
    bits.push(r.drives === 'linear'
      ? `${r.gates} of 1 sliding gate`
      : `${r.gates} of ${SERVO_CHANNELS_PER_BOARD} gates`);   // plural follows the 4, not the count
    if (r.link?.fw) bits.push(`fw ${r.link.fw}`);
    return bits.join(' · ');
  }

  go(path: string): void { void this.router.navigate([path]); }
  /** Back to the canvas in its WIRING view. Boards only matter there, so landing in
   *  the duct view would put you one tap from where you just were, looking at a
   *  drawing that doesn't show the thing you came here to change. */
  backToWiring(): void { void this.router.navigate(['/build'], { queryParams: { layer: 'wiring' } }); }

  // ── internals ─────────────────────────────────────────────────────────────
  private async refreshLinks(): Promise<void> {
    try { this.links = await this.api.getNodes(); } catch { /* leave the last known state */ }
  }

  /** Rows come from the device's pairing list, plus the primary (which is always
   *  present — it's the board serving this page). The layout only supplies gate
   *  counts and the primary's label, and contributes nothing when absent. */
  /** LIVE REPORT FIRST, cache second — the same precedence drivesOf() uses on the
   *  canvas, and for the same reason: `drives` is only written when someone opens
   *  this screen, so reading it first shows a freshly paired slider node as a
   *  4-gate servo board until a poll tick happens to correct it. That is exactly
   *  what it did (2026-09-02): the canvas drew "0/1 SL" while this list, one tap
   *  away, said "0 of 4 gates" about the same board. */
  private rebuild(): void {
    const ctrls = this.topo ? controllersOf(this.topo) : [];
    const primary = ctrls.find((c) => c.role === 'primary');
    this.rows = [{
      id: primary?.id ?? 'primary',
      name: primary?.name || 'This board',
      host: '',
      board: primary?.board ?? '',
      primary: true,
      drives: resolveDrives(this.reportedDrives(primary?.id ?? 'primary'), primary?.drives),
      link: null,
      gates: this.gatesOn(primary?.id ?? 'primary'),
    }];
    for (const l of this.links) {
      this.rows.push({
        id: l.id,
        name: l.name || l.host || l.id,
        host: l.host,
        board: l.board,
        primary: false,
        drives: resolveDrives(this.reportedDrives(l.id), this.controllerDrives(l.id)),
        link: l,
        gates: this.gatesOn(l.id),
      });
    }
  }

  private gatesOn(controllerId: string): number {
    return this.topo ? selectorsOnController(this.topo, controllerId).length : 0;
  }

  private controllerDrives(id: string): Drives | null {
    if (!this.topo) return null;
    const c = controllersOf(this.topo).find((x) => x.id === id);
    return c?.drives ?? null;
  }

  /** Write down what each board REPORTED it drives.
   *
   *  NOT A USER CHOICE, and it was briefly built as one before the obvious
   *  objection landed: the board already knows. A node says so in its WELCOME
   *  caps (`caps.linear`) and the primary says so in its status (`hasLinear`),
   *  both straight from HAS_LINEAR in the firmware — which is derived from the
   *  pin map, so it cannot disagree with the hardware. Asking someone to restate
   *  which build they flashed is asking them to be wrong.
   *
   *  Controller.drives is therefore a CACHE of that fact, kept so the canvas can
   *  draw the right port strip on a board that is asleep or not yet paired. Same
   *  shape as `board` and the node's `owner` hint: the live report always wins.
   *  A board never heard from stays 'servo', which is the default and what every
   *  layout saved before this field meant. */
  private syncDrivesFromHardware(): void {
    if (!this.topo) return;
    let changed = false;
    for (const c of controllersOf(this.topo)) {
      const reported = this.reportedDrives(c.id);
      if (!reported) continue;                      // never heard from it — leave the cache alone
      if (reported === (c.drives ?? DEFAULT_DRIVES)) continue;
      applyDrivesCache(c as unknown as Record<string, unknown>, reported);
      changed = true;
    }
    if (changed) void this.persist();
  }

  /** What a board SAYS it drives, or null if it has not said. */
  private reportedDrives(id: string): Drives | null {
    if (!this.topo) return null;
    const primary = controllersOf(this.topo).find((c) => c.role === 'primary');
    if (primary && id === primary.id) return drivesFromHasLinear(this.api.status$.value?.hasLinear);
    return drivesFromCaps(this.links.find((l) => l.id === id)?.caps);
  }

  /** Keep the layout's controllers[] in step with what's paired — additively, and
   *  ONLY when a layout exists. The schema still wants a controllers entry behind
   *  every gate's controllerId; this makes that bookkeeping rather than a step the
   *  user has to know about. */
  private syncLayoutControllers(): void {
    if (!this.topo) return;
    const controllers = controllersOf(this.topo);
    let changed = false;
    for (const l of this.links) {
      const existing = controllers.find((c) => c.id === l.id);
      if (existing) {
        if (l.name && existing.name !== l.name) { existing.name = l.name; changed = true; }
        continue;
      }
      controllers.push({
        id: l.id,                  // the node's host IS its controllerId
        role: 'secondary',
        name: l.name || l.host,
        board: l.board,
        link: { transport: 'wifi-ws', host: l.host },
      });
      changed = true;
    }
    if (changed) void this.persist();
  }

  private async persist(): Promise<void> {
    if (!this.topo) return;
    try {
      await this.api.putTopology(this.topo);
      this.error = '';
      await this.refreshLinks();
    } catch (e: unknown) {
      this.error = this.message(e);
    }
    this.rebuild();
  }

  private message(e: unknown): string {
    const err = e as { error?: { error?: string; errors?: { message?: string }[] }; message?: string };
    return err?.error?.errors?.[0]?.message
        || err?.error?.error
        || err?.message
        || 'Couldn\'t save — check the controller is reachable.';
  }
}
