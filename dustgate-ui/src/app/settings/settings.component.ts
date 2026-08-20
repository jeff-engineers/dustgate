import { Component, OnInit, ChangeDetectionStrategy, ChangeDetectorRef } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { Router } from '@angular/router';
import { ApiService } from '../services/api.service';
import { HardwareProfileService, PortSize } from '../services/hardware-profile.service';
import type { Topology } from '@topology';
import { DEFAULT_COLLECTOR_OFF_DELAY_MS } from '@topology-device';
import { toShop, systemsOf, type ShopDoc, type RawEl } from '../services/shop-doc';

/**
 * SettingsComponent — device configuration hub, reached via the gear icon.
 * Consolidates everything that isn't part of laying out the shop: port sizes,
 * motor direction, idle timeout, WiFi, and the destructive resets.
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

      <!-- Dust collection -->
      <!-- Per collector, because that is where the value lives: offDelayMs is a
           field on the collector element, and a two-collector shop can legitimately
           want a big cyclone to wind down slowly and a shopvac to cut straight
           away. With one collector — nearly every shop — this renders as the single
           control it reads like. -->
      <div class="section">
        <span class="section-title">Dust collection</span>

        <div class="row">
          <div>
            <div class="row-label">Coast-down</div>
            <div class="row-hint">
              Seconds the collector keeps running after the last tool switches off.
              Clears the duct instead of leaving it packed, and stops the blower
              short-cycling between cuts. 0 = cut immediately.
            </div>
          </div>
        </div>

        <div class="row" *ngFor="let c of coasts">
          <!-- The name only earns its place when there's more than one to tell
               apart; on a one-collector shop it is noise above a single field. -->
          <span class="row-label" *ngIf="coasts.length > 1">{{ c.name }}</span>
          <input type="number" min="0" max="120" [(ngModel)]="c.seconds" (ngModelChange)="clearStatus()" />
        </div>

        <!-- Right-aligned like every other Save on this page. One button rather
             than one per collector: the shop is written whole, so a partial save
             isn't a thing the document can express. -->
        <div class="row" *ngIf="coasts.length" style="justify-content: flex-end">
          <button class="save-btn" [disabled]="savingCoast" (click)="saveCoast()">
            {{ savingCoast ? 'Saving…' : 'Save' }}
          </button>
        </div>

        <div class="row-hint" *ngIf="!coasts.length">
          No dust collector yet — draw the shop layout first and this will follow it.
        </div>
      </div>

      <!-- Hardware -->
      <div class="section">
        <span class="section-title">Hardware</span>

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

  numGates = 1;
  portSize: PortSize = '2.5in';

  savingDirection   = false;
  savingNumGates    = false;

  confirmingReset      = false;
  confirmingWifiReset  = false;

  /** One row per collector in the saved shop — see the template's note on why
   *  this is per collector rather than one device-wide number. */
  coasts: { systemId: string; name: string; seconds: number }[] = [];
  savingCoast = false;
  private doc: ShopDoc | null = null;

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
      this.numGates       = this.api.deviceInfo?.numStops || 1;
      this.cd.markForCheck();
    });
    void this.loadCoasts();
  }

  /** A missing layout is normal, not an error — someone can open Settings on a
   *  device they have not drawn a shop on yet. */
  private async loadCoasts(): Promise<void> {
    try {
      this.doc = toShop(await this.api.getTopology());
    } catch {
      this.doc = null;
    }
    this.coasts = [];
    for (const sys of systemsOf(this.doc)) {
      const c = (sys.elements || []).find(e => (e as RawEl)['type'] === 'collector') as RawEl | undefined;
      if (!c) continue;
      const control = (c['control'] ?? {}) as RawEl;
      const ms = typeof control['offDelayMs'] === 'number'
        ? control['offDelayMs'] as number
        : DEFAULT_COLLECTOR_OFF_DELAY_MS;
      this.coasts.push({
        systemId: sys.id,
        name: (c['name'] as string) || sys.name || 'Dust collector',
        // Whole seconds in the UI: the field is milliseconds because firmware
        // counts in them, but nobody sets a coast-down to 4.25 s.
        seconds: Math.round(ms / 1000),
      });
    }
    this.cd.markForCheck();
  }

  /** Writes every collector in one PUT — the document is saved whole, so a
   *  per-collector endpoint would just be the same write with more steps. */
  async saveCoast(): Promise<void> {
    if (!this.doc) return;
    this.savingCoast = true;
    this.statusMsg = '';
    this.errorMsg  = '';
    this.cd.markForCheck();
    try {
      for (const row of this.coasts) {
        const sys = systemsOf(this.doc).find(x => x.id === row.systemId);
        const c = (sys?.elements || []).find(e => (e as RawEl)['type'] === 'collector') as RawEl | undefined;
        if (!c) continue;
        const secs = Math.max(0, Math.min(120, Math.round(row.seconds)));
        row.seconds = secs;
        // Merge, never replace: `control` also carries the collector's plug.
        c['control'] = { ...((c['control'] ?? {}) as RawEl), offDelayMs: secs * 1000 };
      }
      await this.api.putTopology(this.doc as unknown as Topology);
      this.statusMsg = 'Coast-down saved.';
    } catch {
      // The likeliest cause is a half-drawn shop the controller won't accept —
      // saying "check connection" would send someone hunting the wrong fault.
      this.errorMsg = 'Could not save. If the shop layout is unfinished, finish it first.';
    } finally {
      this.savingCoast = false;
      this.cd.markForCheck();
    }
  }

  back()            { this.router.navigate(['/']); }
  goSetup()         { this.router.navigate(['/build']); }

  clearStatus() { this.statusMsg = ''; this.errorMsg = ''; }

  private async run(action: () => Promise<unknown>, busyFlag: 'savingDirection' | 'savingNumGates', successMsg: string) {
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
