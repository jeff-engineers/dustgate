import { Component, EventEmitter, Input, OnInit, Output, inject } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { ApiService, DiscoveredOutlet } from '../services/api.service';

// ── Finding a tool's smart plug ──────────────────────────────────────────────
// Identify-by-power: you can't tell which Shelly on the network is bolted behind
// which machine by reading hostnames, so you switch the tool on and look for the
// plug that jumped. That's the whole interaction, and it works because the scan
// already probes every hit for live wattage.
//
// ADDING ONE BY ADDRESS is the other half, and it is not a power-user
// convenience. Scanning uses mDNS, and a Tasmota plug does not advertise over
// mDNS in a stock build — so for the sense-only plug this project now prefers,
// typing the address is the ONLY way in (docs/tool-sensing-rfc.md §12). The
// device probes both protocols and answers with the same row a scan produces,
// so a plug added by hand is indistinguishable from a found one from here down.
//
// This component knows nothing about where the choice gets stored — it emits a
// DiscoveredOutlet and stops.

@Component({
  selector: 'app-outlet-picker',
  standalone: true,
  imports: [CommonModule, FormsModule],
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

    /* Adding by address. Always visible, never behind a disclosure — for a
       Tasmota this is the only route in, and a scan that found nothing is
       exactly when a hidden control is hardest to find. */
    .manual-add { margin-top: 10px; padding-top: 10px; border-top: 1px solid var(--border); }
    .manual-add .row { display: flex; gap: 7px; }
    .manual-add input { flex: 1; min-width: 0; padding: 9px 11px; border-radius: 10px;
                        background: var(--bg); border: 1px solid var(--border);
                        color: var(--text); font-size: 14px; font-family: inherit; }
    .manual-add input:disabled { opacity: 0.5; }
    .manual-add button { flex-shrink: 0; padding: 9px 14px; border-radius: 10px;
                         background: var(--bg); border: 1px solid var(--border);
                         color: var(--text); font-size: 13px; }
    .manual-add button:disabled { opacity: 0.45; }
    .manual-add .why { font-size: 11.5px; color: var(--muted); margin: 7px 2px 0; line-height: 1.5; }
    .manual-add .err { font-size: 12px; color: var(--danger, #e05252); margin: 7px 2px 0; line-height: 1.5; }
  `],
  template: `
    <p class="hint">
      <b>Which outlet is this one's?</b> Switch {{ toolName || 'the tool' }} on, then scan —
      the outlet drawing power is the one.
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
        {{ scanning
           ? 'Scanning…'
           : 'No outlets announced themselves. Check it\\'s powered and on this WiFi — or add it by address below.' }}
      </div>
    </ng-template>

    <button class="foot" (click)="scan()" [disabled]="scanning">
      ↻ {{ scanning ? 'Scanning…' : 'Scan again' }}
    </button>

    <div class="manual-add">
      <div class="row">
        <input type="text" inputmode="decimal" placeholder="Or type its address — 192.168.1.42"
               [(ngModel)]="manualIp" [disabled]="adding"
               (keyup.enter)="addByIp()" aria-label="Outlet IP address"/>
        <button (click)="addByIp()" [disabled]="adding || !manualIp.trim()">
          {{ adding ? 'Checking…' : 'Add' }}
        </button>
      </div>
      <p class="err" *ngIf="addError; else addWhy">{{ addError }}</p>
      <ng-template #addWhy>
        <p class="why">Some outlets don't announce themselves and won't show up in a scan.</p>
      </ng-template>
    </div>
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

  manualIp = '';
  adding = false;
  addError = '';

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
    // Name the kind when it is not the ordinary one. A Tasmota plug is
    // SENSE-ONLY — no relay, which is the whole reason for using it — so it
    // behaves differently once paired, and that has to be visible BEFORE
    // picking rather than discovered afterwards.
    const kind = d.kind === 'tasmota' ? 'Tasmota · sense only · ' : '';
    return `${kind}${d.hostname} · ${d.ip}`;
  }

  /** Probe one typed address and put the result in the list.
   *
   *  The device tries both protocols and reports which answered, so nothing here
   *  asks the user what kind of plug they have — they typed an address, which is
   *  all they can reasonably be expected to know.
   *
   *  An UNREACHABLE result is still added to the list rather than thrown away.
   *  It renders as "not responding" and is unpickable, which tells the user the
   *  address was understood and nothing was there — a distinct problem from a
   *  typo, and one they can act on. Discarding it would leave them staring at an
   *  unchanged list with no idea whether anything happened.
   */
  async addByIp(): Promise<void> {
    const ip = this.manualIp.trim();
    if (!ip || this.adding) return;
    this.addError = '';

    // Loose on purpose: the device is the real validator, and a regex strict
    // enough to be useful here would also reject a hostname, which works fine.
    if (!/^[a-zA-Z0-9.\-:]+$/.test(ip)) {
      this.addError = "That doesn't look like an address. Try something like 192.168.1.42.";
      return;
    }

    this.adding = true;
    try {
      const d = await this.api.pingOutlet(ip);
      // Replace rather than append if a scan already found this one, so the same
      // plug never appears twice under two different-looking rows.
      const at = this.outlets.findIndex(o => o.ip === d.ip);
      if (at >= 0) this.outlets[at] = d;
      else this.outlets = [...this.outlets, d];
      this.manualIp = '';
    } catch {
      this.addError = `Couldn't reach ${ip}. Check the address and that it's on this WiFi.`;
    } finally {
      this.adding = false;
    }
  }

  async scan(): Promise<void> {
    if (this.scanning) return;
    this.scanning = true;
    try { this.outlets = await this.api.discoverOutlets(); }
    catch { this.outlets = []; }
    finally { this.scanning = false; }
  }
}
