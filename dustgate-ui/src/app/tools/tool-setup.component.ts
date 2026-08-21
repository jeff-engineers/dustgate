import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { ApiService, DiscoveredOutlet, Topology } from '../services/api.service';
import { elementsOf, ductsOf } from '../gates/selector-types';
import { type ShopDoc, machineOfPort, outletOf, toShop } from '../services/shop-doc';

// ── Tool-tagging pass ────────────────────────────────────────────────────────
// After the plumbing is drawn (build canvas), walk the tool list once and tag
// each: smart outlet or manual, and if smart, which plug + at what threshold.
// The trick is identify-by-power: switch the tool on and watch
// which plug jumps to green. Writes `sensor.outlet` onto each tool element.

interface RawEl { [k: string]: unknown; }
interface Branch { id: string; }

interface ToolCfg {
  id: string;
  name: string;
  gateLabel: string;
  hasPlug: boolean;
  ip: string;
  gen: number;
  hostname: string;
  thresholdW: number;
}

const DEFAULT_THRESHOLD = 50;

@Component({
  selector: 'app-tool-setup',
  standalone: true,
  imports: [CommonModule, FormsModule],
  styles: [`
    :host { display: block; max-width: 460px; margin: 0 auto; padding: 16px 14px 40px; }
    .head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
    .head .step { font-size: 12.5px; color: var(--muted); }
    .dots { display: flex; gap: 5px; align-items: center; }
    .dots i { width: 7px; height: 7px; border-radius: 50%; background: var(--border-strong, #444); }
    .dots i.done { background: var(--success); } .dots i.cur { background: var(--accent); }

    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; padding: 18px 16px; }
    .toolrow { display: flex; align-items: center; gap: 11px; margin-bottom: 16px; }
    .toolrow .ic { width: 42px; height: 42px; border-radius: 11px; background: var(--bg); border: 1px solid var(--border); display: flex; align-items: center; justify-content: center; }
    .toolrow .ic svg { width: 22px; height: 22px; stroke: var(--muted); }
    .name-in { font-size: 17px; font-weight: 500; background: var(--bg); border: 1px solid var(--border); color: var(--text); border-radius: 8px; padding: 6px 9px; width: 100%; }
    .gate { font-size: 11.5px; color: var(--accent); background: rgba(240,165,0,0.12); display: inline-block; padding: 2px 8px; border-radius: 20px; margin-top: 5px; }

    .q { display: flex; align-items: center; justify-content: space-between; padding: 12px 14px; background: var(--bg); border: 1px solid var(--border); border-radius: 12px; margin-bottom: 12px; }
    .q .yesno { display: flex; gap: 6px; }
    .q button { background: var(--surface); border: 1px solid var(--border); color: var(--muted); border-radius: 8px; padding: 6px 14px; font-size: 13px; }
    .q button.on { background: var(--success); border-color: var(--success); color: #05230f; font-weight: 600; }

    .hint { display: flex; gap: 8px; align-items: flex-start; background: rgba(240,165,0,0.10); border-radius: 10px; padding: 10px 12px; margin-bottom: 12px; font-size: 12.5px; color: var(--accent); }

    .scan { display: flex; flex-direction: column; gap: 7px; margin-bottom: 14px; }
    .plug { display: flex; align-items: center; gap: 11px; padding: 11px 13px; background: var(--bg); border: 1px solid var(--border); border-radius: 11px; text-align: left; width: 100%; color: var(--text); }
    .plug.sel { border-color: var(--success); background: rgba(60,190,110,0.08); }
    .plug:disabled { opacity: 0.5; }
    .plug .dot { width: 9px; height: 9px; border-radius: 50%; flex-shrink: 0; background: var(--border-strong, #555); }
    .plug .dot.on { background: var(--success); } .plug .dot.low { background: var(--accent); }
    .plug .pn { font-size: 14px; font-weight: 500; }
    .plug .ps { font-size: 11.5px; color: var(--muted); }
    .plug.sel .pn { color: var(--success); }
    .rescan, .manual { display: flex; align-items: center; justify-content: center; gap: 6px; color: var(--muted); font-size: 12.5px; padding: 8px; background: none; border: none; width: 100%; }
    .empty { text-align: center; color: var(--muted); font-size: 13px; padding: 16px; }

    .thresh { border-top: 1px solid var(--border); padding-top: 13px; margin-bottom: 4px; }
    .thresh .r { display: flex; align-items: center; justify-content: space-between; margin-bottom: 5px; }
    .thresh .r b { font-weight: 500; }
    .thresh input[type=range] { width: 100%; }
    .thresh .why { font-size: 11.5px; color: var(--muted); margin-top: 3px; }

    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .next { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
    .nav .next:disabled { opacity: 0.5; }
    /* Back is disabled on the first tool, and until now said so only by not
       responding — a button that looks live and does nothing reads as a broken
       page, not as "you are at the beginning". */
    .nav .back:disabled { opacity: 0.4; }

    /* Same round control Settings uses, so "leave this screen" looks the same
       wherever it appears. Setup used to have no way out at all except finishing
       it: every tool walked, then Finish. */
    .exit-btn { background: var(--surface); border: 1px solid var(--border);
                border-radius: 50%; width: 34px; height: 34px; flex-shrink: 0;
                display: flex; align-items: center; justify-content: center;
                color: var(--muted); }
    .exit-btn:active { opacity: 0.6; }
    .head .left { display: flex; align-items: center; gap: 10px; }
  `],
  template: `
    <!-- OUTSIDE the tools check, deliberately. With no tools drawn yet this screen
         was a dead end: an empty-state sentence and nothing to press, on a page
         reachable straight from the landing screen. The way out must not depend
         on there being something to set up. -->
    <div class="head">
      <div class="left">
        <button class="exit-btn" (click)="exit()" aria-label="Leave tool setup">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
               stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M19 12H5M12 19l-7-7 7-7"/>
          </svg>
        </button>
        <span class="step">Set up your tools</span>
      </div>
      <div class="dots" *ngIf="tools.length">
        <i *ngFor="let t of tools; let i = index" [class.done]="i < index" [class.cur]="i === index"></i>
        <span class="step" style="margin-left:5px">{{ index + 1 }} of {{ tools.length }}</span>
      </div>
    </div>

    <ng-container *ngIf="cur() as c">

      <div class="card">
        <div class="toolrow">
          <span class="ic"><svg viewBox="0 0 24 24" fill="none" stroke-width="1.6" stroke-linecap="round"><rect x="5" y="7" width="14" height="10" rx="2"/></svg></span>
          <div style="flex:1">
            <input class="name-in" [(ngModel)]="c.name" (ngModelChange)="touched = true"/>
            <div class="gate">{{ c.gateLabel }}</div>
          </div>
        </div>

        <div class="q">
          <span>Smart outlet on this tool?</span>
          <div class="yesno">
            <button [class.on]="c.hasPlug" (click)="c.hasPlug = true; touched = true">Yes</button>
            <button [class.on]="!c.hasPlug" (click)="c.hasPlug = false; touched = true">No</button>
          </div>
        </div>

        <ng-container *ngIf="c.hasPlug; else manual">
          <div class="hint">
            <span>💡</span><span>Turn {{ c.name }} on — the plug that jumps to <b>green</b> is the one.</span>
          </div>
          <div class="scan" *ngIf="outlets.length; else noplugs">
            <button class="plug" *ngFor="let d of outlets"
                    [class.sel]="d.ip === c.ip" [disabled]="isAssignedElsewhere(d.ip, c)"
                    (click)="pick(d, c)">
              <span class="dot" [class.on]="drawLevel(d) === 'on'" [class.low]="drawLevel(d) === 'low'"></span>
              <div style="flex:1">
                <div class="pn">{{ d.name || d.hostname }}</div>
                <div class="ps">{{ plugSub(d, c) }}</div>
              </div>
              <span *ngIf="d.ip === c.ip">✓</span>
            </button>
            <button class="rescan" (click)="scan()">↻ {{ scanning ? 'Scanning…' : 'Rescan' }}</button>
          </div>
          <ng-template #noplugs>
            <div class="empty">{{ scanning ? 'Scanning…' : 'No plugs found on the network.' }}</div>
            <button class="rescan" (click)="scan()">↻ Rescan</button>
          </ng-template>

          <div class="thresh" *ngIf="c.ip">
            <div class="r"><span>Start collection above</span><b>{{ c.thresholdW }} W</b></div>
            <input type="range" min="0" max="400" step="10" [(ngModel)]="c.thresholdW"
                   (ngModelChange)="touched = true"/>
            <div class="why">Catches the motor, ignores standby draw.</div>
          </div>
        </ng-container>
        <ng-template #manual>
          <div class="empty">You'll switch this one on manually from the tool list.</div>
        </ng-template>
      </div>

      <div class="nav">
        <button class="back" (click)="prev()" [disabled]="index === 0">← Back</button>
        <button class="next" (click)="next()" [disabled]="saving">
          {{ index === tools.length - 1 ? (saving ? 'Saving…' : 'Finish') : 'Next tool →' }}
        </button>
      </div>
    </ng-container>

    <div class="empty" *ngIf="!tools.length">No tools to set up yet. Draw your plumbing first.</div>
  `,
})
export class ToolSetupComponent implements OnInit {
  tools: ToolCfg[] = [];
  index = 0;
  outlets: DiscoveredOutlet[] = [];
  scanning = false;
  saving = false;

  private topo: Topology | null = null;
  /**
   * Has the user actually changed anything? Set by the edit affordances
   * themselves rather than derived by comparing a snapshot — see exit().
   */
  touched = false;

  constructor(private api: ApiService, private router: Router) {}

  async ngOnInit(): Promise<void> {
    try {
      await this.api.whenReady();          // else the first fetch 401s and reads as "no tools"
      // Migrated on read, like every other entry point — see shop-doc.ts.
      this.topo = toShop(JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology) as unknown as Topology;
    } catch { return; }
    const els = this.elems();
    this.tools = els.filter(e => e['type'] === 'tool').map(e => this.toCfg(e));
    if (this.tools.length) void this.scan();
  }

  cur(): ToolCfg | null { return this.tools[this.index] ?? null; }

  drawLevel(d: DiscoveredOutlet): 'off' | 'low' | 'on' {
    if (!d.reachable || d.powerW < 1) return 'off';
    return d.powerW >= 5 ? 'on' : 'low';
  }

  plugSub(d: DiscoveredOutlet, c: ToolCfg): string {
    if (this.isAssignedElsewhere(d.ip, c)) return 'already assigned to another tool';
    if (!d.reachable) return 'not responding';
    const lvl = this.drawLevel(d);
    const w = Math.round(d.powerW);
    return lvl === 'on' ? `${w} W · running` : lvl === 'low' ? `${w} W · standby` : `${w} W · idle`;
  }

  isAssignedElsewhere(ip: string, c: ToolCfg): boolean {
    return this.tools.some(t => t !== c && t.hasPlug && t.ip === ip);
  }

  pick(d: DiscoveredOutlet, c: ToolCfg): void {
    if (this.isAssignedElsewhere(d.ip, c)) return;
    c.ip = d.ip; c.gen = d.generation || 2; c.hostname = d.hostname;
    this.touched = true;
    if (d.powerW >= 5 && (c.thresholdW === DEFAULT_THRESHOLD)) {
      c.thresholdW = Math.max(10, Math.round(d.powerW * 0.9 / 10) * 10);
    }
  }

  async scan(): Promise<void> {
    this.scanning = true;
    try { this.outlets = await this.api.discoverOutlets(); }
    catch { this.outlets = []; }
    finally { this.scanning = false; }
  }

  prev(): void { if (this.index > 0) this.index--; }

  /**
   * Leave without finishing. Nothing is written until Finish — save() runs once,
   * on the last tool — so walking out mid-way discards every edit made since the
   * screen opened. That is worth a question, but only when there is something to
   * lose — landing here by accident should cost one tap to leave, not a dialog.
   *
   * `touched` is set by the edit affordances themselves. The first cut compared a
   * JSON snapshot taken at load, and asked on a screen nobody had typed into:
   * binding a range input writes its value back through ngModel, so the model
   * differs from its own snapshot before the user has done anything. An explicit
   * flag cannot drift that way.
   */
  async exit(): Promise<void> {
    if (this.touched &&
        !confirm('Leave without saving?\n\nPlug assignments and names from this session will be lost.')) return;
    await this.router.navigate(['/']);
  }

  async next(): Promise<void> {
    if (this.index < this.tools.length - 1) { this.index++; return; }
    await this.save();
  }

  private async save(): Promise<void> {
    if (!this.topo || this.saving) return;
    this.saving = true;
    try {
      for (const c of this.tools) {
        const el = this.elems().find(e => e['id'] === c.id);
        if (!el) continue;
        // The name and the plug both belong to the MACHINE now: this screen is
        // "which plug is this tool on", and a tool is a machine even when it has
        // one port. Writing them to the port would leave the routing brain — which
        // only ever reads machines — sensing nothing.
        const m = machineOfPort(this.topo as unknown as ShopDoc, el);
        if (!m) continue;
        m.name = c.name;
        if (c.hasPlug && c.ip) {
          const outlet: RawEl = { gen: c.gen || 2, ip: c.ip, thresholdW: c.thresholdW || DEFAULT_THRESHOLD };
          if (c.hostname) outlet['host'] = c.hostname;
          m.sensor = { outlet };
        } else {
          delete m.sensor;
        }
      }
      await this.api.putTopology(this.topo);
      await this.router.navigate(['/shop']);
    } finally {
      this.saving = false;
    }
  }

  // ── helpers ─────────────────────────────────────────────────────────────────
  // Flattened across systems: this screen lists every tool in the shop, which is
  // a shop-wide question — it has no canvas and never draws one duct tree.
  private elems(): RawEl[] { return this.topo ? elementsOf(this.topo) as unknown as RawEl[] : []; }
  private ducts(): RawEl[] { return this.topo ? ductsOf(this.topo) as unknown as RawEl[] : []; }

  private toCfg(e: RawEl): ToolCfg {
    const outlet = outletOf(this.topo as unknown as ShopDoc, e) ?? undefined;
    const machine = machineOfPort(this.topo as unknown as ShopDoc, e);
    return {
      id: e['id'] as string,
      name: (machine?.name as string) || (e['name'] as string) || (e['id'] as string),
      gateLabel: this.gateLabel(e['id'] as string),
      hasPlug: true,                     // default Yes; prefilled if already set
      ip: (outlet?.['ip'] as string) ?? '',
      gen: (outlet?.['gen'] as number) ?? 2,
      hostname: (outlet?.['host'] as string) ?? '',
      thresholdW: (outlet?.['thresholdW'] as number) ?? DEFAULT_THRESHOLD,
    };
  }

  private gateLabel(toolId: string): string {
    const duct = this.ducts().find(d => d['child'] === toolId);
    if (!duct) return 'Not connected';
    const parent = this.elems().find(e => e['id'] === duct['parent']);
    if (!parent || parent['type'] === 'collector') return 'Direct to collector';
    const branches = (parent['branches'] as Branch[] | undefined) ?? [];
    const idx = branches.findIndex(b => b.id === duct['parentBranch']);
    const outlet = idx >= 0 ? ` · outlet ${idx + 1}` : '';
    return `${(parent['name'] as string) || 'Gate'}${outlet}`;
  }
}
