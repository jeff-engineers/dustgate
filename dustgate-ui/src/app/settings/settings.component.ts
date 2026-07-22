import { Component, OnInit, ChangeDetectionStrategy, ChangeDetectorRef } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { ApiService } from '../services/api.service';
import { HardwareProfileService, PortSize } from '../services/hardware-profile.service';

/**
 * SettingsComponent — device configuration hub, reached via the gear icon on
 * the dashboard. Consolidates settings that were previously only reachable
 * (or not reachable at all) from inside the setup wizards, plus entry points
 * to re-run either wizard.
 */
@Component({
  selector: 'app-settings',
  standalone: true,
  imports: [CommonModule, FormsModule],
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

    .header {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 14px 16px 10px;
      flex-shrink: 0;
    }
    .back-btn {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 50%;
      width: 40px; height: 40px;
      display: flex; align-items: center; justify-content: center;
      color: var(--text);
      flex-shrink: 0;
    }
    .back-btn:active { opacity: 0.6; }
    .title { font-size: 18px; font-weight: 700; color: var(--text); }

    .scroll {
      flex: 1;
      overflow-y: auto;
      padding: 8px 16px 32px;
      display: flex;
      flex-direction: column;
      gap: 16px;
    }

    .section {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 16px;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }
    .section-title {
      font-size: 13px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.04em;
      color: var(--muted);
    }
    .section.danger { border-color: var(--danger); }
    .section.danger .section-title { color: var(--danger); }

    .row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
    }
    .row-label { font-size: 14px; font-weight: 500; color: var(--text); }
    .row-hint { font-size: 12px; color: var(--muted); margin-top: 2px; }

    .setup-link {
      display: flex;
      flex-direction: column;
      gap: 2px;
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 12px 14px;
      text-align: left;
    }
    .setup-link:active { opacity: 0.6; }
    .setup-link .name { font-size: 14px; font-weight: 600; color: var(--text); }
    .setup-link .desc { font-size: 12px; color: var(--muted); }

    .toggle-group {
      display: flex;
      gap: 8px;
    }
    .toggle-btn {
      flex: 1;
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px;
      font-size: 13px;
      font-weight: 600;
      background: var(--bg);
      color: var(--muted);
    }
    .toggle-btn.selected {
      background: var(--accent);
      color: #111;
      border-color: var(--accent);
    }
    .toggle-btn:disabled { opacity: 0.4; }

    input[type="number"], select {
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 8px 12px;
      font-size: 14px;
      color: var(--text);
      font-family: inherit;
      width: 90px;
      box-sizing: border-box;
    }
    input:focus, select:focus { outline: none; border-color: var(--accent); }

    input[type="password"] {
      background: var(--bg);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 10px 14px;
      font-size: 14px;
      color: var(--text);
      font-family: inherit;
      box-sizing: border-box;
      width: 100%;
    }

    .save-btn {
      background: var(--accent);
      color: #111;
      font-size: 14px;
      font-weight: 700;
      border: none;
      border-radius: 10px;
      padding: 10px 16px;
      flex-shrink: 0;
    }
    .save-btn:disabled { background: var(--border); color: var(--muted); }
    .save-btn:active:not(:disabled) { opacity: 0.8; }

    .danger-btn {
      background: none;
      border: 1px solid var(--danger);
      color: var(--danger);
      border-radius: 10px;
      padding: 10px 14px;
      font-size: 14px;
      font-weight: 600;
    }
    .danger-btn:active { opacity: 0.6; }
    .danger-btn.confirming {
      background: var(--danger);
      color: #fff;
    }

    .status-msg {
      font-size: 13px;
      color: var(--success, #22c55e);
    }
    .error-msg {
      font-size: 13px;
      color: var(--danger);
    }
  `],
  template: `
    <div class="header">
      <button class="back-btn" (click)="back()" aria-label="Back">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M19 12H5M12 19l-7-7 7-7"/></svg>
      </button>
      <span class="title">Settings</span>
    </div>

    <div class="scroll">

      <!-- Setup -->
      <div class="section">
        <span class="section-title">Setup</span>
        <button type="button" class="setup-link" (click)="goSetup()">
          <span class="name">Guided (AI) Setup →</span>
          <span class="desc">Conversational assistant walks through gates and outlets</span>
        </button>
        <button type="button" class="setup-link" (click)="goManualSetup()">
          <span class="name">Manual Setup →</span>
          <span class="desc">Step through gate positions and outlets yourself</span>
        </button>
      </div>

      <!-- Power -->
      <div class="section">
        <span class="section-title">Power</span>
        <div class="row">
          <div>
            <div class="row-label">Idle power-off</div>
            <div class="row-hint">Minutes of inactivity before the motor driver powers off. Rehomes automatically on next use. 0 = never.</div>
          </div>
        </div>
        <div class="row">
          <input type="number" min="0" max="1440" [(ngModel)]="idleTimeoutMin" (ngModelChange)="clearStatus()" />
          <button class="save-btn" [disabled]="savingIdleTimeout" (click)="saveIdleTimeout()">
            {{ savingIdleTimeout ? 'Saving…' : 'Save' }}
          </button>
        </div>
      </div>

      <!-- Hardware -->
      <div class="section">
        <span class="section-title">Hardware</span>

        <div class="row">
          <span class="row-label">Home endstop side</span>
          <div class="toggle-group" style="flex: 0 0 auto; width: 160px;">
            <button class="toggle-btn" [class.selected]="!(api.deviceInfo?.homeOnRight ?? false)" [disabled]="savingOrientation" (click)="setOrientation(false)">Left</button>
            <button class="toggle-btn" [class.selected]="api.deviceInfo?.homeOnRight ?? false" [disabled]="savingOrientation" (click)="setOrientation(true)">Right</button>
          </div>
        </div>

        <div class="row">
          <span class="row-label">Motor direction</span>
          <div class="toggle-group" style="flex: 0 0 auto; width: 160px;">
            <button class="toggle-btn" [class.selected]="!(api.deviceInfo?.motorInverted ?? false)" [disabled]="savingDirection" (click)="setMotorDirection(false)">Normal</button>
            <button class="toggle-btn" [class.selected]="api.deviceInfo?.motorInverted ?? false" [disabled]="savingDirection" (click)="setMotorDirection(true)">Inverted</button>
          </div>
        </div>

        <div class="row">
          <div>
            <div class="row-label">Number of gates</div>
            <div class="row-hint">Not counting home. Lowering this clears trained positions beyond the new count.</div>
          </div>
        </div>
        <div class="row">
          <input type="number" min="1" max="16" [(ngModel)]="numGates" (ngModelChange)="clearStatus()" />
          <button class="save-btn" [disabled]="savingNumGates" (click)="saveNumGates()">
            {{ savingNumGates ? 'Saving…' : 'Save' }}
          </button>
        </div>

        <div class="row">
          <span class="row-label">Port size</span>
          <select [ngModel]="portSize" (ngModelChange)="setPortSize($event)">
            <option value="2.5in">2.5"</option>
            <!-- 4" disabled until real 4" hardware exists to measure its profile;
                 logic (PortSize '4in', rockler-4) kept for later. -->
            <option value="4in" disabled>4" (soon)</option>
          </select>
        </div>
      </div>

      <div class="status-msg" *ngIf="statusMsg">{{ statusMsg }}</div>
      <div class="error-msg" *ngIf="errorMsg">⚠ {{ errorMsg }}</div>

      <!-- Danger zone -->
      <div class="section danger">
        <span class="section-title">Danger zone</span>

        <div class="row">
          <div>
            <div class="row-label">Reset gate calibration</div>
            <div class="row-hint">Clears trained positions and outlet mappings. Re-run setup afterward.</div>
          </div>
          <button class="danger-btn" [class.confirming]="confirmingReset" (click)="confirmReset()">
            {{ confirmingReset ? 'Tap again to confirm' : 'Start over' }}
          </button>
        </div>

        <div class="row">
          <div>
            <div class="row-label">Forget WiFi</div>
            <div class="row-hint">Erases saved network credentials and reboots into the setup portal.</div>
          </div>
          <button class="danger-btn" [class.confirming]="confirmingWifiReset" (click)="confirmWifiReset()">
            {{ confirmingWifiReset ? 'Tap again to confirm' : 'Forget network' }}
          </button>
        </div>
      </div>

    </div>
  `
})
export class SettingsComponent implements OnInit {

  idleTimeoutMin = 60;
  numGates = 1;
  portSize: PortSize = '2.5in';

  savingIdleTimeout = false;
  savingOrientation = false;
  savingDirection   = false;
  savingNumGates    = false;

  confirmingReset      = false;
  confirmingWifiReset  = false;

  statusMsg = '';
  errorMsg  = '';

  constructor(
    public api: ApiService,
    private router: Router,
    private hardwareProfile: HardwareProfileService,
    private cd: ChangeDetectorRef
  ) {}

  ngOnInit() {
    this.portSize = this.hardwareProfile.portSize;
    // deviceInfo may not have loaded yet on a hard refresh straight into /settings.
    this.api.ready$.subscribe(ready => {
      if (!ready) return;
      this.idleTimeoutMin = Math.round((this.api.deviceInfo?.idleTimeoutSec ?? 3600) / 60);
      this.numGates       = this.api.deviceInfo?.numStops || 1;
      this.cd.markForCheck();
    });
  }

  back()            { this.router.navigate(['/']); }
  goSetup()         { this.router.navigate(['/setup']); }
  goManualSetup()   { this.router.navigate(['/setup/manual']); }

  clearStatus() { this.statusMsg = ''; this.errorMsg = ''; }

  private async run(action: () => Promise<unknown>, busyFlag: 'savingIdleTimeout' | 'savingOrientation' | 'savingDirection' | 'savingNumGates', successMsg: string) {
    this[busyFlag] = true;
    this.statusMsg = '';
    this.errorMsg  = '';
    this.cd.markForCheck();
    try {
      await action();
      this.statusMsg = successMsg;
    } catch {
      this.errorMsg = 'Could not save. Check connection and try again.';
    } finally {
      this[busyFlag] = false;
      this.cd.markForCheck();
    }
  }

  saveIdleTimeout() {
    const sec = Math.max(0, Math.min(1440, Math.round(this.idleTimeoutMin))) * 60;
    this.run(() => this.api.setIdleTimeout(sec), 'savingIdleTimeout', 'Idle timeout saved.');
  }

  setOrientation(homeOnRight: boolean) {
    this.run(() => this.api.setOrientation(homeOnRight), 'savingOrientation', 'Orientation saved.');
  }

  setMotorDirection(invert: boolean) {
    this.run(() => this.api.setMotorDirection(invert), 'savingDirection', 'Motor direction saved.');
  }

  saveNumGates() {
    const n = Math.max(1, Math.min(16, Math.round(this.numGates)));
    this.run(() => this.api.setNumGates(n), 'savingNumGates', 'Gate count saved.');
  }

  setPortSize(size: PortSize) {
    this.portSize = size;
    this.hardwareProfile.set(size);
    this.statusMsg = 'Port size saved.';
    this.errorMsg = '';
    this.cd.markForCheck();
  }

  confirmReset() {
    if (!this.confirmingReset) {
      this.confirmingReset = true;
      this.cd.markForCheck();
      return;
    }
    this.confirmingReset = false;
    this.run(() => this.api.resetSetup(), 'savingNumGates', 'Calibration reset. Run setup again when ready.');
  }

  confirmWifiReset() {
    if (!this.confirmingWifiReset) {
      this.confirmingWifiReset = true;
      this.cd.markForCheck();
      return;
    }
    this.confirmingWifiReset = false;
    this.statusMsg = 'Forgetting WiFi and rebooting — reconnect to the "DustGate-Setup" network to reconfigure.';
    this.errorMsg = '';
    this.cd.markForCheck();
    // Device disconnects almost immediately; ignore the (likely never-arriving) response.
    this.api.forgetWifi().catch(() => {});
  }
}
