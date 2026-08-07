import { Component, EventEmitter, Input, OnInit, Output } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import type { Topology } from '@topology';
import { ServoCalibrationComponent } from './servo-calibration.component';
import { LinearCalibrationComponent } from './linear-calibration.component';
import {
  ConfigurableSelector, Controller, LinearSelector, ServoSelector,
  controllersOf, isCalibrated, isServoKind, kindLabel, servoSelectorsOf,
} from './selector-types';

// ── Configuring one ball valve or manifold ───────────────────────────────────
// Two facts the graph can't infer — what it's called, and which board + PWM channel
// drives it — then the calibration widget. Nothing else: motion settings are either
// unread by firmware (moveMs) or build-wide (holdAtRest), and outlet roles are already
// implied by whatever the canvas connected downstream.
//
// Shared by both entry points: the build canvas inspector and the /gates setup pass.

/** Mirrors SERVO_COUNT in config.h / MAX_SERVOS_PER_HOST in topology.js. */
const SERVO_CHANNELS = [0, 1, 2, 3];

@Component({
  selector: 'app-selector-config',
  standalone: true,
  imports: [CommonModule, FormsModule, ServoCalibrationComponent, LinearCalibrationComponent],
  styles: [`
    :host { display: block; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px;
            padding: 18px 16px; margin-bottom: 12px; }
    .head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 15px; }
    .head .kind { font-size: 12.5px; color: var(--muted); }
    .badge { font-size: 11.5px; padding: 3px 10px; border-radius: 20px; }
    .badge.ok { color: var(--success); background: rgba(60,190,110,0.12); }
    .badge.todo { color: var(--accent); background: rgba(240,165,0,0.12); }

    label { display: block; font-size: 12.5px; color: var(--muted); margin-bottom: 7px; }
    input, select { background: var(--bg); border: 1px solid var(--border); color: var(--text);
                    border-radius: 8px; padding: 9px 10px; font-size: 14px; width: 100%; }
    .field { margin-bottom: 15px; }
    .field:last-child { margin-bottom: 0; }
    .two { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .why { font-size: 11.5px; color: var(--muted); margin-top: 6px; line-height: 1.5; }
  `],
  template: `
    <div class="card">
      <div class="head">
        <span class="kind">{{ kindName }}</span>
        <span class="badge" [class.ok]="calibrated" [class.todo]="!calibrated">
          {{ calibrated ? 'Configured' : 'Needs setup' }}
        </span>
      </div>

      <div class="field">
        <label for="gate-name">Name</label>
        <input id="gate-name" [(ngModel)]="name" (ngModelChange)="touch()"/>
      </div>

      <div class="field">
        <label>Driven by</label>
        <div [class.two]="isServo">
          <select [(ngModel)]="controllerId" (ngModelChange)="touch()">
            <option *ngFor="let c of controllers" [value]="c.id">{{ boardLabel(c) }}</option>
          </select>
          <select *ngIf="isServo" [(ngModel)]="channel" (ngModelChange)="touch()">
            <option *ngFor="let ch of channels" [ngValue]="ch" [disabled]="!!takenBy(ch)">
              {{ channelLabel(ch) }}
            </option>
          </select>
        </div>
        <p class="why">
          {{ isServo
             ? 'Each board drives up to four servo gates, one per channel.'
             : 'A sliding gate uses the board\\'s stepper driver — one per board.' }}
        </p>
      </div>
    </div>

    <app-servo-calibration *ngIf="isServo" [sel]="asServo(working)" [topo]="topo"
                           (saved)="onCalibrated($event)" (cancelled)="cancelled.emit()">
    </app-servo-calibration>
    <app-linear-calibration *ngIf="!isServo" [sel]="asLinear(working)" [topo]="topo"
                            (saved)="onCalibrated($event)" (cancelled)="cancelled.emit()">
    </app-linear-calibration>
  `,
})
export class SelectorConfigComponent implements OnInit {
  @Input({ required: true }) sel!: ConfigurableSelector;
  @Input({ required: true }) topo!: Topology;
  /** Emits the fully-edited selector — name, wiring and calibration together. */
  @Output() saved = new EventEmitter<ConfigurableSelector>();
  @Output() cancelled = new EventEmitter<void>();

  name = '';
  controllerId = '';
  channel = 0;
  controllers: Controller[] = [];
  readonly channels = SERVO_CHANNELS;

  /** The selector the calibration widget edits. ONE stable object, mutated in place by
   *  the fields above — handing the child a fresh copy each change-detection pass would
   *  churn its @Input for no reason. */
  working!: ConfigurableSelector;

  ngOnInit(): void {
    this.name = this.sel.name ?? '';
    this.controllerId = this.sel.controllerId;
    this.channel = isServoKind(this.sel) ? (this.sel.servo?.channel ?? 0) : 0;
    this.controllers = controllersOf(this.topo);
    this.working = isServoKind(this.sel)
      ? { ...this.sel, servo: { ...this.sel.servo } }
      : { ...this.sel, linear: { ...(this.sel as LinearSelector).linear } };
  }

  get calibrated(): boolean { return isCalibrated(this.sel); }
  get isServo(): boolean { return isServoKind(this.sel); }
  get kindName(): string { return kindLabel(this.sel); }

  // The template needs the concrete type for each child's @Input; `isServo` already
  // decided which one renders.
  asServo(s: ConfigurableSelector): ServoSelector { return s as ServoSelector; }
  asLinear(s: ConfigurableSelector): LinearSelector { return s as LinearSelector; }

  boardLabel(c: Controller): string {
    return `${c.name || c.id}${c.role === 'primary' ? ' (primary)' : ''}`;
  }

  /** Which OTHER gate already holds this channel on the selected board — the schema
   *  rejects a collision, so show it here rather than at save time. */
  takenBy(ch: number): string {
    const clash = servoSelectorsOf(this.topo).find(
      (s) => s.id !== this.sel.id && s.controllerId === this.controllerId && s.servo?.channel === ch,
    );
    return clash ? (clash.name || clash.id) : '';
  }

  channelLabel(ch: number): string {
    const taken = this.takenBy(ch);
    return `Servo ${ch + 1} — ${taken || 'free'}`;
  }

  /** Name/wiring edits ride along with the calibration result, so push them into the
   *  object the widget is holding — a channel change has to reach the jog calls. */
  touch(): void {
    this.working.name = this.name;
    this.working.controllerId = this.controllerId;
    if (isServoKind(this.working)) this.working.servo.channel = this.channel;
  }

  /** The calibration widget hands back its own copy, so re-apply the fields edited up
   *  here — otherwise a rename made mid-calibration would be lost. */
  onCalibrated(calibrated: ConfigurableSelector): void {
    this.saved.emit({ ...calibrated, name: this.name, controllerId: this.controllerId });
  }
}
