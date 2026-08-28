import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router } from '@angular/router';
import { ApiService, type NodeLinkState } from '../services/api.service';
import { SelectorConfigComponent } from './selector-config.component';
import type { Topology } from '@topology';
import {
  AnyElement, ConfigurableSelector, commandAngle, elementsOf,
  isCalibrated, isConfigurableSelector, isServoKind, kindLabel, swingSequence,
} from './selector-types';
import { type ShopDoc, systemLabel, systemsInLayoutOrder, toShop } from '../services/shop-doc';

// ── The gates screen ─────────────────────────────────────────────────────────
// Every configured gate in one list, each tappable into the calibration it
// already has. Design and reasoning: docs/mockups/gates-list.html.
//
// /gates existed once and was RETIRED — it was a "set up every gate" pass, and
// the canvas made it redundant by showing which gates are unset and letting you
// tap one. This is not that screen coming back. The canvas is a layout tool you
// open to draw plumbing; this is the errand you run with a wrench in your other
// hand, when a valve got knocked and needs re-teaching. Same calibration
// underneath — SelectorConfigComponent, unchanged — different way in.
//
// A LIST, not a wizard. "Run through all the gates" suggests a march, but
// recalibration is nearly always about one specific gate, and a march makes you
// walk past three working ones to reach it. The list also answers which gates
// still need doing, which a wizard can't.
//
// SPLIT BY SYSTEM (2026-08-25), the same shape /tools took the same day and in
// the same order the canvas draws them (systemsInLayoutOrder). Which blower a
// gate sits under is how you find it on the floor — you are standing at the
// cyclone with a wrench — and a flat list of eight ball valves named after the
// machines below them made you read every one.

/** One row: a gate plus the things this screen needs to decide about it. */
interface GateRow {
  sel: ConfigurableSelector;
  name: string;
  kind: string;
  where: string;
  calibrated: boolean;
  /** Empty when the board is answering; otherwise why it isn't. */
  offline: string;
}

/** One system's gates, under the name the other list screens use for it. */
interface GateGroup {
  id: string;
  name: string;
  rows: GateRow[];
}

@Component({
  selector: 'app-gate-list',
  standalone: true,
  imports: [CommonModule, SelectorConfigComponent],
  styles: [`
    :host { display: block; max-width: 460px; margin: 0 auto; padding: 16px 14px 40px; }
    .head { display: flex; align-items: center; gap: 10px; margin-bottom: 14px; }
    .back-btn { background: var(--surface); border: 1px solid var(--border); border-radius: 50%;
                width: 34px; height: 34px; flex-shrink: 0; display: flex; align-items: center;
                justify-content: center; color: var(--muted); }
    .back-btn:active { opacity: 0.6; }
    .title { font-size: 17px; font-weight: 600; }
    .hint { font-size: 12px; color: var(--muted); line-height: 1.6; margin: 0 2px 12px; }

    /* Space between systems, not a rule — the label above each block is already
       the boundary. Same call /shop and /tools made. */
    .sys + .sys { margin-top: 22px; }
    .syslabel { font-size: 11px; color: var(--muted); letter-spacing: 0.07em;
                text-transform: uppercase; padding: 0 8px 7px; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; }
    .row { display: flex; align-items: center; gap: 10px; padding: 13px 14px;
           border-bottom: 1px solid var(--border); }
    .row:last-child { border-bottom: 0; }
    .grow { flex: 1; min-width: 0; }
    .nm { font-size: 15.5px; font-weight: 500; }
    .sub { font-size: 12.5px; color: var(--muted); margin-top: 2px; }
    .pill { font-size: 11px; padding: 2px 9px; border-radius: 20px; display: inline-block; margin-top: 5px; }
    .pill.ok   { color: var(--success); background: rgba(60,190,110,0.12); }
    .pill.todo { color: var(--accent);  background: rgba(240,165,0,0.12); }
    .pill.dark { color: var(--danger);  background: rgba(217,68,68,0.12); }

    .acts { display: flex; flex-direction: column; gap: 6px; flex-shrink: 0; }
    button.act { border-radius: 8px; padding: 7px 12px; font-size: 12.5px; white-space: nowrap;
                 background: var(--bg); border: 1px solid var(--border); color: var(--text); }
    button.act.warn { border-color: var(--accent); color: var(--accent); }
    button.act:disabled { opacity: 0.45; }

    .err { color: var(--danger); font-size: 12.5px; margin: 12px 2px 0; }
    .empty { color: var(--muted); font-size: 13.5px; padding: 18px 4px; text-align: center; }
  `],
  template: `
    <div class="head">
      <button class="back-btn" (click)="back()" aria-label="Back">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor"
             stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <path d="M19 12H5M12 19l-7-7 7-7"/>
        </svg>
      </button>
      <span class="title">{{ editing ? editingName : 'Gates' }}</span>
    </div>

    <!-- Calibrating one gate: the existing component, unchanged. -->
    <app-selector-config *ngIf="editing && topo"
      [sel]="editing" [topo]="topo"
      (saved)="onConfigured($event)" (cancelled)="editing = null">
    </app-selector-config>

    <ng-container *ngIf="!editing">
      <p class="hint">
        Tap a gate to teach it where its positions are. <b>Test</b> swings it closed, open and
        closed again, using only the positions it already has saved.
      </p>

      <div class="sys" *ngFor="let g of groups">
        <!-- Named only when there is more than one. A one-system shop's gates all
             sit under the same blower, and saying so above every list is a line
             that carries nothing. -->
        <div class="syslabel" *ngIf="groups.length > 1">{{ g.name }}</div>
        <div class="card">
        <div class="row" *ngFor="let r of g.rows">
          <div class="grow">
            <div class="nm">{{ r.name }}</div>
            <div class="sub">{{ r.kind }} · {{ r.where }}</div>
            <span class="pill dark" *ngIf="r.offline">{{ r.offline }}</span>
            <span class="pill ok"   *ngIf="!r.offline && r.calibrated">Calibrated</span>
            <span class="pill todo" *ngIf="!r.offline && !r.calibrated">Never calibrated</span>
          </div>
          <div class="acts">
            <!-- Also out while any gate is swinging: opening the calibrator on a
                 valve that is mid-move puts two things in charge of one servo. -->
            <button class="act" [class.warn]="!r.calibrated" (click)="configure(r)"
                    [disabled]="!!r.offline || !!testing">
              {{ calibrateLabel(r) }}
            </button>
            <!-- Only where there is something saved to drive to. An uncalibrated
                 gate has no known-safe positions, which is the whole reason the
                 test refuses rather than guessing one. -->
            <button class="act" (click)="test(r)"
                    [disabled]="!!r.offline || !r.calibrated || !!testing">
              {{ testing === r.sel.id ? 'Testing…' : 'Test' }}
            </button>
          </div>
        </div>
        <!-- A system with a blower and no gates yet is not the empty screen — it is
             one half of a shop mid-build, and dropping it would leave the other
             system's label looking like the whole story. -->
        <div class="empty" *ngIf="!g.rows.length">No gates on this one yet.</div>
        </div>
      </div>

      <div class="empty" *ngIf="!groups.length && loaded">
        No gates configured yet. Draw your plumbing first.
      </div>
      <p class="err" *ngIf="error">{{ error }}</p>
    </ng-container>
  `,
})
export class GateListComponent implements OnInit {
  /** One block per system, in canvas order. */
  groups: GateGroup[] = [];
  /** Every row, flat — "is any gate swinging" is a shop-wide question. */
  rows: GateRow[] = [];
  topo: Topology | null = null;
  editing: ConfigurableSelector | null = null;
  editingName = '';
  /** Selector id currently being swung, or '' — one at a time, shop-wide. */
  testing = '';
  loaded = false;
  error = '';

  private links: NodeLinkState[] = [];

  constructor(private api: ApiService, private router: Router) {}

  async ngOnInit(): Promise<void> {
    try {
      await this.api.whenReady();
      // Migrated on read, like every other entry point — see shop-doc.ts. This
      // screen read the raw document until 2026-08-25, which was harmless while it
      // only flattened elements and is not once it groups them: a v1 layout has no
      // `systems[]` to group BY.
      this.topo = toShop(await this.api.getTopology()) as unknown as Topology;
      try { this.links = await this.api.getNodes(); } catch { this.links = []; }
    } catch {
      this.error = "Couldn't reach the controller.";
      this.loaded = true;
      return;
    }
    this.rebuild();
    this.loaded = true;
  }

  private rebuild(): void {
    if (!this.topo) { this.groups = []; this.rows = []; return; }
    this.groups = systemsInLayoutOrder(this.topo as unknown as ShopDoc).map((sys) => ({
      id: sys.id,
      name: systemLabel(sys),
      rows: (sys.elements as unknown as AnyElement[])
        .filter(isConfigurableSelector)
        .map((sel) => ({
          sel,
          name: (sel as { name?: string }).name || sel.id,
          kind: kindLabel(sel),
          where: this.where(sel),
          calibrated: isCalibrated(sel),
          offline: this.offlineReason(sel),
        })),
    }));
    this.rows = this.groups.flatMap((g) => g.rows);
  }

  /** Which board drives this gate, in the words the boards screen uses. */
  private where(sel: ConfigurableSelector): string {
    const cid = sel.controllerId ?? '';
    const link = this.links.find((l) => l.id === cid);
    const board = !cid || cid === 'primary' ? 'this board' : (link?.name || cid);
    if (isServoKind(sel)) {
      const ch = sel.servo?.channel;
      return typeof ch === 'number' ? `${board} · channel ${ch + 1}` : board;
    }
    return `${board} · ${sel.states.length} stops`;
  }

  /**
   * A gate on a board that isn't answering is SHOWN and disabled, never hidden —
   * same rule as a claimed board in the boards list. Hiding it makes a gate you
   * configured look like it vanished, and the fix for "gone" is nothing like the
   * fix for "that board is unplugged".
   */
  private offlineReason(sel: ConfigurableSelector): string {
    const cid = sel.controllerId ?? '';
    if (!cid || cid === 'primary') return '';
    const link = this.links.find((l) => l.id === cid);
    if (!link) return 'Board not paired';
    return link.online ? '' : 'Board not answering';
  }

  calibrateLabel(r: GateRow): string {
    // A slider's "calibration" is a reference sweep that re-measures the rail and
    // replaces every stop — a bigger, more disruptive act than nudging one valve,
    // so it does not get called the same thing.
    if (!isServoKind(r.sel)) return 'Run setup again';
    return r.calibrated ? 'Recalibrate' : 'Calibrate';
  }

  configure(r: GateRow): void {
    this.error = '';
    this.editing = r.sel;
    this.editingName = r.name;
  }

  /**
   * Swing a gate closed → open → closed, and stop.
   *
   * ONLY SAVED POSITIONS. Every value sent here comes from the gate's own
   * calibration — commandAngle() for a servo (which is the schema's angle for
   * that state, already clamped to the servo's travel) and a saved stop index for
   * a slider. Nothing is interpolated, derived or nudged: a clutchless servo
   * driven past its hard stop stalls and cooks, and the positions the gate was
   * taught are the only ones known to be reachable.
   *
   * One cycle, not a loop, so it cannot run away while nobody is watching, and
   * one gate at a time shop-wide — the same one-servo-at-a-time current budget
   * the routing runtime keeps.
   */
  async test(r: GateRow): Promise<void> {
    if (this.testing || r.offline || !r.calibrated) return;
    this.error = '';
    this.testing = r.sel.id;
    try {
      // The sequence is decided in selector-types, where it can be tested without
      // a browser — and where the refusal to swing an uncalibrated gate lives.
      const swing = swingSequence(r.sel);
      if (!swing) { this.error = 'This gate has no saved open and closed pair to swing between.'; return; }

      for (const stateId of swing) {
        await this.driveTo(r.sel, stateId);
        await new Promise((res) => setTimeout(res, 900));
      }
    } catch {
      this.error = "Couldn't drive that gate — is its board still answering?";
    } finally {
      this.testing = '';
    }
  }

  private async driveTo(sel: ConfigurableSelector, stateId: string): Promise<void> {
    if (isServoKind(sel)) {
      const angle = commandAngle(sel, stateId);
      // Refuse rather than substitute. A missing angle means the calibration does
      // not cover this state, and picking a plausible one is how a valve gets
      // driven into its stop.
      if (angle === null) throw new Error('no saved angle');
      await this.api.jogServo(sel.servo?.channel ?? 0, Math.round(angle), sel.controllerId);
      return;
    }
    // A slider's stops are numbered by their order in states[], home first — the
    // same numbering the linear calibrator saved them with.
    const stop = sel.states.findIndex((s) => s.id === stateId);
    if (stop < 0) throw new Error('no saved stop');
    if (stop === 0) await this.api.home();
    else await this.api.moveToStop(stop);
  }

  /** Fold the calibrated gate back into the doc and persist it, as the canvas does. */
  async onConfigured(updated: ConfigurableSelector): Promise<void> {
    if (!this.topo) return;
    const els = elementsOf(this.topo);
    const i = els.findIndex((e) => e['id'] === updated.id);
    if (i >= 0) els[i] = updated as unknown as (typeof els)[number];
    this.editing = null;
    try { await this.api.putTopology(this.topo); }
    catch { this.error = "Calibration saved on the gate, but writing the layout failed."; }
    this.rebuild();
  }

  back(): void {
    if (this.editing) { this.editing = null; return; }
    void this.router.navigate(['/shop']);
  }
}
