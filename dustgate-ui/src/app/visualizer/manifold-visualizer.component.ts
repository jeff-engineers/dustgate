import { Component, Input, OnInit, OnDestroy } from '@angular/core';
import { CommonModule, DecimalPipe } from '@angular/common';
import { Subscription } from 'rxjs';
import { ApiService, SystemStatus, OutletStatus, StopInfo } from '../services/api.service';
import { HardwareProfileService } from '../services/hardware-profile.service';

interface GateInfo {
  index:  number;
  label:  string;
  outlet: OutletStatus | null;
  isHome: boolean;
  isLocated: boolean;   // position has been saved — false = reserve slot, draw nothing
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
    /* Reserve the column's width but draw nothing until the gate is located. */
    .gate-box.pending {
      border-color: transparent;
      background: transparent;
    }

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
    .gate-neck.pending { background: transparent; }

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
    /* An elbow connector: drops from the moving gate, bridges over to the
       dust collector's fixed center position, then drops into it. This way
       the arrow always visually terminates at the collector regardless of
       which gate is active. */
    .flow-section {
      position: relative;
      height: 30px;
    }

    .flow-drop-gate {
      position: absolute;
      top: 0;
      height: 50%;
      width: 2px;
      margin-left: -1px;
      background: var(--accent);
      transition-property: left;
      transition-timing-function: linear;
    }
    .flow-drop-gate.hidden { opacity: 0; }

    .flow-bridge {
      position: absolute;
      top: 50%;
      height: 2px;
      margin-top: -1px;
      background: var(--accent);
      transition-property: left, width;
      transition-timing-function: linear;
    }
    .flow-bridge.hidden { opacity: 0; }

    .flow-drop-dc {
      position: absolute;
      top: 50%;
      bottom: 7px;
      left: 50%;
      width: 2px;
      margin-left: -1px;
      background: var(--accent);
    }
    .flow-drop-dc.hidden { opacity: 0; }

    .flow-head {
      position: absolute;
      bottom: 0;
      left: 50%;
      margin-left: -5px;
      width: 0; height: 0;
      border-left:  5px solid transparent;
      border-right: 5px solid transparent;
      border-top:   7px solid var(--accent);
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

      <!-- Gate columns. Unlocated gates reserve their rail slot but stay blank
           until their position is saved, so the layout doesn't pre-populate. -->
      <div class="gates-row">
        <div class="gate-col" *ngFor="let g of gates">

          <div class="gate-label"
               [class.active]="g.index === sliderDisplayStop && isHomed"
               [class.home-lbl]="g.isHome">
            {{ (g.isHome || g.isLocated) ? g.label : '' }}
          </div>

          <div class="gate-box"
               [class.active]="g.index === sliderDisplayStop && isHomed"
               [class.home-gate]="g.isHome"
               [class.pending]="!g.isHome && !g.isLocated"
               [class.unhomed]="!isHomed && !g.isHome && g.isLocated">
            <ng-container *ngIf="g.isHome || g.isLocated">
              <div *ngIf="g.outlet?.hasSwitch"
                   class="outlet-dot"
                   [class.tool-on]="g.outlet!.active"
                   [class.tool-off]="!g.outlet!.active && g.outlet!.reachable"
                   [class.tool-dead]="!g.outlet!.reachable">
              </div>
              <div *ngIf="g.outlet?.hasSwitch" class="outlet-power">
                {{ g.outlet!.powerW | number:'1.0-0' }} W
              </div>
            </ng-container>
          </div>

          <div class="gate-neck"
               [class.active]="g.index === sliderDisplayStop && isHomed"
               [class.home]="g.isHome"
               [class.pending]="!g.isHome && !g.isLocated">
          </div>

        </div>
      </div>

      <!-- Slider rail -->
      <div class="slider-rail">
        <div class="slider-plate"></div>
        <div class="slider-window"
             [class.unhomed]="!isHomed"
             [style.left]="displaySliderLeftPct + '%'"
             [style.width]="sliderWidthPct + '%'"
             [style.transition-duration]="(isJogging ? jogTransitionSec : sliderTransitionSec) + 's'">
        </div>
      </div>

      <!-- Flow arrow down to collector — drops from the active gate, bridges
           over to the collector's fixed center, then drops into it -->
      <div class="flow-section">
        <div class="flow-drop-gate"
             [class.hidden]="!isHomed || (isAtHome && !isJogging)"
             [style.left]="displayArrowCenterPct + '%'"
             [style.transition-duration]="(isJogging ? jogTransitionSec : sliderTransitionSec) + 's'">
        </div>
        <div class="flow-bridge"
             [class.hidden]="!isHomed || (isAtHome && !isJogging)"
             [style.left]="flowBridgeLeftPct + '%'"
             [style.width]="flowBridgeWidthPct + '%'"
             [style.transition-duration]="(isJogging ? jogTransitionSec : sliderTransitionSec) + 's'">
        </div>
        <div class="flow-drop-dc"    [class.hidden]="!isHomed || (isAtHome && !isJogging)"></div>
        <div class="flow-head"      [class.hidden]="!isHomed || (isAtHome && !isJogging)"></div>
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

  constructor(private api: ApiService, private hardwareProfile: HardwareProfileService) {}

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
    return this.visualColumns.map((i: number) => this.gateInfoFor(i));
  }

  private gateInfoFor(i: number): GateInfo {
    const s = this.status;
    const outlet = s?.outlets?.find((o: OutletStatus) => o.stop === i) ?? null;
    const stop   = (s?.stops ?? []).find((st: StopInfo) => st.index === i);
    return {
      index:  i,
      label:  i === 0 ? 'home' : (outlet?.name ?? `Gate${i}`),
      outlet,
      isHome: i === 0,
      isLocated: this.isStopSaved(i),
      stopMm: parseFloat(stop?.mm ?? '0'),
    };
  }

  // ── Physical column ordering ───────────────────────────────────────────────────
  // The rail is a physical space: columns are laid out by actual distance from
  // home, not by the order the user happened to configure gates in. So if Gate 2
  // was saved closer to the endstop than Gate 1, its box renders to the left of
  // Gate 1's. Labels travel with their box; slider/marker/arrow all key off a
  // stop's *rank* in this order, keeping the monotonic mm↔column relationship the
  // position math depends on.

  /** Stop indices ordered by physical distance from home (home first). Saved
   *  gates sort by mm; gates not yet saved keep index order at the far end
   *  until they're placed. */
  private get physicalOrder(): number[] {
    const gateIdxs: number[] = [];
    for (let i = 1; i < this.numGates; i++) gateIdxs.push(i);
    const saved   = gateIdxs.filter(i => this.isStopSaved(i))
                            .sort((a, b) => this.stopMmForIndex(a) - this.stopMmForIndex(b));
    const unsaved = gateIdxs.filter(i => !this.isStopSaved(i));
    return [0, ...saved, ...unsaved];
  }

  /** Visual left-to-right column order (physical order, flipped for home-on-right). */
  private get visualColumns(): number[] {
    return this.homeOnRight ? [...this.physicalOrder].reverse() : this.physicalOrder;
  }

  /** Physical rank of a stop index (0 = home, increasing with distance from home). */
  private physicalRankOf(stopIndex: number): number {
    const r = this.physicalOrder.indexOf(stopIndex);
    return r < 0 ? 0 : r;
  }

  /** Left edge % of the column at a (possibly fractional) physical rank. */
  private leftPctForRank(rank: number): number {
    const visualCol = this.homeOnRight ? (this.numGates - 1 - rank) : rank;
    return visualCol / this.numGates * 100;
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
    return this.leftPctForRank(this.physicalRankOf(Math.max(0, this.sliderDisplayStop)));
  }

  /** Center of the slider window — used to position the flow arrow. */
  get arrowCenterPct(): number {
    return this.leftPctForRank(this.physicalRankOf(Math.max(0, this.sliderDisplayStop))) + this.sliderWidthPct / 2;
  }

  get isAtHome():        boolean { return this.sliderDisplayStop === 0; }
  get isHomed():         boolean { return this.status?.homed ?? false; }
  get isManualOverride():boolean { return this.status?.manualOverride ?? false; }

  // ── Jog tracking ──────────────────────────────────────────────────────────────
  // Raw jogging (used during setup, before a stop is saved) never changes
  // currentStop/targetStop, so the discrete slider window can't track it by
  // itself. While jogging, the slider window's left edge instead follows
  // positionMM continuously, interpolated between the nearest saved stop and
  // the next (possibly not-yet-saved) column.

  /** CSS transition duration applied to the slider window while jogging. */
  readonly jogTransitionSec = 1.0;

  private stopMmForIndex(idx: number): number {
    const stops = this.status?.stops ?? [];
    const found = stops.find((s: StopInfo) => s.index === idx);
    return parseFloat(found?.mm ?? '0');
  }

  /**
   * True if this index has an actual saved position. Home (0) is always known
   * at mm 0; every other slot's mm is null until it's saved, so a non-null mm
   * is what distinguishes "saved" from "not yet".
   */
  private isStopSaved(idx: number): boolean {
    if (idx === 0) return true;
    const found = (this.status?.stops ?? []).find((s: StopInfo) => s.index === idx);
    return found?.mm != null;
  }

  /** Interpolated physical rank for a raw mm position. Uses saved stops as
   *  anchors (home at mm 0, each saved gate at its mm) and extrapolates past the
   *  last one by the estimated spacing. Because ranks come from physicalOrder,
   *  they increase monotonically with mm, so the interpolation is well-behaved
   *  even when gates were configured out of order. */
  private physicalRankForPos(pos: number): number {
    const anchors: { mm: number; rank: number }[] = [{ mm: 0, rank: 0 }];
    for (let i = 1; i < this.numGates; i++) {
      if (this.isStopSaved(i)) {
        anchors.push({ mm: this.stopMmForIndex(i), rank: this.physicalRankOf(i) });
      }
    }
    anchors.sort((a, b) => a.mm - b.mm);

    let lower = anchors[0];
    let upper: { mm: number; rank: number } | null = null;
    for (const a of anchors) {
      if (a.mm <= pos) lower = a;
      else { upper = a; break; }
    }
    if (upper) {
      const span = upper.mm - lower.mm;
      const t = span > 0 ? (pos - lower.mm) / span : 0;
      return lower.rank + t * (upper.rank - lower.rank);
    }
    // Beyond the last saved stop — extrapolate one column per estimated span.
    return lower.rank + (pos - lower.mm) / this.estimatedColumnSpanMm;
  }

  /** Average spacing between saved stops — used to scale the not-yet-saved segment. */
  private get estimatedColumnSpanMm(): number {
    const mms = (this.status?.stops ?? [])
      .filter((s: StopInfo) => s.index > 0 && s.index < this.numGates && this.isStopSaved(s.index))
      .map((s: StopInfo) => parseFloat(s.mm ?? '0'))
      .sort((a, b) => a - b);
    if (mms.length === 0) return this.hardwareProfile.expectedGateSpacingMm; // no calibration data yet
    if (mms.length === 1) return Math.max(mms[0], 20);
    const gaps = mms.slice(1).map((mm, i) => mm - mms[i]).filter(g => g > 0);
    return gaps.length ? gaps.reduce((a, b) => a + b, 0) / gaps.length : Math.max(mms[0], 20);
  }

  /** True while the actuator sits away from wherever the discrete slider is resting. */
  get isJogging(): boolean {
    if (!this.isHomed || this.status?.positionMM == null) return false;
    const settledMm = this.stopMmForIndex(this.sliderDisplayStop);
    return Math.abs(this.status.positionMM - settledMm) > 0.5;
  }

  /** Left edge of the slider window (as a %) while jogging, continuously interpolated. */
  get jogWindowLeftPct(): number {
    const rank = Math.max(0, Math.min(this.numGates - 1,
      this.physicalRankForPos(this.status?.positionMM ?? 0)));
    return this.leftPctForRank(rank);
  }

  /** Left edge actually applied to the slider window — continuous while jogging, discrete otherwise. */
  get displaySliderLeftPct(): number {
    return this.isJogging ? this.jogWindowLeftPct : this.sliderLeftPct;
  }

  /** Center of the slider window, following the same discrete/continuous split — used for the flow arrow. */
  get displayArrowCenterPct(): number {
    return this.isJogging ? this.jogWindowLeftPct + this.sliderWidthPct / 2 : this.arrowCenterPct;
  }

  /** Horizontal center of the dust collector box — always centered under the rail (see .dc-wrap). */
  readonly dcCenterPct = 50;

  /** Left edge of the horizontal bridge connecting the active gate to the collector. */
  get flowBridgeLeftPct(): number {
    return Math.min(this.displayArrowCenterPct, this.dcCenterPct);
  }

  /** Width of the horizontal bridge connecting the active gate to the collector. */
  get flowBridgeWidthPct(): number {
    return Math.abs(this.displayArrowCenterPct - this.dcCenterPct);
  }
}
