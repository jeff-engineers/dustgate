import { Component, Input, OnInit, OnDestroy } from '@angular/core';
import { CommonModule, DecimalPipe } from '@angular/common';
import { Subscription } from 'rxjs';
import { ApiService, SystemStatus, OutletStatus, StopInfo } from '../services/api.service';

interface GateInfo {
  index:  number;
  label:  string;
  outlet: OutletStatus | null;
  isHome: boolean;
  stopMm: number;
}

@Component({
  selector: 'app-manifold-visualizer',
  standalone: true,
  imports: [CommonModule, DecimalPipe],
  styles: [`
    :host { display: block; }

    .viz-card {
      background: var(--surface);
      border: 1px solid var(--border);
      border-radius: var(--radius);
      padding: 12px 12px 14px;
      overflow: hidden;
    }

    /* ── Gates row ────────────────────────────────────────────── */
    .gates-row {
      display: flex;
      gap: 3px;
      align-items: flex-end;
    }

    .gate-col {
      flex: 1;
      min-width: 0;
      display: flex;
      flex-direction: column;
      align-items: center;
    }

    .gate-label {
      font-size: 10px;
      color: var(--muted);
      text-align: center;
      margin-bottom: 5px;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
      width: 100%;
      padding: 0 1px;
      transition: color 0.2s;
    }
    .gate-label.active    { color: var(--accent); font-weight: 600; }
    .gate-label.home-lbl  { font-style: italic; }

    .gate-box {
      width: 100%;
      height: 40px;
      border: 1.5px solid var(--border);
      border-radius: 3px 3px 0 0;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      position: relative;
      background: var(--bg);
      transition: border-color 0.2s, background 0.2s;
    }
    .gate-box.active {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 10%, var(--bg));
    }
    .gate-box.home-gate { border-style: dashed; }
    .gate-box.unhomed   { opacity: 0.3; }

    .outlet-dot {
      position: absolute;
      top: 4px; right: 4px;
      width: 5px; height: 5px;
      border-radius: 50%;
      background: var(--border);
      transition: background 0.3s;
    }
    .outlet-dot.tool-on   { background: var(--success); }
    .outlet-dot.tool-off  { background: var(--muted); }
    .outlet-dot.tool-dead { background: var(--danger); }

    .outlet-power {
      position: absolute;
      bottom: 3px;
      left: 0; right: 0;
      text-align: center;
      font-size: 9px;
      color: var(--muted);
      transition: color 0.2s;
    }
    .gate-box.active .outlet-power { color: var(--accent); }

    /* ── Neck (gate opening → slider body) ───────────────────── */
    .gate-neck {
      width: 35%;
      height: 8px;
      background: var(--border);
      transition: background 0.2s;
    }
    .gate-neck.active { background: var(--accent); }
    .gate-neck.home   { background: transparent; }

    /* ── Slider rail ──────────────────────────────────────────── */
    .slider-rail {
      position: relative;
      height: 22px;
    }

    .slider-plate {
      position: absolute;
      inset: 0;
      background: var(--surface);
      border: 1.5px solid var(--border);
      border-radius: 3px;
    }

    .slider-window {
      position: absolute;
      top: 0;
      height: 100%;
      border: 2px solid var(--accent);
      border-top: none;
      border-radius: 0 0 3px 3px;
      background: color-mix(in srgb, var(--accent) 12%, var(--bg));
      transition-property: left;
      transition-timing-function: linear;
    }
    .slider-window.unhomed {
      border-color: var(--muted);
      border-style: dashed;
      background: transparent;
    }

    /* ── Flow arrow ───────────────────────────────────────────── */
    .flow-section {
      position: relative;
      height: 30px;
    }

    .flow-line {
      position: absolute;
      top: 0; bottom: 7px;
      width: 2px;
      margin-left: -1px;
      background: var(--accent);
      transition-property: left;
      transition-timing-function: linear;
    }
    .flow-line.hidden { opacity: 0; }

    .flow-head {
      position: absolute;
      bottom: 0;
      margin-left: -5px;
      width: 0; height: 0;
      border-left:  5px solid transparent;
      border-right: 5px solid transparent;
      border-top:   7px solid var(--accent);
      transition-property: left;
      transition-timing-function: linear;
    }
    .flow-head.hidden { opacity: 0; }

    /* ── Dust collector entity ────────────────────────────────── */
    .dc-wrap {
      display: flex;
      justify-content: center;
      margin-top: 0;
    }

    .dc-entity {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 9px 18px;
      border: 1.5px solid var(--border);
      border-radius: 10px;
      background: var(--bg);
      min-width: 160px;
      justify-content: center;
      transition: border-color 0.3s, background 0.3s;
    }
    .dc-entity.dc-on {
      border-color: var(--success);
      background: color-mix(in srgb, var(--success) 10%, var(--bg));
    }

    .dc-dot {
      width: 9px; height: 9px;
      border-radius: 50%;
      background: var(--muted);
      flex-shrink: 0;
      transition: background 0.3s;
    }
    .dc-entity.dc-on .dc-dot { background: var(--success); }

    .dc-text { line-height: 1.3; }
    .dc-name {
      font-size: 13px;
      font-weight: 600;
      color: var(--muted);
      transition: color 0.3s;
    }
    .dc-entity.dc-on .dc-name { color: var(--success); }

    .dc-sub {
      font-size: 10px;
      color: var(--muted);
      transition: color 0.3s;
    }
    .dc-entity.dc-on .dc-sub { color: var(--success); }

    /* ── Manual override badge ────────────────────────────────── */
    .override-row {
      display: flex;
      justify-content: center;
      margin: 6px 0 8px;
    }
    .override-badge {
      font-size: 10px;
      font-weight: 700;
      letter-spacing: 0.06em;
      padding: 2px 8px;
      border-radius: 4px;
      border: 1px solid var(--accent);
      color: var(--accent);
      background: color-mix(in srgb, var(--accent) 12%, var(--bg));
    }

    /* ── Unhomed hint ─────────────────────────────────────────── */
    .unhomed-hint {
      font-size: 10px;
      color: var(--muted);
      text-align: center;
      margin-top: 8px;
      font-style: italic;
    }

    /* ── Not-configured placeholder ───────────────────────────── */
    .viz-placeholder {
      background: var(--surface);
      border: 1px dashed var(--border);
      border-radius: var(--radius);
      padding: 18px 16px;
      text-align: center;
      font-size: 13px;
      color: var(--muted);
      font-style: italic;
    }
  `],
  template: `
    <!-- Gate count not yet set — show placeholder rather than a broken empty rail -->
    <div class="viz-placeholder" *ngIf="!isReady">
      Gate layout will appear after setup
    </div>

    <div class="viz-card" *ngIf="isReady">

      <!-- Gate columns -->
      <div class="gates-row">
        <div class="gate-col" *ngFor="let g of gates">

          <div class="gate-label"
               [class.active]="g.index === sliderDisplayStop && isHomed"
               [class.home-lbl]="g.isHome">
            {{ g.label }}
          </div>

          <div class="gate-box"
               [class.active]="g.index === sliderDisplayStop && isHomed"
               [class.home-gate]="g.isHome"
               [class.unhomed]="!isHomed && !g.isHome">
            <div *ngIf="g.outlet"
                 class="outlet-dot"
                 [class.tool-on]="g.outlet.active"
                 [class.tool-off]="!g.outlet.active && g.outlet.reachable"
                 [class.tool-dead]="!g.outlet.reachable">
            </div>
            <div *ngIf="g.outlet" class="outlet-power">
              {{ g.outlet.powerW | number:'1.0-0' }} W
            </div>
          </div>

          <div class="gate-neck"
               [class.active]="g.index === sliderDisplayStop && isHomed"
               [class.home]="g.isHome">
          </div>

        </div>
      </div>

      <!-- Slider rail -->
      <div class="slider-rail">
        <div class="slider-plate"></div>
        <div class="slider-window"
             [class.unhomed]="!isHomed"
             [style.left]="sliderLeftPct + '%'"
             [style.width]="sliderWidthPct + '%'"
             [style.transition-duration]="sliderTransitionSec + 's'">
        </div>
      </div>

      <!-- Flow arrow down to collector -->
      <div class="flow-section">
        <div class="flow-line"
             [class.hidden]="!isHomed || isAtHome"
             [style.left]="arrowCenterPct + '%'"
             [style.transition-duration]="sliderTransitionSec + 's'">
        </div>
        <div class="flow-head"
             [class.hidden]="!isHomed || isAtHome"
             [style.left]="arrowCenterPct + '%'"
             [style.transition-duration]="sliderTransitionSec + 's'">
        </div>
      </div>

      <!-- Manual override badge -->
      <div class="override-row" *ngIf="isManualOverride">
        <span class="override-badge">MANUAL OVERRIDE</span>
      </div>

      <!-- Dust collector entity -->
      <div class="dc-wrap">
        <div class="dc-entity" [class.dc-on]="dcOn">
          <div class="dc-dot"></div>
          <div class="dc-text">
            <div class="dc-name">Dust collector</div>
            <div class="dc-sub">
              {{ dcOn ? 'ON' : 'OFF' }}{{ dcOn ? ' · auto' : '' }}
            </div>
          </div>
        </div>
      </div>

      <div class="unhomed-hint" *ngIf="!isHomed">
        position unknown — home before moving
      </div>

    </div>
  `
})
export class ManifoldVisualizerComponent implements OnInit, OnDestroy {

  @Input() dcOn = false;
  /** When true, home is on the right — gates render right-to-left and slider animates accordingly. */
  @Input() homeOnRight = false;

  status: SystemStatus | null = null;

  /** Stop index the slider is currently animating toward (or resting at). */
  sliderDisplayStop = 0;
  /** CSS transition-duration in seconds applied to the slider window + arrow. */
  sliderTransitionSec = 0; // 0 = instant snap on first render

  private prevState   = '';
  private moveStartMs = 0;
  private moveStartStopIdx = 0;
  /** Estimated mm/s — auto-calibrates after each real move completes. */
  private speedMmPerSec = 50;
  private initialized = false;
  private sub?: Subscription;

  constructor(private api: ApiService) {}

  ngOnInit() {
    this.sub = this.api.status$.subscribe(s => {
      if (!s) return;
      if (!this.initialized) {
        // First frame: snap to current position with no animation.
        this.sliderDisplayStop  = Math.max(0, s.currentStop);
        this.sliderTransitionSec = 0;
        this.prevState           = s.state;
        this.initialized         = true;
      }
      this.updateSlider(s);
      this.status = s;
    });
  }

  ngOnDestroy() { this.sub?.unsubscribe(); }

  // ── Slider animation ──────────────────────────────────────────────────────────

  private updateSlider(s: SystemStatus) {
    const isMoving  = s.state === 'MOVING';
    const wasMoving = this.prevState === 'MOVING';
    this.prevState  = s.state;

    if (isMoving && s.targetStop !== this.sliderDisplayStop) {
      // New move or target changed — calculate duration from mm distance.
      const fromMm = this.stopMm(s, s.currentStop);
      const toMm   = this.stopMm(s, s.targetStop);
      const distMm = Math.abs(toMm - fromMm);
      const dur    = distMm > 0 ? distMm / this.speedMmPerSec : 0.5;

      this.sliderTransitionSec = dur;
      this.sliderDisplayStop   = s.targetStop;
      this.moveStartMs         = Date.now();
      this.moveStartStopIdx    = s.currentStop;

    } else if (!isMoving && wasMoving) {
      // Move just completed — auto-calibrate speed from actual elapsed time.
      if (this.moveStartMs > 0) {
        const actualSec = (Date.now() - this.moveStartMs) / 1000;
        const fromMm    = this.stopMm(s, this.moveStartStopIdx);
        const toMm      = this.stopMm(s, s.currentStop);
        const distMm    = Math.abs(toMm - fromMm);
        if (distMm > 5 && actualSec > 0.2) {
          // Exponential moving average; weight actual measurement heavily.
          this.speedMmPerSec = this.speedMmPerSec * 0.3 + (distMm / actualSec) * 0.7;
        }
        this.moveStartMs = 0;
      }
      this.sliderTransitionSec = 0.3;
      this.sliderDisplayStop   = s.currentStop;

    } else if (!isMoving && this.sliderDisplayStop !== s.currentStop) {
      // Steady state drift correction (e.g. after reconnect).
      this.sliderTransitionSec = 0.3;
      this.sliderDisplayStop   = s.currentStop;
    }
  }

  private stopMm(s: SystemStatus, idx: number): number {
    const stops = s.stops ?? [];
    const found = stops.find((st: StopInfo) => st.index === idx) ?? stops[idx];
    return parseFloat(found?.mm ?? '0');
  }

  // ── Computed layout ───────────────────────────────────────────────────────────

  get gates(): GateInfo[] {
    const s = this.status;
    const n = this.numGates;
    const result: GateInfo[] = [];
    for (let i = 0; i < n; i++) {
      const outlet = s?.outlets?.find((o: OutletStatus) => o.stop === i) ?? null;
      const stop   = (s?.stops ?? []).find((st: StopInfo) => st.index === i)
                     ?? (s?.stops ?? [])[i];
      result.push({
        index:  i,
        label:  i === 0 ? 'home' : (outlet?.name ?? `S${i}`),
        outlet,
        isHome: i === 0,
        stopMm: parseFloat(stop?.mm ?? '0'),
      });
    }
    // When home is on the right, render gates in reverse order so stop 0
    // appears on the right and stops increase left-to-right visually becomes
    // right-to-left — matching the physical layout.
    return this.homeOnRight ? [...result].reverse() : result;
  }

  /** True once the device has reported a gate count > 0 (set via setup agent). */
  get isReady(): boolean {
    return (this.api.deviceInfo?.numStops ?? 0) > 0;
  }

  get numGates(): number {
    // Always use the runtime-configured count from /api/info (numStops = g_numActiveStops).
    // status.stops has NUM_STOPS+1 entries regardless of how many are active — never
    // use its length for layout sizing.
    return (this.api.deviceInfo?.numStops ?? 0) + 1; // +1 for home (stop 0)
  }

  /** Width of the slider window as a percentage of rail width. */
  get sliderWidthPct(): number {
    return 100 / this.numGates;
  }

  /** Left edge of the slider window as a percentage of rail width. */
  get sliderLeftPct(): number {
    const stop = Math.max(0, this.sliderDisplayStop);
    // When home is on the right the gates array is reversed, so the slider
    // window must count from the opposite end.
    const col = this.homeOnRight ? (this.numGates - 1 - stop) : stop;
    return col / this.numGates * 100;
  }

  /** Center of the slider window — used to position the flow arrow. */
  get arrowCenterPct(): number {
    const stop = Math.max(0, this.sliderDisplayStop);
    const col  = this.homeOnRight ? (this.numGates - 1 - stop) : stop;
    return (col + 0.5) / this.numGates * 100;
  }

  get isAtHome():        boolean { return this.sliderDisplayStop === 0; }
  get isHomed():         boolean { return this.status?.homed ?? false; }
  get isManualOverride():boolean { return this.status?.manualOverride ?? false; }
}
