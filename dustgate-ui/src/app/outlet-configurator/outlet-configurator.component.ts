import {
  Component, Input, Output, EventEmitter,
  OnInit, OnChanges, SimpleChanges, ChangeDetectionStrategy, ChangeDetectorRef
} from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ApiService, OutletConfigCmd, DiscoveredOutlet } from '../services/api.service';

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

    .plug-toggle {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
    }
    .plug-label {
      font-size: 14px;
      font-weight: 500;
    }
    .plug-toggle .gen-toggle { flex: 0 0 auto; width: 140px; }

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

    .ping-hint {
      font-size: 12px;
      color: var(--muted);
      line-height: 1.5;
      margin: -4px 0 0;
    }

    .suggest-btn {
      align-self: flex-start;
      background: none;
      border: 1px dashed var(--border);
      border-radius: 8px;
      padding: 6px 10px;
      font-size: 12px;
      color: var(--accent);
    }
    .suggest-btn:active { opacity: 0.6; }

    .scan-btn {
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px 18px;
      font-size: 14px;
      font-weight: 600;
      color: var(--text);
    }
    .scan-btn:disabled { opacity: 0.4; }

    .scan-list {
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .scan-item {
      display: flex;
      flex-direction: column;
      align-items: flex-start;
      gap: 2px;
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px 14px;
      text-align: left;
    }
    .scan-item:active { opacity: 0.6; }
    .scan-item:disabled { opacity: 0.4; }
    .scan-item .host { font-size: 14px; font-weight: 600; }
    .scan-item .meta { font-size: 12px; color: var(--muted); }
    .scan-empty {
      font-size: 13px;
      color: var(--muted);
    }
    .manual-toggle {
      align-self: flex-start;
      background: none;
      border: none;
      font-size: 12px;
      color: var(--accent);
      padding: 0;
    }

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
        <span class="sub">outlet &amp; name</span>
      </div>

      <!-- Smart plug (optional) — asked first so a located outlet's own
           Shelly-app name can be suggested for the gate name below, instead
           of making the user retype what they already named the plug. -->
      <div class="plug-toggle">
        <span class="plug-label">Smart plug on this gate?</span>
        <div class="gen-toggle">
          <button class="gen-btn" [class.selected]="hasPlug" (click)="setHasPlug(true)">Yes</button>
          <button class="gen-btn" [class.selected]="!hasPlug" (click)="setHasPlug(false)">No</button>
        </div>
      </div>

      <ng-container *ngIf="hasPlug">
        <!-- Scan-first flow: find the outlet on the network instead of typing an IP -->
        <ng-container *ngIf="!manualEntry">
          <button class="scan-btn" [disabled]="scanning" (click)="scan()">
            {{ scanning ? 'Scanning…' : (scanResults === null ? 'Scan for outlets' : 'Scan again') }}
          </button>

          <div class="scan-list" *ngIf="scanResults !== null && scanResults.length > 0">
            <button type="button" class="scan-item"
                    *ngFor="let d of scanResults"
                    [disabled]="isExcluded(d.ip)"
                    (click)="selectDiscovered(d)">
              <span class="host">{{ d.name || d.hostname }}</span>
              <span class="meta">
                {{ d.hostname }} · {{ d.ip }} —
                {{ isExcluded(d.ip) ? 'already assigned to another gate' : (d.reachable ? ('Gen ' + d.generation + ' · ' + (d.powerW | number:'1.0-0') + ' W') : 'not responding') }}
              </span>
            </button>
          </div>
          <p class="scan-empty" *ngIf="scanResults !== null && scanResults.length === 0">
            No Shelly outlets found on the network. Make sure it's powered on and
            connected to the same WiFi, then scan again — or enter its IP manually.
          </p>

          <button type="button" class="manual-toggle" (click)="manualEntry = true">
            Enter IP manually instead
          </button>
        </ng-container>

        <!-- Manual IP entry (fallback) -->
        <ng-container *ngIf="manualEntry">
          <div class="field">
            <label>IP address</label>
            <input type="text"
                   placeholder="e.g. 192.168.1.100"
                   inputmode="decimal"
                   [(ngModel)]="ip"
                   (ngModelChange)="pingResult = null; host = ''; clearError()" />
          </div>

          <!-- Ping — the device tries Gen 1 then Gen 2 automatically, so there's
               nothing to pick here, just an IP to confirm. -->
          <div class="ping-row">
            <button class="ping-btn"
                    [disabled]="!isValidIp(ip) || pinging"
                    (click)="ping()">
              {{ pinging ? 'Pinging…' : 'Ping' }}
            </button>
            <span class="ping-result ok" *ngIf="pingResult?.reachable && !isExcluded(ip.trim())">
              ✓ Reachable (Gen {{ pingResult!.generation }}){{ pingResult!.name ? ' — "' + pingResult!.name + '"' : '' }} — {{ pingResult!.powerW | number:'1.0-0' }} W
            </span>
            <span class="ping-result err" *ngIf="pingResult?.reachable && isExcluded(ip.trim())">
              ⚠ Already assigned to another gate — pick a different outlet.
            </span>
            <span class="ping-result err" *ngIf="pingResult !== null && !pingResult.reachable">
              ✗ Not reachable
            </span>
          </div>

          <button type="button" class="manual-toggle" (click)="manualEntry = false; ip = ''; pingResult = null">
            Back to scan
          </button>
        </ng-container>

        <p class="ping-hint" *ngIf="pingResult?.reachable">
          Tip: turn the tool on at its lowest setting with no load (nothing feeding,
          blade/bit spinning free), then ping again to capture its running wattage.
        </p>
      </ng-container>

      <!-- Gate name — asked once the outlet is resolved (or once the user says
           there's no plug for this gate), prefilled from the Shelly device's
           own name when one was detected. -->
      <div class="field" *ngIf="outletResolved">
        <label>Gate name</label>
        <input type="text"
               placeholder="e.g. Bandsaw"
               [(ngModel)]="toolName"
               (ngModelChange)="nameEdited = true; clearError()" />
      </div>

      <!-- Wattage threshold -->
      <div class="field" *ngIf="hasPlug && outletResolved">
        <label>Detection threshold (W)</label>
        <input type="number"
               placeholder="e.g. 5"
               min="0"
               [(ngModel)]="thresholdW" />
        <button type="button"
                class="suggest-btn"
                *ngIf="suggestedThreshold !== null"
                (click)="thresholdW = suggestedThreshold">
          Use suggested {{ suggestedThreshold }} W (from {{ pingResult!.powerW | number:'1.0-0' }} W reading)
        </button>
      </div>

      <!-- Error -->
      <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

      <!-- Actions -->
      <div class="actions">
        <button class="save-btn"
                [disabled]="!canSave || saving"
                (click)="saveOutlet()">
          {{ saving ? 'Saving…' : 'Save &amp; continue' }}
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
   * IPs already assigned to other gates in this wizard session — scan results
   * matching one of these are shown but disabled, so the same physical outlet
   * can't accidentally be wired to two gates.
   */
  @Input() excludeIps: string[] = [];

  /**
   * Emits the completed config when saved, or null when the user skips.
   * The wizard shell decides whether to call saveOutletConfig() after all gates.
   */
  @Output() saved = new EventEmitter<OutletConfigCmd | null>();

  // Form state
  toolName   = '';
  hasPlug    = false;
  ip         = '';
  /** mDNS hostname of the selected outlet, if it came from a scan rather than manual entry. */
  host       = '';
  thresholdW: number | null = null;
  /** Populated from the existing config on reconfigure, until a fresh ping supersedes it. */
  private existingGeneration: number | null = null;
  /** True once the user has typed into the name field — stops auto-fill from overwriting it. */
  nameEdited = false;

  // UI state
  pinging    = false;
  saving     = false;
  errorMsg   = '';
  pingResult: { reachable: boolean; powerW: number; generation: number; name?: string } | null = null;

  // Scan-first discovery state
  manualEntry  = false;
  scanning     = false;
  scanResults: DiscoveredOutlet[] | null = null;

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
      this.hasPlug    = false;
      this.ip         = '';
      this.host       = '';
      this.thresholdW = null;
      this.pinging    = false;
      this.saving     = false;
      this.errorMsg   = '';
      this.pingResult = null;
      this.existingGeneration = null;
      this.nameEdited = false;
      this.manualEntry = false;
      this.scanning = false;
      this.scanResults = null;
      this.applyExisting();
      this.cd.markForCheck();
    }
  }

  private applyExisting() {
    if (this.existing) {
      this.toolName   = this.existing.name       ?? '';
      this.ip         = this.existing.ip          ?? '';
      this.host       = this.existing.host        ?? '';
      this.hasPlug    = this.ip.trim().length > 0;
      this.existingGeneration = this.existing.generation ?? null;
      this.thresholdW = this.existing.threshold_w ?? null;
      // Already has a name from a prior save — don't let a re-scan/re-ping clobber it.
      this.nameEdited = this.toolName.trim().length > 0;
    }
  }

  setHasPlug(v: boolean) {
    this.hasPlug = v;
    this.pingResult = null;
    this.scanResults = null;
    this.manualEntry = false;
    if (!v) this.host = '';
    this.clearError();
  }

  /** Generation to save: a fresh successful ping wins, else whatever was already configured. */
  get resolvedGeneration(): number | null {
    return this.pingResult?.reachable ? this.pingResult.generation : this.existingGeneration;
  }

  /** True once the outlet question is settled: no plug, or a plug has been located and confirmed reachable. */
  get outletResolved(): boolean {
    return !this.hasPlug || (this.isValidIp(this.ip) && this.resolvedGeneration !== null);
  }

  // Name is required for every gate. A smart plug is optional; when present its
  // IP must be valid, its generation known (a successful ping — there's no
  // manual picker to fall back on), and not already assigned to another gate.
  get canSave(): boolean {
    if (this.toolName.trim().length === 0) return false;
    if (this.hasPlug && this.isExcluded(this.ip.trim())) return false;
    return this.hasPlug ? (this.isValidIp(this.ip) && this.resolvedGeneration !== null) : true;
  }

  isValidIp(ip: string): boolean {
    return /^(\d{1,3}\.){3}\d{1,3}$/.test(ip.trim());
  }

  /** True if this IP is already assigned to a different gate in this wizard session. */
  isExcluded(ip: string): boolean {
    return this.excludeIps.includes(ip);
  }

  clearError() { this.errorMsg = ''; }

  /**
   * Suggests a detection threshold from the last ping's power reading: ~10%
   * below the reading (margin below running draw, clear of standby power),
   * rounded to a clean step — nearest 50W above 200W, nearest 10W otherwise.
   */
  get suggestedThreshold(): number | null {
    const w = this.pingResult?.reachable ? this.pingResult.powerW : null;
    if (w === null || w <= 0) return null;
    const target = w * 0.9;
    // Step size scales down with the reading — a flat 10W step forced every
    // suggestion up to at least 10W (e.g. a real 7W reading rounded to a 10W
    // threshold, which is above the reading it's supposed to detect and
    // would never trigger). Small tools need a finer step.
    const step = w >= 200 ? 50 : w >= 20 ? 10 : 1;
    return Math.max(step, Math.floor(target / step) * step);
  }

  async scan() {
    if (this.scanning) return;
    this.scanning = true;
    this.errorMsg = '';
    this.cd.markForCheck();
    try {
      this.scanResults = await this.api.discoverOutlets();
    } catch {
      this.scanResults = [];
      this.errorMsg = 'Scan failed. Check the device is connected, or enter the IP manually.';
    } finally {
      this.scanning = false;
      this.cd.markForCheck();
    }
  }

  selectDiscovered(d: DiscoveredOutlet) {
    if (!d.reachable || this.isExcluded(d.ip)) return;
    this.ip = d.ip;
    this.host = d.hostname;
    this.pingResult = { reachable: true, powerW: d.powerW, generation: d.generation, name: d.name };
    this.existingGeneration = d.generation;
    this.applyDetectedName(d.name || d.hostname);
    this.clearError();
    this.cd.markForCheck();
  }

  async ping() {
    if (!this.isValidIp(this.ip) || this.pinging) return;
    this.pinging = true;
    this.pingResult = null;
    this.errorMsg = '';
    this.cd.markForCheck();
    try {
      this.pingResult = await this.api.pingOutlet(this.ip.trim());
      if (this.pingResult.reachable && this.pingResult.name) {
        this.applyDetectedName(this.pingResult.name);
      }
    } catch {
      this.pingResult = { reachable: false, powerW: 0, generation: 0 };
    } finally {
      this.pinging = false;
      this.cd.markForCheck();
    }
  }

  /** Suggests a detected device name for the gate — only if the user hasn't already typed one in. */
  private applyDetectedName(name: string) {
    if (this.nameEdited || !name) return;
    this.toolName = name;
  }

  async saveOutlet() {
    if (!this.canSave || this.saving) return;
    this.errorMsg = '';
    this.saving = true;
    this.cd.markForCheck();

    // Name-only gates send an empty ip — the device stores the label but does no
    // power polling for them.
    const cmd: OutletConfigCmd = {
      slot:        this.slotIndex,
      generation:  this.resolvedGeneration ?? 2,
      ip:          this.hasPlug ? this.ip.trim() : '',
      host:        this.hasPlug ? this.host : '',
      name:        this.toolName.trim(),
      stop:        this.gateIndex,
      threshold_w: this.thresholdW ?? 5.0,
    };

    try {
      await this.api.configureOutlet(cmd);
      this.saving = false;
      this.saved.emit(cmd);
    } catch {
      this.errorMsg = 'Could not save gate. Check connection and try again.';
      this.saving = false;
      this.cd.markForCheck();
    }
  }
}
