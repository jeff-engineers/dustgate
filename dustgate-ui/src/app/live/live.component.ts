import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterLink } from '@angular/router';
import { ApiService, Topology, TopologyStatus } from '../services/api.service';
import { airflowIssues } from '@topology';
import { validateShop } from '@shop';
import { configurableSelectorsOf, isCalibrated } from '../gates/selector-types';
import { type ShopDoc, machinesOf, portsOf, systemViews, toShop } from '../services/shop-doc';

// One tool row's static identity (from the topology) merged with its live state
// (from /api/status). `collecting` is the routing winner — the single tool
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

const POLL_MS = 2000;

/**
 * The "Live view" — the daily driver. A plain list of tools: what's
 * collecting reads at a glance, everything else is one tap away. Auto tools
 * sense their own power; every tool (auto included) is manually overridable,
 * because sometimes you just need to run the collector to clear a clog.
 *
 * In demo mode DemoApiService seeds a topology so it has something to show.
 * Drives tools through `simTool` (simulated power draw), which is the same
 * lever the real firmware's /api/sim/tool exposes.
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

    .ctlerr {
      font-size: 13px; color: var(--danger); margin: 0 8px 10px; line-height: 1.5;
    }
    /* Three across, not two. At 375px a 14px label plus an 18px icon no longer
       fits, and "Shop layout" wrapped to two lines while its neighbours stayed on
       one — a ragged row. Everything here is a notch smaller so all three labels
       stay single-line on the narrowest phone we target. */
    .nav { display: flex; gap: 8px; margin-top: 18px; }
    .nav a { flex: 1; display: flex; align-items: center; justify-content: center; gap: 7px;
             padding: 12px 6px; border: 1px solid var(--border); border-radius: var(--radius);
             color: var(--muted); text-decoration: none; font-size: 13px; white-space: nowrap; }
    .nav a svg { width: 17px; height: 17px; flex: none; }
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
          <div class="c-sub">{{ collectorSub() }}</div>
        </div>
        <button class="sw" [class.on]="collectorOn"
                [attr.aria-label]="collectorOn ? 'Stop collection' : 'System idle'"
                (click)="stopAll()"></button>
      </div>

      <p class="ctlerr" *ngIf="error">{{ error }}</p>

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

      <!-- The way out of the Live view. Since / forwards straight here, the
           switcher has to live on the page it leaves — and that includes
           Settings, which had a route and a back button but nothing anywhere in
           the app pointing at it. -->
      <div class="nav">
        <a routerLink="/build">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <path d="M4 8h9M17 8h3"/><circle cx="15" cy="8" r="2"/>
            <path d="M4 16h4M12 16h8"/><circle cx="10" cy="16" r="2"/>
          </svg>
          Shop layout
        </a>
        <a routerLink="/tools">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <rect x="4" y="4" width="16" height="16" rx="3"/>
            <circle cx="9.5" cy="11" r="1.1" fill="currentColor" stroke="none"/>
            <circle cx="14.5" cy="11" r="1.1" fill="currentColor" stroke="none"/>
            <path d="M9 16h6"/>
          </svg>
          Tools
        </a>
        <a routerLink="/settings">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <!-- A cog, not a sunburst: the first draft was a hub plus eight
                 detached rays, which at 17px read as a sun. The outer ring is what
                 makes it a gear — the ticks are teeth crossing a rim, not rays. -->
            <circle cx="12" cy="12" r="7.4"/>
            <circle cx="12" cy="12" r="2.9"/>
            <path d="M12 2.8v2.3M12 18.9v2.3M2.8 12h2.3M18.9 12h2.3
                     M5.5 5.5l1.6 1.6M16.9 16.9l1.6 1.6M5.5 18.5l1.6-1.6M16.9 7.1l1.6-1.6"/>
          </svg>
          Settings
        </a>
      </div>
    </ng-container>

    <ng-template #noShop>
      <div class="empty">
        <p>No shop configured yet.</p>
        <p><a routerLink="/build">Set up your shop →</a></p>
        <!-- The nav above is inside the has-tools branch, so without this a device
             with no layout — the one most likely to need Forget WiFi — has no way
             to reach Settings at all. -->
        <p><a routerLink="/settings">Settings</a></p>
      </div>
    </ng-template>
  `,
})
export class LiveViewComponent implements OnInit, OnDestroy {
  shopName = 'The Shop';
  collectorName = 'Dust collector';
  tools: ToolRow[] = [];
  collectorOn = false;
  /** The blower is still on, but only to finish clearing the ducts — every tool is
   *  already off. Firmware has always published this (TopologyRuntime::writeStatus);
   *  until now the view ignored it and showed "Collecting · " with nothing after the
   *  separator, since there's no active tool left to name. */
  collectorCoasting = false;
  activeName = '';
  /** False while the saved layout is unfinished — the build canvas lets you save a
   *  work-in-progress shop, so this view is where that gets enforced: no gate and
   *  no collector moves until it's whole. */
  ready = true;
  notReadyReason = '';
  /** Last control failure, shown inline. A tap that silently does nothing is the
   *  exact failure mode this view just had. */
  error = '';
  /** Where "Finish setup" goes — the gate pass when that's what's missing, the canvas
   *  otherwise. Sending someone to the layout to fix a calibration would just confuse. */
  fixLink = '/build';

  private poll: ReturnType<typeof setInterval> | null = null;
  private busy = false;

  constructor(private api: ApiService) {}

  async ngOnInit(): Promise<void> {
    try {
      await this.api.whenReady();          // else the first fetch 401s and reads as "no shop"
      const topo = toShop(await this.api.getTopology());
      if (topo) this.parseTopology(topo as unknown as Topology);
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

  /** What the collector is doing, in the same `thing · detail` shape as the tool
   *  rows. The coast-down gets said out loud: a blower running with every tool off
   *  otherwise reads as a stuck relay, and someone would go looking for the fault
   *  instead of waiting the few seconds out. */
  collectorSub(): string {
    if (!this.collectorOn) return 'Idle';
    if (this.collectorCoasting) return 'Collecting · coasting down';
    return this.activeName ? 'Collecting · ' + this.activeName : 'Collecting';
  }

  sourceLine(t: ToolRow): string {
    if (t.collecting) return t.auto ? 'Auto · sensing power' : 'Manual · on';
    if (t.on)         return t.auto ? 'Auto · powered' : 'Manual · on';
    return t.auto ? 'Auto · idle' : 'Manual';
  }

  async toggle(t: ToolRow): Promise<void> {
    if (this.busy || !this.ready) return;
    this.busy = true;
    this.error = '';
    try {
      await this.api.setToolManual(t.id, !t.on);
      // Optimistic, then confirmed: the device answers the POST before its main
      // loop has routed, so the authoritative state comes from the refresh.
      t.on = !t.on;
      await this.refresh(true);
    } catch {
      this.error = 'Couldn\'t reach the controller — nothing was switched.';
    } finally {
      this.busy = false;
    }
  }

  async stopAll(): Promise<void> {
    if (this.busy || !this.collectorOn || !this.ready) return;
    this.busy = true;
    this.error = '';
    try {
      for (const t of this.tools) if (t.on) await this.api.setToolManual(t.id, false);
      await this.refresh(true);
    } catch {
      this.error = 'Couldn\'t reach the controller — nothing was switched.';
    } finally {
      this.busy = false;
    }
  }

  private async refresh(force = false): Promise<void> {
    if (this.busy && !force) return;
    try {
      this.applyStatus(await this.api.getStatus());
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
    this.collectorCoasting = !!status.collectorCoasting;
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
      const v = validateShop(topo);
      if (!v.ok) reason = v.errors[0]?.message ?? 'the layout is incomplete.';
      else {
        // Per system — a leak is a statement about one blower's ducts, and
        // airflowIssues handed a whole shop would find no elements at the root
        // and report all-clear. See systemViews().
        const leaks = systemViews(topo as unknown as ShopDoc).flatMap(view => airflowIssues(view));
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
          // The canvas, not a separate pass: each unset gate wears an orange dot
          // there, and tapping it opens the same configurator.
          this.fixLink = '/build';
        }
      }
    } catch { reason = 'the layout could not be read.'; }
    this.ready = !reason;
    this.notReadyReason = reason;
  }

  private parseTopology(topo: Topology): void {
    this.checkReady(topo);
    const doc = topo as unknown as ShopDoc;
    this.shopName = doc.name ?? 'The Shop';

    // The list is of MACHINES, not ports. This view answers "what is running",
    // and what runs is a machine — a table saw with a cabinet port and an overarm
    // pickup is one row with one switch, not two. The status blob keys its `tools`
    // map by machine id for the same reason, so these ids line up with it.
    this.tools = machinesOf(doc).map(m => ({
      id: m.id,
      name: m.name || m.id,
      auto: !!m.sensor?.outlet,
      on: false,
      collecting: false,
    }));

    // With one system this is the blower's name, as it always was. With several
    // there is no single answer, so say so rather than picking one arbitrarily and
    // showing the wrong name half the time.
    const collectors = systemViews(doc)
      .flatMap(view => ((view as { elements?: Array<Record<string, unknown>> }).elements ?? []))
      .filter(e => e['type'] === 'collector');
    this.collectorName = collectors.length === 1
      ? ((collectors[0]['name'] as string) || 'Dust collector')
      : `${collectors.length} dust collectors`;
  }
}
