import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterLink } from '@angular/router';
import { ApiService, Topology, TopologyStatus } from '../services/api.service';
import { validateTopology, airflowIssues } from '@topology';
import { configurableSelectorsOf, isCalibrated } from '../gates/selector-types';

// One tool row's static identity (from the topology) merged with its live state
// (from /api/v2/status). `collecting` is the routing winner — the single tool
// with a clear open path to the collector right now — as opposed to `on`, which
// only means it's drawing / requesting power (it may have been out-voted by a
// more-recently-started tool sharing the same gate).
interface ToolRow {
  id: string;
  name: string;
  auto: boolean;          // has a smart outlet → senses its own power
  on: boolean;            // drawing / requested power
  collecting: boolean;    // won a clear path (reachable) — the green one
}

// Watts to simulate when a row is switched on. Comfortably above any tool's
// threshold; the exact value doesn't matter, only that it clears the bar.
const SIM_ON_WATTS = 200;
const POLL_MS = 2000;

/**
 * The v2 "Live view" — the daily driver. A plain list of tools: what's
 * collecting reads at a glance, everything else is one tap away. Auto tools
 * sense their own power; every tool (auto included) is manually overridable,
 * because sometimes you just need to run the collector to clear a clog.
 *
 * Silent lazy route (`/shop`) — nothing links to it yet. Consumes the existing
 * v2 API; in demo mode DemoApiService seeds a topology so it has something to
 * show. Drives tools through `simTool` (simulated power draw), which is the same
 * lever the real firmware's /api/v2/sim/tool exposes.
 */
@Component({
  selector: 'app-live',
  standalone: true,
  imports: [CommonModule, RouterLink],
  styles: [`
    :host {
      display: block;
      max-width: 460px;
      margin: 0 auto;
      padding: 14px 12px 28px;
      min-height: 100dvh;
      min-height: 100vh;
    }
    .top {
      display: flex; align-items: center; justify-content: space-between;
      padding: 4px 8px 14px;
    }
    .shop { font-size: 14px; color: var(--muted); }
    .dot  { width: 8px; height: 8px; border-radius: 50%; background: var(--muted); }
    .dot.live { background: var(--success); }

    /* collector card */
    .collector {
      display: flex; align-items: center; gap: 12px;
      background: var(--surface); border: 1px solid var(--border);
      border-radius: var(--radius); padding: 16px; margin-bottom: 18px;
    }
    .collector.running { border-color: var(--success); }
    .cyc {
      width: 44px; height: 44px; border-radius: 50%; flex-shrink: 0;
      display: flex; align-items: center; justify-content: center;
      background: var(--bg); color: var(--muted);
    }
    .collector.running .cyc { color: var(--success); }
    .cyc svg { width: 24px; height: 24px; }
    .c-body { flex: 1; min-width: 0; }
    .c-name { font-size: 16px; font-weight: 600; }
    .c-sub  { font-size: 13px; color: var(--muted); margin-top: 2px; }
    .collector.running .c-sub { color: var(--success); }

    .label {
      font-size: 12px; color: var(--muted); letter-spacing: 0.06em;
      text-transform: uppercase; padding: 0 8px 8px;
    }

    /* tool rows */
    .rows { display: flex; flex-direction: column; gap: 10px; }
    .row {
      display: flex; align-items: center; gap: 12px;
      background: var(--surface); border: 1px solid var(--border);
      border-radius: var(--radius); padding: 14px 16px;
      text-align: left; width: 100%; color: inherit;
      transition: border-color 0.12s;
    }
    .row.collecting { background: rgba(60,190,110,0.10); border-color: var(--success); }
    .r-body { flex: 1; min-width: 0; }
    .r-name { font-size: 16px; font-weight: 500; }
    .r-src {
      font-size: 12.5px; color: var(--muted); margin-top: 2px;
      display: flex; align-items: center; gap: 5px;
    }
    .row.collecting .r-name { color: var(--success); }

    .pill {
      font-size: 12px; font-weight: 600; color: var(--success);
      background: rgba(60,190,110,0.16); padding: 5px 12px; border-radius: 999px;
      flex-shrink: 0;
    }

    /* toggle */
    .sw {
      width: 46px; height: 28px; border-radius: 999px; flex-shrink: 0;
      background: var(--bg); border: 1px solid var(--border); position: relative;
      transition: background 0.14s, border-color 0.14s;
    }
    .sw::after {
      content: ''; position: absolute; top: 3px; left: 3px;
      width: 20px; height: 20px; border-radius: 50%; background: var(--muted);
      transition: transform 0.14s, background 0.14s;
    }
    .sw.on { background: var(--success); border-color: var(--success); }
    .sw.on::after { transform: translateX(18px); background: #fff; }

    .setup {
      display: flex; align-items: center; justify-content: center; gap: 6px;
      color: var(--muted); font-size: 13px; text-decoration: none;
      padding: 22px 0 4px;
    }
    .setup svg { width: 15px; height: 15px; }

    .empty {
      text-align: center; color: var(--muted); padding: 48px 20px;
    }
    .empty a { color: var(--accent); }

    /* Layout isn't finished — nothing here may drive a gate or the collector. */
    .incomplete {
      display: flex; flex-direction: column; gap: 10px;
      background: color-mix(in srgb, var(--danger) 12%, var(--surface));
      border: 1px solid var(--danger); color: var(--danger);
      border-radius: var(--radius); padding: 14px 16px; margin-bottom: 18px;
      font-size: 13.5px; line-height: 1.45;
    }
    .incomplete a {
      align-self: flex-start; color: #fff; background: var(--danger);
      border-radius: 8px; padding: 6px 12px; font-size: 13px;
      text-decoration: none; font-weight: 600;
    }
    .locked { opacity: 0.45; pointer-events: none; filter: grayscale(1); }
  `],
  template: `
    <div class="top">
      <span class="shop">{{ shopName }}</span>
      <span class="dot" [class.live]="collectorOn"></span>
    </div>

    <ng-container *ngIf="tools.length; else noShop">
      <!-- An unfinished shop must not drive hardware: everything below is inert
           until the layout is whole (see the ready check). -->
      <div class="incomplete" *ngIf="!ready">
        <span>Shop layout incomplete — {{ notReadyReason }} Nothing can be switched on until it’s sorted out.</span>
        <a [routerLink]="fixLink">Finish setup →</a>
      </div>

      <div class="collector" [class.running]="collectorOn" [class.locked]="!ready">
        <span class="cyc">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <path d="M13 5a7 7 0 1 0 6 7"/><path d="M12 8a4 4 0 1 1-4 4"/>
            <circle cx="12" cy="12" r="1.2" fill="currentColor" stroke="none"/>
          </svg>
        </span>
        <div class="c-body">
          <div class="c-name">{{ collectorName }}</div>
          <div class="c-sub">{{ collectorOn ? 'Collecting · ' + activeName : 'Idle' }}</div>
        </div>
        <button class="sw" [class.on]="collectorOn"
                [attr.aria-label]="collectorOn ? 'Stop collection' : 'System idle'"
                (click)="stopAll()"></button>
      </div>

      <div class="label">Tools</div>
      <div class="rows" [class.locked]="!ready">
        <button class="row" *ngFor="let t of tools" [class.collecting]="t.collecting"
                (click)="toggle(t)"
                [attr.aria-pressed]="t.on">
          <div class="r-body">
            <div class="r-name">{{ t.name }}</div>
            <div class="r-src">{{ sourceLine(t) }}</div>
          </div>
          <span class="pill" *ngIf="t.collecting; else sw">Collecting</span>
          <ng-template #sw><span class="sw" [class.on]="t.on"></span></ng-template>
        </button>
      </div>

      <a class="setup" routerLink="/setup">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
             stroke-linecap="round" stroke-linejoin="round">
          <path d="M4 8h9M17 8h3"/><circle cx="15" cy="8" r="2"/>
          <path d="M4 16h4M12 16h8"/><circle cx="10" cy="16" r="2"/>
        </svg>
        Shop setup
      </a>
    </ng-container>

    <ng-template #noShop>
      <div class="empty">
        <p>No shop configured yet.</p>
        <p><a routerLink="/setup">Set up your shop →</a></p>
      </div>
    </ng-template>
  `,
})
export class LiveViewComponent implements OnInit, OnDestroy {
  shopName = 'The Shop';
  collectorName = 'Dust collector';
  tools: ToolRow[] = [];
  collectorOn = false;
  activeName = '';
  /** False while the saved layout is unfinished — the build canvas lets you save a
   *  work-in-progress shop, so this view is where that gets enforced: no gate and
   *  no collector moves until it's whole. */
  ready = true;
  notReadyReason = '';
  /** Where "Finish setup" goes — the gate pass when that's what's missing, the canvas
   *  otherwise. Sending someone to the layout to fix a calibration would just confuse. */
  fixLink = '/build';

  private poll: ReturnType<typeof setInterval> | null = null;
  private busy = false;

  constructor(private api: ApiService) {}

  async ngOnInit(): Promise<void> {
    try {
      await this.api.whenReady();          // else the first fetch 401s and reads as "no shop"
      const topo = await this.api.getTopology();
      this.parseTopology(topo);
    } catch {
      this.tools = []; // no topology → empty state
      return;
    }
    await this.refresh();
    this.poll = setInterval(() => { void this.refresh(); }, POLL_MS);
  }

  ngOnDestroy(): void {
    if (this.poll) clearInterval(this.poll);
  }

  sourceLine(t: ToolRow): string {
    if (t.collecting) return t.auto ? 'Auto · sensing power' : 'Manual · on';
    if (t.on)         return t.auto ? 'Auto · powered' : 'Manual · on';
    return t.auto ? 'Auto · idle' : 'Manual';
  }

  async toggle(t: ToolRow): Promise<void> {
    if (this.busy || !this.ready) return;
    this.busy = true;
    try {
      const status = await this.api.simTool(t.id, t.on ? 0 : SIM_ON_WATTS);
      this.applyStatus(status);
    } finally {
      this.busy = false;
    }
  }

  async stopAll(): Promise<void> {
    if (this.busy || !this.collectorOn || !this.ready) return;
    this.busy = true;
    try {
      let last: TopologyStatus | null = null;
      for (const t of this.tools) {
        if (t.on) last = await this.api.simTool(t.id, 0);
      }
      if (last) this.applyStatus(last);
    } finally {
      this.busy = false;
    }
  }

  private async refresh(): Promise<void> {
    if (this.busy) return;
    try {
      this.applyStatus(await this.api.getV2Status());
    } catch { /* transient — keep last known state */ }
  }

  private applyStatus(status: TopologyStatus): void {
    const st = status.tools ?? {};
    const reach = status.reachable ?? {};
    for (const t of this.tools) {
      t.on = !!st[t.id]?.active;
      t.collecting = reach[t.id] === true;
    }
    this.collectorOn = !!status.collectorOn;
    this.activeName = this.tools.find(t => t.collecting)?.name ?? '';
  }

  /** The shop has to be structurally whole, free of always-open leaks, AND have every
   *  servo gate calibrated before it may run. The first two are what the build canvas
   *  reports as work-in-progress; the third is the /gates pass. An uncalibrated gate is
   *  as unsafe as a leak — we'd be driving to positions nobody has checked. */
  private checkReady(topo: Topology): void {
    let reason = '';
    this.fixLink = '/build';
    try {
      const v = validateTopology(topo);
      if (!v.ok) reason = v.errors[0]?.message ?? 'the layout is incomplete.';
      else {
        const leaks = airflowIssues(topo);
        const unset = configurableSelectorsOf(topo).filter(s => !isCalibrated(s));
        const open = leaks.filter(l => l.kind === 'always-open');
        const shared = leaks.filter(l => l.kind === 'co-open');
        if (open.length) {
          const names = open.map(l => l.name).join(', ');
          reason = `${names} ${open.length === 1 ? 'has' : 'have'} no gate between ${open.length === 1 ? 'it' : 'them'} and the collector, so suction would leak there.`;
        } else if (shared.length) {
          const names = shared.map(l => l.name).join(', ');
          reason = shared.length === 1
            ? `${names} shares an outlet with ${(shared[0].with ?? []).map(w => w.name).join(', ')} with no gate in between, so running it would pull air through them too.`
            : `${names} share an outlet with no gate between them, so running one would pull air through the others.`;
        } else if (unset.length) {
          const names = unset.map(s => s.name || s.id).join(', ');
          reason = `${names} ${unset.length === 1 ? "hasn't" : "haven't"} been set up yet — no one has shown ${unset.length === 1 ? 'it' : 'them'} where the valve positions are.`;
          this.fixLink = '/gates';
        }
      }
    } catch { reason = 'the layout could not be read.'; }
    this.ready = !reason;
    this.notReadyReason = reason;
  }

  private parseTopology(topo: Topology): void {
    this.checkReady(topo);
    const doc = topo as { name?: string; elements?: Array<Record<string, unknown>> };
    this.shopName = doc.name ?? 'The Shop';
    const els = doc.elements ?? [];
    this.tools = els
      .filter(e => e['type'] === 'tool')
      .map(e => ({
        id: e['id'] as string,
        name: (e['name'] as string) || (e['id'] as string),
        auto: !!(e['sensor'] as Record<string, unknown> | undefined)?.['outlet'],
        on: false,
        collecting: false,
      }));
    const collector = els.find(e => e['type'] === 'collector');
    this.collectorName = (collector?.['name'] as string) || 'Dust collector';
  }
}
