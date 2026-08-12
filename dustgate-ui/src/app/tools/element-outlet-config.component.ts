import { Component, EventEmitter, Input, OnInit, Output } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { DiscoveredOutlet } from '../services/api.service';
import { OutletPickerComponent } from './outlet-picker.component';

// ── Pairing one element with its smart plug ──────────────────────────────────
// Opened from the build canvas, alongside the gate config sheet, because a plug is
// a fact about the machine you just drew — same class of thing as which board
// drives a gate. The shop list deliberately doesn't offer this: that view is for
// switching things on while you're working, and it stays uncluttered by staying
// read-only.
//
// TWO ROLES, one sheet, because finding the plug is the identical job either way
// (switch the thing on, look for the one drawing power):
//
//   sensor — a TOOL's plug. We only WATCH it: crossing thresholdW is what tells
//            the brain that machine is running. Written to `sensor.outlet`.
//   switch — the COLLECTOR's plug. We COMMAND it, and never read it, so there's
//            no threshold to set. Written to `control.outlet`.
//
// See docs/topology-schema.md. Firmware picks both up on topology adopt
// (syncTopologyOutlets in firmware.ino) — the layout is what the poller
// and the blower switch are configured from.

interface RawEl { [k: string]: unknown; }

const DEFAULT_THRESHOLD = 50;

@Component({
  selector: 'app-element-outlet-config',
  standalone: true,
  imports: [CommonModule, FormsModule, OutletPickerComponent],
  styles: [`
    :host { display: block; }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; padding: 18px 16px; }
    .head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 15px; }
    .head .kind { font-size: 12.5px; color: var(--muted); }
    .badge { font-size: 11.5px; padding: 3px 10px; border-radius: 20px; }
    .badge.ok   { color: var(--success); background: rgba(60,190,110,0.12); }
    .badge.todo { color: var(--accent);  background: rgba(240,165,0,0.12); }

    .q { display: flex; align-items: center; justify-content: space-between; gap: 10px;
         padding: 11px 13px; background: var(--bg); border: 1px solid var(--border);
         border-radius: 12px; margin-bottom: 14px; font-size: 14px; }
    .q .yesno { display: flex; gap: 6px; flex-shrink: 0; }
    .q button { background: var(--surface); border: 1px solid var(--border); color: var(--muted);
                border-radius: 8px; padding: 6px 14px; font-size: 13px; }
    .q button.on { background: var(--success); border-color: var(--success); color: #05230f; font-weight: 600; }

    .paired { background: var(--bg); border: 1px solid var(--border); border-radius: 12px;
              padding: 11px 13px; margin-bottom: 14px; }
    .paired .r { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
    .paired .nm { font-size: 14px; font-weight: 500; }
    .paired .meta { font-size: 11.5px; color: var(--muted); margin-top: 3px; }
    .paired .chg { background: none; border: none; color: var(--accent); font-size: 12.5px; padding: 0; }

    .thresh { border-top: 1px solid var(--border); padding-top: 13px; margin-bottom: 4px; }
    .thresh .r { display: flex; align-items: center; justify-content: space-between; margin-bottom: 5px; font-size: 13px; }
    .thresh input[type=range] { width: 100%; }
    .thresh .why { font-size: 11.5px; color: var(--muted); margin-top: 4px; line-height: 1.5; }

    .manual { font-size: 12.5px; color: var(--muted); line-height: 1.6; margin: 0 0 4px; }

    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .save { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
  `],
  template: `
    <div class="card">
      <div class="head">
        <span class="kind">{{ name || 'Tool' }} · smart outlet</span>
        <span class="badge" [class.ok]="hasPlug && !!ip" [class.todo]="!hasPlug || !ip">
          {{ hasPlug && ip ? 'Paired' : 'Manual' }}
        </span>
      </div>

      <div class="q">
        <span>{{ isSwitch ? 'Switch the collector automatically?' : 'Smart outlet on this tool?' }}</span>
        <div class="yesno">
          <button [class.on]="hasPlug" (click)="hasPlug = true">Yes</button>
          <button [class.on]="!hasPlug" (click)="hasPlug = false">No</button>
        </div>
      </div>

      <ng-container *ngIf="hasPlug; else noPlug">
        <!-- Already chosen: show it and offer to change, rather than making someone
             re-identify a plug they've already found. -->
        <div class="paired" *ngIf="ip && !changing">
          <div class="r">
            <div>
              <div class="nm">{{ plugName || host || ip }}</div>
              <div class="meta">{{ host ? host + ' · ' : '' }}{{ ip }}</div>
            </div>
            <button class="chg" (click)="changing = true">Change</button>
          </div>
        </div>

        <app-outlet-picker *ngIf="!ip || changing"
                           [toolName]="name" [selectedIp]="ip"
                           [excludeIps]="excludeIps" [excludeReason]="excludeReason"
                           (picked)="pick($event)">
        </app-outlet-picker>

        <!-- Sensor role only. The collector's plug is commanded, never read, so a
             threshold would be a setting that does nothing. -->
        <div class="thresh" *ngIf="ip && !isSwitch">
          <div class="r"><span>Start collection above</span><b>{{ thresholdW }} W</b></div>
          <input type="range" min="0" max="1500" step="10" [(ngModel)]="thresholdW"/>
          <p class="why">Catches the motor, ignores standby draw.</p>
        </div>
      </ng-container>
      <ng-template #noPlug>
        <p class="manual">
          {{ isSwitch
             ? 'You\\'ll start the collector yourself. Gates still route to whatever tool is running.'
             : 'You\\'ll switch this one on yourself from the shop list.' }}
        </p>
      </ng-template>

      <div class="nav">
        <button class="back" (click)="cancelled.emit()">Cancel</button>
        <button class="save" (click)="save()">Save</button>
      </div>
    </div>
  `,
})
export class ElementOutletConfigComponent implements OnInit {
  /** The element from the topology — a tool, or the collector. Edited on a COPY:
   *  the caller splices the result back in, matching how the gate sheet works. */
  @Input({ required: true }) element!: RawEl;
  /** 'sensor' watches a tool's draw; 'switch' commands the collector. */
  @Input() mode: 'sensor' | 'switch' = 'sensor';
  /** Plugs already spoken for: other tools' sensors and the collector's own switch. */
  @Input() excludeIps: string[] = [];
  @Input() excludeReason: Record<string, string> = {};
  @Output() saved = new EventEmitter<RawEl>();
  @Output() cancelled = new EventEmitter<void>();

  name = '';
  hasPlug = false;
  changing = false;
  ip = '';
  host = '';
  plugName = '';
  gen = 2;
  thresholdW = DEFAULT_THRESHOLD;

  get isSwitch(): boolean { return this.mode === 'switch'; }
  /** Where the plug lives on the element, per role. */
  private get field(): string { return this.isSwitch ? 'control' : 'sensor'; }

  ngOnInit(): void {
    const outlet = (this.element[this.field] as RawEl | undefined)?.['outlet'] as RawEl | undefined;
    this.name       = (this.element['name'] as string) || (this.element['id'] as string) || '';
    this.ip         = (outlet?.['ip'] as string) ?? '';
    this.host       = (outlet?.['host'] as string) ?? '';
    this.gen        = (outlet?.['gen'] as number) ?? 2;
    this.thresholdW = (outlet?.['thresholdW'] as number) ?? DEFAULT_THRESHOLD;
    // Default to whatever this element already is, rather than assuming: re-opening a
    // manual tool shouldn't silently arm a plug picker it never had.
    this.hasPlug = !!this.ip;
  }

  pick(d: DiscoveredOutlet): void {
    this.ip = d.ip; this.host = d.hostname; this.gen = d.generation || 2;
    this.plugName = d.name || '';
    this.changing = false;
    // Seed the threshold from what the tool is drawing right now, ~10% under so it
    // clears standby but still trips. Only when it's still the untouched default —
    // a number someone chose on the bench outranks a guess.
    if (!this.isSwitch && d.powerW >= 5 && this.thresholdW === DEFAULT_THRESHOLD) {
      this.thresholdW = Math.max(10, Math.round(d.powerW * 0.9 / 10) * 10);
    }
  }

  save(): void {
    const el: RawEl = { ...this.element };
    if (this.hasPlug && this.ip) {
      const outlet: RawEl = { gen: this.gen, ip: this.ip };
      if (!this.isSwitch) outlet['thresholdW'] = this.thresholdW;
      if (this.host) outlet['host'] = this.host;
      // The collector's `control` carries offDelayMs alongside the plug — keep
      // whatever's there rather than dropping it on a re-pair.
      const prev = (this.element[this.field] as RawEl | undefined) ?? {};
      el[this.field] = { ...prev, outlet };
    } else {
      // Only the plug goes; a collector with an offDelayMs but no plug is still a
      // valid element, so don't delete the whole branch.
      const prev = { ...((this.element[this.field] as RawEl | undefined) ?? {}) };
      delete prev['outlet'];
      if (Object.keys(prev).length) el[this.field] = prev;
      else delete el[this.field];
    }
    this.saved.emit(el);
  }
}
