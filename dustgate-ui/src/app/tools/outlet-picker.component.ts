import { Component, EventEmitter, Input, OnInit, Output, inject } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ApiService, DiscoveredOutlet } from '../services/api.service';

// ── Finding a tool's smart plug ──────────────────────────────────────────────
// The identify-by-power trick, from Phase 1: you can't tell which Shelly on the
// network is bolted behind which machine by reading hostnames, so you switch the
// tool on and look for the plug that jumped. That's the whole interaction, and it
// works because the scan already probes every hit for live wattage.
//
// Split out of OutletConfiguratorComponent so the canvas tool sheet and the v1
// gate wizard can't drift apart on it. This half knows nothing about where the
// choice gets stored — it emits a DiscoveredOutlet and stops. (The v1
// configurator still has its own copy; it's welded to the slot/stop model and
// wasn't worth rewiring mid-bring-up. Fold it in when v1 retires.)

@Component({
  selector: 'app-outlet-picker',
  standalone: true,
  imports: [CommonModule],
  styles: [`
    :host { display: block; }
    .hint { font-size: 12.5px; color: var(--muted); line-height: 1.55; margin: 0 0 10px; }
    .hint b { color: var(--text); font-weight: 500; }
    .list { display: flex; flex-direction: column; gap: 7px; }
    .plug { display: block; width: 100%; text-align: left; padding: 10px 12px; border-radius: 11px;
            background: var(--bg); border: 1px solid var(--border); color: var(--text); }
    .plug.sel { border-color: var(--success); background: rgba(60,190,110,0.08); }
    /* Drawing current: green for a running tool, amber for standby — the cue that
       makes "switch it on and look" work at a glance. */
    .plug.on  { border-color: var(--success); background: rgba(60,190,110,0.12); }
    .plug.low { border-color: var(--accent);  background: rgba(240,165,0,0.10); }
    .plug:disabled { opacity: 0.45; }
    .top { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
    .nm { font-size: 14px; font-weight: 500; }
    .plug.on .nm { color: var(--success); }
    .w  { font-size: 12px; color: var(--success); flex-shrink: 0; }
    .plug.low .w { color: var(--accent); }
    .meta { font-size: 11.5px; color: var(--muted); margin-top: 2px; }
    .foot { display: flex; align-items: center; justify-content: center; gap: 6px;
            background: none; border: none; color: var(--muted); font-size: 12.5px; padding: 9px; width: 100%; }
    .empty { text-align: center; color: var(--muted); font-size: 13px; padding: 14px 8px; line-height: 1.6; }
  `],
  template: `
    <p class="hint">
      <b>Which plug is this one's?</b> Switch {{ toolName || 'the tool' }} on, then scan —
      the plug drawing power is the one.
    </p>

    <div class="list" *ngIf="outlets.length; else none">
      <button class="plug" *ngFor="let d of outlets"
              [class.sel]="d.ip === selectedIp"
              [class.on]="level(d) === 'on'" [class.low]="level(d) === 'low'"
              [disabled]="!!excludeIps.includes(d.ip) || !d.reachable"
              (click)="picked.emit(d)">
        <span class="top">
          <span class="nm">{{ d.name || d.hostname }}</span>
          <span class="w" *ngIf="level(d) !== 'off'">{{ d.powerW | number:'1.0-0' }} W</span>
        </span>
        <span class="meta">{{ sub(d) }}</span>
      </button>
    </div>
    <ng-template #none>
      <div class="empty">
        {{ scanning ? 'Scanning…' : 'No smart plugs found. Check it\\'s powered and on the same WiFi.' }}
      </div>
    </ng-template>

    <button class="foot" (click)="scan()" [disabled]="scanning">
      ↻ {{ scanning ? 'Scanning…' : 'Scan again' }}
    </button>
  `,
})
export class OutletPickerComponent implements OnInit {
  /** Used only in the prompt, so it reads as an instruction about a real machine. */
  @Input() toolName = '';
  /** Currently-chosen plug, so it can render as selected. */
  @Input() selectedIp = '';
  /** Plugs spoken for elsewhere — shown, but not pickable. One physical outlet
   *  must never end up wired to two tools. */
  @Input() excludeIps: string[] = [];
  /** Why a given IP is unavailable, keyed by IP — lets the caller say "dust
   *  collector" rather than the generic "already assigned". */
  @Input() excludeReason: Record<string, string> = {};
  @Output() picked = new EventEmitter<DiscoveredOutlet>();

  private readonly api = inject(ApiService);
  outlets: DiscoveredOutlet[] = [];
  scanning = false;

  ngOnInit(): void { void this.scan(); }

  /** Draw tier. 'off' below 1 W (a Shelly's own standby), 'low' up to 5 W —
   *  something idling — and 'on' above it, which is a running motor. */
  level(d: DiscoveredOutlet): 'off' | 'low' | 'on' {
    if (!d.reachable || d.powerW < 1) return 'off';
    return d.powerW >= 5 ? 'on' : 'low';
  }

  sub(d: DiscoveredOutlet): string {
    const why = this.excludeReason[d.ip];
    if (why) return why;
    if (this.excludeIps.includes(d.ip)) return 'already paired with another tool';
    if (!d.reachable) return 'not responding';
    return `${d.hostname} · ${d.ip}`;
  }

  async scan(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    try { this.outlets = await this.api.discoverOutlets(); }
    catch { this.outlets = []; }
    finally { this.scanning = false; }
  }
}
