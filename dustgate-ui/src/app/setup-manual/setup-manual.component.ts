import {
  Component, OnInit, OnDestroy, ChangeDetectionStrategy, ChangeDetectorRef
} from '@angular/core';
import { Router } from '@angular/router';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Subscription } from 'rxjs';

import { ApiService, OutletConfigCmd, SystemStatus } from '../services/api.service';
import { UnitPreferenceService } from '../services/unit-preference.service';
import { HardwareProfileService } from '../services/hardware-profile.service';
import { ManifoldVisualizerComponent } from '../visualizer/manifold-visualizer.component';
import { GatePositionerComponent } from '../gate-positioner/gate-positioner.component';
import { OutletConfiguratorComponent } from '../outlet-configurator/outlet-configurator.component';
import { DustCollectorConfiguratorComponent, DustCollectorCmd } from '../dust-collector-configurator/dust-collector-configurator.component';

// ── Step machine ──────────────────────────────────────────────────────────────

type Step =
  | { id: 'port-size' }
  | { id: 'gate-count' }
  | { id: 'home-side' }
  | { id: 'homing' }
  | { id: 'spacing-method' }
  | { id: 'calibrating' }
  | { id: 'position'; gate: number }
  | { id: 'equal-spacing-offer'; gate: number; spacing: number }
  | { id: 'outlet'; gate: number }
  | { id: 'dust-collector' }
  | { id: 'review' }
  | { id: 'done' };

interface GateRecord {
  index: number;       // 1-based stop index
  mm: number;          // saved position in mm
  outletCmd: OutletConfigCmd | null;  // null = skip
}

@Component({
  selector: 'app-setup-manual',
  standalone: true,
  imports: [
    CommonModule,
    FormsModule,
    ManifoldVisualizerComponent,
    GatePositionerComponent,
    OutletConfiguratorComponent,
    DustCollectorConfiguratorComponent,
  ],
  changeDetection: ChangeDetectionStrategy.OnPush,
  styles: [`
    :host {
      display: flex;
      flex-direction: column;
      height: 100dvh;
      height: 100vh;
      overflow: hidden;
      background: var(--bg);
    }

    /* ── Header ── */
    .header {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 14px 16px 10px;
      flex-shrink: 0;
      border-bottom: 1px solid var(--border);
    }
    .back-btn {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 50%;
      width: 40px; height: 40px;
      display: flex; align-items: center; justify-content: center;
      color: var(--text);
      font-size: 18px;
      flex-shrink: 0;
    }
    .back-btn:active { opacity: 0.6; }

    .header h1 { font-size: 18px; font-weight: 700; flex: 1; }

    .reset-btn {
      background: none;
      border: none;
      color: var(--muted);
      font-size: 13px;
      padding: 6px 8px;
      border-radius: 8px;
      flex-shrink: 0;
    }
    .confirm-reset {
      display: flex; align-items: center; gap: 8px;
      font-size: 13px; color: var(--muted); flex-shrink: 0;
    }
    .confirm-reset span { white-space: nowrap; }
    .confirm-yes {
      border: none; border-radius: 6px; padding: 4px 10px;
      font-size: 12px; font-weight: 600;
      background: var(--danger); color: #fff;
    }
    .confirm-no {
      border: 1px solid var(--border); border-radius: 6px; padding: 4px 10px;
      font-size: 12px; font-weight: 600;
      background: var(--surface); color: var(--text);
    }

    /* ── Visualizer strip ── */
    .viz-section {
      flex-shrink: 0;
      padding: 10px 12px 0;
      border-bottom: 1px solid var(--border);
    }

    /* ── Scroll content ── */
    .content {
      flex: 1;
      overflow-y: auto;
      padding: 24px 16px;
      display: flex;
      flex-direction: column;
      gap: 20px;
    }

    /* ── Step cards ── */
    .step-title {
      font-size: 22px;
      font-weight: 700;
      line-height: 1.3;
    }
    .step-hint {
      font-size: 14px;
      color: var(--muted);
      line-height: 1.6;
      margin-top: -8px;
    }

    /* ── Gate count stepper ── */
    .stepper {
      display: flex;
      align-items: center;
      gap: 20px;
    }
    .step-dec, .step-inc {
      width: 48px; height: 48px;
      border-radius: 50%;
      border: 1px solid var(--border);
      background: var(--surface);
      font-size: 22px;
      color: var(--text);
      display: flex; align-items: center; justify-content: center;
    }
    .step-dec:active, .step-inc:active { background: var(--bg); }
    .step-dec:disabled, .step-inc:disabled { opacity: 0.3; }
    .step-count {
      font-size: 36px;
      font-weight: 800;
      font-variant-numeric: tabular-nums;
      min-width: 48px;
      text-align: center;
    }

    /* ── Unit toggle ── */
    .big-toggle {
      display: flex;
      gap: 12px;
    }
    .big-toggle-btn {
      flex: 1;
      border: 2px solid var(--border);
      border-radius: 14px;
      padding: 20px 12px;
      font-size: 20px;
      font-weight: 700;
      background: var(--surface);
      color: var(--muted);
      text-align: center;
    }
    .big-toggle-btn.selected {
      border-color: var(--accent);
      color: var(--text);
      background: color-mix(in srgb, var(--accent) 12%, transparent);
    }
    .big-toggle-btn:disabled {
      opacity: 0.4;
      cursor: not-allowed;
    }
    .soon-tag {
      font-size: 0.6em;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      opacity: 0.8;
      vertical-align: middle;
      margin-left: 4px;
    }

    /* ── Home side ── */
    .home-side-btns {
      display: flex;
      gap: 12px;
    }
    .side-btn {
      flex: 1;
      border: 2px solid var(--border);
      border-radius: 14px;
      padding: 24px 12px;
      font-size: 16px;
      font-weight: 700;
      background: var(--surface);
      color: var(--muted);
      text-align: center;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 8px;
    }
    .side-btn .side-diagram { font-size: 28px; }
    .side-btn:active { border-color: var(--accent); }
    .side-btn.selected {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 12%, transparent);
      color: var(--text);
    }

    /* ── Homing / direction ── */
    .info-card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 20px 16px;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }
    .info-card p { font-size: 14px; color: var(--muted); line-height: 1.6; margin: 0; }
    .info-card strong { color: var(--text); }

    .dir-btns {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }

    /* ── Equal spacing offer ── */
    .spacing-card {
      background: var(--surface);
      border: 1px solid var(--accent);
      border-radius: 16px;
      padding: 20px 16px;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }
    .spacing-card h3 { font-size: 16px; font-weight: 700; margin: 0; }
    .spacing-card p  { font-size: 14px; color: var(--muted); margin: 0; }
    .spacing-value {
      font-size: 28px;
      font-weight: 800;
      color: var(--accent);
      font-variant-numeric: tabular-nums;
    }

    /* ── Review table ── */
    .review-table {
      border: 1px solid var(--border);
      border-radius: 14px;
      overflow: hidden;
    }
    .review-row {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 12px 16px;
      border-bottom: 1px solid var(--border);
      font-size: 14px;
    }
    .review-row:last-child { border-bottom: none; }
    .review-row .gate-num {
      font-weight: 700;
      min-width: 60px;
    }
    .review-row .pos {
      color: var(--muted);
      min-width: 60px;
      font-variant-numeric: tabular-nums;
    }
    .review-row .tool {
      flex: 1;
      color: var(--text);
    }
    .review-row .edit-btn {
      background: none;
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 4px 10px;
      font-size: 12px;
      color: var(--muted);
    }

    /* ── Banners ── */
    .spinner { animation: spin 1s linear infinite; display: inline-block; }
    @keyframes spin { to { transform: rotate(360deg); } }

    .status-banner {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 12px 14px;
      font-size: 14px;
      color: var(--muted);
      display: flex;
      align-items: center;
      gap: 8px;
    }

    .error-banner {
      background: color-mix(in srgb, var(--danger) 10%, transparent);
      border: 1px solid var(--danger);
      border-radius: 10px;
      padding: 12px 14px;
      font-size: 13px;
      color: var(--danger);
    }

    /* ── Primary / secondary buttons ── */
    .primary-btn {
      background: var(--accent);
      color: #111;
      font-size: 16px;
      font-weight: 700;
      border: none;
      border-radius: 14px;
      padding: 16px;
      width: 100%;
    }
    .primary-btn:disabled { background: var(--border); color: var(--muted); }
    .primary-btn:active:not(:disabled) { opacity: 0.8; }

    .secondary-btn {
      background: var(--surface);
      color: var(--text);
      font-size: 15px;
      font-weight: 600;
      border: 1px solid var(--border);
      border-radius: 14px;
      padding: 14px;
      width: 100%;
    }
    .secondary-btn:active { opacity: 0.7; }

    .danger-btn {
      background: color-mix(in srgb, var(--danger) 15%, transparent);
      color: var(--danger);
      font-size: 15px;
      font-weight: 600;
      border: 1px solid var(--danger);
      border-radius: 14px;
      padding: 14px;
      width: 100%;
    }
    .danger-btn:active { opacity: 0.7; }

    .done-icon { font-size: 64px; text-align: center; }
    .done-msg { font-size: 18px; font-weight: 700; text-align: center; }
  `],
  template: `
    <!-- Header -->
    <div class="header">
      <button class="back-btn" (click)="goBack()" aria-label="Back">←</button>
      <button class="back-btn" *ngIf="future.length" (click)="stepForward()" aria-label="Forward">→</button>
      <h1>Manual Setup</h1>

      <button class="reset-btn" *ngIf="!confirmingReset" (click)="confirmingReset = true">
        ↺ Start over
      </button>
      <div class="confirm-reset" *ngIf="confirmingReset">
        <span>Reset?</span>
        <button class="confirm-yes" (click)="doReset()">Yes</button>
        <button class="confirm-no"  (click)="confirmingReset = false">No</button>
      </div>
    </div>

    <!-- Visualizer strip -->
    <div class="viz-section">
      <app-manifold-visualizer [liveTravel]="false"></app-manifold-visualizer>
    </div>

    <!-- Step content -->
    <div class="content">

      <!-- ── Phase 0: Port size ── -->
      <ng-container *ngIf="step.id === 'port-size'">
        <div class="step-title">Which size DustGate system are you using?</div>
        <div class="step-hint">
          This sets the manifold profile we use to auto-space your gates after
          measuring the actuator's travel.
        </div>

        <div class="big-toggle">
          <button class="big-toggle-btn"
                  [class.selected]="hardwareProfile.portSize === '2.5in'"
                  (click)="hardwareProfile.set('2.5in')">
            2.5"
          </button>
          <!-- 4" is disabled until we have real 4" hardware to measure its
               manifold profile. The logic (PortSize '4in', rockler-4 profile) is
               kept in place for when it's available. -->
          <button class="big-toggle-btn" disabled title="Coming soon — 4 inch not yet supported">
            4" <span class="soon-tag">soon</span>
          </button>
        </div>

        <button class="primary-btn" (click)="goToStep({ id: 'gate-count' })">Next</button>
      </ng-container>

      <!-- ── Phase 1.1: Gate Count ── -->
      <ng-container *ngIf="step.id === 'gate-count'">
        <div class="step-title">How many blast gates?</div>
        <div class="step-hint">Count the gates on your manifold (1–16).</div>

        <div class="stepper">
          <button class="step-dec" [disabled]="numGates <= 1" (click)="numGates = numGates - 1">−</button>
          <span class="step-count">{{ numGates }}</span>
          <button class="step-inc" [disabled]="numGates >= 16" (click)="numGates = numGates + 1">+</button>
        </div>

        <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

        <button class="primary-btn" [disabled]="busy" (click)="confirmGateCount()">
          {{ busy ? 'Saving…' : 'Next' }}
        </button>
      </ng-container>

      <!-- ── Phase 2.1: Homing ── -->
      <ng-container *ngIf="step.id === 'homing'">
        <div class="step-title">Home the actuator</div>

        <div class="info-card">
          <p>The actuator will move until it reaches the endstop.<br>
             <strong>Keep hands clear of the manifold.</strong></p>

          <div class="status-banner" *ngIf="isHoming">
            <span class="spinner">⟳</span> Homing in progress…
          </div>

          <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

          <button class="primary-btn" [disabled]="isHoming || busy" (click)="doHome()">
            Home Now
          </button>
        </div>
      </ng-container>


      <!-- ── Phase 2.3: Home Side (observed) ── -->
      <ng-container *ngIf="step.id === 'home-side'">
        <div class="step-title">Did it home to the left?</div>
        <div class="step-hint">
          You just watched the actuator travel to its endstop. Which side did it stop on?
          We always use the left end as home — if it stopped on the right, we'll switch
          to the other endstop so it homes left from now on.
        </div>

        <div class="home-side-btns">
          <button class="side-btn"
                  [class.selected]="homeSideSelected === 'left'"
                  (click)="homeSideSelected = 'left'">
            <span class="side-diagram">◀ |</span>
            <span>Yes — left</span>
          </button>
          <button class="side-btn"
                  [class.selected]="homeSideSelected === 'right'"
                  (click)="homeSideSelected = 'right'">
            <span class="side-diagram">| ▶</span>
            <span>No — right</span>
          </button>
        </div>

        <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

        <button class="primary-btn"
                [disabled]="!homeSideSelected || busy"
                (click)="confirmHomeSide()">
          {{ busy ? 'Saving…' : 'Next' }}
        </button>
      </ng-container>

      <!-- ── Phase 2.5: Auto-detect in progress ── -->
      <ng-container *ngIf="step.id === 'calibrating'">
        <div class="step-title">Detecting gate spacing…</div>

        <div class="info-card">
          <p>The actuator will home, drive to the far end, and measure the full travel.<br>
             <strong>Keep hands clear of the manifold.</strong></p>

          <div class="status-banner" *ngIf="isCalibrating">
            <span class="spinner">⟳</span> Measuring travel and placing gates…
          </div>

          <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

          <button class="primary-btn" *ngIf="!isCalibrating && errorMsg"
                  (click)="startAutoCalibrate()">
            Try again
          </button>
        </div>
      </ng-container>

      <!-- ── Phase 3: Gate Positioning ── -->
      <ng-container *ngIf="step.id === 'position'">
        <div class="step-title">Position Gate {{ step.gate }}</div>
        <div class="step-hint">
          Jog the actuator until it aligns with Gate {{ step.gate }},
          then tap "Save as Gate {{ step.gate }}".
        </div>

        <app-gate-positioner
          [gateIndex]="step.gate"
          [initialMm]="gateStartMm"
         
          (saved)="onGateSaved($event)">
        </app-gate-positioner>
      </ng-container>

      <!-- ── Phase 3 equal-spacing offer ── -->
      <ng-container *ngIf="step.id === 'equal-spacing-offer'">
        <div class="step-title">Equal spacing?</div>
        <div class="step-hint">Gates 1 and 2 are {{ units.format(step.spacing) }} apart.</div>

        <div class="spacing-card">
          <h3>Detected spacing</h3>
          <div class="spacing-value">{{ units.format(step.spacing) }}</div>
          <p>Apply this interval to the remaining {{ numGates - 2 }} gate(s)?
             You'll still be able to jog-trim each one before saving.</p>

          <button class="primary-btn" (click)="applyEqualSpacing()">
            Apply equal spacing
          </button>
          <button class="secondary-btn" style="margin-top: 8px" (click)="skipEqualSpacing()">
            Set manually
          </button>
        </div>
      </ng-container>

      <!-- ── Name + optional smart plug (per gate, right after locating it) ── -->
      <ng-container *ngIf="step.id === 'outlet'">
        <div class="step-title">Name Gate {{ step.gate }}</div>
        <div class="step-hint">Give this gate a name. Add a smart plug too if a tool's power should open it automatically.</div>

        <app-outlet-configurator
          [gateIndex]="step.gate"
          [slotIndex]="step.gate - 1"
          [existing]="editing ? outletCmdFor(step.gate) : undefined"
          [excludeIps]="assignedOutletIps(step.gate)"
          (saved)="onOutletSaved($event)">
        </app-outlet-configurator>
      </ng-container>

      <!-- ── Phase 4.5: Optional dust collector outlet ── -->
      <ng-container *ngIf="step.id === 'dust-collector'">
        <app-dust-collector-configurator
          [excludeIps]="assignedOutletIps(0)"
          (saved)="onDustCollectorSaved($event)">
        </app-dust-collector-configurator>
      </ng-container>

      <!-- ── Phase 5.1: Review ── -->
      <ng-container *ngIf="step.id === 'review'">
        <div class="step-title">Review & save</div>
        <div class="step-hint">Check the configuration, then tap Save to write it to the device.</div>

        <div class="review-table">
          <div class="review-row" *ngFor="let g of gates">
            <span class="gate-num">Gate {{ g.index }}</span>
            <span class="pos">{{ units.format(g.mm) }}</span>
            <span class="tool">
              {{ g.outletCmd?.name || 'Gate ' + g.index }}{{ g.outletCmd?.ip ? '' : ' · no plug' }}
            </span>
            <button class="edit-btn" (click)="editGate(g.index)">Edit</button>
          </div>
        </div>

        <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

        <button class="primary-btn" [disabled]="saving" (click)="saveAll()">
          {{ saving ? 'Saving…' : 'Save configuration' }}
        </button>
      </ng-container>

      <!-- ── Phase 5.2: Done ── -->
      <ng-container *ngIf="step.id === 'done'">
        <div class="done-icon">✅</div>
        <div class="done-msg">Setup complete!</div>
        <div class="step-hint" style="text-align:center">
          Your {{ numGates }} gates are configured and ready to use.
        </div>
        <button class="primary-btn" (click)="router.navigate(['/'])">
          Go to dashboard
        </button>
      </ng-container>

    </div>
  `
})
export class ManualSetupComponent implements OnInit, OnDestroy {

  // ── Step machine ──────────────────────────────────────────────────────────
  step: Step = { id: 'port-size' };
  /** Steps navigated away from — popped by stepBack() to return to them. */
  private history: Step[] = [];
  /** Steps undone by stepBack() — popped by stepForward() to redo them.
   *  Cleared on any new forward navigation, same as browser back/forward. */
  future: Step[] = [];

  // ── Phase 1 state ─────────────────────────────────────────────────────────
  numGates = 4;
  homeSideSelected: 'left' | 'right' | null = null;

  // ── Phase 3 state ─────────────────────────────────────────────────────────
  gates: GateRecord[] = [];
  /** Starting mm for the currently active GatePositioner. */
  gateStartMm = 0;
  /** True when the user chose equal spacing and we pre-calculated targets. */
  equalSpacingMm: number | null = null;
  /** Equal spacing is offered at most once (after gate 2). */
  private equalSpacingOffered = false;
  /** True while editing a single gate from the review screen (return to review on save). */
  editing = false;
  /** True when gates were auto-placed by the reference sweep (skip per-gate jogging;
   *  the outlet-config loop advances straight from one gate to the next). */
  autoSpacing = false;

  // ── UI state ──────────────────────────────────────────────────────────────
  confirmingReset = false;
  busy     = false;
  saving   = false;
  errorMsg = '';
  isHoming = false;
  /** True while the reference sweep is running (drives the 'calibrating' screen). */
  isCalibrating = false;

  private subs = new Subscription();

  constructor(
    public units: UnitPreferenceService,
    public api: ApiService,
    public hardwareProfile: HardwareProfileService,
    public router: Router,
    private cd: ChangeDetectorRef,
  ) {}

  ngOnInit() {
    this.subs.add(
      this.api.status$.subscribe((s: SystemStatus | null) => {
        const wasHoming = this.isHoming;
        this.isHoming = s?.state === 'HOMING';

        // Homing completed → ask the one orientation question. The firmware
        // auto-detects a backwards motor itself (via which endstop fired), so
        // there's no "which way did it go?" step — just confirm the home side.
        if (wasHoming && !this.isHoming && this.step.id === 'homing') {
          this.homeSideSelected = 'left';   // default; user confirms/flips next
          this.goToStep({ id: 'home-side' });
        }

        // Reference-sweep completion. The sweep runs HOMING → CALIBRATING → back
        // home; the device settles at IDLE/AT_STOP, homed, with gate positions
        // now populated (stops[1].mm non-null for a known manifold). ERROR means
        // the far endstop wasn't found. Only act while on the 'calibrating' screen.
        if (this.isCalibrating && this.step.id === 'calibrating' && s) {
          if (s.state === 'ERROR') {
            this.isCalibrating = false;
            this.errorMsg = 'Auto-detect failed — the far endstop was not reached. Check the far endstop wiring, then try again.';
          } else if (s.homed && (s.state === 'IDLE' || s.state === 'AT_STOP') &&
                     s.stops?.[1]?.mm != null) {
            this.isCalibrating = false;
            this.onCalibrationComplete(s);
          }
        }
        this.cd.markForCheck();
      })
    );
    this.subs.add(
      this.units.unit$.subscribe(() => this.cd.markForCheck())
    );
  }

  ngOnDestroy() { this.subs.unsubscribe(); }

  // ── Navigation ────────────────────────────────────────────────────────────

  /** Move to a new step, recording the current one so stepBack() can return to it. */
  goToStep(step: Step) {
    this.history.push(this.step);
    this.future = [];
    this.step = step;
    this.cd.markForCheck();
  }

  /** Header back button: previous wizard step if there is one, else leave the wizard. */
  goBack() {
    if (this.history.length > 0) { this.stepBack(); return; }
    this.router.navigate(['/']);
  }

  stepBack() {
    if (!this.history.length) return;
    this.future.push(this.step);
    this.step = this.history.pop()!;
    this.cd.markForCheck();
  }

  stepForward() {
    if (!this.future.length) return;
    this.history.push(this.step);
    this.step = this.future.pop()!;
    this.cd.markForCheck();
  }

  async doReset() {
    this.confirmingReset = false;
    try {
      await this.api.resetSetup();
      await this.api.refreshInfo();
    } catch { /* optimistic */ }
    this.step            = { id: 'gate-count' };
    this.history          = [];
    this.future           = [];
    this.gates           = [];
    this.gateStartMm     = 0;
    this.equalSpacingMm  = null;
    this.equalSpacingOffered = false;
    this.editing         = false;
    this.homeSideSelected = null;
    this.errorMsg        = '';
    this.busy            = false;
    this.saving          = false;
    this.cd.markForCheck();
  }

  // ── Phase 1 ───────────────────────────────────────────────────────────────

  async confirmGateCount() {
    this.errorMsg = '';
    this.busy     = true;
    this.cd.markForCheck();
    try {
      await this.api.setNumGates(this.numGates);
      this.goToStep({ id: 'homing' });
    } catch {
      this.errorMsg = 'Could not set gate count. Check connection.';
    } finally {
      this.busy = false;
      this.cd.markForCheck();
    }
  }

  async confirmHomeSide() {
    if (!this.homeSideSelected) return;
    this.errorMsg = '';
    this.busy     = true;
    this.cd.markForCheck();
    try {
      // Tell the firmware which side it homed to. If it came up on the right, the
      // firmware switches the datum to the other (left) endstop; the sweep that
      // follows then homes there. Auto-detect is the only placement path.
      await this.api.setHomedLeft(this.homeSideSelected === 'left');
      await this.startAutoCalibrate();
    } catch {
      this.errorMsg = 'Could not save home side. Check connection.';
    } finally {
      this.busy = false;
      this.cd.markForCheck();
    }
  }

  // ── Gate spacing: auto-detect (reference sweep) vs manual jog ───────────────

  /** Manifold profile id for the reference sweep, from the chosen port size. */
  private modelId(): string {
    return this.hardwareProfile.portSize === '4in' ? 'rockler-4' : 'rockler-2.5';
  }

  /** Auto-detect: run the reference sweep, which homes, drives to the far endstop,
   *  measures the span and auto-places every gate by the manifold profile. */
  async startAutoCalibrate() {
    this.errorMsg     = '';
    this.autoSpacing  = true;
    this.isCalibrating = true;
    this.goToStep({ id: 'calibrating' });
    try {
      // Real device: POST returns immediately (pending pattern) and the WS status
      // stream drives completion. Demo: resolves after the sim, whose final status
      // emission also triggers onCalibrationComplete via the subscription.
      await this.api.calibrate(this.modelId(), this.numGates);
    } catch {
      this.isCalibrating = false;
      this.errorMsg = 'Could not start auto-detect. Check connection.';
    }
    this.cd.markForCheck();
  }

  /** Manual: jog to each gate and save it, the original per-gate flow. */
  chooseManualSpacing() {
    this.autoSpacing = false;
    this.startPositioningPhase();
  }

  /** Sweep finished: seed gate records from the device's auto-placed positions,
   *  then jump into the outlet-config loop (no jogging needed in auto mode). */
  private onCalibrationComplete(s: SystemStatus) {
    this.gates = [];
    for (let i = 1; i <= this.numGates; i++) {
      const mm = parseFloat((s.stops?.[i]?.mm as string | undefined) ?? '0');
      this.gates.push({ index: i, mm: isNaN(mm) ? 0 : mm, outletCmd: null });
    }
    this.editing = false;
    this.goToStep({ id: 'outlet', gate: 1 });
  }

  // ── Phase 2 ───────────────────────────────────────────────────────────────

  async doHome() {
    this.errorMsg = '';
    this.busy = true;
    this.cd.markForCheck();
    try {
      await this.api.home();
      // isHoming will be set true by WS; the subscription handles the transition
    } catch {
      this.errorMsg = 'Homing command failed. Check connection.';
      this.busy = false;
      this.cd.markForCheck();
    }
    this.busy = false;
    this.cd.markForCheck();
  }

  private startPositioningPhase() {
    this.gates               = [];
    this.gateStartMm         = 0;
    this.equalSpacingMm      = null;
    this.equalSpacingOffered = false;
    this.editing             = false;
    this.autoSpacing         = false;
    this.goToStep({ id: 'position', gate: 1 });
  }

  // ── Phase 3+4 interleaved: locate a gate, then name/configure it ────────────

  onGateSaved(mm: number) {
    const posStep = this.step as { id: 'position'; gate: number };

    // Record (or update, on re-position) this gate's saved position.
    const existing = this.gates.find(g => g.index === posStep.gate);
    if (existing) existing.mm = mm;
    else this.gates.push({ index: posStep.gate, mm, outletCmd: null });

    // Immediately prompt for this gate's name + optional smart plug.
    this.goToStep({ id: 'outlet', gate: posStep.gate });
  }

  onOutletSaved(cmd: OutletConfigCmd | null) {
    const outletStep = this.step as { id: 'outlet'; gate: number };
    const gateRec = this.gates.find(g => g.index === outletStep.gate);
    if (gateRec) gateRec.outletCmd = cmd;

    // Editing a single gate from the review screen — go straight back.
    if (this.editing) {
      this.editing = false;
      this.goToStep({ id: 'review' });
      return;
    }

    const nextGate = outletStep.gate + 1;
    if (nextGate > this.numGates) {
      this.goToStep({ id: 'dust-collector' });
      return;
    }

    // Auto-detect: gates are already placed, so just advance to the next gate's
    // outlet — no jogging, no equal-spacing offer.
    if (this.autoSpacing) {
      this.goToStep({ id: 'outlet', gate: nextGate });
      return;
    }

    // Offer equal spacing once, moving from gate 2 → 3, if the first two gates
    // were placed in order (positive spacing). Out-of-order makes the
    // extrapolation meaningless, so skip it and position each gate manually.
    if (nextGate === 3 && this.numGates >= 3 && !this.equalSpacingOffered) {
      this.equalSpacingOffered = true;
      const spacing = this.gates[1].mm - this.gates[0].mm;
      if (spacing > 0) {
        this.goToStep({ id: 'equal-spacing-offer', gate: nextGate, spacing });
        return;
      }
    }

    this.goToPosition(nextGate);
  }

  onDustCollectorSaved(_cmd: DustCollectorCmd | null) {
    // Config (and any on/off test) already happened inside the child
    // component — nothing more to record here, just move on.
    this.goToStep({ id: 'review' });
  }

  applyEqualSpacing() {
    const offerStep = this.step as { id: 'equal-spacing-offer'; gate: number; spacing: number };
    this.equalSpacingMm = offerStep.spacing;
    this.goToPosition(offerStep.gate);
  }

  skipEqualSpacing() {
    const offerStep = this.step as { id: 'equal-spacing-offer'; gate: number; spacing: number };
    this.equalSpacingMm = null;
    this.goToPosition(offerStep.gate);
  }

  private async goToPosition(gate: number) {
    // Calculate the start mm for the jog widget.
    const lastGate = this.gates[this.gates.length - 1];
    if (this.equalSpacingMm !== null) {
      this.gateStartMm = this.gates[0].mm + (gate - 1) * this.equalSpacingMm;

      // The equal-spacing target is just a calculation — the actuator hasn't
      // actually moved there yet. Drive it to that position now, otherwise
      // the widget's displayed position (and the overlap check, which
      // compares against the real device position) would disagree with
      // where the actuator physically sits.
      const currentMm = this.api.status$.value?.positionMM ?? 0;
      const delta = this.gateStartMm - currentMm;
      if (Math.abs(delta) > 0.5) {
        this.busy = true;
        this.cd.markForCheck();
        try {
          await this.api.jog(delta);
        } catch {
          this.errorMsg = 'Could not auto-travel to the calculated position. Check connection.';
        } finally {
          this.busy = false;
        }
      }
    } else {
      this.gateStartMm = lastGate ? lastGate.mm : 0;
    }
    this.goToStep({ id: 'position', gate });
  }

  // ── Phase 5 ───────────────────────────────────────────────────────────────

  editGate(gateIndex: number) {
    // Re-open the name/outlet screen for one gate; return to review on save.
    // The saved position is untouched (already on the device).
    this.editing = true;
    this.goToStep({ id: 'outlet', gate: gateIndex });
  }

  outletCmdFor(gate: number): Partial<OutletConfigCmd> | undefined {
    return this.gates.find(g => g.index === gate)?.outletCmd ?? undefined;
  }

  /** IPs already assigned to gates other than `gate`, so the same outlet can't be picked twice. */
  assignedOutletIps(gate: number): string[] {
    return this.gates
      .filter(g => g.index !== gate && g.outletCmd?.ip)
      .map(g => g.outletCmd!.ip);
  }

  async saveAll() {
    this.errorMsg = '';
    this.saving   = true;
    this.cd.markForCheck();
    try {
      await this.api.saveOutletConfig();
      this.goToStep({ id: 'done' });
    } catch {
      this.errorMsg = 'Could not save configuration. Check connection.';
    } finally {
      this.saving = false;
      this.cd.markForCheck();
    }
  }
}
