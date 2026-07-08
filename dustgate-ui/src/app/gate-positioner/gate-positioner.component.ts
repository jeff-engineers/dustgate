import {
  Component, Input, Output, EventEmitter,
  OnInit, OnChanges, OnDestroy, SimpleChanges, ChangeDetectionStrategy, ChangeDetectorRef
} from '@angular/core';
import { CommonModule } from '@angular/common';
import { Subscription } from 'rxjs';
import { ApiService, SystemStatus } from '../services/api.service';
import { UnitPreferenceService } from '../services/unit-preference.service';

/**
 * GatePositionerComponent — reusable jog widget for positioning the actuator
 * at a gate stop.
 *
 * Usage (wizard):
 *   <app-gate-positioner
 *     [gateIndex]="currentGate"
 *     [initialMm]="gateStartMm"
 *     (saved)="onGateSaved($event)">
 *   </app-gate-positioner>
 *
 * Usage (dashboard reconfigure):
 *   <app-gate-positioner
 *     [gateIndex]="gate.index"
 *     [initialMm]="gate.mm"
 *     [prePositioned]="true"
 *     (saved)="onReconfigured($event)">
 *   </app-gate-positioner>
 *
 * The component tracks position locally (initialMm + joggedMm) so it stays
 * independent of firmware steps-to-mm conversion accuracy.
 */
@Component({
  selector: 'app-gate-positioner',
  standalone: true,
  imports: [CommonModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  styles: [`
    :host { display: block; }

    .positioner {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 20px 16px 16px;
      display: flex;
      flex-direction: column;
      gap: 18px;
    }

    .pos-header {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: 12px;
    }

    .gate-label {
      font-size: 16px;
      font-weight: 700;
    }

    .position-readout {
      font-size: 22px;
      font-weight: 700;
      font-variant-numeric: tabular-nums;
      color: var(--accent);
    }

    .unit-toggle {
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 4px 12px;
      font-size: 13px;
      color: var(--muted);
    }
    .unit-toggle:active { opacity: 0.6; }

    .jog-section {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }

    .jog-direction {
      display: flex;
      align-items: center;
      gap: 8px;
    }

    .dir-label {
      font-size: 11px;
      color: var(--muted);
      width: 70px;
      flex-shrink: 0;
      text-align: center;
    }

    .jog-btns {
      display: flex;
      gap: 6px;
      flex: 1;
    }

    .jog-btn {
      flex: 1;
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px 2px;
      font-size: 12px;
      font-weight: 600;
      color: var(--text);
      text-align: center;
      min-width: 0;
    }
    .jog-btn:active:not(:disabled) { background: var(--surface-2, #2a2a2a); }
    .jog-btn:disabled { opacity: 0.35; }

    .moving-banner {
      display: flex;
      align-items: center;
      gap: 8px;
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px 14px;
      font-size: 14px;
      color: var(--muted);
    }
    .spinner {
      animation: spin 1s linear infinite;
      display: inline-block;
    }
    @keyframes spin { to { transform: rotate(360deg); } }

    .error-banner {
      background: color-mix(in srgb, var(--danger) 10%, transparent);
      border: 1px solid var(--danger);
      border-radius: 10px;
      padding: 10px 14px;
      font-size: 13px;
      color: var(--danger);
    }

    .save-row {
      display: flex;
      gap: 10px;
    }

    .save-btn {
      flex: 1;
      background: var(--accent);
      color: #111;
      font-size: 15px;
      font-weight: 700;
      border: none;
      border-radius: 12px;
      padding: 14px;
    }
    .save-btn:disabled { background: var(--border); color: var(--muted); }
    .save-btn:active:not(:disabled) { opacity: 0.8; }
  `],
  template: `
    <div class="positioner">
      <div class="pos-header">
        <span class="gate-label">Gate {{ gateIndex }}</span>
        <span class="position-readout">{{ units.format(currentMm) }}</span>
        <button class="unit-toggle" (click)="units.toggle()">
          {{ units.unit === 'mm' ? 'Switch to in' : 'Switch to mm' }}
        </button>
      </div>

      <!-- Moving overlay -->
      <div class="moving-banner" *ngIf="isMoving">
        <span class="spinner">⟳</span> Moving…
      </div>

      <!-- Error -->
      <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

      <!-- Jog controls — disabled while moving -->
      <div class="jog-section" *ngIf="!isMoving">

        <!-- Toward home row (reversed order: largest first) -->
        <div class="jog-direction">
          <span class="dir-label">{{ homeOnRight ? '→' : '←' }} toward home</span>
          <div class="jog-btns">
            <button *ngFor="let s of reversedSteps"
                    class="jog-btn toward"
                    [disabled]="isMoving || isSaving || towardWouldPassHome(s.mm)"
                    (click)="jog(-s.mm)">
              {{ s.label }}
            </button>
          </div>
        </div>

        <!-- Away from home row (smallest first) -->
        <div class="jog-direction">
          <span class="dir-label">away from home {{ homeOnRight ? '←' : '→' }}</span>
          <div class="jog-btns">
            <button *ngFor="let s of units.jogSteps"
                    class="jog-btn"
                    [disabled]="isMoving || isSaving"
                    (click)="jog(s.mm)">
              {{ s.label }}
            </button>
          </div>
        </div>

      </div>

      <!-- Save -->
      <div class="save-row">
        <button class="save-btn"
                [disabled]="isMoving || isSaving"
                (click)="save()">
          {{ isSaving ? 'Saving…' : 'Save as Gate ' + gateIndex }}
        </button>
      </div>
    </div>
  `
})
export class GatePositionerComponent implements OnInit, OnChanges, OnDestroy {

  /** Which stop index this widget is positioning (1-based gate number). */
  @Input() gateIndex = 1;

  /**
   * Starting position in mm (from the previously saved stop, or 0 for gate 1).
   * The widget tracks further movement locally.
   */
  @Input() initialMm = 0;

  /**
   * If true, the actuator is already at this position (e.g. reconfigure flow).
   * The widget skips the "you must be at this position" warning.
   */
  @Input() prePositioned = false;

  /** True when home is on the right side of the manifold — flips the jog arrow labels. */
  @Input() homeOnRight = false;

  /** Emits the final mm position when the stop is successfully saved. */
  @Output() saved = new EventEmitter<number>();

  /** Local jog accumulator — added to initialMm for display. */
  joggedMm = 0;

  isMoving = false;
  isSaving = false;
  errorMsg = '';

  private subs = new Subscription();

  constructor(
    public units: UnitPreferenceService,
    private api: ApiService,
    private cd: ChangeDetectorRef,
  ) {}

  ngOnInit() {
    // Track firmware MOVING/HOMING state so buttons disable during motion
    this.subs.add(
      this.api.status$.subscribe((s: SystemStatus | null) => {
        const moving = s?.state === 'MOVING' || s?.state === 'HOMING';
        if (moving !== this.isMoving) {
          this.isMoving = moving;
          this.cd.markForCheck();
        }
      })
    );
    // React to unit changes so the display rerenders
    this.subs.add(
      this.units.unit$.subscribe(() => this.cd.markForCheck())
    );
  }

  ngOnChanges(changes: SimpleChanges) {
    // The wizard reuses this component instance across gates (the *ngIf stays
    // true while step.id === 'position'). Reset the local jog accumulator +
    // UI state whenever the gate changes so the previous gate's movement
    // doesn't bleed into the new one's starting position.
    if (changes['gateIndex'] && !changes['gateIndex'].firstChange) {
      this.joggedMm = 0;
      this.isSaving = false;
      this.errorMsg = '';
      this.cd.markForCheck();
    }
  }

  ngOnDestroy() { this.subs.unsubscribe(); }

  get currentMm(): number {
    return this.initialMm + this.joggedMm;
  }

  /**
   * True if a toward-home jog of this many mm would drive past the home endstop
   * (position 0). The actuator physically can't move beyond home, so we disable
   * the button rather than let the local position tracking desync. At the home
   * position this greys out the whole toward-home row.
   */
  towardWouldPassHome(stepMm: number): boolean {
    return stepMm > this.currentMm + 1e-6;
  }

  get reversedSteps() {
    return [...this.units.jogSteps].reverse();
  }

  async jog(mm: number) {
    if (this.isMoving || this.isSaving) return;
    this.errorMsg = '';
    this.isMoving = true;
    this.cd.markForCheck();
    try {
      await this.api.jog(mm);
      this.joggedMm += mm;
    } catch {
      this.errorMsg = `Jog failed (${mm > 0 ? '+' : ''}${mm.toFixed(1)} mm)`;
    } finally {
      // isMoving will be reset by the WS status update; set a fallback here
      // in case the WS is slow.
      setTimeout(() => {
        this.isMoving = false;
        this.cd.markForCheck();
      }, 2000);
    }
    this.cd.markForCheck();
  }

  async save() {
    if (this.isMoving || this.isSaving) return;
    this.errorMsg = '';
    this.isSaving = true;
    this.cd.markForCheck();
    try {
      await this.api.saveStop(this.gateIndex);
      this.isSaving = false;
      this.saved.emit(this.currentMm);
    } catch (e: unknown) {
      // A plain Error means our own validation rejected the save (e.g. too
      // close to another saved gate) — show that message. Anything else is
      // a real request failure.
      this.errorMsg = e instanceof Error
        ? e.message
        : `Could not save Gate ${this.gateIndex}. Check connection.`;
      this.isSaving = false;
    }
    this.cd.markForCheck();
  }
}
