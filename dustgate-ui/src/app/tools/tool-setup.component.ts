import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { ApiService, DiscoveredOutlet, Topology } from '../services/api.service';
import { PairedOutletRowComponent } from './paired-outlet-row.component';
import { elementsOf, ductsOf } from '../gates/selector-types';
import { type ShopDoc, machineOfPort, outletOf, renameMachine, toShop } from '../services/shop-doc';

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
  /** Cached display name for the plug — see PairedOutletRowComponent. */
  label: string;
  thresholdW: number;
}

const DEFAULT_THRESHOLD = 50;

@Component({
  selector: 'app-tool-setup',
  standalone: true,
  imports: [CommonModule, FormsModule, PairedOutletRowComponent],
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
      <span class="title">{{ editing ? editingName : 'Tools' }}</span>
    </div>

    <!-- ── The list ─────────────────────────────────────────────────────────── -->
    <ng-container *ngIf="!editing">
      <p class="hint-line">
        Tap a tool to say which smart outlet it's plugged into, and how much power means
        it's running. A tool without a plug is one you switch on yourself from the shop list.
      </p>

      <div class="list" *ngIf="tools.length">
        <button class="row" *ngFor="let t of tools" (click)="configure(t)">
          <div class="grow">
            <div class="nm">{{ t.name }}</div>
            <div class="sub">{{ t.gateLabel }}</div>
            <span class="pill ok"    *ngIf="t.hasPlug && t.ip">{{ plugPill(t) }}</span>
            <span class="pill todo"  *ngIf="t.hasPlug && !t.ip">No plug paired yet</span>
            <span class="pill plain" *ngIf="!t.hasPlug">Switched on by hand</span>
          </div>
          <svg class="chev" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
               stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
            <path d="M9 18l6-6-6-6"/>
          </svg>
        </button>
      </div>

      <div class="empty" *ngIf="!tools.length">No tools to set up yet. Draw your plumbing first.</div>
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
                                 (change)="unpair(c, '')"
                                 (removed)="unpair(c, $event)">
          </app-paired-outlet-row>

          <div class="hint" *ngIf="unpairNote">{{ unpairNote }}</div>
          <div class="hint" *ngIf="!c.ip">
            <span>💡</span><span>Turn {{ c.name }} on — the plug that jumps to <b>green</b> is the one.</span>
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
              <div class="empty">{{ scanning ? 'Scanning…' : 'No plugs found on the network.' }}</div>
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
  tools: ToolCfg[] = [];
  /** The tool being edited — a WORKING COPY, so Cancel leaves the list untouched. */
  editing: ToolCfg | null = null;
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

  constructor(private api: ApiService, private router: Router) {}

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
    if (this.tools.length) void this.scan();
  }

  private rebuild(): void {
    this.tools = this.elems().filter(e => e['type'] === 'tool').map(e => this.toCfg(e));
  }

  /** What the list says about a paired tool: the plug's name, and its threshold. */
  plugPill(t: ToolCfg): string {
    const who = t.label || t.hostname || t.ip;
    return `${who} · above ${t.thresholdW} W`;
  }

  configure(t: ToolCfg): void {
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

  /** Compared by id, not by object: `c` is a working copy of one of `tools`. */
  isAssignedElsewhere(ip: string, c: ToolCfg): boolean {
    return this.tools.some(t => t.id !== c.id && t.hasPlug && t.ip === ip);
  }

  /** This tool's plug as the last scan saw it — carries who owns it, which is
   *  what decides whether the row offers a rename. */
  seenOutlet(c: ToolCfg): DiscoveredOutlet | null {
    return this.outlets.find(o => o.ip === c.ip) ?? null;
  }

  /** Take the plug off this tool, locally. The device half already ran inside the
   *  row; this drops it from the working copy so the scan list comes back and the
   *  plug is pickable for something else. Written out on Save. */
  unpair(c: ToolCfg, note: string): void {
    c.ip = ''; c.hostname = ''; c.label = '';
    this.unpairNote = note;
    this.touched = true;
    void this.scan();   // the freed plug should reappear as available
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
    const base = "Leave without saving?\n\nThis tool's name, its plug and its trip point go back to what they were.";
    return this.plugRenamed
      ? `${base}\n\nThe outlet's own name is not part of that — that rename already went to the plug.`
      : base;
  }

  /** The plug's new name, already written to the plug itself. Cached on the
   *  layout by Save, as a fallback for when the plug can't be reached. */
  onRenamed(c: ToolCfg, label: string): void {
    c.label = label;
    this.plugRenamed = true;
    this.touched = true;
  }

  /** Back out of a tool, or off the screen entirely — same as Gates. */
  back(): void {
    if (this.editing) { this.cancel(); return; }
    void this.router.navigate(['/']);
  }

  /** Write the one tool being edited, then return to the list. */
  async save(): Promise<void> {
    const c = this.editing;
    if (!this.topo || !c || this.saving) return;
    this.saving = true;
    this.error = '';
    try {
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
      } else {
        delete m.sensor;
      }
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
