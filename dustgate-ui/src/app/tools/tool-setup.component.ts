import { Component, OnInit, ViewChild } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ActivatedRoute, Router } from '@angular/router';
import { ApiService, DiscoveredOutlet, Topology } from '../services/api.service';
import { ElementOutletConfigComponent } from './element-outlet-config.component';
import { PairedOutletRowComponent } from './paired-outlet-row.component';
import { resolveDeepLink } from './deep-link';
import { elementsOf, ductsOf } from '../gates/selector-types';
import { type ShopDoc, type RawEl as DocEl, collectorOf, isPortSupplemental, machineOfPort,
         outletExcludes, outletOf, outletTakenByAnotherMachine, renameMachine, systemLabel,
         systemsInLayoutOrder, systemsOf, toShop } from '../services/shop-doc';

// ── The tools screen ─────────────────────────────────────────────────────────
// Every tool in the shop in one list, each tappable into its own tagging page:
// smart outlet or manual, and if smart, which plug + at what threshold. The
// trick for finding the plug is identify-by-power — switch the tool on and
// watch which one jumps to green. Writes `sensor.outlet` onto the tool's
// machine.
//
// This was a WIZARD until 2026-08-22: tool 1 of N, Next, Next, Finish, with
// every edit held in memory until the last screen. It shares its shape with
// /gates now, and for the same reasons (gate-list.component.ts has the long
// version). The short one: coming back to change the plug on the bandsaw meant
// walking past every other tool to reach it, and a walk-out anywhere along the
// way threw away the whole session's work. A list answers "which tools still
// have no plug" at a glance, which the wizard could only answer by walking it,
// and each tool now saves on its own Save.
//
// SPLIT BY SYSTEM, and each system's COLLECTOR heads its own group (2026-08-25).
// Two reasons, and only the second is about tidiness. The first: a collector's
// outlet could be paired nowhere but the build canvas, so the shop page's "No
// outlet paired — there is nothing to switch" sent you to a layout tool to fix a
// wiring fact. It links here now (`?el=<id>`, see deep-link.ts), and the row it
// lands on has to exist. The second: which blower a tool breathes through is the
// most useful thing to know about it, and a flat list threw that away — the same
// argument the shop page made when it grouped.

interface RawEl { [k: string]: unknown; }
interface Branch { id: string; }

interface ToolCfg {
  id: string;
  /** The machine this port belongs to — what the shop page's links are keyed by. */
  machineId: string;
  /** A machine-id link opens the PRIMARY port; see deep-link.ts. */
  primary: boolean;
  name: string;
  gateLabel: string;
  hasPlug: boolean;
  ip: string;
  gen: number;
  hostname: string;
  /** Cached display name for the plug — see PairedOutletRowComponent. */
  label: string;
  thresholdW: number;
}

/**
 * One system's collector, as a list row.
 *
 * No threshold, because there is nothing to threshold: a collector's outlet is
 * COMMANDED and never read (SmartOutletControl.h), which is the same asymmetry
 * the pairing sheet's two modes exist for.
 */
interface CollectorCfg {
  id: string;
  systemId: string;
  name: string;
  ip: string;
  hostname: string;
  /** Cached display name for the outlet — see PairedOutletRowComponent. */
  label: string;
}

/** One airflow system: its blower, then the tools that breathe through it. */
interface SysGroup {
  id: string;
  name: string;
  collector: CollectorCfg | null;
  tools: ToolCfg[];
}

const DEFAULT_THRESHOLD = 50;

@Component({
  selector: 'app-tool-setup',
  standalone: true,
  imports: [CommonModule, FormsModule, ElementOutletConfigComponent, PairedOutletRowComponent],
  styles: [`
    :host { display: block; max-width: 460px; margin: 0 auto; padding: 16px 14px 40px; }


    .head { display: flex; align-items: center; gap: 10px; margin-bottom: 14px; }
    /* Same round control Settings and Gates use, so "leave this screen" looks
       the same wherever it appears. */
    .back-btn { background: var(--surface); border: 1px solid var(--border); border-radius: 50%;
                width: 34px; height: 34px; flex-shrink: 0; display: flex; align-items: center;
                justify-content: center; color: var(--muted); }
    .back-btn:active { opacity: 0.6; }
    .title { font-size: 17px; font-weight: 600; }
    .hint-line { font-size: 12px; color: var(--muted); line-height: 1.6; margin: 0 2px 12px; }

    /* ── the list ── */
    /* One block per system, separated by space rather than a rule: the system
       name above each block is already a boundary, and a divider on top of it is
       one too many. Same call the shop page made. */
    .sys + .sys { margin-top: 22px; }
    .syslabel { font-size: 11px; color: var(--muted); letter-spacing: 0.07em;
                text-transform: uppercase; padding: 0 8px 7px; }
    .list { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; }
    .row { display: flex; align-items: center; gap: 10px; padding: 13px 14px; width: 100%;
           text-align: left; background: none; border: 0; border-bottom: 1px solid var(--border);
           color: var(--text); }
    .row:last-child { border-bottom: 0; }
    .row:active { background: var(--bg); }
    .grow { flex: 1; min-width: 0; }
    .nm { font-size: 15.5px; font-weight: 500; }
    .sub { font-size: 12.5px; color: var(--muted); margin-top: 2px; }
    .pill { font-size: 11px; padding: 2px 9px; border-radius: 20px; display: inline-block; margin-top: 5px; }
    .pill.ok    { color: var(--success); background: rgba(60,190,110,0.12); }
    .pill.todo  { color: var(--accent);  background: rgba(240,165,0,0.12); }
    .pill.plain { color: var(--muted);   background: rgba(128,128,128,0.12); }
    .chev { color: var(--muted); flex-shrink: 0; }

    /* The collector's row. It gets the cyclone glyph the shop page's card uses —
       the ONLY thing separating it from a tool row at a glance, since a blower
       named "Cyclone" and a tool named "Cyclone" read identically otherwise. */
    .row .cyc { width: 30px; height: 30px; border-radius: 50%; flex-shrink: 0;
                display: flex; align-items: center; justify-content: center;
                background: var(--bg); border: 1px solid var(--border); color: var(--muted); }
    .row .cyc svg { width: 17px; height: 17px; }

    /* ── one tool ── */
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; padding: 18px 16px; }
    .toolrow { display: flex; align-items: center; gap: 11px; margin-bottom: 16px; }
    .toolrow .ic { width: 42px; height: 42px; border-radius: 11px; background: var(--bg); border: 1px solid var(--border); display: flex; align-items: center; justify-content: center; }
    .toolrow .ic svg { width: 22px; height: 22px; stroke: var(--muted); }
    .fld { display: block; font-size: 11px; letter-spacing: 0.07em; text-transform: uppercase;
           color: var(--muted); margin-bottom: 4px; }
    app-paired-outlet-row { display: block; margin-bottom: 12px; }
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
    .nav .cancel { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .save { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
    .nav .save:disabled { opacity: 0.5; }

    .err { color: var(--danger); font-size: 12.5px; margin: 12px 2px 0; }
  `],
  template: `
    <!-- OUTSIDE the tools check, deliberately. With no tools drawn yet this screen
         was a dead end: an empty-state sentence and nothing to press, on a page
         reachable straight from the landing screen. The way out must not depend
         on there being something to set up. -->
    <div class="head">
      <button class="back-btn" (click)="back()" aria-label="Back">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
             stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M19 12H5M12 19l-7-7 7-7"/>
        </svg>
      </button>
      <span class="title">{{ editing || collectorEl ? editingName : 'Tools' }}</span>
    </div>


    <!-- ── The list ─────────────────────────────────────────────────────────── -->
    <ng-container *ngIf="!editing && !collectorEl">
      <p class="hint-line">
        Tap a tool to say which smart outlet it's plugged into, and how much power means
        it's running. A tool with no outlet is one you switch on yourself from the shop list.
      </p>

      <div class="sys" *ngFor="let g of groups">
        <!-- Named only when there is more than one. On a one-system shop the
             collector row at the top of the list already says whose tools these
             are, and a header repeating it is a line of noise. -->
        <div class="syslabel" *ngIf="groups.length > 1">{{ g.name }}</div>
        <div class="list">
          <button class="row" *ngIf="g.collector as dc" (click)="configureCollector(dc)">
            <span class="cyc">
              <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"
                   stroke-linecap="round" stroke-linejoin="round">
                <path d="M13 5a7 7 0 1 0 6 7"/><path d="M12 8a4 4 0 1 1-4 4"/>
                <circle cx="12" cy="12" r="1.2" fill="currentColor" stroke="none"/>
              </svg>
            </span>
            <div class="grow">
              <div class="nm">{{ dc.name }}</div>
              <div class="sub">Dust collector</div>
              <span class="pill ok"   *ngIf="dc.ip">{{ collectorPill(dc) }}</span>
              <!-- Orange where an unpaired TOOL is grey, and the difference is
                   real: a tool with no outlet is one you hand-run from the shop
                   list, which works. A blower with no outlet is one nothing on
                   that page can switch on at all. The shop card says the same in
                   the same colour. -->
              <span class="pill todo" *ngIf="!dc.ip">No outlet paired</span>
            </div>
            <svg class="chev" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
                 stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M9 18l6-6-6-6"/>
            </svg>
          </button>

          <button class="row" *ngFor="let t of g.tools" (click)="configure(t)">
            <div class="grow">
              <div class="nm">{{ t.name }}</div>
              <div class="sub">{{ t.gateLabel }}</div>
              <span class="pill ok"    *ngIf="t.hasPlug && t.ip">{{ plugPill(t) }}</span>
              <span class="pill todo"  *ngIf="t.hasPlug && !t.ip">No outlet paired yet</span>
              <span class="pill plain" *ngIf="!t.hasPlug">Switched on by hand</span>
            </div>
            <svg class="chev" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
                 stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M9 18l6-6-6-6"/>
            </svg>
          </button>

          <div class="empty" *ngIf="!g.tools.length">Nothing plumbed into this one yet.</div>
        </div>
      </div>

      <div class="empty" *ngIf="!anythingToSetUp">No tools to set up yet. Draw your plumbing first.</div>
      <p class="err" *ngIf="error">{{ error }}</p>
    </ng-container>

    <!-- ── One collector ────────────────────────────────────────────────────── -->
    <!-- The same sheet the build canvas opens, in its 'switch' role — a collector's
         outlet is commanded, never sensed, so there is no threshold on it. Reused
         rather than rebuilt inline: two pairing panels for the same job is exactly
         the drift this repo spends its comments on. -->
    <ng-container *ngIf="collectorEl as el">
      <app-element-outlet-config [element]="el" mode="switch"
                                 [excludeIps]="collectorExcludeIps"
                                 [excludeReason]="collectorExcludeReason"
                                 [outlets]="outlets" [owner]="owner"
                                 (saved)="saveCollector($event)"
                                 (cancelled)="cancelCollector()"
                                 (note)="unpairNote = $event"
                                 (rescan)="scan()">
      </app-element-outlet-config>
      <div class="hint" *ngIf="unpairNote" style="margin-top:12px">{{ unpairNote }}</div>
      <p class="err" *ngIf="error">{{ error }}</p>
    </ng-container>

    <!-- ── One tool ─────────────────────────────────────────────────────────── -->
    <ng-container *ngIf="editing as c">

      <div class="card">
        <!-- Labelled, because the plug's name field below is the same control at
             the same size and nothing else says which name you are editing. -->
        <div class="toolrow">
          <span class="ic"><svg viewBox="0 0 24 24" fill="none" stroke-width="1.6" stroke-linecap="round"><rect x="5" y="7" width="14" height="10" rx="2"/></svg></span>
          <div style="flex:1">
            <label class="fld" for="tool-name">Tool name</label>
            <input id="tool-name" class="name-in" [(ngModel)]="c.name" (ngModelChange)="touched = true"
                   title="What you call this machine in the shop. Not the smart outlet's name."/>
            <div class="gate">{{ c.gateLabel }}</div>
          </div>
        </div>

        <!-- Only while nothing is paired: once there is an outlet, Remove on the
             row below is how you say No. -->
        <div class="q" *ngIf="!c.ip">
          <span>Smart outlet on this tool?</span>
          <div class="yesno">
            <button [class.on]="c.hasPlug" (click)="c.hasPlug = true; touched = true"
                    title="Pair a smart outlet, so DustGate knows when this tool is running">Yes</button>
            <button [class.on]="!c.hasPlug" (click)="c.hasPlug = false; touched = true"
                    title="No smart outlet — you switch this tool on yourself from the shop list">No</button>
          </div>
        </div>

        <ng-container *ngIf="c.hasPlug; else manual">
          <!-- Paired: the scan list collapses to the row, which is where the name
               and the way out live. -->
          <app-paired-outlet-row *ngIf="c.ip"
                                 [toolName]="c.name" [ip]="c.ip" [host]="c.hostname" [label]="c.label"
                                 [seen]="seenOutlet(c)" [owner]="owner"
                                 fieldId="setup-outlet-name"
                                 (renamed)="onRenamed(c, $event)"
                                 (rescan)="scan()"
                                 (changeOutlet)="unpair(c, '')"
                                 (removed)="unpair(c, $event, 'remove')">
          </app-paired-outlet-row>

          <div class="hint" *ngIf="unpairNote">{{ unpairNote }}</div>
          <div class="hint" *ngIf="!c.ip">
            <span>💡</span><span>Turn {{ c.name }} on — the outlet that jumps to <b>green</b> is the one.</span>
          </div>
          <div class="scan" *ngIf="!c.ip && outlets.length; else noplugs">
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
            <ng-container *ngIf="!c.ip">
              <div class="empty">{{ scanning ? 'Scanning…' : 'No outlets found on the network.' }}</div>
              <button class="rescan" (click)="scan()" title="Sweep the network for smart outlets again">↻ Rescan</button>
            </ng-container>
          </ng-template>

          <div class="thresh" *ngIf="c.ip">
            <div class="r">
              <span title="How much power this tool has to draw before DustGate treats it as running">Start collection above</span>
              <b>{{ c.thresholdW }} W</b>
            </div>
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
        <button class="cancel" (click)="cancel()">Cancel</button>
        <button class="save" (click)="save()" [disabled]="saving">
          {{ saving ? 'Saving…' : 'Save' }}
        </button>
      </div>
      <p class="err" *ngIf="error">{{ error }}</p>
    </ng-container>
  `,
})
export class ToolSetupComponent implements OnInit {
  /** One block per system, in document order. */
  groups: SysGroup[] = [];
  /** Every tool row across every system, flat — "is this outlet taken" is a
   *  shop-wide question and was one before the list was grouped. */
  tools: ToolCfg[] = [];
  /** The tool being edited — a WORKING COPY, so Cancel leaves the list untouched. */
  editing: ToolCfg | null = null;
  /** The collector being edited, as a deep copy for the pairing sheet. Same
   *  contract as `editing`: the doc is only touched on Save. */
  collectorEl: DocEl | null = null;
  collectorExcludeIps: string[] = [];
  collectorExcludeReason: Record<string, string> = {};
  private collectorSysId = '';
  editingName = '';
  outlets: DiscoveredOutlet[] = [];

  scanning = false;
  saving = false;
  error = '';
  /** Our mDNS name — the owner suffix stamped on plugs we own. */
  owner = '';
  /** What happened to the plug on the last unpair, when it's worth saying. The
   *  row that reported it is gone by then, so it lands here. */
  unpairNote = '';

  private topo: Topology | null = null;
  /** A rename landed ON THE PLUG during this edit — see discardWarning(). */
  private plugRenamed = false;
  /**
   * Has the user actually changed anything on the tool they're editing? Set by
   * the edit affordances themselves rather than derived by comparing a snapshot
   * — see cancel().
   */
  touched = false;

  constructor(private api: ApiService, private router: Router, private route: ActivatedRoute) {}

  /** Is there anything on this screen at all? A system with a collector and no
   *  tools drawn yet is not empty — its blower still wants an outlet. */
  get anythingToSetUp(): boolean {
    return this.groups.some(g => g.collector || g.tools.length);
  }

  async ngOnInit(): Promise<void> {
    try {
      await this.api.whenReady();          // else the first fetch 401s and reads as "no tools"
      // Migrated on read, like every other entry point — see shop-doc.ts.
      this.topo = toShop(JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology) as unknown as Topology;
      // whenReady() has already fetched /api/info; deviceInfo is the cached copy.
      this.owner = this.api.deviceInfo?.owner ?? '';
    } catch (e) {
      // A device with nothing drawn yet answers 404 to /api/topology. That is an
      // empty shop, not a dead controller, and the empty state below already says
      // the right thing — claiming "couldn't reach" there sends someone hunting a
      // network fault that isn't real.
      if ((e as { status?: number })?.status !== 404) this.error = "Couldn't reach the controller.";
      return;
    }
    this.rebuild();
    // Up front, not on opening a tool: the list itself shows which plug each tool
    // is on, and a scan started here has finished by the time anyone taps a row.
    if (this.anythingToSetUp) void this.scan();
    this.openDeepLink();
  }

  /**
   * `/tools?el=<id>` — open one piece's panel instead of the list.
   *
   * Consumed once and then wiped off the URL, so backing out of the panel leaves
   * you looking at a list whose address says "the list". The id may name a port,
   * a collector, or a MACHINE (which is the vocabulary the shop page links in);
   * resolveDeepLink() is what reconciles those, and is tested on its own.
   */
  private openDeepLink(): void {
    const el = this.route.snapshot.queryParamMap.get('el');
    if (!el) return;
    void this.router.navigate([], { relativeTo: this.route, queryParams: {}, replaceUrl: true });

    const rows = [
      ...this.groups.flatMap(g => g.collector
        ? [{ id: g.collector.id, machineId: '', primary: true, collector: g.collector }]
        : []),
      ...this.tools.map(t => ({ id: t.id, machineId: t.machineId, primary: t.primary, tool: t })),
    ] as Array<{ id: string; machineId: string; primary: boolean; collector?: CollectorCfg; tool?: ToolCfg }>;

    const hit = resolveDeepLink(rows, el);
    if (!hit) return;              // deleted since the link was drawn — the list is the right answer
    if (hit.collector) this.configureCollector(hit.collector);
    else if (hit.tool) this.configure(hit.tool);
  }

  private rebuild(): void {
    const doc = this.topo as unknown as ShopDoc;
    this.groups = systemsInLayoutOrder(doc).map(sys => {
      const dc = collectorOf(sys);
      return {
        id: sys.id,
        name: systemLabel(sys),
        collector: dc ? this.toCollectorCfg(dc, sys.id) : null,
        tools: (sys.elements as DocEl[]).filter(e => e['type'] === 'tool').map(e => this.toCfg(e)),
      };
    });
    this.tools = this.groups.flatMap(g => g.tools);
  }

  private toCollectorCfg(el: DocEl, systemId: string): CollectorCfg {
    const outlet = ((el['control'] as DocEl | undefined)?.['outlet'] as DocEl | undefined) ?? undefined;
    return {
      id: el['id'] as string,
      systemId,
      name: (el['name'] as string) || 'Dust collector',
      ip: (outlet?.['ip'] as string) ?? '',
      hostname: (outlet?.['host'] as string) ?? '',
      label: (outlet?.['name'] as string) ?? '',
    };
  }

  /**
   * What the list says about a paired collector: the outlet's name, and nothing
   * else. The live name first, for the same reason plugPill() prefers it — the
   * cached label is a copy written on Save and goes stale the moment someone
   * renames the outlet in the Shelly app.
   */
  collectorPill(c: CollectorCfg): string {
    return this.outlets.find(o => o.ip === c.ip)?.name || c.label || c.hostname || c.ip;
  }

  /** Open a collector's pairing panel. Edits a deep copy — Cancel must leave the
   *  saved layout untouched, exactly as it does for a tool. */
  configureCollector(c: CollectorCfg): void {
    const doc = this.topo as unknown as ShopDoc;
    const el = collectorOf(systemsOf(doc).find(sys => sys.id === c.systemId));
    if (!el) return;
    this.error = '';
    this.unpairNote = '';
    this.editing = null;
    this.editingName = c.name;
    this.collectorSysId = c.systemId;
    this.collectorEl = JSON.parse(JSON.stringify(el)) as DocEl;
    // Computed on open rather than from the template: a getter would hand the
    // sheet freshly-allocated arrays on every change-detection pass.
    const ex = outletExcludes(doc, c.id);
    this.collectorExcludeIps = ex.ips;
    this.collectorExcludeReason = ex.reason;
  }

  cancelCollector(): void {
    this.collectorEl = null;
    this.unpairNote = '';
  }

  /**
   * Write the collector back.
   *
   * No confirm-on-discard here, unlike a tool. The sheet commits an unpair the
   * moment it happens (the outlet has already been let go on the network, so a
   * layout still claiming it would be the two halves disagreeing), and everything
   * else it can change is one pick — there is no half-typed state to lose.
   */
  async saveCollector(updated: DocEl): Promise<void> {
    if (!this.topo || this.saving) return;
    this.saving = true;
    this.error = '';
    try {
      const sys = systemsOf(this.topo as unknown as ShopDoc).find(x => x.id === this.collectorSysId);
      const els = sys?.elements as DocEl[] | undefined;
      const i = els ? els.findIndex(e => e['id'] === updated['id']) : -1;
      if (!els || i < 0) { this.error = 'That collector is no longer in the layout.'; return; }
      els[i] = updated;
      await this.api.putTopology(this.topo);
      this.collectorEl = null;
      this.rebuild();
    } catch {
      // Same as save(): the doc in memory now disagrees with the device, and
      // re-reading it would throw away the edit. Say so; Save again is the retry.
      this.error = "Couldn't save that collector — is the controller still answering?";
    } finally {
      this.saving = false;
    }
  }

  /** What the list says about a paired tool: the plug's name, and its threshold. */
  /**
   * What the list says about a paired tool: the outlet's name, and its threshold.
   *
   * THE LIVE NAME FIRST. `t.label` is the layout's cached copy, written on Save —
   * so the list showed a factory hostname for an outlet that had been given a
   * friendly name but whose layout had not been saved yet, and it would go on
   * showing a stale name for one renamed in the Shelly app. The scan is what the
   * outlet actually calls itself; the cache is the fallback for when there has
   * been no scan, which is exactly the order the row's own displayName() uses.
   */
  plugPill(t: ToolCfg): string {
    const who = this.seenOutlet(t)?.name || t.label || t.hostname || t.ip;
    return `${who} · above ${t.thresholdW} W`;
  }

  configure(t: ToolCfg): void {
    this.collectorEl = null;
    this.error = '';
    this.unpairNote = '';
    this.touched = false;
    this.plugRenamed = false;
    this.editingName = t.name;
    this.editing = { ...t };   // working copy; the list keeps the saved values
  }

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

  /**
   * Is this outlet spoken for by a DIFFERENT MACHINE?
   *
   * By machine, not by row. This screen lists PORTS — a table saw with a cabinet
   * gate and an overarm is two rows — while the outlet belongs to the machine
   * (one box, one plug; the brain only ever senses machines). Comparing `t.id`,
   * which is a port id, made each of a two-port tool's rows see the other as
   * "another tool", so the saw's own outlet showed as `already assigned to
   * another tool` and `pick()` refused to re-select it.
   *
   * That was not just a wrong label. pick() bails on a blocked outlet, so `c.ip`
   * stayed empty, and save() then read "no outlet" and deleted the pairing —
   * a rename-and-save on a two-port tool silently unpaired it and asked you to
   * choose all over again (reported 2026-08-24).
   */
  isAssignedElsewhere(ip: string, c: ToolCfg): boolean {
    return outletTakenByAnotherMachine(
      this.topo as unknown as ShopDoc, this.tools, ip, c.id);
  }

  /** This tool's plug as the last scan saw it — carries who owns it, which is
   *  what decides whether the row offers a rename. */
  seenOutlet(c: ToolCfg): DiscoveredOutlet | null {
    return this.outlets.find(o => o.ip === c.ip) ?? null;
  }

  /** Take the plug off this tool, locally. The device half already ran inside the
   *  row; this drops it from the working copy so the scan list comes back and the
   *  plug is pickable for something else. Written out on Save. */
  /**
   * Drop the outlet from the working copy.
   *
   * `intent` is the difference between the two callers, and it matters at Save:
   * "change" is mid-swap and must leave `hasPlug` alone, so an unfinished swap
   * keeps the existing pairing; "remove" is the user saying No, and is the only
   * thing that deletes it.
   */
  unpair(c: ToolCfg, note: string, intent: 'change' | 'remove' = 'change'): void {
    c.ip = ''; c.hostname = ''; c.label = '';
    if (intent === 'remove') c.hasPlug = false;
    this.unpairNote = note;
    this.touched = true;
    void this.scan();   // the freed outlet should reappear as available
  }

  pick(d: DiscoveredOutlet, c: ToolCfg): void {
    if (this.isAssignedElsewhere(d.ip, c)) return;
    c.ip = d.ip; c.gen = d.generation || 2; c.hostname = d.hostname;
    c.label = d.name || '';
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

  /**
   * Drop this tool's edits and go back to the list.
   *
   * A tool is written on its own Save, so backing out loses only what was
   * changed on this one tool — but that is still worth a question when there is
   * something to lose. `touched` is set by the edit affordances themselves. The
   * first cut compared a JSON snapshot taken on open, and asked on a tool nobody
   * had typed into: binding a range input writes its value back through ngModel,
   * so the model differs from its own snapshot before the user has done
   * anything. An explicit flag cannot drift that way.
   */
  cancel(): void {
    if (this.touched && !confirm(this.discardWarning())) return;
    this.editing = null;
    this.touched = false;
    this.plugRenamed = false;
  }

  /**
   * What Cancel actually throws away — which is not everything that happened.
   *
   * Renaming the plug is a write to a device on the far side of the LAN and
   * landed the moment you tapped away (see PairedOutletRowComponent); no Cancel
   * here can reach over and undo it. Saying "your name changes will be lost"
   * would be a straight lie about the one edit that already stuck.
   */
  private discardWarning(): string {
    const base = "Leave without saving?\n\nThis tool's name, its outlet and its trip point go back to what they were.";
    return this.plugRenamed
      ? `${base}\n\nThe outlet's own name is not part of that — that rename already went to the outlet.`
      : base;
  }

  /** The outlet's new name, already written to the outlet itself. Cached on the
   *  layout by Save, as a fallback for when the outlet can't be reached. The
   *  scan cache is already current: the row mutates the very entry seenOutlet()
   *  handed it, so what is on the network and what this screen shows cannot
   *  drift apart. */
  onRenamed(c: ToolCfg, label: string): void {
    c.label = label;
    this.plugRenamed = true;
    this.touched = true;
  }

  /** Back out of a tool, or off the screen entirely — same as Gates. */
  back(): void {
    if (this.editing) { this.cancel(); return; }
    if (this.collectorEl) { this.cancelCollector(); return; }
    void this.router.navigate(['/']);
  }

  /** The paired-outlet row, when one is on screen. Only needed so Save can wait
   *  to land the outlet name before the layout is written — see flush(). */
  @ViewChild(PairedOutletRowComponent) private outletRow?: PairedOutletRowComponent;

  /** Write the one tool being edited, then return to the list. */
  async save(): Promise<void> {
    const c = this.editing;
    if (!this.topo || !c || this.saving) return;
    this.saving = true;
    this.error = '';
    try {
      // A rename may still be on the wire. Blur fires BEFORE the click that
      // caused it, so tapping Save straight after typing a name starts that
      // write and lands here while it is still out — and c.label is only set
      // when it comes back. Writing the layout now would store the OLD name
      // beside an outlet that has already taken the new one, which reads on the
      // list as a rename that silently didn't happen.
      await this.outletRow?.flush();
      const el = this.elems().find(e => e['id'] === c.id);
      // The name and the plug both belong to the MACHINE now: this screen is
      // "which plug is this tool on", and a tool is a machine even when it has
      // one port. Writing them to the port would leave the routing brain — which
      // only ever reads machines — sensing nothing.
      const m = el ? machineOfPort(this.topo as unknown as ShopDoc, el) : null;
      if (!m) { this.error = "That tool is no longer in the layout."; return; }
      // Through renameMachine, not m.name — the machine's name is also carried on
      // each of its ports, which is what the canvas draws. Writing one copy is
      // what left the two screens disagreeing (2026-08-22).
      renameMachine(this.topo as unknown as ShopDoc, m.id as string, c.name);
      if (c.hasPlug && c.ip) {
        const outlet: RawEl = { gen: c.gen || 2, ip: c.ip, thresholdW: c.thresholdW || DEFAULT_THRESHOLD };
        if (c.hostname) outlet['host'] = c.hostname;
        if (c.label) outlet['name'] = c.label;
        m.sensor = { outlet };
      } else if (!c.hasPlug) {
        // Said No, or used Remove. An explicit act, so honour it.
        delete m.sensor;
      }
      // The remaining case — wants an outlet, hasn't chosen one — is a swap left
      // half-finished (Change outlet, then Save). Deleting there destroys a
      // working pairing on the strength of an empty field, which is how a
      // blocked pick() turned into a lost outlet. Leaving it alone means Save
      // does nothing to the pairing, and Remove stays the one way to drop it.
      await this.api.putTopology(this.topo);
      this.editing = null;
      this.touched = false;
      this.plugRenamed = false;
      this.rebuild();
    } catch {
      // The doc in memory now disagrees with the device. Re-reading it here would
      // throw away the edit the user just made, so say so and leave the screen as
      // it is — Save again is the retry.
      this.error = "Couldn't save that tool — is the controller still answering?";
    } finally {
      this.saving = false;
    }
  }

  // ── helpers ─────────────────────────────────────────────────────────────────
  // Flattened across systems. The LIST is grouped by system now, but these two
  // answer "what is this port plumbed into", and a secondary port's run may cross
  // the seam — so the lookup has to see the whole shop, not one band of it.
  private elems(): RawEl[] { return this.topo ? elementsOf(this.topo) as unknown as RawEl[] : []; }
  private ducts(): RawEl[] { return this.topo ? ductsOf(this.topo) as unknown as RawEl[] : []; }

  private toCfg(e: RawEl): ToolCfg {
    const outlet = outletOf(this.topo as unknown as ShopDoc, e) ?? undefined;
    const machine = machineOfPort(this.topo as unknown as ShopDoc, e);
    return {
      id: e['id'] as string,
      machineId: (machine?.id as string) ?? '',
      primary: !isPortSupplemental(e),
      name: (machine?.name as string) || (e['name'] as string) || (e['id'] as string),
      gateLabel: this.gateLabel(e['id'] as string),
      // Mirrors what is actually paired. This was hardcoded true, so a tool
      // deliberately set to manual re-opened showing "Yes" with no plug beside
      // it — a toggle stating the opposite of the truth. The cost is one extra
      // tap on a brand-new tool, which is the right trade.
      hasPlug: !!outlet,
      ip: (outlet?.['ip'] as string) ?? '',
      gen: (outlet?.['gen'] as number) ?? 2,
      hostname: (outlet?.['host'] as string) ?? '',
      label: (outlet?.['name'] as string) ?? '',
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
