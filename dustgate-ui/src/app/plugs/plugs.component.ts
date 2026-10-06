import { Component, OnDestroy, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { Subscription } from 'rxjs';
import { ApiService, DiscoveredOutlet, SweepProgress } from '../services/api.service';
import type { Topology } from '@topology';
import {
  type ShopDoc, type RawEl,
  collectorOf, machinesOf, outletOf, setOutlet, systemsOf, toShop,
} from '../services/shop-doc';

// ── Your plugs ───────────────────────────────────────────────────────────────
// The twin of /boards: one row per plug, with its live draw and who owns it, and the places a plug is found, paired,
// renamed, released and taken over. It is the one home for picking a plug, so the picker inside a tool's sheet is not a
// second place to learn how (docs/mockups/plugs-clamp-delete.html).
//
// What it does NOT do: choose a plug for the COLLECTOR (that is the collector's own sheet, because a collector's plug can
// also switch it), or set a tool's trip point (tool setup). It pairs a plug to a TOOL and takes it away again.

const DEFAULT_THRESHOLD = 10;

interface PlugRow {
  key: string;
  ip: string;
  /** The tool or collector it is paired to. */
  forName: string;
  /** 'tool' | 'collector' — a collector's plug is released here but paired in the collector's sheet. */
  forKind: 'tool' | 'collector';
  machineId?: string;
  collectorId?: string;
  kind: 'shelly' | 'tasmota';
  thresholdW?: number;
  /** The plug's own name, as the plug reports it (fallback: what the layout cached). */
  plugName: string;
  seen: DiscoveredOutlet | null;
}

@Component({
  selector: 'app-plugs',
  standalone: true,
  imports: [CommonModule, FormsModule],
  styles: [`
    :host { display: block; padding: 16px 14px 28px; max-width: 560px; margin: 0 auto; }
    .head { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 10px; }
    .step { font-size: 11.5px; letter-spacing: .08em; text-transform: uppercase; color: var(--muted); }
    .net { display: flex; align-items: center; gap: 8px; font-size: 12px; color: var(--muted); margin-bottom: 10px; }
    .net b { color: var(--text); font-weight: 500; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: var(--radius); padding: 4px 14px; margin-bottom: 12px; }
    .row { display: flex; gap: 10px; align-items: flex-start; justify-content: space-between; padding: 12px 0; border-top: 1px solid var(--border); }
    .row:first-child { border-top: 0; }
    .info { min-width: 0; flex: 1; }
    .nm { font-size: 14.5px; font-weight: 500; display: flex; align-items: center; flex-wrap: wrap; gap: 6px; }
    .sub { font-size: 12px; color: var(--muted); margin-top: 2px; line-height: 1.5; }
    .sub b { color: var(--text); font-weight: 500; }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--border); flex: none; }
    .dot.on { background: var(--success); } .dot.off { background: var(--danger); } .dot.idle { background: var(--muted); }
    .badge { font-size: 10.5px; padding: 1px 7px; border-radius: 99px; border: 1px solid var(--border); color: var(--muted); }
    .badge.warn { color: var(--accent); border-color: var(--accent); }
    .actions { display: flex; flex-direction: column; gap: 6px; flex: none; align-items: stretch; }
    .act { font: inherit; font-size: 12.5px; padding: 6px 11px; border-radius: 9px; background: var(--raised, var(--bg));
           border: 1px solid var(--border); color: var(--text); cursor: pointer; text-align: center; }
    .act.add { background: var(--accent); border-color: var(--accent); color: #1a1200; font-weight: 600; }
    .act:disabled { opacity: .5; }
    .act:focus-visible, .btn:focus-visible, .sw:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
    .draw { font-variant-numeric: tabular-nums; font-size: 13px; text-align: right; color: var(--success); font-weight: 500; white-space: nowrap; }
    .draw.idle { color: var(--muted); font-weight: 400; }
    .sect { font-size: 11px; letter-spacing: .08em; text-transform: uppercase; color: var(--muted); margin: 14px 2px 6px; }
    .hint { font-size: 12px; color: var(--muted); line-height: 1.55; margin: 0 0 10px; }
    .empty { text-align: center; color: var(--muted); font-size: 13px; padding: 18px 10px; line-height: 1.6; }
    .err { font-size: 12.5px; color: var(--danger); margin: 6px 0; }
    .btnrow { display: flex; gap: 8px; }
    .btn { flex: 1; font: inherit; font-size: 13.5px; padding: 10px 14px; border-radius: 11px; background: var(--surface);
           border: 1px solid var(--border); color: var(--text); cursor: pointer; }
    .btn:disabled { opacity: .55; }
    .btn.danger { background: var(--danger); border-color: var(--danger); color: #fff; font-weight: 600; }
    .confirm { margin: 0 0 12px; padding: 10px 11px; border: 1px solid var(--accent); border-radius: 10px; font-size: 12.5px; line-height: 1.5; }
    .confirm .btnrow { margin-top: 8px; }
    .field { display: flex; gap: 7px; margin-top: 10px; }
    .field input, .rename { flex: 1; min-width: 0; padding: 9px 11px; border-radius: 10px; background: var(--bg);
                            border: 1px solid var(--border); color: var(--text); font: inherit; font-size: 14px; }
    .field button { flex: none; }
    .bar { height: 6px; border-radius: 99px; background: var(--border); margin-top: 6px; overflow: hidden; }
    .bar > div { height: 100%; background: var(--accent); transition: width .3s; }
    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { flex: 1; border-radius: var(--radius); padding: 12px 16px; font-size: 14px; background: var(--accent);
                  border: none; color: #1a1200; font-weight: 600; }
  `],
  template: `
    <div class="head">
      <span class="step">Your plugs</span>
      <span class="step">{{ rows.length }} {{ rows.length === 1 ? 'plug' : 'plugs' }}</span>
    </div>
    <div class="net" *ngIf="ssid"><span>◉</span><span>Everything here is on <b>{{ ssid }}</b></span></div>

    <div class="card" *ngIf="rows.length">
      <div class="row" *ngFor="let r of rows">
        <div class="info">
          <ng-container *ngIf="renaming !== r.key; else renameBox">
            <div class="nm">
              <span class="dot" [class.on]="state(r) === 'on'" [class.idle]="state(r) === 'idle'" [class.off]="state(r) === 'off'"></span>
              {{ r.plugName || r.forName }}
              <span class="badge warn" *ngIf="state(r) === 'off'">Not answering</span>
              <span class="badge" *ngIf="readOnly(r)">Read only</span>
            </div>
            <div class="sub">{{ r.kind === 'tasmota' ? 'Tasmota' : 'Shelly' }} · {{ r.ip }} · {{ how(r) }}</div>
            <div class="sub" *ngIf="r.forKind === 'tool'">Paired to the <b>{{ r.forName }}</b><ng-container *ngIf="r.thresholdW"> · starts collection above {{ r.thresholdW }} W</ng-container></div>
            <div class="sub" *ngIf="r.forKind === 'collector'">Paired to the collector <b>{{ r.forName }}</b></div>
            <div class="sub" *ngIf="state(r) === 'off'">It may have a new address. Sweep to find it.</div>
            <div class="sub" *ngIf="readOnly(r)">Belongs to {{ r.seen?.holder || 'another brain' }}. Polled, never written. Taking it over repoints its push address to this brain.</div>
          </ng-container>
          <ng-template #renameBox>
            <input class="rename" [(ngModel)]="renameText" (keyup.enter)="commitRename(r)" aria-label="Plug name" placeholder="e.g. Table saw"/>
          </ng-template>
        </div>
        <div class="actions">
          <div class="draw" [class.idle]="watts(r) < 1" *ngIf="state(r) !== 'off'">{{ watts(r) | number:'1.0-0' }} W</div>
          <ng-container *ngIf="renaming !== r.key; else renameActs">
            <button class="act" *ngIf="!readOnly(r) && state(r) !== 'off'" (click)="startRename(r)">Rename</button>
            <button class="act add" *ngIf="state(r) === 'off'" [disabled]="sweeping" (click)="sweep()">Sweep</button>
            <button class="act" *ngIf="readOnly(r) && r.seen?.takeable" (click)="confirmTakeover = r.key">Take over…</button>
            <button class="act" (click)="confirmRelease = r.key">Release</button>
          </ng-container>
          <ng-template #renameActs>
            <button class="act add" (click)="commitRename(r)">Save</button>
            <button class="act" (click)="renaming = null">Cancel</button>
          </ng-template>
        </div>
      </div>
    </div>
    <div class="empty" *ngIf="!rows.length && loaded">No plugs are paired to a tool yet. Find one below, then pair it.</div>

    <ng-container *ngFor="let r of rows">
      <div class="confirm" *ngIf="confirmRelease === r.key">
        Release <b>{{ r.plugName || r.forName }}</b>? The {{ r.forName }} goes back to having no plug. The plug itself is handed back to whoever had it before.
        <div class="btnrow"><button class="btn danger" (click)="release(r)">Release</button><button class="btn" (click)="confirmRelease = null">Cancel</button></div>
      </div>
      <div class="confirm" *ngIf="confirmTakeover === r.key">
        Take over <b>{{ r.plugName || r.forName }}</b> from {{ r.seen?.holder || 'another brain' }}? Its push address is saved so releasing it later puts it back. That brain stops hearing this plug.
        <div class="btnrow"><button class="btn danger" (click)="takeover(r)">Take over</button><button class="btn" (click)="confirmTakeover = null">Cancel</button></div>
      </div>
    </ng-container>
    <p class="err" *ngIf="error">{{ error }}</p>
    <p class="hint" *ngIf="note"><b>{{ note }}</b></p>

    <ng-container *ngIf="found().length">
      <div class="sect">Found, not paired</div>
      <div class="card">
        <div class="row" *ngFor="let d of found()">
          <div class="info">
            <div class="nm"><span class="dot" [class.on]="d.reachable && d.powerW >= 5" [class.idle]="d.reachable && d.powerW < 5" [class.off]="!d.reachable"></span>{{ d.name || d.hostname || d.ip }}</div>
            <div class="sub">{{ d.kind === 'tasmota' ? 'Tasmota' : 'Shelly' }} · {{ d.ip }} · {{ d.claim === 'foreign' ? 'belongs to ' + (d.holder || 'another brain') : (d.claim === 'dustgate' || d.claim === 'ours' ? 'ours' : 'unclaimed') }} · {{ d.reachable ? (d.powerW | number:'1.0-0') + ' W' : 'not answering' }}</div>
            <div class="sub" *ngIf="d.reachable">Switch a tool on and watch for the plug that jumps.</div>
          </div>
          <div class="actions"><button class="act add" [disabled]="!d.reachable || !targets().length" (click)="pairing = d.ip">Pair to…</button></div>
        </div>
      </div>
      <div class="confirm" *ngIf="pairing as ip">
        Pair <b>{{ ip }}</b> to which tool?
        <div class="btnrow" style="flex-wrap:wrap; margin-top:8px">
          <button class="btn" style="flex:0 0 auto" *ngFor="let t of targets()" (click)="pair(ip, t.id)">{{ t.name }}</button>
          <button class="btn" style="flex:0 0 auto" (click)="pairing = null">Cancel</button>
        </div>
        <p class="hint" style="margin:8px 0 0">Tools that already have a plug are not listed.</p>
      </div>
    </ng-container>

    <div class="sect">Look for plugs</div>
    <div class="btnrow">
      <button class="btn" [disabled]="scanning" (click)="scan()">↻ {{ scanning ? 'Scanning…' : 'Scan again' }}</button>
      <button class="btn" [disabled]="sweeping" (click)="sweep()"
              title="Knock on every address on the network. About a minute.">{{ sweeping ? 'Sweeping…' : 'Sweep network' }}</button>
    </div>
    <div *ngIf="sweeping || (progress && progress.running)" style="margin-top:10px">
      <div class="hint" style="margin:0">Knocking on every address… {{ pct() }}%</div>
      <div class="bar"><div [style.width.%]="pct()"></div></div>
      <button class="act" style="margin-top:8px" (click)="stopSweep()">Stop</button>
    </div>
    <div class="field">
      <input type="text" inputmode="decimal" placeholder="Or type its address — 192.168.1.42" [(ngModel)]="manualIp"
             [disabled]="adding" (keyup.enter)="addByIp()" aria-label="Plug IP address"/>
      <button class="btn" [disabled]="adding || !manualIp.trim()" (click)="addByIp()">{{ adding ? 'Checking…' : 'Add' }}</button>
    </div>
    <p class="err" *ngIf="addError">{{ addError }}</p>
    <p class="hint" style="margin-top:6px">Some plugs, a Tasmota above all, never announce themselves, so typing the address is the only way in.</p>

    <div class="nav"><button (click)="back()">Shop layout →</button></div>
  `,
})
export class PlugsComponent implements OnInit, OnDestroy {
  rows: PlugRow[] = [];
  outlets: DiscoveredOutlet[] = [];
  loaded = false;
  ssid = '';
  error = '';
  note = '';
  scanning = false;
  sweeping = false;
  progress: SweepProgress | null = null;
  manualIp = '';
  adding = false;
  addError = '';
  renaming: string | null = null;
  renameText = '';
  confirmRelease: string | null = null;
  confirmTakeover: string | null = null;
  pairing: string | null = null;
  private topo: ShopDoc | null = null;
  private tools: Record<string, { watts?: number }> = {};
  private poll: ReturnType<typeof setInterval> | null = null;
  private tick = 0;
  private sweepTimer: ReturnType<typeof setInterval> | null = null;
  private statusSub: Subscription | null = null;

  constructor(private api: ApiService, private router: Router) {}

  async ngOnInit(): Promise<void> {
    await this.api.whenReady();
    await this.loadLayout();
    this.statusSub = this.api.status$.subscribe(s => { this.ssid = s?.ssid ?? ''; });
    await this.scan();
    this.loaded = true;
    // Live draw: the brain already publishes every tool's watts, so this asks for that rather than re-probing plugs.
    this.poll = setInterval(() => { void this.refreshWatts(); if (++this.tick % 5 === 0) void this.probePaired(); }, 2000);
    void this.refreshWatts();
  }

  ngOnDestroy(): void {
    if (this.poll) clearInterval(this.poll);
    if (this.sweepTimer) clearInterval(this.sweepTimer);
    this.statusSub?.unsubscribe();
  }

  // ── data ──────────────────────────────────────────────────────────────────
  private async loadLayout(): Promise<void> {
    try { this.topo = toShop(JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology); }
    catch { this.topo = null; }   // no layout yet is normal: plugs exist before anyone has drawn anything
    this.rebuild();
  }

  private async refreshWatts(): Promise<void> {
    try { this.tools = ((await this.api.getStatus()) as unknown as { tools?: Record<string, { watts?: number }> }).tools ?? {}; }
    catch { /* keep the last reading */ }
  }

  private rebuild(): void {
    const doc = this.topo, rows: PlugRow[] = [];
    const seen = (ip: string) => this.outlets.find(o => o.ip === ip) ?? null;
    for (const m of machinesOf(doc)) {
      const o = ((m.sensor as RawEl | undefined)?.['outlet'] as RawEl | undefined);
      if (!o?.['ip']) continue;
      const ip = o['ip'] as string, s = seen(ip);
      rows.push({
        key: 'm:' + m.id, ip, forName: (m.name as string) || (m.id as string), forKind: 'tool', machineId: m.id as string,
        kind: (o['kind'] === 'tasmota' || s?.kind === 'tasmota') ? 'tasmota' : 'shelly',
        thresholdW: o['thresholdW'] as number | undefined, plugName: s?.name || (o['name'] as string) || '', seen: s,
      });
    }
    for (const sys of systemsOf(doc)) {
      const c = collectorOf(sys), o = outletOf(doc, c);
      if (!c || !o?.['ip']) continue;
      const ip = o['ip'] as string, s = seen(ip);
      rows.push({
        key: 'c:' + c['id'], ip, forName: (c['name'] as string) || 'Dust collector', forKind: 'collector', collectorId: c['id'] as string,
        kind: (o['kind'] === 'tasmota' || s?.kind === 'tasmota') ? 'tasmota' : 'shelly', plugName: s?.name || (o['name'] as string) || '', seen: s,
      });
    }
    this.rows = rows;
  }

  found(): DiscoveredOutlet[] {
    const paired = new Set(this.rows.map(r => r.ip));
    return this.outlets.filter(o => !paired.has(o.ip));
  }
  /** Tools that can take a plug: no plug and no clamp yet. */
  targets(): { id: string; name: string }[] {
    return machinesOf(this.topo)
      .filter(m => !m.sensor)
      .map(m => ({ id: m.id as string, name: (m.name as string) || (m.id as string) }));
  }

  // ── display ───────────────────────────────────────────────────────────────
  state(r: PlugRow): 'on' | 'idle' | 'off' {
    // Until every paired plug has been asked once, "no answer yet" is not "not answering".
    if (!r.seen) return this.probedOnce ? 'off' : 'idle';
    if (!r.seen.reachable) return 'off';
    return this.watts(r) >= (r.thresholdW ?? DEFAULT_THRESHOLD) ? 'on' : 'idle';
  }
  watts(r: PlugRow): number {
    const live = r.machineId ? this.tools[r.machineId]?.watts : undefined;
    return typeof live === 'number' ? live : (r.seen?.powerW ?? 0);
  }
  readOnly(r: PlugRow): boolean { return r.seen?.claim === 'foreign'; }
  how(r: PlugRow): string { return r.kind === 'tasmota' ? 'polled every half second' : 'pushes to this brain'; }
  pct(): number {
    const p = this.progress;
    return p && p.total ? Math.min(100, Math.round((p.scanned / p.total) * 100)) : (this.sweeping ? 1 : 0);
  }

  // ── finding ───────────────────────────────────────────────────────────────
  async scan(): Promise<void> {
    this.scanning = true; this.error = '';
    try { this.outlets = await this.api.discoverOutlets(); }
    catch { this.outlets = []; this.error = "Couldn't reach the controller to look for plugs."; }
    finally { this.scanning = false; this.rebuild(); }
    await this.probePaired();
  }

  /** Ask every PAIRED plug for itself. A scan only knows what announces itself (and on the native brain, nothing does), so
   *  "is the Table saw's plug answering" has to be asked of the plug. Kept alone, one in flight at a time. */
  private probing = false;
  private probedOnce = false;
  private async probePaired(): Promise<void> {
    if (this.probing || !this.rows.length) return;
    this.probing = true;
    try {
      const got = await Promise.allSettled(this.rows.map(r => this.api.pingOutlet(r.ip)));
      for (const g of got) if (g.status === 'fulfilled') {
        const at = this.outlets.findIndex(o => o.ip === g.value.ip);
        if (at >= 0) this.outlets[at] = g.value; else this.outlets = [...this.outlets, g.value];
      }
    } finally { this.probing = false; this.probedOnce = true; this.rebuild(); }
  }

  async sweep(): Promise<void> {
    if (this.sweeping) return;
    this.sweeping = true; this.error = '';
    try { await this.api.startOutletSweep(); } catch { this.sweeping = false; this.error = "Couldn't start the sweep."; return; }
    this.sweepTimer = setInterval(() => void this.pollSweep(), 1000);
  }
  private async pollSweep(): Promise<void> {
    try {
      const p = await this.api.outletSweepProgress();
      this.progress = p;
      this.merge(p.found);
      if (!p.running && p.everRan && p.scanned > 0) this.endSweep();
    } catch { this.endSweep(); }
  }
  private endSweep(): void {
    if (this.sweepTimer) clearInterval(this.sweepTimer);
    this.sweepTimer = null; this.sweeping = false; this.rebuild();
  }
  async stopSweep(): Promise<void> {
    try { await this.api.cancelOutletSweep(); } catch { /* the next poll tells the truth */ }
  }
  /** What a sweep finds merges into what a scan found, by address. */
  private merge(list: DiscoveredOutlet[]): void {
    for (const d of list) {
      const at = this.outlets.findIndex(o => o.ip === d.ip);
      if (at >= 0) this.outlets[at] = d; else this.outlets = [...this.outlets, d];
    }
    this.rebuild();
  }

  async addByIp(): Promise<void> {
    const ip = this.manualIp.trim();
    if (!ip || this.adding) return;
    this.addError = '';
    // Loose on purpose: the device is the real validator, and a hostname works fine.
    if (!/^[a-zA-Z0-9.\-:]+$/.test(ip)) { this.addError = "That doesn't look like an address. Try something like 192.168.1.42."; return; }
    this.adding = true;
    try { this.merge([await this.api.pingOutlet(ip)]); this.manualIp = ''; }
    catch { this.addError = `Couldn't reach ${ip}. Check the address and that it's on this WiFi.`; }
    finally { this.adding = false; }
  }

  // ── acting on a plug ──────────────────────────────────────────────────────
  startRename(r: PlugRow): void { this.renaming = r.key; this.renameText = r.plugName || r.forName; }
  async commitRename(r: PlugRow): Promise<void> {
    const label = this.renameText.trim();
    this.renaming = null;
    if (!label || label === r.plugName) return;
    this.error = ''; this.note = '';
    try {
      const res = await this.api.renameOutlet(r.ip, label);
      if (!res.ok) { this.error = res.error || "The plug didn't take the new name."; return; }
      if (r.seen) r.seen.name = res.name || label;
      r.plugName = res.name || label;
      this.note = 'Renamed on the plug itself.';
    } catch { this.error = "Couldn't reach the plug to rename it."; }
  }

  async release(r: PlugRow): Promise<void> {
    this.confirmRelease = null; this.error = ''; this.note = '';
    try {
      const res = await this.api.releaseOutlet(r.ip);
      // Not ok is usually an explanation, not a failure ("polled only — nothing was written to this plug"), and either way
      // the pairing is ours to end, so it is said and the release carries on.
      this.note = res.ok ? (res.note || (res.released ? 'Released.' : 'Unpaired. Nothing was written to the plug.'))
                         : `Unpaired. ${res.error || res.note || ''}`.trim();
    } catch { this.note = "Unpaired here. The plug didn't answer, so nothing was written to it."; }
    // Off the layout whether or not the plug answered: the pairing is ours to end.
    if (this.topo) {
      if (r.machineId) {
        const m = machinesOf(this.topo).find(x => x.id === r.machineId);
        if (m) delete m['sensor'];
      } else if (r.collectorId) {
        for (const sys of systemsOf(this.topo)) {
          const c = collectorOf(sys);
          if (c && c['id'] === r.collectorId) setOutlet(this.topo, c, null);
        }
      }
      await this.save();
    }
    await this.scan();
  }

  async takeover(r: PlugRow): Promise<void> {
    this.confirmTakeover = null; this.error = ''; this.note = '';
    try {
      const res = await this.api.takeoverOutlet(r.ip);
      if (!res.ok) { this.error = res.error || "The takeover didn't go through."; return; }
      this.note = 'Taken over. The plug now pushes to this brain.';
    } catch { this.error = "Couldn't reach the plug."; return; }
    await this.scan();
  }

  async pair(ip: string, machineId: string): Promise<void> {
    this.pairing = null; this.error = ''; this.note = '';
    const d = this.outlets.find(o => o.ip === ip), m = machinesOf(this.topo).find(x => x.id === machineId);
    if (!this.topo || !d || !m) return;
    const tasmota = d.kind === 'tasmota';
    const outlet: RawEl = { gen: tasmota ? 0 : (d.generation || 2), ip: d.ip, thresholdW: DEFAULT_THRESHOLD };
    if (tasmota) outlet['kind'] = 'tasmota';
    if (d.mac) outlet['mac'] = d.mac;
    if (d.hostname) outlet['host'] = d.hostname;
    if (d.name) outlet['name'] = d.name;
    if (d.powerW >= 5) outlet['thresholdW'] = Math.max(10, Math.round(d.powerW * 0.9 / 10) * 10);
    m.sensor = { outlet };
    await this.save();
    this.note = `The ${(m.name as string) || machineId} now has a plug. Set its trip point in Tools.`;
    await this.scan();
  }

  private async save(): Promise<void> {
    if (!this.topo) return;
    try { await this.api.putTopology(this.topo as unknown as Topology); this.error = ''; }
    catch { this.error = "Couldn't save the layout. Is the controller still answering?"; }
    this.rebuild();
  }

  back(): void { void this.router.navigate(['/build']); }
}
