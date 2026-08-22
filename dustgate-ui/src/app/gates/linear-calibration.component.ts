import { Component, EventEmitter, Input, OnDestroy, OnInit, Output } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Subscription } from 'rxjs';
import { ApiService, SystemStatus } from '../services/api.service';
import type { Topology } from '@topology';
import { LinearSelector, positionLabels } from './selector-types';

// ── Sliding gate calibration ─────────────────────────────────────────────────
// Same physical procedure a slider has always needed — home, confirm which end that
// was, sweep the rail, then place each outlet — writing into the TOPOLOGY element
// (linear.calibration + states[].positionMm).
//
// It drives the hardware over the motion endpoints (/api/home, /api/calibrate,
// /api/jog, /api/setstop), which address the stepper directly rather than by
// selector id. That's unambiguous today: the schema allows at most ONE linear
// selector per controller (MAX_LINEAR_PER_HOST), and only the primary board has a
// stepper wired. A slider hosted on a SECONDARY board would need selector-addressed
// endpoints first — see the firmware staging notes in docs/ui-design.md.
//
// Unlike the servo widget, millimetres ARE shown: a distance along a rail is
// something a woodworker can measure and sanity-check, where a servo angle is an
// implementation detail of the linkage.
//
// The reference sweep also writes the device's own stop table, and capture calls
// /api/setstop, so device and topology stay in step — that's what makes Test work.

const COARSE_MM = 10;
const FINE_MM = 1;

/** Manifold profiles offered during calibration. Mirrors MANIFOLD_PROFILES in device-model.js;
 *  a Rockler ships in 2-gate units, so its outlet count must be even. */
const MANIFOLDS: Array<{ id: string; label: string; pitchMm: number | null; evenOnly: boolean }> = [
  { id: 'rockler-2.5', label: 'Rockler 2½" manifold', pitchMm: 82.9, evenOnly: true },
  { id: 'rockler-4',   label: 'Rockler 4" manifold',  pitchMm: 127,  evenOnly: true },
  { id: 'custom',      label: 'Something else',       pitchMm: null, evenOnly: false },
];

type Phase = 'manifold' | 'home' | 'side' | 'sweep' | 'positions' | 'review';

@Component({
  selector: 'app-linear-calibration',
  standalone: true,
  imports: [CommonModule],
  styles: [`
    :host { display: block; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; padding: 18px 16px; }
    h3 { font-size: 16px; font-weight: 500; margin: 0 0 4px; }
    .sub { font-size: 13px; color: var(--muted); line-height: 1.55; margin: 0 0 16px; }

    .dots { display: flex; gap: 5px; align-items: center; margin-bottom: 14px; }
    .dots i { flex: 1; height: 3px; border-radius: 2px; background: var(--border-strong, #444); }
    .dots i.done { background: var(--success); } .dots i.cur { background: var(--accent); }
    .dots .n { font-size: 12px; color: var(--muted); margin-left: 6px; }

    .opt { display: flex; align-items: center; gap: 11px; width: 100%; text-align: left;
           background: var(--bg); border: 1px solid var(--border); color: var(--text);
           border-radius: 12px; padding: 13px; font-size: 14px; margin-bottom: 8px; }
    .opt.sel { border-color: var(--success); background: rgba(60,190,110,0.08); }
    .opt .os { display: block; font-size: 11.5px; color: var(--muted); margin-top: 2px; }

    .go { display: block; width: 100%; background: var(--accent); border: none; color: #1a1200;
          font-weight: 600; border-radius: var(--radius); padding: 13px; font-size: 15px; }
    .go:disabled { opacity: 0.5; }

    .rail { margin: 6px 0 16px; }
    .rail .bar { position: relative; height: 8px; border-radius: 4px; background: var(--bg);
                 border: 1px solid var(--border); }
    .rail .car { position: absolute; top: -4px; width: 14px; height: 14px; border-radius: 4px;
                 background: var(--accent); transform: translateX(-50%); }
    .rail .pin { position: absolute; top: 12px; width: 2px; height: 7px; background: var(--success);
                 transform: translateX(-50%); }
    .rail .ends { display: flex; justify-content: space-between; font-size: 11px; color: var(--muted);
                  margin-top: 13px; }
    .readout { text-align: center; font-size: 22px; font-weight: 500; font-variant-numeric: tabular-nums;
               color: var(--accent); margin-bottom: 12px; }

    .pad { display: grid; grid-template-columns: repeat(4, 1fr); gap: 9px; margin-bottom: 15px; }
    .pad button { background: var(--bg); border: 1px solid var(--border); color: var(--text);
                  border-radius: 12px; padding: 15px 0; font-size: 20px; line-height: 1; }
    .pad button:disabled { opacity: 0.35; }

    .row { display: flex; align-items: center; gap: 10px; padding: 9px 0; border-bottom: 1px solid var(--border); }
    .row:last-of-type { border-bottom: none; }
    .row .tick { color: var(--success); font-size: 15px; width: 16px; }
    .row .pn { font-size: 14px; min-width: 62px; }
    .row .pd { font-size: 12.5px; color: var(--muted); flex: 1; }
    .row .mm { font-size: 12.5px; color: var(--muted); font-variant-numeric: tabular-nums; }
    .row button { background: var(--surface); border: 1px solid var(--border); color: var(--muted);
                  border-radius: 8px; padding: 6px 13px; font-size: 12.5px; }

    .warn { font-size: 12px; color: var(--accent); margin: 9px 0 0; line-height: 1.5; }
    .err  { font-size: 12.5px; color: var(--danger); margin: 10px 0 0; line-height: 1.5; }
    .busy { text-align: center; font-size: 13px; color: var(--muted); padding: 10px 0; }

    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .next { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
    .nav .next:disabled { opacity: 0.5; }
  `],
  template: `
    <div class="card">
      <!-- ── 1. which manifold ────────────────────────────────────────────── -->
      <ng-container *ngIf="phase === 'manifold'">
        <h3>What's the gate built on?</h3>
        <p class="sub">This sets the spacing the sweep uses to place your {{ outletCount }} outlets.</p>
        <button class="opt" *ngFor="let m of manifolds" [class.sel]="m.id === model" (click)="model = m.id">
          <div style="flex:1">
            {{ m.label }}
            <span class="os">{{ m.pitchMm ? (m.pitchMm + ' mm between outlets') : 'place every outlet by hand' }}</span>
          </div>
          <span *ngIf="m.id === model">✓</span>
        </button>
        <p class="warn" *ngIf="oddWarning">{{ oddWarning }}</p>
        <div class="nav"><button class="next" (click)="phase = 'home'">Next →</button></div>
      </ng-container>

      <!-- ── 2. home ──────────────────────────────────────────────────────── -->
      <ng-container *ngIf="phase === 'home'">
        <h3>Send it home</h3>
        <p class="sub">The gate runs to one end and stops there. Keep clear of the rail.</p>
        <button class="go" (click)="goHome()" [disabled]="busy || homing">
          {{ homing ? 'Homing…' : 'Find home' }}
        </button>
        <p class="err" *ngIf="error">{{ error }}</p>
      </ng-container>

      <!-- ── 3. which end was that ────────────────────────────────────────── -->
      <ng-container *ngIf="phase === 'side'">
        <h3>Which end did it stop at?</h3>
        <p class="sub">Stand where you normally use the gate and tell us what you see.</p>
        <button class="opt" [class.sel]="homedLeft === true" (click)="homedLeft = true">
          <div style="flex:1">The left end<span class="os">as you face the gate</span></div>
          <span *ngIf="homedLeft === true">✓</span>
        </button>
        <button class="opt" [class.sel]="homedLeft === false" (click)="homedLeft = false">
          <div style="flex:1">The right end<span class="os">as you face the gate</span></div>
          <span *ngIf="homedLeft === false">✓</span>
        </button>
        <p class="err" *ngIf="error">{{ error }}</p>
        <div class="nav">
          <button class="next" (click)="confirmSide()" [disabled]="homedLeft === null || busy">Next →</button>
        </div>
      </ng-container>

      <!-- ── 4. sweep ─────────────────────────────────────────────────────── -->
      <ng-container *ngIf="phase === 'sweep'">
        <h3>Measure the rail</h3>
        <p class="sub">
          The gate runs end to end once to measure its travel, then spaces your outlets
          along it. Takes about half a minute.
        </p>
        <button class="go" (click)="sweep()" [disabled]="busy || sweeping">
          {{ sweeping ? 'Measuring…' : 'Measure it' }}
        </button>
        <p class="err" *ngIf="error">{{ error }}</p>
      </ng-container>

      <!-- ── 5. place each outlet ─────────────────────────────────────────── -->
      <ng-container *ngIf="phase === 'positions' && current() as st">
        <div class="dots">
          <i *ngFor="let s of movable; let i = index" [class.done]="i < index" [class.cur]="i === index"></i>
          <span class="n">{{ index + 1 }} of {{ movable.length }}</span>
        </div>

        <h3>Outlet {{ index + 1 }}<span *ngIf="detail(st)" style="font-weight:400"> — {{ detail(st) }}</span></h3>
        <p class="sub">
          The sweep already put it about here. Nudge until the opening lines up with the
          port, then capture it.
        </p>

        <div class="readout">{{ liveMm.toFixed(1) }} mm</div>
        <div class="rail">
          <div class="bar">
            <span class="car" [style.left.%]="pct(liveMm)"></span>
            <span class="pin" *ngFor="let m of capturedMm()" [style.left.%]="pct(m)"></span>
          </div>
          <div class="ends"><span>home</span><span>{{ spanMm.toFixed(0) }} mm</span></div>
        </div>

        <div class="pad">
          <button (click)="jog(-COARSE_MM)" [disabled]="busy" aria-label="back 10 mm">≪</button>
          <button (click)="jog(-FINE_MM)"   [disabled]="busy" aria-label="back 1 mm">‹</button>
          <button (click)="jog(FINE_MM)"    [disabled]="busy" aria-label="forward 1 mm">›</button>
          <button (click)="jog(COARSE_MM)"  [disabled]="busy" aria-label="forward 10 mm">≫</button>
        </div>

        <button class="go" (click)="capture()" [disabled]="busy">Capture this outlet</button>
        <p class="err" *ngIf="error">{{ error }}</p>
        <div class="nav"><button class="back" (click)="stepBack()">← Back</button></div>
      </ng-container>

      <!-- ── 6. review ────────────────────────────────────────────────────── -->
      <ng-container *ngIf="phase === 'review'">
        <h3>{{ sel.name || 'This gate' }}</h3>
        <p class="sub">Tap Test to drive back to an outlet and check it before saving.</p>
        <div class="row" *ngFor="let s of movable; let i = index">
          <span class="tick">✓</span>
          <span class="pn">Outlet {{ i + 1 }}</span>
          <span class="pd">{{ detail(s) }}</span>
          <span class="mm">{{ (positions.get(s.id) ?? 0).toFixed(1) }} mm</span>
          <button (click)="test(i)" [disabled]="busy">Test</button>
        </div>
        <p class="err" *ngIf="error">{{ error }}</p>
        <div class="nav">
          <button class="back" (click)="redo()">Redo</button>
          <button class="next" (click)="finish()" [disabled]="busy">Save</button>
        </div>
      </ng-container>
    </div>
  `,
})
export class LinearCalibrationComponent implements OnInit, OnDestroy {
  @Input({ required: true }) sel!: LinearSelector;
  @Input({ required: true }) topo!: Topology;
  @Output() saved = new EventEmitter<LinearSelector>();
  @Output() cancelled = new EventEmitter<void>();

  readonly manifolds = MANIFOLDS;
  readonly COARSE_MM = COARSE_MM;
  readonly FINE_MM = FINE_MM;

  phase: Phase = 'manifold';
  model = 'rockler-2.5';
  homedLeft: boolean | null = null;
  index = 0;
  busy = false;
  homing = false;
  sweeping = false;
  error = '';
  liveMm = 0;
  spanMm = 1;

  /** The states that actually move — every non-closed state, in order. */
  movable: LinearSelector['states'] = [];
  /** stateId → millimetres from home, as captured. */
  positions = new Map<string, number>();

  private labels = new Map<string, string>();
  private cal: { stepsPerMm: number; measuredSpanSteps: number; homeIsMaxEndstop: boolean } | null = null;
  private sub = new Subscription();

  constructor(private api: ApiService) {}

  ngOnInit(): void {
    this.labels = positionLabels(this.topo, this.sel);
    this.movable = this.sel.states.filter((s) => !s.isClosed);
    this.model = this.sel.linear?.calibration?.manifoldModel ?? 'rockler-2.5';
    for (const s of this.movable) if (typeof s.positionMm === 'number') this.positions.set(s.id, s.positionMm);
    this.spanMm = this.estimateSpan();
    this.sub.add(this.api.status$.subscribe((s) => this.onStatus(s)));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  get outletCount(): number { return this.movable.length; }

  /** A Rockler manifold ships in 2-gate units, so an odd outlet count means one port
   *  won't line up. The canvas is where outlets are added, so say so rather than
   *  silently rounding here. */
  get oddWarning(): string {
    const m = MANIFOLDS.find((x) => x.id === this.model);
    if (!m?.evenOnly || this.outletCount % 2 === 0) return '';
    return `A Rockler manifold comes in pairs, so ${this.outletCount} outlets won't line up. `
      + 'Go back to the layout and add or remove one before setting this up.';
  }

  detail(s: { id: string }): string { return this.labels.get(s.id) ?? ''; }
  current(): LinearSelector['states'][number] | null { return this.movable[this.index] ?? null; }
  capturedMm(): number[] { return this.movable.slice(0, this.index).map((s) => this.positions.get(s.id) ?? 0); }
  pct(mm: number): number { return Math.max(0, Math.min(100, (mm / Math.max(1, this.spanMm)) * 100)); }

  // ── phases 2–4: the physical run ──────────────────────────────────────────
  async goHome(): Promise<void> {
    this.error = ''; this.homing = true;
    try { await this.api.home(); } catch { this.error = this.reach; this.homing = false; }
  }

  async confirmSide(): Promise<void> {
    if (this.homedLeft === null) return;
    this.busy = true; this.error = '';
    try { await this.api.setHomedLeft(this.homedLeft); this.phase = 'sweep'; }
    catch { this.error = this.reach; }
    finally { this.busy = false; }
  }

  async sweep(): Promise<void> {
    this.error = ''; this.sweeping = true;
    try { await this.api.calibrate(this.model, this.outletCount); }
    catch { this.error = this.reach; this.sweeping = false; }
  }

  /** The device is the source of truth for where things are, so the whole widget reads
   *  its live status rather than dead-reckoning from jog commands. */
  private onStatus(s: SystemStatus | null): void {
    if (!s) return;
    if (typeof s.positionMM === 'number') this.liveMm = s.positionMM;

    if (this.homing && s.state !== 'HOMING' && s.homed) {
      this.homing = false;
      this.phase = 'side';
    }

    if (this.sweeping) {
      if (s.state === 'ERROR') {
        this.sweeping = false;
        this.error = 'The gate never reached the far end. Check the far endstop wiring, then measure again.';
      } else if (s.homed && (s.state === 'IDLE' || s.state === 'AT_STOP') && s.stops?.[1]?.mm != null) {
        this.sweeping = false;
        this.onSweepDone(s);
      }
    }
  }

  /** Seed every outlet from where the sweep placed it, and keep what it measured. */
  private onSweepDone(s: SystemStatus): void {
    this.cal = {
      stepsPerMm: s.stepsPerMm ?? 0,
      measuredSpanSteps: s.measuredSpanSteps ?? 0,
      homeIsMaxEndstop: this.homedLeft === false,
    };
    if (this.cal.stepsPerMm > 0 && this.cal.measuredSpanSteps > 0) {
      this.spanMm = this.cal.measuredSpanSteps / this.cal.stepsPerMm;
    }
    this.movable.forEach((st, i) => {
      const mm = parseFloat((s.stops?.[i + 1]?.mm as string | undefined) ?? '');
      this.positions.set(st.id, isNaN(mm) ? 0 : mm);
    });
    this.index = 0;
    this.phase = 'positions';
    void this.driveTo(0);
  }

  // ── phase 5: place each outlet ────────────────────────────────────────────
  async jog(mm: number): Promise<void> {
    if (this.busy) return;
    this.busy = true; this.error = '';
    try { await this.api.jog(mm); }
    catch { this.error = this.reach; }
    finally { this.busy = false; }
  }

  /** Save where the gate physically IS as this outlet — on the device's stop table as
   *  well as ours, so Test and the motion status view agree with the topology. */
  async capture(): Promise<void> {
    const st = this.current();
    if (!st || this.busy) return;
    this.busy = true; this.error = '';
    try {
      await this.api.saveStop(this.index + 1);
      this.positions.set(st.id, this.liveMm);
    } catch (e: unknown) {
      this.error = (e as { error?: { error?: string } })?.error?.error
        ?? 'Couldn\'t save that position — it may be too close to another outlet.';
      this.busy = false;
      return;
    }
    this.busy = false;
    if (this.index >= this.movable.length - 1) { this.phase = 'review'; return; }
    this.index++;
    await this.driveTo(this.index);
  }

  async stepBack(): Promise<void> {
    if (this.index > 0) { this.index--; await this.driveTo(this.index); return; }
    this.phase = 'sweep';
  }

  // ── phase 6: review ───────────────────────────────────────────────────────
  async test(i: number): Promise<void> { await this.driveTo(i); }

  redo(): void { this.index = 0; this.error = ''; this.phase = 'positions'; void this.driveTo(0); }

  finish(): void {
    const cal = this.cal ?? this.sel.linear?.calibration;
    if (!cal || !cal.measuredSpanSteps) {
      this.error = 'Measure the rail before saving — that\'s what tells us how far it can travel.';
      return;
    }
    const states = this.sel.states.map((s) =>
      s.isClosed ? s : { ...s, positionMm: Math.round((this.positions.get(s.id) ?? 0) * 10) / 10 });
    this.saved.emit({
      ...this.sel,
      states,
      linear: {
        ...this.sel.linear,
        calibration: {
          stepsPerMm: cal.stepsPerMm,
          measuredSpanSteps: cal.measuredSpanSteps,
          homeIsMaxEndstop: cal.homeIsMaxEndstop,
          manifoldModel: this.model,
        },
      },
    });
  }

  // ── helpers ───────────────────────────────────────────────────────────────
  private async driveTo(i: number): Promise<void> {
    if (this.busy) return;
    this.busy = true;
    try { await this.api.moveToStop(i + 1); } catch { /* non-fatal: the user can jog */ }
    finally { this.busy = false; }
  }

  /** A rail length to draw against before the sweep has measured one. */
  private estimateSpan(): number {
    const cal = this.sel.linear?.calibration;
    if (cal?.stepsPerMm && cal?.measuredSpanSteps) return cal.measuredSpanSteps / cal.stepsPerMm;
    const furthest = Math.max(0, ...this.movable.map((s) => s.positionMm ?? 0));
    return furthest > 0 ? furthest * 1.1 : 400;
  }

  private readonly reach = 'Couldn\'t reach the gate. Check it\'s powered and on the network.';
}
