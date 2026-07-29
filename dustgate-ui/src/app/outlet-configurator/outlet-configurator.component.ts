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

    .scan-hint {
      font-size: 12px;
      color: var(--muted);
      line-height: 1.55;
      margin: 0;
    }
    .scan-hint strong { color: var(--text); }
    .scan-hint .chip {
      font-weight: 700;
      font-style: normal;
    }
    .scan-hint .chip-on  { color: var(--success, #22c55e); }
    .scan-hint .chip-low { color: var(--warning, #f59e0b); }

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
    /* An outlet drawing current — tinted so a tool's plug stands out at a glance
       (switch the tool on, scan, spot the highlighted one). Green = running tool
       (≥5W); amber = low/standby draw (1–5W). */
    .scan-item.drawing-on {
      border-color: var(--success, #22c55e);
      background: color-mix(in srgb, var(--success, #22c55e) 14%, var(--bg));
    }
    .scan-item.drawing-low {
      border-color: var(--warning, #f59e0b);
      background: color-mix(in srgb, var(--warning, #f59e0b) 12%, var(--bg));
    }
    .scan-item .host {
      font-size: 14px; font-weight: 600; color: var(--text);
      display: flex; align-items: center; gap: 8px;
    }
    .draw-badge {
      font-size: 11px;
      font-weight: 700;
      border-radius: 999px;
      padding: 1px 7px;
      white-space: nowrap;
    }
    .drawing-on .draw-badge {
      color: var(--success, #22c55e);
      border: 1px solid color-mix(in srgb, var(--success, #22c55e) 45%, transparent);
    }
    .drawing-low .draw-badge {
      color: var(--warning, #f59e0b);
      border: 1px solid color-mix(in srgb, var(--warning, #f59e0b) 45%, transparent);
    }
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
          <p class="scan-hint">
            <strong>Which plug is this gate's?</strong> Turn this gate's tool on at
            its lowest setting (blade/bit spinning free, nothing feeding), then tap
            <em>Scan</em>. The outlet drawing power lights up —
            <span class="chip chip-on">green</span> for a running tool,
            <span class="chip chip-low">amber</span> for a low/standby draw. Pick the
            green one, then switch the tool off.
          </p>

          <button class="scan-btn" [disabled]="scanning" (click)="scan()">
            {{ scanning ? 'Scanning…' : (scanResults === null ? 'Scan for outlets' : 'Scan again') }}
          </button>

          <div class="scan-list" *ngIf="scanResults !== null && scanResults.length > 0">
            <button type="button" class="scan-item"
                    *ngFor="let d of scanResults"
                    [class.drawing-on]="drawLevel(d) === 'on'"
                    [class.drawing-low]="drawLevel(d) === 'low'"
                    [disabled]="isExcluded(d.ip)"
                    (click)="selectDiscovered(d)">
              <span class="host">
                {{ d.name || d.hostname }}
                <!-- Live power cue: an outlet drawing current lights up — green for
                     a running tool, amber for a low/standby draw — so you can spot a
                     tool's plug by switching it on and re-scanning. -->
                <span class="draw-badge" *ngIf="drawLevel(d) !== 'off'">⚡ {{ d.powerW | number:'1.0-0' }} W</span>
              </span>
              <span class="meta">
                {{ d.hostname }} · {{ d.ip }} —
                {{ isDustCollector(d.ip) ? 'reserved — dust collector' : (isExcluded(d.ip) ? 'already assigned to another gate' : (d.reachable ? ('Gen ' + d.generation + ' · ' + (d.powerW | number:'1.0-0') + ' W') : 'not responding')) }}
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

          <!-- Ping — Gen2+ plugs only (Gen1 not supported), so there's nothing
               to pick here, just an IP to confirm. -->
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
   * IP of the plug reserved for the dust collector, if one is configured.
   * Treated as excluded (can't be picked as a tool sensor) but labelled
   * distinctly so the user understands *why* it's unavailable.
   */
  @Input() dustCollectorIp?: string;

  /**
   * Emits the completed config when saved, or null when the user skips.
   * The wizard shell decides whether to call saveOutletConfig() after all gates.
   */
  @Output() saved = new EventEmitter<OutletConfigCmd | null>();

  // Form state
  toolName   = '';
  hasPlug    = true;   // most gates have a plug — default Yes, and auto-scan for it
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
    this.autoScan();
  }

  /**
   * Kick off a network scan automatically when a gate step opens, so the list of
   * discovered plugs is ready without the user hunting for a "Scan" button. No-op
   * when there's nothing to scan for: no plug on this gate, manual IP entry, an
   * already-configured gate (existing IP), or results already cached from an
   * earlier gate in this run. The user can still hit "Scan again" to refresh.
   */
  private autoScan() {
    if (!this.hasPlug || this.manualEntry) return;
    if (this.ip.trim().length > 0) return;              // already has a plug
    if (this.scanResults !== null || this.scanning) return;
    this.scan();
  }

  ngOnChanges(changes: SimpleChanges) {
    // The wizard reuses this component instance across gates (the *ngIf stays
    // true while step.id === 'outlet').  Reset all form + UI state whenever
    // the gate changes so the previous gate's values don't bleed through.
    if (changes['gateIndex'] && !changes['gateIndex'].firstChange) {
      this.toolName   = '';
      this.hasPlug    = true;
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
      // NB: scanResults is intentionally NOT cleared here — the discovered-outlet
      // list is network-wide, so we reuse it across gates (already-assigned plugs
      // show as excluded) instead of re-scanning per gate. autoScan() only fires
      // a fresh scan when we have none yet.
      this.applyExisting();
      this.autoScan();
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
    this.manualEntry = false;
    if (!v) { this.host = ''; this.scanResults = null; }
    this.clearError();
    this.autoScan();   // switching to Yes with no results yet → scan right away
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

  /** True if this IP can't be picked here — assigned to another gate, or
   *  reserved for the dust collector. */
  isExcluded(ip: string): boolean {
    return this.excludeIps.includes(ip) || this.isDustCollector(ip);
  }

  /** True if this IP is the one reserved for the dust collector. */
  isDustCollector(ip: string): boolean {
    return !!this.dustCollectorIp && ip === this.dustCollectorIp;
  }

  /**
   * Current-draw tier for highlighting a discovered outlet:
   *   'off'  — unreachable or < 1W (Shelly standby ~0W): no highlight.
   *   'low'  — 1–5W: something's plugged in but idling (charger, electronics,
   *            a tool at standby). Amber, so it's noticeable but not mistaken
   *            for a running tool.
   *   'on'   — ≥ 5W (the default detection threshold): a running tool. Green.
   * Lets the user identify a tool's plug by switching it on and re-scanning.
   */
  drawLevel(d: DiscoveredOutlet): 'off' | 'low' | 'on' {
    if (!d.reachable || d.powerW < 1) return 'off';
    return d.powerW >= 5 ? 'on' : 'low';
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
    // Prefill the gate name from the plug's Shelly-app name only if it looks like
    // a real name the user chose — not blank and not a factory identifier or the
    // mDNS hostname (which would make a confusing gate label).
    if (this.isCustomName(d.name, d.hostname)) this.applyDetectedName(d.name);
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
      // Manual IP entry has no mDNS hostname to compare against, but the name
      // filter still rejects blanks and factory identifiers.
      if (this.pingResult.reachable && this.isCustomName(this.pingResult.name, '')) {
        this.applyDetectedName(this.pingResult.name!);
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

  /**
   * True when a plug's Shelly-app name looks like a real name the user chose, so
   * it's worth pre-filling as the gate name — as opposed to blank or a factory /
   * auto-generated identifier, which would make a confusing gate label.
   *
   * The friendly name is only ever a display label here (we key/match plugs on
   * ip + mDNS host, never the name), so this filter is purely cosmetic — a bad
   * guess costs nothing but a name the user retypes. Rejects:
   *   - blank / whitespace
   *   - the mDNS hostname itself (the device id, e.g. "shellyplugsg3-a8032ab…")
   *   - factory device-id labels starting with "shelly…"
   *   - anything ending in a MAC/hex fragment like "-A8032AB"
   */
  private isCustomName(name: string | undefined, hostname: string | undefined): boolean {
    const n = (name ?? '').trim();
    if (!n) return false;
    const lower = n.toLowerCase();
    if (hostname && lower === hostname.trim().toLowerCase()) return false;
    if (/^shelly[a-z0-9]/i.test(n)) return false;
    if (/[-_ ][0-9a-f]{4,}$/i.test(n)) return false;
    return true;
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
