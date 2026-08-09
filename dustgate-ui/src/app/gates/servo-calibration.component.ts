import { Component, EventEmitter, Input, OnInit, Output } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ApiService } from '../services/api.service';
import type { Topology } from '@topology';
import {
  ServoSelector, ServoState,
  absoluteAngles, applyAbsoluteAngles, isCalibrated,
  positionLabels, positionName,
} from './selector-types';

// ── Servo gate calibration ───────────────────────────────────────────────────
// Teaching a ball valve or manifold where its positions actually are.
//
// The sliding gate gets this from a reference sweep between two endstops. A servo
// valve has no endstops to sweep — just hard stops we must NOT drive into (a
// clutchless servo stalls; see the mechanical notes in docs/v2-topology-schema.md).
// So it's done by eye: nudge the valve, watch the handle, capture where it lands.
//
// Two things make that work for someone standing at the gate with a phone:
//
//   1. No degrees anywhere. The angles are an implementation detail of the schema;
//      what the user has is a handle and their eyes. Steps are coarse/fine arrows.
//   2. A fixed handedness. The servo can be mounted behind the gate, which
//      mirrors every arrow — but that is a property of how the BUILD mounts its
//      servos, not of each individual gate, and in this build it is not mirrored.
//      So it's SERVO_HANDED_REVERSED below rather than a question at the top of
//      every calibration: the nudge-and-confirm step cost two taps per gate to
//      re-derive an answer that never varies.
//
// Captured angles are absolute; applyAbsoluteAngles folds them back into the
// schema's referenceAngle + per-state offsetDeg on save.

/**
 * Is the servo mounted BEHIND the valve on this build, mirroring the arrows?
 *
 * Compile-time, deliberately. `servo.reversed` still exists in the schema and a
 * value already saved there still wins, so a mixed shop stays possible — but
 * nothing writes it any more, and new gates take this default.
 */
const SERVO_HANDED_REVERSED = false;

const COARSE_DEG = 15;
const FINE_DEG = 3;
/** How close to 0°/180° counts as "running out of travel" — worth warning about. */
const RAIL_WARN_DEG = 10;

type Phase = 'capture' | 'review';

@Component({
  selector: 'app-servo-calibration',
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

    .dial { display: flex; justify-content: center; margin: 4px 0 16px; }

    .pad { display: grid; grid-template-columns: repeat(4, 1fr); gap: 9px; margin-bottom: 15px; }
    .pad button { background: var(--bg); border: 1px solid var(--border); color: var(--text);
                  border-radius: 12px; padding: 15px 0; font-size: 20px; line-height: 1; }
    .pad button:disabled { opacity: 0.35; }

    .cap { display: block; width: 100%; background: var(--accent); border: none; color: #1a1200;
           font-weight: 600; border-radius: var(--radius); padding: 13px; font-size: 15px; }
    .cap:disabled { opacity: 0.5; }
    .warn { font-size: 12px; color: var(--accent); text-align: center; margin: 9px 0 0; }
    .err  { font-size: 12.5px; color: var(--danger); margin: 10px 0 0; line-height: 1.5; }

    .row { display: flex; align-items: center; gap: 10px; padding: 9px 0; border-bottom: 1px solid var(--border); }
    .row:last-of-type { border-bottom: none; }
    .row .tick { color: var(--success); font-size: 15px; width: 16px; }
    .row .pn { font-size: 14px; min-width: 60px; }
    .row .pd { font-size: 12.5px; color: var(--muted); flex: 1; }
    .row button { background: var(--surface); border: 1px solid var(--border); color: var(--muted);
                  border-radius: 8px; padding: 6px 13px; font-size: 12.5px; }

    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .next { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
    .nav .next:disabled { opacity: 0.5; }
  `],
  template: `
    <div class="card">
      <!-- ── Phase 2: capture each position ──────────────────────────────── -->
      <ng-container *ngIf="phase === 'capture' && current() as st">
        <div class="dots">
          <i *ngFor="let s of sel.states; let i = index" [class.done]="i < index" [class.cur]="i === index"></i>
          <span class="n">{{ index + 1 }} of {{ sel.states.length }}</span>
        </div>

        <h3>{{ label(st) }}</h3>
        <p class="sub">{{ instruction(st) }}</p>

        <div class="dial">
          <svg width="150" height="132" viewBox="0 0 150 132" role="img"
               [attr.aria-label]="'Handle position for ' + label(st)">
            <circle cx="75" cy="70" r="52" fill="none" stroke="var(--border)"/>
            <circle *ngFor="let d of capturedDisplayAngles()" [attr.cx]="75 + 52 * sinDeg(d)"
                    [attr.cy]="70 - 52 * cosDeg(d)" r="3" fill="var(--success)"/>
            <g [attr.transform]="'rotate(' + displayAngle() + ' 75 70)'">
              <rect x="71" y="29" width="8" height="45" rx="4" fill="var(--accent)"/>
              <circle cx="75" cy="70" r="11" fill="none" stroke="var(--accent)" stroke-width="3"/>
            </g>
            <text x="75" y="128" text-anchor="middle" font-size="11" fill="var(--muted)">handle, as you see it</text>
          </svg>
        </div>

        <div class="pad">
          <button (click)="jog(-1, true)"  [disabled]="!canJog(-1)" aria-label="turn left, coarse">≪</button>
          <button (click)="jog(-1, false)" [disabled]="!canJog(-1)" aria-label="turn left, fine">‹</button>
          <button (click)="jog(1, false)"  [disabled]="!canJog(1)"  aria-label="turn right, fine">›</button>
          <button (click)="jog(1, true)"   [disabled]="!canJog(1)"  aria-label="turn right, coarse">≫</button>
        </div>

        <button class="cap" (click)="capture()" [disabled]="busy || !!jogError">
          Capture {{ label(st).toLowerCase() }}
        </button>
        <p class="warn" *ngIf="nearRail() && !jogError">Near the end of its travel — nudge gently.</p>
        <p class="err" *ngIf="jogError">{{ jogError }}</p>

        <div class="nav">
          <button class="back" (click)="stepBack()">← Back</button>
        </div>
      </ng-container>

      <!-- ── Phase 3: review ─────────────────────────────────────────────── -->
      <ng-container *ngIf="phase === 'review'">
        <h3>{{ sel.name || 'This gate' }}</h3>
        <p class="sub">Tap Test to send it back to a position and check it before saving.</p>
        <div class="row" *ngFor="let s of sel.states">
          <span class="tick">✓</span>
          <span class="pn">{{ label(s) }}</span>
          <span class="pd">{{ detail(s) }}</span>
          <button (click)="test(s)" [disabled]="busy || !!jogError">Test</button>
        </div>
        <p class="err" *ngIf="saveError">{{ saveError }}</p>
        <p class="err" *ngIf="jogError">{{ jogError }}</p>
        <div class="nav">
          <button class="back" (click)="redo()">Redo</button>
          <button class="next" (click)="finish()" [disabled]="busy">Save</button>
        </div>
      </ng-container>
    </div>
  `,
})
export class ServoCalibrationComponent implements OnInit {
  @Input({ required: true }) sel!: ServoSelector;
  @Input({ required: true }) topo!: Topology;
  /** Emits a COPY of the selector with referenceAngle + offsets rewritten. */
  @Output() saved = new EventEmitter<ServoSelector>();
  @Output() cancelled = new EventEmitter<void>();

  phase: Phase = 'capture';
  index = 0;
  busy = false;
  jogError = '';
  saveError = '';

  /** stateId → absolute servo angle. The live value while jogging, and what gets folded
   *  back into referenceAngle + offsets on save. */
  private angles = new Map<string, number>();
  private labels = new Map<string, string>();
  private reversed = false;

  constructor(private api: ApiService) {}

  ngOnInit(): void {
    this.labels = positionLabels(this.topo, this.sel);
    this.angles = absoluteAngles(this.sel);
    // A value saved by an older build still wins; otherwise the build-wide default.
    this.reversed = this.sel.servo?.reversed ?? SERVO_HANDED_REVERSED;
    // Move the valve to where the dial claims it is, so the two agree from the
    // first tap rather than after the first nudge.
    if (isCalibrated(this.sel)) void this.drive(this.angle());
  }

  // ── phase 2: capture ──────────────────────────────────────────────────────
  current(): ServoState | null { return this.sel.states[this.index] ?? null; }

  label(s: ServoState): string { return positionName(s.id); }
  detail(s: ServoState): string { return this.labels.get(s.id) ?? ''; }

  private static readonly SEAT = 'Stop as soon as it seats — don\'t push it into the hard stop.';

  instruction(s: ServoState): string {
    if (s.isClosed) {
      return this.sel.kind === 'servoGate'
        ? `Turn until the valve is fully shut. ${ServoCalibrationComponent.SEAT}`
        : `Turn until the valve seals both outlets. ${ServoCalibrationComponent.SEAT}`;
    }
    // Name the destination when there is one; an outlet with nothing on it yet still
    // needs its angle, so fall back to the position rather than a blank.
    const what = this.labels.get(s.id);
    const where = what && what !== 'capped' ? `open to ${what}` : `fully open ${this.positionWord(s)}`;
    return `Turn until the valve is ${where}. ${ServoCalibrationComponent.SEAT}`;
  }

  /** "to the left" / "to the right" for a manifold, plain "open" for a binary gate. */
  private positionWord(s: ServoState): string {
    return this.sel.kind === 'servoManifold' ? `to the ${s.id}` : '';
  }

  /** ui direction (+1 right / −1 left) → the servo delta that achieves it. */
  private servoDelta(dir: number, coarse: boolean): number {
    const step = coarse ? COARSE_DEG : FINE_DEG;
    return (this.reversed ? -dir : dir) * step;
  }

  canJog(dir: number): boolean {
    if (this.busy || this.jogError) return false;
    const to = this.angle() + this.servoDelta(dir, false);
    return to >= 0 && to <= 180;
  }

  async jog(dir: number, coarse: boolean): Promise<void> {
    const to = this.angle() + this.servoDelta(dir, coarse);
    const clamped = Math.min(180, Math.max(0, to));
    if (clamped === this.angle()) return;
    if (await this.drive(clamped)) this.setAngle(clamped);
  }

  async capture(): Promise<void> {
    if (!this.current()) return;
    if (this.index >= this.sel.states.length - 1) { this.phase = 'review'; return; }
    this.index++;
    // Take the valve TOWARD the next position rather than leaving it parked at the one
    // just captured — otherwise the dial would show a position the ball isn't at.
    await this.drive(this.angle());
  }

  async stepBack(): Promise<void> {
    if (this.index > 0) { this.index--; await this.drive(this.angle()); return; }
    this.cancelled.emit();   // backing out of the first position leaves the widget
  }

  nearRail(): boolean {
    const a = this.angle();
    return a <= RAIL_WARN_DEG || a >= 180 - RAIL_WARN_DEG;
  }

  // ── the dial: servo angle → what the user sees ────────────────────────────
  /** Mirrored when the servo is behind the gate, so a rightward tap always rotates
   *  the drawn handle rightward. */
  displayAngle(): number { return this.reversed ? 180 - this.angle() : this.angle(); }

  capturedDisplayAngles(): number[] {
    return this.sel.states.slice(0, this.index).map((s) => {
      const a = this.angles.get(s.id) ?? 0;
      return this.reversed ? 180 - a : a;
    });
  }

  sinDeg(d: number): number { return Math.sin((d * Math.PI) / 180); }
  cosDeg(d: number): number { return Math.cos((d * Math.PI) / 180); }

  // ── phase 3: review ───────────────────────────────────────────────────────
  async test(s: ServoState): Promise<void> {
    const a = this.angles.get(s.id);
    if (typeof a === 'number') await this.drive(a);
  }

  redo(): void { this.phase = 'capture'; this.index = 0; this.saveError = ''; }

  finish(): void {
    const res = applyAbsoluteAngles(this.sel, this.angles);
    if (!res.ok || !res.selector) { this.saveError = res.error ?? 'could not save these positions'; return; }
    this.saved.emit(res.selector);
  }

  // ── driving the servo ─────────────────────────────────────────────────────
  private angle(): number {
    return this.angles.get(this.current()?.id ?? this.sel.states[0]?.id ?? '') ?? 0;
  }

  private setAngle(a: number): void {
    const st = this.current() ?? this.sel.states[0];
    if (st) this.angles.set(st.id, a);
  }

  /** Send one absolute angle. Returns false (and latches a message) if the device
   *  can't do it — better a stuck control with an explanation than arrows that look
   *  like they're working while nothing moves. */
  private async drive(angle: number): Promise<boolean> {
    if (this.busy) return false;
    this.busy = true;
    try {
      // controllerId comes from the selector being edited, which the board picker
      // above mutates live — so re-assigning a gate to a node re-points the jog
      // arrows at that node immediately, without a save/reload in between.
      await this.api.jogServo(this.sel.servo?.channel ?? 0, Math.round(angle), this.sel.controllerId);
      this.jogError = '';
      return true;
    } catch (e: unknown) {
      const status = (e as { status?: number })?.status;
      this.jogError = status === 501
        ? 'This board was built without servo support, so nothing will move. Reflash with servos enabled to calibrate.'
        : status === 404
          ? 'This device doesn\'t have the servo jog endpoint yet — update its firmware to calibrate.'
          : 'Couldn\'t reach the gate. Check it\'s powered and on the network.';
      return false;
    } finally {
      this.busy = false;
    }
  }
}
