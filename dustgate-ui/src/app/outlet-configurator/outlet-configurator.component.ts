import {
  Component, Input, Output, EventEmitter,
  OnInit, OnChanges, SimpleChanges, ChangeDetectionStrategy, ChangeDetectorRef
} from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ApiService, OutletConfigCmd } from '../services/api.service';

/**
 * OutletConfiguratorComponent — reusable standalone form for assigning a
 * Shelly outlet to a blast gate.
 *
 * Usage (wizard):
 *   <app-outlet-configurator
 *     [gateIndex]="currentGate"
 *     [slotIndex]="currentGate - 1"
 *     (saved)="onOutletSaved($event)">
 *   </app-outlet-configurator>
 *
 * Usage (dashboard reconfigure):
 *   <app-outlet-configurator
 *     [gateIndex]="gate.index"
 *     [slotIndex]="gate.slot"
 *     [existing]="gate.outletConfig"
 *     (saved)="onOutletUpdated($event)">
 *   </app-outlet-configurator>
 *
 * Emits null when the user taps "Skip — no outlet".
 * Emits OutletConfigCmd when the user taps "Save outlet".
 *
 * NOTE: this component only calls configureOutlet(); it does NOT call
 * saveOutletConfig(). The wizard shell calls save() after all gates are done.
 */
@Component({
  selector: 'app-outlet-configurator',
  standalone: true,
  imports: [CommonModule, FormsModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  styles: [`
    :host { display: block; }

    .configurator {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 20px 16px 16px;
      display: flex;
      flex-direction: column;
      gap: 16px;
    }

    .config-header {
      font-size: 16px;
      font-weight: 700;
    }
    .config-header .sub {
      font-size: 13px;
      font-weight: 400;
      color: var(--muted);
      margin-left: 6px;
    }

    .field {
      display: flex;
      flex-direction: column;
      gap: 6px;
    }

    label {
      font-size: 13px;
      color: var(--muted);
      font-weight: 500;
    }

    input[type="text"],
    input[type="number"] {
      width: 100%;
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px 14px;
      font-size: 15px;
      color: var(--text);
      font-family: inherit;
      box-sizing: border-box;
    }
    input:focus { outline: none; border-color: var(--accent); }

    .gen-toggle {
      display: flex;
      gap: 8px;
    }
    .gen-btn {
      flex: 1;
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px;
      font-size: 14px;
      font-weight: 600;
      background: var(--bg);
      color: var(--muted);
    }
    .gen-btn.selected {
      background: var(--accent);
      color: #111;
      border-color: var(--accent);
    }

    .ping-row {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .ping-btn {
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px 18px;
      font-size: 14px;
      font-weight: 600;
      color: var(--text);
      flex-shrink: 0;
    }
    .ping-btn:disabled { opacity: 0.4; }
    .ping-result {
      font-size: 13px;
      flex: 1;
    }
    .ping-result.ok  { color: var(--success, #22c55e); }
    .ping-result.err { color: var(--danger); }

    .error-banner {
      background: color-mix(in srgb, var(--danger) 10%, transparent);
      border: 1px solid var(--danger);
      border-radius: 10px;
      padding: 10px 14px;
      font-size: 13px;
      color: var(--danger);
    }

    .actions {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }

    .save-btn {
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

    .skip-btn {
      background: none;
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 12px;
      font-size: 14px;
      color: var(--muted);
    }
    .skip-btn:active { opacity: 0.6; }
  `],
  template: `
    <div class="configurator">
      <div class="config-header">
        Gate {{ gateIndex }}
        <span class="sub">outlet setup</span>
      </div>

      <!-- Tool name -->
      <div class="field">
        <label>Tool name</label>
        <input type="text"
               placeholder="e.g. Bandsaw"
               [(ngModel)]="toolName"
               (ngModelChange)="clearError()" />
      </div>

      <!-- Shelly generation -->
      <div class="field">
        <label>Shelly generation</label>
        <div class="gen-toggle">
          <button class="gen-btn" [class.selected]="generation === 1" (click)="generation = 1">Gen 1</button>
          <button class="gen-btn" [class.selected]="generation === 2" (click)="generation = 2">Gen 2</button>
        </div>
      </div>

      <!-- IP address -->
      <div class="field">
        <label>IP address</label>
        <input type="text"
               placeholder="192.168.1.100"
               inputmode="decimal"
               [(ngModel)]="ip"
               (ngModelChange)="pingResult = null; clearError()" />
      </div>

      <!-- Ping -->
      <div class="ping-row">
        <button class="ping-btn"
                [disabled]="!isValidIp(ip) || pinging"
                (click)="ping()">
          {{ pinging ? 'Pinging…' : 'Ping' }}
        </button>
        <span class="ping-result ok"  *ngIf="pingResult?.reachable">
          ✓ Reachable — {{ pingResult!.powerW | number:'1.0-0' }} W
        </span>
        <span class="ping-result err" *ngIf="pingResult !== null && !pingResult.reachable">
          ✗ Not reachable
        </span>
      </div>

      <!-- Wattage threshold -->
      <div class="field">
        <label>Detection threshold (W)</label>
        <input type="number"
               placeholder="5"
               min="0"
               [(ngModel)]="thresholdW" />
      </div>

      <!-- Error -->
      <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

      <!-- Actions -->
      <div class="actions">
        <button class="save-btn"
                [disabled]="!canSave || saving"
                (click)="saveOutlet()">
          {{ saving ? 'Saving…' : 'Save outlet' }}
        </button>
        <button class="skip-btn" (click)="skip()">
          Skip — no outlet for Gate {{ gateIndex }}
        </button>
      </div>
    </div>
  `
})
export class OutletConfiguratorComponent implements OnInit, OnChanges {

  /** 1-based gate number being configured. */
  @Input() gateIndex = 1;

  /** 0-based slot index in the outlet array (typically gateIndex - 1). */
  @Input() slotIndex = 0;

  /**
   * Optional existing outlet config for pre-populating the form
   * (used in reconfigure flow from the dashboard).
   */
  @Input() existing?: Partial<OutletConfigCmd>;

  /**
   * Emits the completed config when saved, or null when the user skips.
   * The wizard shell decides whether to call saveOutletConfig() after all gates.
   */
  @Output() saved = new EventEmitter<OutletConfigCmd | null>();

  // Form state
  toolName   = '';
  generation = 2;
  ip         = '';
  thresholdW: number | null = null;

  // UI state
  pinging    = false;
  saving     = false;
  errorMsg   = '';
  pingResult: { reachable: boolean; powerW: number } | null = null;

  constructor(private api: ApiService, private cd: ChangeDetectorRef) {}

  ngOnInit() {
    this.applyExisting();
  }

  ngOnChanges(changes: SimpleChanges) {
    // The wizard reuses this component instance across gates (the *ngIf stays
    // true while step.id === 'outlet').  Reset all form + UI state whenever
    // the gate changes so the previous gate's values don't bleed through.
    if (changes['gateIndex'] && !changes['gateIndex'].firstChange) {
      this.toolName   = '';
      this.generation = 2;
      this.ip         = '';
      this.thresholdW = null;
      this.pinging    = false;
      this.saving     = false;
      this.errorMsg   = '';
      this.pingResult = null;
      this.applyExisting();
      this.cd.markForCheck();
    }
  }

  private applyExisting() {
    if (this.existing) {
      this.toolName   = this.existing.name       ?? '';
      this.generation = this.existing.generation ?? 2;
      this.ip         = this.existing.ip          ?? '';
      this.thresholdW = this.existing.threshold_w ?? null;
    }
  }

  get canSave(): boolean {
    return this.toolName.trim().length > 0 && this.isValidIp(this.ip);
  }

  isValidIp(ip: string): boolean {
    return /^(\d{1,3}\.){3}\d{1,3}$/.test(ip.trim());
  }

  clearError() { this.errorMsg = ''; }

  async ping() {
    if (!this.isValidIp(this.ip) || this.pinging) return;
    this.pinging = true;
    this.pingResult = null;
    this.errorMsg = '';
    this.cd.markForCheck();
    try {
      const result = await this.api.pingOutlet(this.generation, this.ip.trim());
      this.pingResult = result;
    } catch {
      this.pingResult = { reachable: false, powerW: 0 };
    } finally {
      this.pinging = false;
      this.cd.markForCheck();
    }
  }

  async saveOutlet() {
    if (!this.canSave || this.saving) return;
    this.errorMsg = '';
    this.saving = true;
    this.cd.markForCheck();

    const cmd: OutletConfigCmd = {
      slot:        this.slotIndex,
      generation:  this.generation,
      ip:          this.ip.trim(),
      name:        this.toolName.trim(),
      stop:        this.gateIndex,
      threshold_w: this.thresholdW ?? 5.0,
    };

    try {
      await this.api.configureOutlet(cmd);
      this.saving = false;
      this.saved.emit(cmd);
    } catch {
      this.errorMsg = 'Could not save outlet. Check connection and try again.';
      this.saving = false;
      this.cd.markForCheck();
    }
  }

  skip() {
    this.saved.emit(null);
  }
}
