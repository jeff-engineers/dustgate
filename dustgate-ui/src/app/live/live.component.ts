import { Component, OnInit, OnDestroy } from '@angular/core';
import { CommonModule } from '@angular/common';
import { RouterLink } from '@angular/router';
import { ApiService, Topology, TopologyStatus } from '../services/api.service';
import { airflowIssues } from '@topology';
import { validateShop } from '@shop';
import { configurableSelectorsOf, isCalibrated } from '../gates/selector-types';
import { type ShopDoc, machinesOf, portsOf, primaryPortOf, systemsOf, systemViews, toShop } from '../services/shop-doc';

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

/**
 * One airflow system: its blower, and the tools that breathe through it.
 *
 * The view used to be one collector card over one flat list, which had nothing to
 * say about a two-system shop — the card read "2 dust collectors" and the switch
 * on it stopped every tool in the building. A blower is per system and so is the
 * decision to shut one down, so the page is per system too.
 *
 * `id` is '' for the orphan group: machines with no port in any system. They are
 * a layout fault the readiness check already reports, but they must still be
 * LISTED — they were visible in the flat list, and quietly dropping a tool from
 * the page is a worse answer than showing it can't collect.
 */
interface SystemGroup {
  id: string;
  name: string;
  tools: ToolRow[];
  on: boolean;
  coasting: boolean;
  /** The tool currently winning this system's air, for the subtitle. */
  activeName: string;
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

    /* Systems are separated by space, not by a rule: the collector card already
       reads as a header, and a divider on top of it is one boundary too many. */
    .sys + .sys { margin-top: 26px; }
    .orphan {
      font-size: 13.5px; color: var(--accent); background: rgba(240,165,0,0.10);
      border-radius: var(--radius); padding: 12px 14px; margin-bottom: 18px;
      line-height: 1.45;
    }
    .rows-empty { font-size: 13px; color: var(--muted); padding: 6px 8px; }

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
    /* A GRID, not a flex row: four destinations at 13px with an icon each are
       wider than a phone, and flex:1 on a nowrap label cannot shrink below its
       own text — so the last one hung off the right edge of a 375px screen (found
       2026-08-22, once /gates made it four).
       auto-fit rather than a fixed 2x2: this column is capped at 460px, so four
       entries land two-by-two at every width, but the fifth entry — or the fourth
       going away again — re-flows on its own instead of needing this rule edited
       to match. */
    .nav { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
           gap: 8px; margin-top: 18px; }
    .nav a { display: flex; align-items: center; justify-content: center; gap: 7px;
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

      <p class="ctlerr" *ngIf="error">{{ error }}</p>

      <!-- One block per airflow system, in the order the shop layout draws them
           top to bottom. A blower and the tools that breathe through it belong
           together: which collector a tool runs is the single most useful thing
           this page knows about it, and a flat list threw it away. -->
      <div class="sys" *ngFor="let g of groups">
        <div class="collector" *ngIf="g.id" [class.running]="g.on" [class.locked]="!ready">
          <span class="cyc">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
                 stroke-linecap="round" stroke-linejoin="round">
              <path d="M13 5a7 7 0 1 0 6 7"/><path d="M12 8a4 4 0 1 1-4 4"/>
              <circle cx="12" cy="12" r="1.2" fill="currentColor" stroke="none"/>
            </svg>
          </span>
          <div class="c-body">
            <div class="c-name">{{ g.name }}</div>
            <div class="c-sub">{{ collectorSub(g) }}</div>
          </div>
          <!-- Stops THIS system's tools. It used to stop every tool in the shop,
               which on a two-system layout switched off a machine in the other
               half of the building that had nothing to do with the card. -->
          <button class="sw" [class.on]="g.on"
                  [attr.aria-label]="g.on ? 'Stop ' + g.name : g.name + ' idle'"
                  [title]="g.on ? 'Switch off every tool running on ' + g.name : g.name + ' is idle'"
                  (click)="stopSystem(g)"></button>
        </div>

        <!-- No card, because there is no collector to card. Named rather than
             silently mixed in: these tools cannot collect, and the readiness
             banner above says why. -->
        <div class="orphan" *ngIf="!g.id">{{ g.name }}</div>

        <div class="label">Tools</div>
        <div class="rows" [class.locked]="!ready">
          <button class="row" *ngFor="let t of g.tools" [class.collecting]="t.collecting"
                  (click)="toggle(t)"
                  [attr.aria-pressed]="t.on">
            <div class="r-body">
              <div class="r-name">{{ t.name }}</div>
              <div class="r-src">{{ sourceLine(t) }}</div>
            </div>
            <span class="pill" *ngIf="t.collecting; else sw">Collecting</span>
            <ng-template #sw><span class="sw" [class.on]="t.on"></span></ng-template>
          </button>
          <div class="rows-empty" *ngIf="!g.tools.length">Nothing plumbed into this one yet.</div>
        </div>
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
        <!-- Gate calibration used to be reachable only by finding the gate on the
             build canvas and tapping it — a layout tool, opened with a wrench in
             your other hand. Recalibrating a knocked valve is a shop-floor errand,
             so it gets an entry where the app already puts "go somewhere else". -->
        <a routerLink="/gates">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
               stroke-linecap="round" stroke-linejoin="round">
            <!-- A butterfly valve seen edge-on: a duct, and a disc across it at an
                 angle. The gate glyph the canvas uses is a dot on a line, which at
                 17px in a nav bar is indistinguishable from the layout icon. -->
            <path d="M4 7h16M4 17h16"/>
            <path d="M8 16.5L16 7.5"/>
            <circle cx="12" cy="12" r="1.5" fill="currentColor" stroke="none"/>
          </svg>
          Gates
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
  /** One block per airflow system, in shop-layout order. */
  groups: SystemGroup[] = [];
  /** Every tool across every system, flat. Kept because the shop-wide questions —
   *  is anything running, is the layout ready — are still shop-wide. */
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
  collectorSub(g: SystemGroup): string {
    if (!g.on) return 'Idle';
    if (g.coasting) return 'Collecting · coasting down';
    return g.activeName ? 'Collecting · ' + g.activeName : 'Collecting';
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

  /** Switch off everything running on ONE blower. The old stopAll() stopped every
   *  tool in the shop from whichever card you tapped, which on a two-system layout
   *  reached across and shut down a machine in the other half of the building. */
  async stopSystem(g: SystemGroup): Promise<void> {
    if (this.busy || !g.on || !this.ready) return;
    this.busy = true;
    this.error = '';
    try {
      for (const t of g.tools) if (t.on) await this.api.setToolManual(t.id, false);
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

    // Per-blower truth if the device sends it — both the firmware and the model
    // always have. A device that doesn't falls back to the shop-wide pair, which
    // is exactly right for a one-system shop and the best available guess
    // anywhere else; the alternative is showing nothing.
    const per = status.systems;
    for (const g of this.groups) {
      const s = per?.[g.id];
      g.on = s ? !!s.collectorOn : this.collectorOn;
      g.coasting = s ? !!s.coasting : this.collectorCoasting;
      g.activeName = g.tools.find(t => t.collecting)?.name ?? '';
    }
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
    // secondary port is one row with one switch, not two. The status blob keys its `tools`
    // map by machine id for the same reason, so these ids line up with it.
    this.tools = machinesOf(doc).map(m => ({
      id: m.id,
      name: m.name || m.id,
      auto: !!m.sensor?.outlet,
      on: false,
      collecting: false,
    }));

    this.groups = this.buildGroups(doc);
  }

  /**
   * Split the tools across their systems, in the order the build canvas draws
   * them.
   *
   * ORDER comes from `ui.layout` — the canvas stripes its systems by the topmost
   * row any of their pieces stands on (systemRowBands()), so reading the same
   * saved cells and sorting the same way is what makes this page and that one
   * agree. Document order is the fallback when a shop has never been laid out;
   * it is what the canvas would auto-layout from anyway.
   *
   * A machine belongs to the system of its PRIMARY port. A secondary port's run
   * may cross the seam — it is the one thing allowed to — but the tool itself
   * lives in one system, and that is the one whose blower it starts.
   */
  private buildGroups(doc: ShopDoc): SystemGroup[] {
    const layout = (doc as unknown as { ui?: { layout?: Record<string, { row: number }> } }).ui?.layout;
    const topRow = (sysId: string): number => {
      const sys = systemsOf(doc).find(s => s.id === sysId);
      if (!sys || !layout) return Number.POSITIVE_INFINITY;
      let lo = Number.POSITIVE_INFINITY;
      for (const e of sys.elements as Array<Record<string, unknown>>) {
        // Junctions skipped, exactly as systemRowBands() does: a loose run end is
        // where pipe happens to have reached, not a row the system stands on.
        if (e['type'] === 'junction') continue;
        const c = layout[e['id'] as string];
        if (c) lo = Math.min(lo, c.row);
      }
      return lo;
    };

    const order = systemsOf(doc).map((s, i) => ({ s, i, row: topRow(s.id) }))
      // Ties and un-laid-out systems keep document order, so the sort is stable
      // in the cases where the canvas has nothing to say.
      .sort((a, b) => (a.row - b.row) || (a.i - b.i));

    const byId = new Map<string, SystemGroup>();
    for (const { s } of order) {
      const dc = (s.elements as Array<Record<string, unknown>>).find(e => e['type'] === 'collector');
      byId.set(s.id, {
        id: s.id,
        name: (dc?.['name'] as string) || (s.name as string) || 'Dust collector',
        tools: [], on: false, coasting: false, activeName: '',
      });
    }

    // Machines with no port anywhere. A layout fault the readiness banner already
    // explains — but they were visible in the flat list this replaces, and a tool
    // that silently vanishes off the page is worse than one shown as unable to
    // collect.
    const orphans: SystemGroup = {
      id: '', name: 'Not connected to a collector — finish the layout to run these.',
      tools: [], on: false, coasting: false, activeName: '',
    };

    for (const t of this.tools) {
      const primary = primaryPortOf(doc, t.id);
      const sysId = primary
        ? portsOf(doc, t.id).find(p => p.port === primary)?.systemId
        : undefined;
      (byId.get(sysId ?? '') ?? orphans).tools.push(t);
    }

    const groups = order.map(({ s }) => byId.get(s.id) as SystemGroup);
    if (orphans.tools.length) groups.push(orphans);
    return groups;
  }
}
