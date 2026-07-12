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

    /* Error / homing / moving banner. This slot is always rendered — even
       with nothing to show — so the state banner popping in and out (e.g.
       every time a move starts or finishes) doesn't shift the layout below
       it. Sized to the banner's own rendered height (line-height + padding). */
    .banner-slot {
      min-height: 44px;
    }
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
        <a class="gear-btn" href="https://github.com/jeff-engineers/dustgate" target="_blank" rel="noopener noreferrer" aria-label="View on GitHub">
          <svg width="18" height="18" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true">
            <path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.01 8.01 0 0 0 16 8c0-4.42-3.58-8-8-8z"/>
          </svg>
        </a>
        <button class="manual-btn" (click)="goManualSetup()" aria-label="Manual Setup">Manual Setup</button>
        <button class="manual-btn" (click)="goSetup()" aria-label="Guided Setup">Guided Setup</button>
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

        <!-- Banners — fixed-height slots so a banner appearing/disappearing
             (e.g. every time a move starts or finishes) doesn't shift the
             rest of the page. -->
        <div class="banner-slot">
          <div class="banner" [class.warn]="stateBanner!.cls === 'warn'" [class.danger]="stateBanner!.cls === 'danger'" *ngIf="stateBanner as b">
            {{ b.text }}
          </div>
        </div>
        <div class="banner warn" *ngIf="notHomedBanner">Not homed — tap HOME first.</div>

        <!-- Manifold visualizer — the interactive control surface: tap a
             gate to move to it, tap home to home, tap the dust collector to
             toggle it. (Formerly a separate row of buttons below this — see
             ManualControlsComponent if that needs to come back.) -->
        <app-manifold-visualizer
          [dcOn]="dcOn"
          [dcConfigured]="dcConfigured"
          [interactive]="true"
          [homeOnRight]="api.deviceInfo?.homeOnRight ?? false">
        </app-manifold-visualizer>

      </div>
    </ng-container>
  `
})
export class DashboardComponent implements OnInit, OnDestroy {

  status: SystemStatus | null = null;
  connected = false;
  ready = false;

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

  /**
   * The state-driven banner (homing / moving / error), if any. 'state' only
   * ever holds one value at a time so these are mutually exclusive — a single
   * getter lets the template reserve one fixed-height slot for whichever is
   * active instead of stacking/unstacking separate *ngIf elements, which is
   * what caused the page to jump every time a move started or finished.
   */
  get stateBanner(): { text: string; cls: 'warn' | 'danger' } | null {
    switch (this.status?.state) {
      case 'HOMING': return { text: 'Homing in progress…', cls: 'warn' };
      case 'MOVING': return { text: `Moving to stop ${this.status?.targetStop}…`, cls: 'warn' };
      case 'ERROR':  return { text: 'Error — type home in serial to recover, or use Setup.', cls: 'danger' };
      default:       return null;
    }
  }

  /** Independent of stateBanner — can be true at the same time as e.g. ERROR. */
  get notHomedBanner(): boolean {
    return !!this.status && !this.status.homed && this.status.state !== 'HOMING';
  }

  /** Live dust-collector switch state from the device status. */
  get dcOn(): boolean { return this.status?.dcOn ?? false; }

  /** True once a dust-collector plug is assigned (toggle is otherwise inert). */
  get dcConfigured(): boolean { return this.status?.dcConfigured ?? false; }

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
  // Motion / dust-collector commands now live on the interactive visualizer
  // itself (ManifoldVisualizerComponent.onGateClick / onDcClick) since it's
  // the control surface. Only navigation stays here.

  goSetup()           { this.router.navigate(['/setup']); }
  goManualSetup()     { this.router.navigate(['/setup/manual']); }
}
