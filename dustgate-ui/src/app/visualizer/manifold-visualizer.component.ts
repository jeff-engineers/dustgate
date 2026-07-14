import { Component, Input, OnInit, OnDestroy, ViewChild, ElementRef, AfterViewInit } from '@angular/core';
import { CommonModule, DecimalPipe } from '@angular/common';
import { Subscription } from 'rxjs';
import { ApiService, SystemStatus, OutletStatus, StopInfo } from '../services/api.service';

interface GateInfo {
  index:  number;
  label:  string;
  outlet: OutletStatus | null;
  isLocated: boolean;   // position has been saved — false = reserve slot, draw nothing
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

    /* ── Scroll viewport (gates only) ────────────────────────────────── */
    .rail-scroll {
      overflow-x: auto;
      overflow-y: hidden;
      scrollbar-width: thin;
      position: relative;
    }

    /* Gate content only grows wider than the card once there are more gates
       than comfortably fit at this minimum column width — up to that point
       columns still stretch to fill. */
    .gates-row {
      display: flex;
      gap: 3px;
      align-items: flex-end;
      min-width: 100%;
      width: max-content;
    }

    .gate-col {
      flex: 1;
      min-width: 46px;
      display: flex;
      flex-direction: column;
      align-items: center;
    }

    /* ── Edge indicators — point toward the active gate when it has been
       scrolled out of view, keeping the "active gate ↔ collector" link
       legible even while the rail is scrolled. ─────────────────────── */
    .edge-hint {
      position: absolute;
      top: 0;
      bottom: 0;
      width: 22px;
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 13px;
      font-weight: 700;
      color: var(--accent);
      background: linear-gradient(to right, color-mix(in srgb, var(--bg) 85%, transparent), transparent);
      pointer-events: none;
    }
    .edge-hint.left  { left: 0; }
    .edge-hint.right { right: 0; transform: scaleX(-1); }

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
    }
    .gate-label.active { color: var(--accent); font-weight: 600; }

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
    }
    .gate-box.active {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 10%, var(--bg));
    }
    .gate-box.unhomed { opacity: 0.3; }
    /* Reserve the column's width but draw nothing until the gate is located. */
    .gate-box.pending {
      border-color: transparent;
      background: transparent;
    }

    /* ── Interactive tap targets (dashboard use) ─────────────────────────── */
    .gate-col.clickable { cursor: pointer; }
    .gate-col.clickable:active .gate-box { opacity: 0.7; }
    .home-pill.clickable { cursor: pointer; }
    .home-pill.clickable:active { opacity: 0.7; }
    .dc-entity.clickable { cursor: pointer; }
    .dc-entity.clickable:active { opacity: 0.7; }

    .outlet-dot {
      position: absolute;
      top: 4px; right: 4px;
      width: 5px; height: 5px;
      border-radius: 50%;
      background: var(--border);
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
    }
    .gate-box.active .outlet-power { color: var(--accent); }

    /* ── Neck (gate opening → flow arrow) ───────────────────── */
    .gate-neck {
      width: 35%;
      height: 8px;
      background: var(--border);
    }
    .gate-neck.active { background: var(--accent); }
    .gate-neck.pending { background: transparent; }

    /* ── Flow arrow ───────────────────────────────────────────── */
    /* An elbow connector: drops from the active gate, bridges over to the
       dust collector's fixed center position, then drops into it. Positions
       snap instantly — no travel animation. */
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
    }
    .flow-drop-gate.hidden { opacity: 0; }

    .flow-bridge {
      position: absolute;
      top: 50%;
      height: 2px;
      margin-top: -1px;
      background: var(--accent);
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

    /* ── Status row: home indicator + dust collector entity ─────────────── */
    /* Both are system-status readouts rather than gates, so they live
       together below the rail. Home still leads/trails on whichever side
       the endstop actually is, but as a compact badge rather than a
       full-height column jammed against the gate rail. */
    .dc-wrap {
      display: flex;
      justify-content: center;
      align-items: stretch;
      gap: 8px;
      margin-top: 0;
    }
    .dc-wrap.home-right { flex-direction: row-reverse; }

    .home-pill {
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 9px 14px;
      border: 1.5px solid var(--border);
      border-radius: 10px;
      background: var(--bg);
      transition: border-color 0.3s, background 0.3s;
    }
    .home-pill.homed {
      border-color: var(--success);
      background: color-mix(in srgb, var(--success) 10%, var(--bg));
    }
    /* A homing cycle is actively running — distinct from the settled
       "homed" state, which reflects a completed, known-good home. */
    .home-pill.homing-now {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 10%, var(--bg));
    }

    .home-dot {
      width: 9px; height: 9px;
      border-radius: 50%;
      background: var(--muted);
      flex-shrink: 0;
      transition: background 0.3s;
    }
    .home-pill.homed .home-dot { background: var(--success); }
    .home-pill.homing-now .home-dot { background: var(--accent); }

    .home-label {
      font-size: 13px;
      font-weight: 600;
      color: var(--muted);
      transition: color 0.3s;
    }
    .home-pill.homed .home-label { color: var(--success); }
    .home-pill.homing-now .home-label { color: var(--accent); }

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

      <!-- Scrollable rail: once there are more gates than fit comfortably,
           this scrolls horizontally instead of squeezing columns further. -->
      <div class="rail-scroll" #railScroll (scroll)="recomputeArrow()">
        <div class="gates-row">
          <div class="gate-col"
               *ngFor="let g of gates"
               [attr.data-gate-index]="g.index"
               [class.clickable]="interactive && g.isLocated"
               (click)="onGateClick(g)">

            <div class="gate-label" [class.active]="g.index === activeStop">
              {{ g.isLocated ? g.label : '' }}
            </div>

            <div class="gate-box"
                 [class.active]="g.index === activeStop"
                 [class.pending]="!g.isLocated"
                 [class.unhomed]="!isHomed && g.isLocated">
              <ng-container *ngIf="g.isLocated">
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
                 [class.active]="g.index === activeStop"
                 [class.pending]="!g.isLocated">
            </div>

          </div>
        </div>

        <!-- Edge hints — point toward the active gate once it's scrolled out
             of view, so the active-gate ↔ collector link stays legible. -->
        <div class="edge-hint left"  *ngIf="activeGateOffscreen === 'left'">‹</div>
        <div class="edge-hint right" *ngIf="activeGateOffscreen === 'right'">›</div>
      </div>

      <!-- Flow arrow down to collector — drops from the active gate (clamped to
           the visible edge if scrolled out of view), bridges over to the
           collector's fixed center, then drops into it. Snaps, no animation. -->
      <div class="flow-section">
        <div class="flow-drop-gate"
             [class.hidden]="!showFlowArrow"
             [style.left]="arrowViewportPct + '%'">
        </div>
        <div class="flow-bridge"
             [class.hidden]="!showFlowArrow"
             [style.left]="flowBridgeLeftPct + '%'"
             [style.width]="flowBridgeWidthPct + '%'">
        </div>
        <div class="flow-drop-dc" [class.hidden]="!showFlowArrow"></div>
        <div class="flow-head"   [class.hidden]="!showFlowArrow"></div>
      </div>

      <!-- Manual override badge -->
      <div class="override-row" *ngIf="isManualOverride">
        <span class="override-badge">MANUAL OVERRIDE</span>
      </div>

      <!-- Status row: home indicator + dust collector entity. Home still
           leads/trails on whichever side the endstop is on, but as a compact
           badge next to the collector rather than a column jammed against
           the gate rail. -->
      <div class="dc-wrap" [class.home-right]="homeOnRight">
        <div class="home-pill"
             [class.homed]="isHomed"
             [class.homing-now]="!isHomed && isHomingNow"
             [class.clickable]="interactive"
             (click)="onHomeClick()"
             [title]="isHomed ? 'Homed' : (isHomingNow ? 'Homing…' : 'Not homed')">
          <div class="home-dot"></div>
          <span class="home-label">Home</span>
        </div>

        <div class="dc-entity"
             [class.dc-on]="dcOn"
             [class.clickable]="interactive && dcConfigured"
             (click)="onDcClick()">
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
      <div class="unhomed-hint" *ngIf="interactive && isHomed">
        tap a gate to move it
      </div>

    </div>
  `
})
export class ManifoldVisualizerComponent implements OnInit, OnDestroy, AfterViewInit {

  @ViewChild('railScroll') railScroll?: ElementRef<HTMLElement>;

  @Input() dcOn = false;
  /** True once a dust-collector plug is assigned — gates whether tapping the DC entity does anything. */
  @Input() dcConfigured = false;
  /**
   * When true, this becomes the actual dashboard control surface: tapping a
   * located gate moves to it, tapping home homes, tapping the dust collector
   * toggles it. False in the setup wizards, where the rail is a passive
   * position readout.
   */
  @Input() interactive = false;
  /** When true, home is on the right — the home pill and gate order flip accordingly. */
  @Input() homeOnRight = false;
  /** Unused now that travel is never animated; kept so existing callers don't break. */
  @Input() liveTravel = true;

  status: SystemStatus | null = null;

  private sub?: Subscription;
  private prevActiveStop = -1;

  /** 'left' | 'right' | null — which edge the active gate is scrolled behind, if any. */
  activeGateOffscreen: 'left' | 'right' | null = null;
  /** Left position (%) of the flow arrow's drop from the gate, clamped to the visible rail viewport. */
  arrowViewportPct = 50;

  constructor(private api: ApiService) {}

  ngOnInit() {
    this.sub = this.api.status$.subscribe(s => {
      this.status = s;
      if (s && this.activeStop !== this.prevActiveStop) {
        this.prevActiveStop = this.activeStop;
        setTimeout(() => this.scrollActiveIntoView(), 0);
      } else {
        setTimeout(() => this.recomputeArrow(), 0);
      }
    });
  }

  ngAfterViewInit() {
    setTimeout(() => this.recomputeArrow(), 0);
  }

  ngOnDestroy() { this.sub?.unsubscribe(); }

  // ── Computed layout ───────────────────────────────────────────────────────────

  get gates(): GateInfo[] {
    const order: number[] = [];
    for (let i = 1; i < this.numGates + 1; i++) order.push(i);
    if (this.homeOnRight) order.reverse();
    return order.map(i => this.gateInfoFor(i));
  }

  private gateInfoFor(i: number): GateInfo {
    const s = this.status;
    const outlet = s?.outlets?.find((o: OutletStatus) => o.stop === i) ?? null;
    return {
      index:  i,
      label:  outlet?.name ?? `Gate${i}`,
      outlet,
      isLocated: this.isStopSaved(i),
    };
  }

  /** True once the device has reported a gate count > 0 (set via setup agent). */
  get isReady(): boolean {
    return (this.api.deviceInfo?.numStops ?? 0) > 0;
  }

  /** Number of real tool gates (excludes home). */
  get numGates(): number {
    return this.api.deviceInfo?.numStops ?? 0;
  }

  private isStopSaved(idx: number): boolean {
    const found = (this.status?.stops ?? []).find((s: StopInfo) => s.index === idx);
    return found?.mm != null;
  }

  /** Stop the actuator is currently at/moving to (0 = home). */
  get activeStop(): number { return Math.max(0, this.status?.currentStop ?? 0); }

  get isAtHome():        boolean { return this.activeStop === 0; }
  get isHomed():         boolean { return this.status?.homed ?? false; }
  get isManualOverride():boolean { return this.status?.manualOverride ?? false; }
  /** True while a homing cycle is actively running (device is pressing toward the endstop). */
  get isHomingNow():     boolean { return this.status?.state === 'HOMING'; }

  /**
   * Guards the flow arrow beyond just isHomed/isAtHome — those two alone
   * previously let the arrow render pointing at a gate index with no
   * corresponding gate element (e.g. right after "Start Over," when the
   * device briefly reported homed=true at a stale gate while numGates had
   * already dropped to 0). Bounding activeStop to the configured gate range
   * closed that gap, but a gate can be "in range" (1..numGates) without a
   * trained position yet (e.g. mid-wizard) or without a rendered DOM element
   * for it — either way there's nothing for the arrow to point at. Requiring
   * isStopSaved(activeStop) rules out the former; recomputeArrow()'s
   * gateEl-not-found fallback still covers the latter as a last resort, but
   * shouldn't be the primary guard.
   */
  get showFlowArrow(): boolean {
    return (
      this.isHomed &&
      !this.isAtHome &&
      this.activeStop >= 1 &&
      this.activeStop <= this.numGates &&
      this.isStopSaved(this.activeStop)
    );
  }

  // ── Interactive controls (dashboard use only — see `interactive` input) ────────

  onGateClick(g: GateInfo) {
    if (!this.interactive) return;
    if (g.isLocated) { this.api.moveToStop(g.index).catch(console.error); }
  }

  onHomeClick() {
    if (!this.interactive) return;
    this.api.home().catch(console.error);
  }

  onDcClick() {
    if (!this.interactive || !this.dcConfigured) return;
    this.api.setDustCollector(!this.dcOn).catch(console.error);
  }

  // ── Flow-arrow tracking ─────────────────────────────────────────────────────
  // The arrow drops from wherever the active gate actually sits on screen. If
  // the rail is scrolled so the active gate is off to one side, the drop point
  // clamps to that edge of the visible rail (and an edge hint lights up) so the
  // active-gate ↔ collector link is never just lost off-screen.

  /** Horizontal center of the dust collector box — always centered under the rail (see .dc-wrap). */
  readonly dcCenterPct = 50;

  recomputeArrow() {
    const railEl = this.railScroll?.nativeElement;
    if (!railEl) return;

    if (!this.showFlowArrow) {
      this.activeGateOffscreen = null;
      return;
    }

    const gateEl = railEl.querySelector<HTMLElement>(`[data-gate-index="${this.activeStop}"]`);
    if (!gateEl) {
      // Element not there yet (e.g. this ran before *ngFor re-rendered after
      // numGates changed) — don't leave arrowViewportPct pointing at whatever
      // stale position it last had. showFlowArrow is false-guarded above for
      // the steady-state case; this covers the transient one.
      this.activeGateOffscreen = null;
      this.arrowViewportPct = this.dcCenterPct;
      return;
    }

    const railRect = railEl.getBoundingClientRect();
    const gateRect = gateEl.getBoundingClientRect();
    const centerPx = gateRect.left + gateRect.width / 2 - railRect.left;

    if (centerPx < 0) {
      this.activeGateOffscreen = 'left';
    } else if (centerPx > railRect.width) {
      this.activeGateOffscreen = 'right';
    } else {
      this.activeGateOffscreen = null;
    }

    const clampedPx = Math.min(Math.max(centerPx, 0), railRect.width);
    this.arrowViewportPct = railRect.width > 0 ? (clampedPx / railRect.width) * 100 : 50;
  }

  private scrollActiveIntoView() {
    const railEl = this.railScroll?.nativeElement;
    if (!railEl || this.isAtHome) { this.recomputeArrow(); return; }
    const gateEl = railEl.querySelector<HTMLElement>(`[data-gate-index="${this.activeStop}"]`);
    gateEl?.scrollIntoView({ block: 'nearest', inline: 'center' });
    this.recomputeArrow();
  }

  /** Left edge of the horizontal bridge connecting the active gate to the collector. */
  get flowBridgeLeftPct(): number {
    return Math.min(this.arrowViewportPct, this.dcCenterPct);
  }

  /** Width of the horizontal bridge connecting the active gate to the collector. */
  get flowBridgeWidthPct(): number {
    return Math.abs(this.arrowViewportPct - this.dcCenterPct);
  }
}
