import { Component, OnInit, OnDestroy } from '@angular/core';
import { Router } from '@angular/router';
import { CommonModule } from '@angular/common';
import { Subscription } from 'rxjs';
import { ApiService, SystemStatus, OutletStatus } from '../services/api.service';
import { ManifoldVisualizerComponent } from '../visualizer/manifold-visualizer.component';

interface ToolButton {
  stop: number;
  name: string;
  powerW: number;
  active: boolean;
  reachable: boolean;
}

@Component({
  selector: 'app-dashboard',
  standalone: true,
  imports: [CommonModule, ManifoldVisualizerComponent],
  styles: [`
    :host {
      display: flex;
      flex-direction: column;
      height: 100dvh;
      height: 100vh;
      overflow: hidden;
      background: var(--bg);
    }

    /* ── Header ──────────────────────────────────────────────── */
    .header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 14px 16px 10px;
      flex-shrink: 0;
    }

    .wordmark {
      font-size: 18px;
      font-weight: 700;
      letter-spacing: 0.04em;
      color: var(--text);
    }

    .status-dot {
      width: 8px; height: 8px;
      border-radius: 50%;
      background: var(--muted);
      display: inline-block;
      margin-right: 6px;
      vertical-align: middle;
    }
    .status-dot.connected  { background: var(--success); }
    .status-dot.error      { background: var(--danger); }

    .state-label {
      font-size: 12px;
      color: var(--muted);
      vertical-align: middle;
    }

    .gear-btns {
      display: flex;
      gap: 8px;
      align-items: center;
    }

    .gear-btn {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 50%;
      width: 40px; height: 40px;
      display: flex; align-items: center; justify-content: center;
      color: var(--muted);
      font-size: 18px;
    }
    .gear-btn:active { opacity: 0.6; }

    .manual-btn {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: 10px;
      padding: 0 12px;
      height: 40px;
      display: flex; align-items: center;
      color: var(--muted);
      font-size: 12px;
      font-weight: 600;
      white-space: nowrap;
    }
    .manual-btn:active { opacity: 0.6; }

    /* ── Scroll area ─────────────────────────────────────────── */
    .scroll {
      flex: 1;
      overflow-y: auto;
      padding: 8px 16px 24px;
      display: flex;
      flex-direction: column;
      gap: var(--gap);
    }

    /* ── Section labels ──────────────────────────────────────── */
    .section-label {
      font-size: 11px;
      font-weight: 600;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--muted);
      margin-top: 4px;
    }

    /* ── Base button ─────────────────────────────────────────── */
    .tool-btn {
      width: 100%;
      min-height: var(--btn-h);
      border-radius: var(--radius);
      background: var(--surface);
      border: 2px solid var(--border);
      color: var(--text);
      display: flex;
      align-items: center;
      padding: 0 20px;
      gap: 14px;
      transition: border-color 0.15s, background 0.15s;
      text-align: left;
    }

    .tool-btn .btn-name {
      font-size: 20px;
      font-weight: 600;
      flex: 1;
    }

    .tool-btn .btn-power {
      font-size: 13px;
      color: var(--muted);
      white-space: nowrap;
    }

    .tool-btn .btn-dot {
      width: 10px; height: 10px;
      border-radius: 50%;
      background: var(--border);
      flex-shrink: 0;
    }

    /* Active (current gate) */
    .tool-btn.current {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 10%, var(--surface));
    }
    .tool-btn.current .btn-dot { background: var(--accent); }

    /* Tool drawing power */
    .tool-btn.tool-on .btn-dot { background: var(--success); }
    .tool-btn.tool-on .btn-power { color: var(--success); }

    /* Offline */
    .tool-btn.offline { opacity: 0.45; }

    /* HOME button */
    .home-btn {
      background: var(--surface);
      border: 2px solid var(--border);
      border-radius: var(--radius);
      color: var(--text);
      width: 100%;
      min-height: var(--btn-h);
      font-size: 20px;
      font-weight: 700;
      letter-spacing: 0.05em;
    }
    .home-btn.current {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 10%, var(--surface));
      color: var(--accent);
    }

    /* Dust collector toggle */
    .dc-btn {
      width: 100%;
      min-height: var(--btn-h);
      border-radius: var(--radius);
      display: flex;
      align-items: center;
      padding: 0 20px;
      gap: 14px;
      font-size: 20px;
      font-weight: 600;
      border: 2px solid var(--border);
      background: var(--surface);
      color: var(--text);
    }
    .dc-btn .dc-icon { font-size: 22px; }
    .dc-btn.dc-on {
      border-color: var(--success);
      background: color-mix(in srgb, var(--success) 10%, var(--surface));
      color: var(--success);
    }
    .dc-btn .dc-status {
      margin-left: auto;
      font-size: 13px;
      color: var(--muted);
      font-weight: 400;
    }
    .dc-btn.dc-on .dc-status { color: var(--success); }

    /* Not-configured state */
    .setup-prompt {
      flex: 1;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: 16px;
      color: var(--muted);
      text-align: center;
      padding: 32px;
    }
    .setup-prompt h2 { color: var(--text); font-size: 22px; }
    .setup-prompt p  { font-size: 15px; line-height: 1.5; }
    .setup-cta {
      margin-top: 8px;
      padding: 16px 32px;
      border-radius: var(--radius);
      background: var(--accent);
      color: #111;
      font-size: 17px;
      font-weight: 700;
      border: none;
    }
    .setup-cta-manual {
      margin-top: 8px;
      padding: 14px 32px;
      border-radius: var(--radius);
      background: var(--surface);
      color: var(--text);
      font-size: 16px;
      font-weight: 600;
      border: 1px solid var(--border);
    }

    /* Error / homing banner */
    .banner {
      padding: 12px 16px;
      border-radius: var(--radius);
      font-size: 14px;
      font-weight: 500;
    }
    .banner.warn   { background: color-mix(in srgb, var(--accent) 15%, var(--surface)); color: var(--accent); }
    .banner.danger { background: color-mix(in srgb, var(--danger) 15%, var(--surface)); color: var(--danger); }

    /* Loading */
    .loading {
      flex: 1;
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--muted);
      font-size: 15px;
    }
  `],
  template: `
    <!-- Header -->
    <div class="header">
      <div>
        <span class="wordmark">DustGate</span>
      </div>
      <div>
        <span class="status-dot"
              [class.connected]="connected"
              [class.error]="!connected && status !== null"></span>
        <span class="state-label">{{ stateLabel }}</span>
      </div>
      <div class="gear-btns">
        <button class="manual-btn" (click)="goManualSetup()" aria-label="Manual Setup">Manual Setup</button>
        <button class="gear-btn" (click)="goSetup()" aria-label="AI Setup">⚙</button>
      </div>
    </div>

    <!-- Loading -->
    <div class="loading" *ngIf="!ready">
      Connecting to DustGate…
    </div>

    <ng-container *ngIf="ready">

      <!-- No outlets configured yet -->
      <div class="setup-prompt" *ngIf="toolButtons.length === 0">
        <h2>Not configured</h2>
        <p>Run the setup wizard to map your tools and Shelly outlets to blast gate positions.</p>
        <button class="setup-cta" (click)="goSetup()">AI Setup →</button>
        <button class="setup-cta-manual" (click)="goManualSetup()">Manual Setup →</button>
      </div>

      <!-- Main layout when configured -->
      <div class="scroll" *ngIf="toolButtons.length > 0">

        <!-- Banners -->
        <div class="banner warn"   *ngIf="status?.state === 'HOMING'">Homing in progress…</div>
        <div class="banner warn"   *ngIf="status?.state === 'MOVING'">Moving to stop {{ status?.targetStop }}…</div>
        <div class="banner danger" *ngIf="status?.state === 'ERROR'">
          Error — type <code>home</code> in serial to recover, or use Setup.
        </div>
        <div class="banner warn"   *ngIf="status && !status.homed && status.state !== 'HOMING'">
          Not homed — tap HOME first.
        </div>

        <!-- Manifold visualizer -->
        <app-manifold-visualizer
          [dcOn]="dcOn"
          [homeOnRight]="api.deviceInfo?.homeOnRight ?? false">
        </app-manifold-visualizer>

        <!-- HOME -->
        <span class="section-label">Position</span>
        <button class="home-btn"
                [class.current]="status?.currentStop === 0"
                (click)="sendHome()">
          HOME
        </button>

        <!-- Tool buttons -->
        <span class="section-label" *ngIf="toolButtons.length > 0">Tools</span>
        <button *ngFor="let t of toolButtons"
                class="tool-btn"
                [class.current]="status?.currentStop === t.stop"
                [class.tool-on]="t.active"
                [class.offline]="!t.reachable"
                (click)="sendMove(t.stop)">
          <span class="btn-dot"></span>
          <span class="btn-name">{{ t.name }}</span>
          <span class="btn-power" *ngIf="t.reachable">{{ t.powerW | number:'1.0-0' }} W</span>
          <span class="btn-power" *ngIf="!t.reachable">offline</span>
        </button>

        <!-- Dust collector dummy -->
        <span class="section-label">Dust Collector</span>
        <button class="dc-btn"
                [class.dc-on]="dcOn"
                (click)="toggleDc()">
          <span class="dc-icon">{{ dcOn ? '💨' : '🌀' }}</span>
          <span>Dust Collector</span>
          <span class="dc-status">{{ dcOn ? 'ON' : 'OFF' }}</span>
        </button>

      </div>
    </ng-container>
  `
})
export class DashboardComponent implements OnInit, OnDestroy {

  status: SystemStatus | null = null;
  connected = false;
  ready = false;
  dcOn = false;  // dummy state — no API call yet

  toolButtons: ToolButton[] = [];

  private subs = new Subscription();

  constructor(public api: ApiService, private router: Router) {}

  ngOnInit() {
    this.subs.add(
      this.api.ready$.subscribe(r => { this.ready = r; })
    );
    this.subs.add(
      this.api.connected$.subscribe(c => { this.connected = c; })
    );
    this.subs.add(
      this.api.status$.subscribe(s => {
        this.status = s;
        this.buildToolButtons(s);
      })
    );
  }

  ngOnDestroy() { this.subs.unsubscribe(); }

  // ── Computed ─────────────────────────────────────────────────────────────────

  get stateLabel(): string {
    if (!this.connected) return 'disconnected';
    const s = this.status?.state?.toLowerCase() ?? 'idle';
    return s;
  }

  private buildToolButtons(status: SystemStatus | null) {
    if (!status?.outlets?.length) {
      this.toolButtons = [];
      return;
    }
    this.toolButtons = status.outlets
      .filter(o => o.stop > 0)
      .sort((a, b) => a.stop - b.stop)
      .map(o => ({
        stop:      o.stop,
        name:      o.name,
        powerW:    o.powerW,
        active:    o.active,
        reachable: o.reachable
      }));
  }

  // ── Actions ──────────────────────────────────────────────────────────────────

  sendHome()          { this.api.home().catch(console.error); }
  sendMove(stop: number) { this.api.moveToStop(stop).catch(console.error); }
  toggleDc()          { this.dcOn = !this.dcOn; } // dummy — no API yet
  goSetup()           { this.router.navigate(['/setup']); }
  goManualSetup()     { this.router.navigate(['/setup/manual']); }
}
