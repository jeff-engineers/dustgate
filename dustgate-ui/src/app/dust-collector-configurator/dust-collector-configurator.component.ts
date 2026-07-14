import {
  Component, Input, Output, EventEmitter, ChangeDetectionStrategy, ChangeDetectorRef
} from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ApiService, DiscoveredOutlet } from '../services/api.service';

export interface DustCollectorCmd {
  generation: number;
  ip: string;
  host?: string;
}

type Phase = 'ask' | 'form' | 'confirm-on' | 'testing' | 'result' | 'confirm-off';

/**
 * DustCollectorConfiguratorComponent — optional step (used by both setup
 * wizards) for assigning a Shelly smart outlet to the dust collector itself,
 * separate from any blast gate. Unlike a per-tool outlet, this one has no
 * wattage threshold to tune — it's just a remote switch — so instead the
 * flow walks the user through actually testing it: turn the collector on
 * via its own switch, confirm it's OK to flip it remotely, verify DustGate
 * sees a real load, then confirm it's OK to switch it back off.
 *
 * Emits null when skipped; emits DustCollectorCmd once configured (and
 * optionally tested).
 */
@Component({
  selector: 'app-dust-collector-configurator',
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

    p.hint {
      font-size: 14px;
      color: var(--muted);
      line-height: 1.6;
      margin: 0;
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

    input[type="text"] {
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
    .ping-result { font-size: 13px; flex: 1; }
    .ping-result.ok  { color: var(--success, #22c55e); }
    .ping-result.err { color: var(--danger); }

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

    .status-banner {
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 12px 14px;
      font-size: 14px;
      color: var(--muted);
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .spinner { animation: spin 1s linear infinite; display: inline-block; }
    @keyframes spin { to { transform: rotate(360deg); } }

    .load-result {
      border-radius: 10px;
      padding: 12px 14px;
      font-size: 14px;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }
    .load-result.ok {
      background: color-mix(in srgb, var(--success, #22c55e) 10%, transparent);
      border: 1px solid var(--success, #22c55e);
      color: var(--success, #22c55e);
    }
    .load-result.low {
      background: color-mix(in srgb, var(--danger) 10%, transparent);
      border: 1px solid var(--danger);
      color: var(--danger);
    }
    .load-result .reading {
      font-size: 22px;
      font-weight: 800;
      font-variant-numeric: tabular-nums;
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
    .actions.row { flex-direction: row; }

    .save-btn {
      background: var(--accent);
      color: #111;
      font-size: 15px;
      font-weight: 700;
      border: none;
      border-radius: 12px;
      padding: 14px;
      flex: 1;
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
      flex: 1;
    }
    .skip-btn:active { opacity: 0.6; }
  `],
  template: `
    <div class="configurator">
      <div class="config-header">
        Dust collector
        <span class="sub">optional smart plug</span>
      </div>

      <!-- Phase: ask whether to add one at all -->
      <ng-container *ngIf="phase === 'ask'">
        <p class="hint">
          If your dust collector is on a Shelly smart plug too, DustGate can
          switch it on and off automatically alongside the gates. No wattage
          threshold needed here — it's just a remote switch.
        </p>
        <div class="actions">
          <button class="save-btn" (click)="phase = 'form'">Add dust collector outlet</button>
          <button class="skip-btn" (click)="skip()">Skip — no outlet</button>
        </div>
      </ng-container>

      <!-- Phase: locate the outlet — scan-first, the device tries Gen 1 then
           Gen 2 automatically so there's nothing to pick, just a device or
           IP to confirm. -->
      <ng-container *ngIf="phase === 'form'">
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
                {{ isExcluded(d.ip) ? 'already assigned to a gate' : (d.reachable ? ('Gen ' + d.generation + ' · ' + (d.powerW | number:'1.0-0') + ' W') : 'not responding') }}
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

        <ng-container *ngIf="manualEntry">
          <div class="field">
            <label>IP address</label>
            <input type="text"
                   placeholder="e.g. 192.168.1.102"
                   inputmode="decimal"
                   [(ngModel)]="ip"
                   (ngModelChange)="pingResult = null; host = ''; clearError()" />
          </div>

          <div class="ping-row">
            <button class="ping-btn"
                    [disabled]="!isValidIp(ip) || pinging"
                    (click)="ping()">
              {{ pinging ? 'Pinging…' : 'Ping' }}
            </button>
            <span class="ping-result ok" *ngIf="pingResult?.reachable && !isExcluded(ip.trim())">
              ✓ Reachable (Gen {{ pingResult!.generation }}){{ pingResult!.name ? ' — "' + pingResult!.name + '"' : '' }}
            </span>
            <span class="ping-result err" *ngIf="pingResult?.reachable && isExcluded(ip.trim())">
              ⚠ Already assigned to a gate — pick a different outlet.
            </span>
            <span class="ping-result err" *ngIf="pingResult !== null && !pingResult.reachable">✗ Not reachable</span>
          </div>

          <button type="button" class="manual-toggle" (click)="manualEntry = false; ip = ''; pingResult = null">
            Back to scan
          </button>
        </ng-container>

        <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

        <div class="actions">
          <button class="save-btn"
                  [disabled]="!canContinue || saving"
                  (click)="confirmForm()">
            {{ saving ? 'Saving…' : 'Continue' }}
          </button>
          <button class="skip-btn" (click)="skip()">Skip — no outlet</button>
        </div>
      </ng-container>

      <!-- Phase: confirm it's OK to switch it on remotely -->
      <ng-container *ngIf="phase === 'confirm-on'">
        <p class="hint">
          Turn the dust collector on now using its own built-in switch, so
          it's ready to draw power. Then, is it OK for DustGate to switch it
          <strong>off and on again remotely</strong> to confirm it can detect
          when the collector is running?
        </p>

        <div class="error-banner" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

        <div class="actions">
          <button class="save-btn" [disabled]="saving" (click)="runTest()">
            {{ saving ? 'Starting…' : 'Yes, test it' }}
          </button>
          <button class="skip-btn" (click)="finish()">Skip test, just save</button>
        </div>
      </ng-container>

      <!-- Phase: testing -->
      <ng-container *ngIf="phase === 'testing'">
        <div class="status-banner">
          <span class="spinner">⟳</span> Switching on and checking for load…
        </div>
      </ng-container>

      <!-- Phase: result -->
      <ng-container *ngIf="phase === 'result'">
        <div class="load-result" [class.ok]="loadOk" [class.low]="!loadOk">
          <span>{{ loadOk ? '✓ Load detected' : '⚠ Little to no load detected' }}</span>
          <span class="reading">{{ lastReadingW | number:'1.0-1' }} W</span>
          <span *ngIf="!loadOk">
            Make sure the collector's own switch is on and it's plugged into
            this outlet, then try again.
          </span>
        </div>

        <div class="actions row">
          <button class="skip-btn" [disabled]="saving" (click)="retryTest()">Test again</button>
          <button class="save-btn" [disabled]="saving" (click)="turnOffAndFinish()">
            {{ saving ? 'Turning off…' : 'Turn off & continue' }}
          </button>
        </div>
      </ng-container>
    </div>
  `
})
export class DustCollectorConfiguratorComponent {

  /** IPs already assigned to gate outlets — disabled in the scan list / blocked on manual entry. */
  @Input() excludeIps: string[] = [];

  @Output() saved = new EventEmitter<DustCollectorCmd | null>();

  phase: Phase = 'ask';

  ip   = '';
  /** mDNS hostname of the selected outlet, if it came from a scan rather than manual entry. */
  host = '';

  pinging = false;
  saving  = false;
  errorMsg = '';
  pingResult: { reachable: boolean; powerW: number; generation: number; name?: string } | null = null;

  // Scan-first discovery state
  manualEntry  = false;
  scanning     = false;
  scanResults: DiscoveredOutlet[] | null = null;

  loadOk = false;
  lastReadingW = 0;

  /** A dust collector pulling less than this while switched on is treated as "no load detected". */
  private readonly minLoadW = 100;

  constructor(private api: ApiService, private cd: ChangeDetectorRef) {}

  isValidIp(ip: string): boolean {
    return /^(\d{1,3}\.){3}\d{1,3}$/.test(ip.trim());
  }

  /** True if this IP is already assigned to a gate. */
  isExcluded(ip: string): boolean {
    return this.excludeIps.includes(ip);
  }

  get canContinue(): boolean {
    return !!this.pingResult?.reachable && !this.isExcluded(this.ip.trim());
  }

  clearError() { this.errorMsg = ''; }

  skip() {
    this.saved.emit(null);
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
    } catch {
      this.pingResult = { reachable: false, powerW: 0, generation: 0 };
    } finally {
      this.pinging = false;
      this.cd.markForCheck();
    }
  }

  async confirmForm() {
    if (!this.canContinue || this.saving) return;
    this.errorMsg = '';
    this.saving = true;
    this.cd.markForCheck();
    try {
      await this.api.configureDustCollector(this.pingResult!.generation, this.ip.trim(), this.host);
      this.phase = 'confirm-on';
    } catch {
      this.errorMsg = 'Could not save the dust collector outlet. Check connection and try again.';
    } finally {
      this.saving = false;
      this.cd.markForCheck();
    }
  }

  async runTest() {
    if (this.saving) return;
    this.errorMsg = '';
    this.saving = true;
    this.cd.markForCheck();
    try {
      await this.api.setDustCollector(true);
      this.phase = 'testing';
      this.cd.markForCheck();
      // Give the plug a moment to report an updated reading before we ping it.
      await new Promise(r => setTimeout(r, 1500));
      const result = await this.api.pingOutlet(this.ip.trim());
      this.lastReadingW = result.powerW;
      this.loadOk = result.reachable && result.powerW >= this.minLoadW;
      this.phase = 'result';
    } catch {
      this.errorMsg = 'Could not switch the dust collector on. Check connection and try again.';
      this.phase = 'confirm-on';
    } finally {
      this.saving = false;
      this.cd.markForCheck();
    }
  }

  async retryTest() {
    if (this.saving) return;
    this.saving = true;
    this.cd.markForCheck();
    try {
      const result = await this.api.pingOutlet(this.ip.trim());
      this.lastReadingW = result.powerW;
      this.loadOk = result.reachable && result.powerW >= this.minLoadW;
    } catch {
      this.errorMsg = 'Could not read the outlet. Check connection and try again.';
    } finally {
      this.saving = false;
      this.cd.markForCheck();
    }
  }

  async turnOffAndFinish() {
    if (this.saving) return;
    this.saving = true;
    this.cd.markForCheck();
    try {
      await this.api.setDustCollector(false);
    } catch {
      // Non-fatal — the outlet's already configured; user can toggle from the dashboard.
    } finally {
      this.saving = false;
      this.finish();
    }
  }

  finish() {
    this.saved.emit({ generation: this.pingResult?.generation ?? 2, ip: this.ip.trim(), host: this.host });
  }
}
