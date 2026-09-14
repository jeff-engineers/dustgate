import { Component, OnInit, ChangeDetectionStrategy, ChangeDetectorRef } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router } from '@angular/router';
import { ApiService } from '../services/api.service';

/**
 * SettingsComponent — device configuration hub, reached via the gear icon.
 * Consolidates everything that isn't part of laying out the shop: port sizes,
 * motor direction, idle timeout, WiFi, and the destructive resets.
 */
@Component({
  selector: 'app-settings',
  standalone: true,
  imports: [CommonModule],
  changeDetection: ChangeDetectionStrategy.OnPush,
  styles: [`
    :host {
      display: flex;
      flex-direction: column;
      height: 100vh;
      height: 100dvh;
      overflow: hidden;
      background: var(--bg);
      /* The same cap /shop, /boards and /tools set for themselves. Settings had
         been riding the app column's width, which was fine while that was 960 and
         stopped being fine when it went to 1440: a settings form stretched that
         wide is a row of labels at one edge and their controls at the other. */
      max-width: 460px;
      margin: 0 auto;
      width: 100%;
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
          <span class="name">Shop Layout →</span>
          <span class="desc">Lay out the collector, ducts, gates and tools</span>
        </button>
      </div>

      <!-- NO "Hardware" SECTION ANY MORE (2026-09-14). It held one control,
           "Port size", and that control was wrong twice over: the size is a
           property of each RACK — the calibration screen asks for the manifold
           by name before anything else, and a shop can hold a 2½" rack and a 4"
           one — and the dropdown could not be changed regardless, because its
           4" option was disabled. Its one consumer, the overlap guard in
           ApiService.checkStopConflict(), now takes the pitch from the rack
           being calibrated. Coast-down left with it: it lives on each collector,
           edited where that collector is set up. -->

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

  /** One action at a time on this page. It was called `savingNumGates` and was
   *  already the flag for every call by the time that field was removed — a
   *  name that outlived the only thing it described. */
  busy = false;

  confirmingReset      = false;
  confirmingWifiReset  = false;

  statusMsg = '';
  errorMsg  = '';

  constructor(
    public api: ApiService,
    private router: Router,
    private cd: ChangeDetectorRef
  ) {}

  ngOnInit() {
    // Nothing here waits on deviceInfo any more: the only reader was the gate
    // count, and the wait went with it. (It was a whenReady() rather than a
    // subscribe() on purpose — this component has no ngOnDestroy, so a bare
    // subscribe on an app-lifetime BehaviorSubject leaked one destroyed
    // component per visit. Worth knowing before adding another.)
  }

  /** A missing layout is normal, not an error — someone can open Settings on a
   *  device they have not drawn a shop on yet. */
  back()            { this.router.navigate(['/']); }
  goSetup()         { this.router.navigate(['/build']); }

  clearStatus() { this.statusMsg = ''; this.errorMsg = ''; }

  private async run(action: () => Promise<unknown>, busyFlag: 'busy', successMsg: string) {
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

  confirmReset() {
    if (!this.confirmingReset) {
      this.confirmingReset = true;
      this.cd.markForCheck();
      return;
    }
    this.confirmingReset = false;
    this.run(() => this.api.resetSetup(), 'busy', 'Calibration reset. Run setup again when ready.');
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
