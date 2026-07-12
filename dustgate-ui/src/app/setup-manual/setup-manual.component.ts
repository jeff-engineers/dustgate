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
  | { id: 'unit-system' }
  | { id: 'home-side' }
  | { id: 'homing' }
  | { id: 'direction-confirm' }
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
      <app-manifold-visualizer [homeOnRight]="api.deviceInfo?.homeOnRight ?? false" [liveTravel]="false"></app-manifold-visualizer>
    </div>

    <!-- Step content -->
    <div class="content">

      <!-- ── Phase 0: Port size ── -->
      <ng-container *ngIf="step.id === 'port-size'">
        <div class="step-title">Which size DustGate system are you using?</div>
        <div class="step-hint">
          This just seeds a starting guess for gate spacing — jogging to the real
          position always takes over once you have one saved.
        </div>

        <div class="big-toggle">
          <button class="big-toggle-btn"
                  [class.selected]="hardwareProfile.portSize === '2.5in'"
                  (click)="hardwareProfile.set('2.5in')">
            2.5"
          </button>
          <button class="big-toggle-btn"
                  [class.selected]="hardwareProfile.portSize === '4in'"
                  (click)="hardwareProfile.set('4in')">
            4"
          </button>
        </div>

        <button class="primary-btn" (click)="step = { id: 'gate-count' }">Next</button>
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

      <!-- ── Phase 1.2: Unit System ── -->
      <ng-container *ngIf="step.id === 'unit-system'">
        <div class="step-title">Measurement units</div>
        <div class="step-hint">You can switch at any time during setup.</div>

        <div class="big-toggle">
          <button class="big-toggle-btn"
                  [class.selected]="units.unit === 'mm'"
                  (click)="units.set('mm')">
            mm
          </button>
          <button class="big-toggle-btn"
                  [class.selected]="units.unit === 'in'"
                  (click)="units.set('in')">
            inches
          </button>
        </div>

        <button class="primary-btn" (click)="step = { id: 'home-side' }">Next</button>
      </ng-container>

      <!-- ── Phase 1.3: Home Side ── -->
      <ng-container *ngIf="step.id === 'home-side'">
        <div class="step-title">Which side is the endstop on?</div>
        <div class="step-hint">This is the end the actuator travels toward when homing.</div>

        <div class="home-side-btns">
          <button class="side-btn"
                  [class.selected]="homeSideSelected === 'left'"
                  (click)="homeSideSelected = 'left'">
            <span class="side-diagram">◀ |</span>
            <span>Left</span>
          </button>
          <button class="side-btn"
                  [class.selected]="homeSideSelected === 'right'"
                  (click)="homeSideSelected = 'right'">
            <span class="side-diagram">| ▶</span>
            <span>Right</span>
          </button>
        </div>

        <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

        <button class="primary-btn"
                [disabled]="!homeSideSelected || busy"
                (click)="confirmHomeSide()">
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

      <!-- ── Phase 2.2: Direction Confirm ── -->
      <ng-container *ngIf="step.id === 'direction-confirm'">
        <div class="step-title">Did it move the right way?</div>

        <div class="info-card">
          <p>Did the actuator move <strong>toward</strong> the endstop, or <strong>away</strong> from it?</p>

          <div class="status-banner" *ngIf="isHoming">
            <span class="spinner">⟳</span> Re-homing…
          </div>

          <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

          <div class="dir-btns" *ngIf="!isHoming">
            <button class="primary-btn" (click)="directionCorrect()">
              ✓ Toward — correct
            </button>
            <button class="danger-btn" (click)="directionWrong()">
              ✗ Away — wrong direction
            </button>
          </div>
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
          [homeOnRight]="api.deviceInfo?.homeOnRight ?? false"
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
          (saved)="onOutletSaved($event)">
        </app-outlet-configurator>
      </ng-container>

      <!-- ── Phase 4.5: Optional dust collector outlet ── -->
      <ng-container *ngIf="step.id === 'dust-collector'">
        <app-dust-collector-configurator
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

  // ── UI state ──────────────────────────────────────────────────────────────
  confirmingReset = false;
  busy     = false;
  saving   = false;
  errorMsg = '';
  isHoming = false;

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

        // Homing completed → advance from 'homing' to 'direction-confirm'
        if (wasHoming && !this.isHoming && this.step.id === 'homing') {
          this.step = { id: 'direction-confirm' };
        }
        // Re-homing after direction invert → advance to positioning
        if (wasHoming && !this.isHoming && this.step.id === 'direction-confirm') {
          this.startPositioningPhase();
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

  goBack() { this.router.navigate(['/']); }

  async doReset() {
    this.confirmingReset = false;
    try {
      await this.api.resetSetup();
      await this.api.refreshInfo();
    } catch { /* optimistic */ }
    this.step            = { id: 'gate-count' };
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
      this.step = { id: 'unit-system' };
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
      await this.api.setOrientation(this.homeSideSelected === 'right');
      this.step = { id: 'homing' };
    } catch {
      this.errorMsg = 'Could not save home side. Check connection.';
    } finally {
      this.busy = false;
      this.cd.markForCheck();
    }
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

  directionCorrect() {
    this.startPositioningPhase();
  }

  async directionWrong() {
    this.errorMsg = '';
    this.busy = true;
    this.cd.markForCheck();
    try {
      await this.api.setMotorDirection(true);
      await this.api.home();
      // WS will set isHoming → true; when it transitions back to false the
      // subscription fires and calls startPositioningPhase().
    } catch {
      this.errorMsg = 'Could not invert direction. Check connection.';
      this.busy = false;
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
    this.step                = { id: 'position', gate: 1 };
    this.cd.markForCheck();
  }

  // ── Phase 3+4 interleaved: locate a gate, then name/configure it ────────────

  onGateSaved(mm: number) {
    const posStep = this.step as { id: 'position'; gate: number };

    // Record (or update, on re-position) this gate's saved position.
    const existing = this.gates.find(g => g.index === posStep.gate);
    if (existing) existing.mm = mm;
    else this.gates.push({ index: posStep.gate, mm, outletCmd: null });

    // Immediately prompt for this gate's name + optional smart plug.
    this.step = { id: 'outlet', gate: posStep.gate };
    this.cd.markForCheck();
  }

  onOutletSaved(cmd: OutletConfigCmd | null) {
    const outletStep = this.step as { id: 'outlet'; gate: number };
    const gateRec = this.gates.find(g => g.index === outletStep.gate);
    if (gateRec) gateRec.outletCmd = cmd;

    // Editing a single gate from the review screen — go straight back.
    if (this.editing) {
      this.editing = false;
      this.step = { id: 'review' };
      this.cd.markForCheck();
      return;
    }

    const nextGate = outletStep.gate + 1;
    if (nextGate > this.numGates) {
      this.step = { id: 'dust-collector' };
      this.cd.markForCheck();
      return;
    }

    // Offer equal spacing once, moving from gate 2 → 3, if the first two gates
    // were placed in order (positive spacing). Out-of-order makes the
    // extrapolation meaningless, so skip it and position each gate manually.
    if (nextGate === 3 && this.numGates >= 3 && !this.equalSpacingOffered) {
      this.equalSpacingOffered = true;
      const spacing = this.gates[1].mm - this.gates[0].mm;
      if (spacing > 0) {
        this.step = { id: 'equal-spacing-offer', gate: nextGate, spacing };
        this.cd.markForCheck();
        return;
      }
    }

    this.goToPosition(nextGate);
  }

  onDustCollectorSaved(_cmd: DustCollectorCmd | null) {
    // Config (and any on/off test) already happened inside the child
    // component — nothing more to record here, just move on.
    this.step = { id: 'review' };
    this.cd.markForCheck();
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
    this.step = { id: 'position', gate };
    this.cd.markForCheck();
  }

  // ── Phase 5 ───────────────────────────────────────────────────────────────

  editGate(gateIndex: number) {
    // Re-open the name/outlet screen for one gate; return to review on save.
    // The saved position is untouched (already on the device).
    this.editing = true;
    this.step = { id: 'outlet', gate: gateIndex };
    this.cd.markForCheck();
  }

  outletCmdFor(gate: number): Partial<OutletConfigCmd> | undefined {
    return this.gates.find(g => g.index === gate)?.outletCmd ?? undefined;
  }

  async saveAll() {
    this.errorMsg = '';
    this.saving   = true;
    this.cd.markForCheck();
    try {
      await this.api.saveOutletConfig();
      this.step = { id: 'done' };
    } catch {
      this.errorMsg = 'Could not save configuration. Check connection.';
    } finally {
      this.saving = false;
      this.cd.markForCheck();
    }
  }
}
