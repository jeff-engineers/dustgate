import { Component, EventEmitter, Input, OnInit, Output, QueryList, ViewChildren } from '@angular/core';
import { CommonModule } from '@angular/common';
import { FormsModule } from '@angular/forms';
import { DiscoveredOutlet } from '../services/api.service';
import { OutletPickerComponent } from './outlet-picker.component';
import { PairedOutletRowComponent } from './paired-outlet-row.component';
import { RfAddressComponent } from './rf-address.component';
import { CollectorForm, CtlKind, RawEl, SenseKind, fused, readCollector, writeCollector }
  from './collector-doc';

// ── Setting up the collector ─────────────────────────────────────────────────
//
// One sheet for the one machine DustGate is allowed to command. It replaces the
// single-slot pairing `element-outlet-config` did in its 'switch' role, which
// could only ever say "this plug switches the cyclone" — and so could not
// describe the collector on the bench: switched by its remote, watched by a
// metering plug, with a beam across the bin.
//
// TWO QUESTIONS, NEVER ONE, and that is the whole design:
//
//   control — how we SWITCH it. A plug, an RF frame, or nothing.
//   sensor  — how we WATCH it, which is independent, because every way of
//             commanding a collector except a plug is STATELESS. A servo arm and
//             an RF frame both send an EDGE against a TOGGLE, so what we sent
//             proves nothing about whether the blower is turning.
//
// The model has said this since `control` and `sensor` were split (topology.js,
// and TopologyRuntime::collectorSensorOutlet() on the firmware side); only the
// UI never caught up. Design + decisions: docs/mockups/collector-setup.html.
//
// THE ONE CASE WHERE THE TWO HAVE ONE ANSWER is a switchable Shelly, which
// reports its own power. The sheet collapses the watch question to a locked row
// naming that same plug rather than asking twice — and the firmware already
// behaves this way, falling back to the control plug when `sensor.outlet` is
// absent. It stays drawn as two questions because that fusion is a property of
// that hardware, not of collectors.
//
// WHAT IT DELIBERATELY CANNOT DO YET, drawn greyed with the reason on the pill
// rather than hidden — hiding them is how a fourth screen for the same question
// gets designed by the next person:
//
//   servo on the collector's own switch — no schema, no CollectorPresser
//        implementation, and it needs a torque measurement first (RFC §4.2c)
//   CT clamp — a CT cannot honour a threshold in watts, and the learned-baseline
//        shape that replaces it is unsettled (RFC §5.4a). Committing a shape now
//        is exactly what that section says not to do.

// READING AND WRITING THE ELEMENT LIVES IN collector-doc.ts, with a spec. This
// file owns the screen; that one owns the document, which is the half that can
// be silently wrong — a misplaced field still validates and the collector just
// never starts.

@Component({
  selector: 'app-collector-setup',
  standalone: true,
  imports: [CommonModule, FormsModule, OutletPickerComponent, PairedOutletRowComponent,
            RfAddressComponent],
  styles: [`
    :host { display: block; }
    .card { background: var(--surface); border: 1px solid var(--border);
            border-radius: 16px; padding: 18px 16px; }
    .head { margin-bottom: 16px; }
    .head .who { font-size: 17px; font-weight: 600; }
    .head .kind { font-size: 12.5px; color: var(--muted); margin-top: 1px; }

    /* ── a question block ──────────────────────────────────────────────────
       The label says what DustGate will DO; the line under it says why we are
       asking. */
    .q { margin-bottom: 20px; }
    .q > .lbl { font-size: 11px; letter-spacing: .085em; text-transform: uppercase;
                color: var(--muted); font-weight: 600; }
    .q > .why { font-size: 12.5px; color: var(--muted); line-height: 1.5; margin: 3px 0 10px; }

    .opt { display: flex; align-items: flex-start; gap: 11px; width: 100%; text-align: left;
           background: var(--bg); border: 1px solid var(--border); color: var(--text);
           border-radius: 12px; padding: 11px 12px; margin-bottom: 7px; font: inherit; }
    button.opt { cursor: pointer; }
    .opt:disabled { cursor: default; opacity: .55; }
    .opt.on { border-color: var(--accent); background: rgba(240,165,0,.07); }
    .opt:focus-visible, .sw:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
    .radio { width: 17px; height: 17px; border-radius: 50%; border: 1.5px solid var(--muted);
             flex: none; margin-top: 2px; position: relative; }
    .opt.on .radio { border-color: var(--accent); }
    .opt.on .radio::after { content: ''; position: absolute; inset: 3px;
                            border-radius: 50%; background: var(--accent); }
    .opt .body { flex: 1; min-width: 0; }
    .opt .name { font-size: 14px; font-weight: 500; display: flex; align-items: center;
                 gap: 7px; flex-wrap: wrap; }
    .opt .detail { font-size: 12px; color: var(--muted); margin-top: 2px; line-height: 1.45; }
    .pill { font-size: 10px; letter-spacing: .05em; text-transform: uppercase; font-weight: 700;
            padding: 2px 7px; border-radius: 20px; flex: none; }
    .pill.soon { color: var(--muted); background: var(--surface); border: 1px solid var(--border); }
    .pill.ok { color: var(--success); background: rgba(60,190,110,.12); }

    /* The follow-on belongs to the OPTION, not to the list. Rendered after the
       list it reads as belonging to whichever option happens to be last — which
       is the opposite of what it says. So the selected option loses its bottom
       radius and this continues its border, making one object that expanded. */
    .opt.open { margin-bottom: 0; border-bottom-left-radius: 0; border-bottom-right-radius: 0;
                border-bottom-color: rgba(240,165,0,.22); }
    .expand { border: 1px solid var(--accent); border-top: 0; background: rgba(240,165,0,.07);
              border-radius: 0 0 12px 12px; padding: 11px 12px 12px; margin: 0 0 7px; }
    .expand .note { font-size: 12.5px; color: var(--muted); line-height: 1.55; margin: 0; }
    .expand .note b { color: var(--text); font-weight: 600; }
    .expand app-paired-outlet-row, .expand app-outlet-picker { display: block; }

    /* The DIP, small enough for a summary row: the PART, never the number. 94
       means nothing to a woodworker, and printing it invites them to think it
       should. The number stays on the matching screen, for a support call. */
    .rfrow { display: flex; align-items: center; justify-content: space-between; gap: 10px;
             background: var(--bg); border: 1px solid var(--border); border-radius: 10px;
             padding: 9px 11px; font-size: 13px; }
    .rfrow .what { display: flex; align-items: center; gap: 9px; }
    .dipmini { display: flex; gap: 2px; background: #d8452f; border-radius: 3px;
               padding: 3px; flex: none; }
    .dipmini i { width: 5px; height: 15px; background: #2a2b2d; border-radius: 1px; position: relative; }
    .dipmini i::after { content: ''; position: absolute; left: 1px; right: 1px; height: 6px;
                        border-radius: 1px; background: #f2f2ef; top: 8px; }
    .dipmini i.up::after { top: 1px; }
    .rfrow .change { background: none; border: 0; color: var(--accent); font: inherit;
                     font-size: 13px; cursor: pointer; white-space: nowrap; padding: 0; }

    .binrow { display: flex; align-items: center; gap: 11px; background: var(--bg);
              border: 1px solid var(--border); border-radius: 12px; padding: 11px 12px;
              margin-bottom: 7px; }
    .binrow.on { border-color: var(--accent); background: rgba(240,165,0,.07);
                 margin-bottom: 0; border-bottom-left-radius: 0; border-bottom-right-radius: 0;
                 border-bottom-color: rgba(240,165,0,.22); }
    .binrow .body { flex: 1; min-width: 0; }
    .sw { width: 42px; height: 24px; border-radius: 20px; background: var(--surface);
          border: 1px solid var(--border); flex: none; position: relative; cursor: pointer; padding: 0; }
    .sw::after { content: ''; position: absolute; top: 2px; left: 2px; width: 18px; height: 18px;
                 border-radius: 50%; background: var(--muted); transition: left .14s ease; }
    .sw.on { background: rgba(60,190,110,.25); border-color: var(--success); }
    .sw.on::after { left: 20px; background: var(--success); }
    @media (prefers-reduced-motion: reduce) { .sw::after { transition: none; } }

    .board { display: flex; align-items: center; justify-content: space-between; gap: 10px;
             font-size: 12.5px; color: var(--muted); margin-bottom: 9px; }
    .board select { background: var(--bg); border: 1px solid var(--border); color: var(--text);
                    border-radius: 9px; padding: 7px 9px; font: inherit; font-size: 13px; }

    .slider { background: var(--bg); border: 1px solid var(--border);
              border-radius: 12px; padding: 11px 12px; }
    .slider .r { display: flex; justify-content: space-between; font-size: 13px; margin-bottom: 6px; }
    .slider .r b { color: var(--accent); font-weight: 600; }
    .slider input { width: 100%; accent-color: var(--accent); }

    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--bg); border: 1px solid var(--border); color: var(--text); }
    .nav .save { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
  `],
  template: `
    <!-- Matching the remote is its own screen, opened from the Remote option's
         own row. Rendered INSTEAD of the sheet rather than over it: it is a step
         in the same errand, not a dialog about a different one. -->
    <app-rf-address *ngIf="matchingRemote; else sheet"
                    [address]="form.rfAddress"
                    (saved)="form.rfAddress = $event; matchingRemote = false"
                    (cancelled)="matchingRemote = false">
    </app-rf-address>

    <ng-template #sheet>
    <div class="card">
      <!-- The NAME is the heading and what it is goes underneath — the other way
           round read "Dust collector: Dust collector" on a shop that never renamed
           it, which is most of them. -->
      <div class="head">
        <div class="who">{{ form.name }}</div>
        <div class="kind">Dust collector<span *ngIf="systemName"> · {{ systemName }}</span></div>
      </div>

      <!-- ── how it is SWITCHED ─────────────────────────────────────────── -->
      <div class="q">
        <div class="lbl" id="ctl-lbl">Switched by</div>
        <p class="why">How DustGate turns it on when a tool starts.</p>

        <button type="button" class="opt" role="radio" aria-labelledby="ctl-lbl"
                [attr.aria-checked]="form.ctl === 'plug'"
                [class.on]="form.ctl === 'plug'"
                [class.open]="form.ctl === 'plug'" (click)="setCtl('plug')">
          <span class="radio"></span>
          <span class="body">
            <span class="name">Smart plug</span>
            <span class="detail">A switchable plug between wall and collector.</span>
          </span>
        </button>
        <div class="expand" *ngIf="form.ctl === 'plug'">
          <app-paired-outlet-row *ngIf="form.ctlPlug.ip && !changingCtl"
                                 [toolName]="form.name" [ip]="form.ctlPlug.ip" [host]="form.ctlPlug.host" [label]="form.ctlPlug.label"
                                 [seen]="seen(form.ctlPlug.ip)" [owner]="owner" [isSwitch]="true"
                                 fieldId="collector-switch-name"
                                 (renamed)="form.ctlPlug.label = $event"
                                 (rescan)="rescan.emit()"
                                 (changeOutlet)="changingCtl = true"
                                 (removed)="unpairCtl($event)">
          </app-paired-outlet-row>
          <app-outlet-picker *ngIf="!form.ctlPlug.ip || changingCtl"
                             [toolName]="form.name" [selectedIp]="form.ctlPlug.ip"
                             [excludeIps]="switchExcludes()" [excludeReason]="switchReasons()"
                             (picked)="pickCtl($event)">
          </app-outlet-picker>
        </div>

        <button type="button" class="opt" role="radio" aria-labelledby="ctl-lbl"
                [attr.aria-checked]="form.ctl === 'rf'"
                [class.on]="form.ctl === 'rf'"
                [class.open]="form.ctl === 'rf'" (click)="setCtl('rf')">
          <span class="radio"></span>
          <span class="body">
            <span class="name">Remote</span>
            <span class="detail">DustGate transmits the collector remote's own frame.</span>
          </span>
        </button>
        <div class="expand" *ngIf="form.ctl === 'rf'">
          <div class="rfrow">
            <span class="what">
              <span class="dipmini">
                <i *ngFor="let open of rfBitsOpen" [class.up]="!open"></i>
              </span>
              Matched to your remote
            </span>
            <button type="button" class="change" (click)="matchingRemote = true">Change ›</button>
          </div>
          <p class="note" style="margin-top:7px">Set once, from the switches inside the fob.</p>
        </div>

        <button type="button" class="opt" role="radio" aria-checked="false" disabled>
          <span class="radio"></span>
          <span class="body">
            <span class="name">Servo on its switch<span class="pill soon">not built yet</span></span>
            <span class="detail">An arm on the collector's own paddle or toggle. Not with a
              receiver in the cord — the collector's own switch would have to stay on for the
              receiver to control anything.</span>
          </span>
        </button>

        <button type="button" class="opt" role="radio" aria-labelledby="ctl-lbl"
                [attr.aria-checked]="form.ctl === 'none'"
                [class.on]="form.ctl === 'none'" (click)="setCtl('none')">
          <span class="radio"></span>
          <span class="body">
            <span class="name">Nothing — I switch it myself</span>
            <span class="detail">DustGate moves the gates and leaves the blower alone.</span>
          </span>
        </button>
      </div>

      <!-- ── how it is WATCHED ──────────────────────────────────────────── -->
      <div class="q">
        <div class="lbl" id="sense-lbl">Watched by</div>
        <p class="why">{{ watchWhy }}</p>

        <ng-container *ngIf="fused; else askWatch">
          <!-- One answer, not a third state to configure. Offering a picker here
               would invite pairing two plugs to one blower. -->
          <div class="opt on open">
            <span class="radio"></span>
            <span class="body">
              <span class="name">{{ switchPlugName }}<span class="pill ok">in use</span></span>
              <span class="detail">The same plug, reporting power back.</span>
            </span>
          </div>
          <div class="expand">
            <p class="note">A switchable plug reports its own power, so there is nothing else to set.</p>
          </div>
        </ng-container>

        <ng-template #askWatch>
          <button type="button" class="opt" role="radio" aria-labelledby="sense-lbl"
                  [attr.aria-checked]="form.sense === 'plug'"
                  [class.on]="form.sense === 'plug'"
                  [class.open]="form.sense === 'plug'" (click)="setSense('plug')">
            <span class="radio"></span>
            <span class="body">
              <span class="name">Metering plug</span>
              <span class="detail">A plug that reports power. It does not have to switch.</span>
            </span>
          </button>
          <div class="expand" *ngIf="form.sense === 'plug'">
            <app-paired-outlet-row *ngIf="form.sensePlug.ip && !changingSense"
                                   [toolName]="form.name" [ip]="form.sensePlug.ip" [host]="form.sensePlug.host" [label]="form.sensePlug.label"
                                   [seen]="seen(form.sensePlug.ip)" [owner]="owner"
                                   fieldId="collector-sensor-name"
                                   (renamed)="form.sensePlug.label = $event"
                                   (rescan)="rescan.emit()"
                                   (changeOutlet)="changingSense = true"
                                   (removed)="unpairSense($event)">
            </app-paired-outlet-row>
            <app-outlet-picker *ngIf="!form.sensePlug.ip || changingSense"
                               [toolName]="form.name" [selectedIp]="form.sensePlug.ip"
                               [excludeIps]="sensorExcludes()" [excludeReason]="sensorReasons()"
                               (picked)="pickSense($event)">
            </app-outlet-picker>
          </div>

          <button type="button" class="opt" role="radio" aria-checked="false" disabled>
            <span class="radio"></span>
            <span class="body">
              <span class="name">Current clamp<span class="pill soon">not built yet</span></span>
              <span class="detail">A clamp on the collector's feed — for 240 V, where there is
                no plug to pair.</span>
            </span>
          </button>

          <button type="button" class="opt" role="radio" aria-labelledby="sense-lbl"
                  [attr.aria-checked]="form.sense === 'none'"
                  [class.on]="form.sense === 'none'"
                  [class.open]="form.sense === 'none' && openLoop" (click)="setSense('none')">
            <span class="radio"></span>
            <span class="body">
              <span class="name">Nothing</span>
              <span class="detail">DustGate assumes it started.</span>
            </span>
          </button>
          <!-- Allowed on purpose — a shop that hasn't bought a plug yet — but it is
               the configuration where a missed press stays wrong, so it says so
               here rather than in a log nobody opens. -->
          <div class="expand" *ngIf="form.sense === 'none' && openLoop">
            <p class="note"><b>⚠ Nothing will notice a missed press.</b> DustGate can only tell
              the collector to <i>change</i>, never to be on. If one press is lost, the gates and
              the blower disagree until you spot it.</p>
          </div>
        </ng-template>
      </div>

      <!-- ── the bin ────────────────────────────────────────────────────── -->
      <div class="q">
        <div class="lbl">Dust bin</div>
        <p class="why">A beam across the bin, wired to the board at the collector.</p>

        <div class="binrow" [class.on]="form.bin">
          <span class="body">
            <span class="name" style="font-size:14px;font-weight:500">Bin sensor</span>
            <span class="detail" style="display:block;font-size:12px;color:var(--muted)">
              {{ form.bin ? 'Warns you before it overflows.' : 'Off' }}
            </span>
          </span>
          <button type="button" class="sw" [class.on]="form.bin" (click)="form.bin = !form.bin"
                  role="switch" [attr.aria-checked]="form.bin" aria-label="Bin sensor"></button>
        </div>
        <div class="expand" *ngIf="form.bin">
          <!-- Which board it is wired to is the only real question — the same one a
               gate asks. Absent means "this board", matching every selector and
               NodeBus's own rule. "invert" is not asked: it is a fact about how
               the sensor is wired, settled in the wiring doc, not a preference. -->
          <div class="board">
            <label for="bin-board">Wired to</label>
            <select id="bin-board" [(ngModel)]="form.binControllerId">
              <option value="">This board</option>
              <option *ngFor="let c of controllers" [value]="c.id">{{ c.name || c.id }}</option>
            </select>
          </div>
          <p class="note">When it trips, the lamp on the collector fires and
            <b>every board on {{ systemName || 'this system' }} flashes red</b> — not the whole
            shop, and not another collector's boards.</p>
        </div>
      </div>

      <!-- ── coast-down ─────────────────────────────────────────────────── -->
      <div class="q" style="margin-bottom:4px">
        <div class="lbl">Coast-down</div>
        <p class="why">How long it keeps pulling after the last tool stops.</p>
        <div class="slider">
          <div class="r"><span>Coast-down</span><b>{{ form.coastSec }} s</b></div>
          <input type="range" min="0" max="30" step="1" [(ngModel)]="form.coastSec"
                 aria-label="Coast-down seconds"/>
        </div>
      </div>

      <div class="nav">
        <button type="button" class="back" (click)="cancelled.emit()">Cancel</button>
        <button type="button" class="save" (click)="save()">Save</button>
      </div>
    </div>
    </ng-template>
  `,
})
export class CollectorSetupComponent implements OnInit {
  /** The collector element from the layout. Edited on a COPY by the caller, which
   *  splices the result back in — same contract the gate and tool sheets use. */
  @Input({ required: true }) element!: RawEl;
  /** For the bin's "wired to" list, and only that. */
  @Input() controllers: { id: string; name?: string }[] = [];
  /** Named in the bin alert's copy, so the scope is concrete rather than abstract. */
  @Input() systemName = '';
  /** Plugs already spoken for elsewhere in the shop, and why. */
  @Input() excludeIps: string[] = [];
  @Input() excludeReason: Record<string, string> = {};
  /** The last scan, from whoever opened this sheet. */
  @Input() outlets: DiscoveredOutlet[] = [];
  /** Our mDNS name, from GET /api/info — the owner suffix we stamp on our plugs. */
  @Input() owner = '';
  @Output() saved = new EventEmitter<RawEl>();
  @Output() cancelled = new EventEmitter<void>();
  @Output() note = new EventEmitter<string>();
  @Output() rescan = new EventEmitter<void>();

  /** The form the sheet edits, read off the element and written back on Save. */
  form!: CollectorForm;

  matchingRemote = false;
  changingCtl = false;
  changingSense = false;

  ngOnInit(): void {
    this.form = readCollector(this.element);
  }

  get fused(): boolean { return fused(this.form); }

  /** Commanded somehow, watched by nothing: a lost press stays lost. */
  get openLoop(): boolean { return this.form.ctl !== 'none'; }

  get watchWhy(): string {
    if (this.fused) return 'Answered already — the plug that switches it also reports its power.';
    if (this.form.ctl === 'none') return 'So the shop list can show whether it is running.';
    return 'How DustGate knows it really started.';
  }

  /** index i = rocker i+1 OPEN, which is the nub DOWN and a logic 1. The glyph
   *  therefore draws `.up` on the bits that are ZERO — mirroring
   *  RfAddressComponent, which owns the explanation of why up is a zero. Drawn
   *  the other way round it is a picture of a different remote, and nothing on
   *  screen would say so. */
  get rfBitsOpen(): boolean[] {
    return Array.from({ length: 8 }, (_, i) => ((this.form.rfAddress >> i) & 1) === 1);
  }

  seen(ip: string): DiscoveredOutlet | null {
    return this.outlets.find(o => o.ip === ip) ?? null;
  }

  /** What to call the switching plug in the collapsed watch row. THE LIVE NAME
   *  FIRST, the same order paired-outlet-row uses: the stored label is the
   *  layout's cached copy, written on Save, and goes stale the moment someone
   *  renames the plug in its own app. */
  get switchPlugName(): string {
    const p = this.form.ctlPlug;
    return this.seen(p.ip)?.name || p.label || p.host || p.ip || 'That plug';
  }

  // ── the other slot's plug ────────────────────────────────────────────────
  // The two slots must never be the same plug: one plug, one claim. But only
  // while the other slot is actually USING it — a plug left behind when you
  // switch from Smart plug to Remote is released on Save (writeCollector only
  // writes control.outlet for ctl === 'plug'), so greying it out here would
  // reserve it for a role this collector no longer has.
  //
  // And it says WHY. Without a reason the picker falls back to "already paired
  // with another tool", which is both wrong and confusing: it is this same
  // collector's other plug, two rows up the screen.
  private get otherSwitchIp(): string {
    return this.form.ctl === 'plug' ? this.form.ctlPlug.ip : '';
  }
  private get otherSensorIp(): string {
    return this.form.sense === 'plug' ? this.form.sensePlug.ip : '';
  }

  switchExcludes(): string[] {
    const ip = this.otherSensorIp;
    return ip ? [...this.excludeIps, ip] : this.excludeIps;
  }
  sensorExcludes(): string[] {
    const ip = this.otherSwitchIp;
    return ip ? [...this.excludeIps, ip] : this.excludeIps;
  }
  switchReasons(): Record<string, string> {
    const ip = this.otherSensorIp;
    return ip ? { ...this.excludeReason, [ip]: 'already watching this collector' } : this.excludeReason;
  }
  sensorReasons(): Record<string, string> {
    const ip = this.otherSwitchIp;
    return ip ? { ...this.excludeReason, [ip]: 'already switching this collector' } : this.excludeReason;
  }

  setCtl(c: CtlKind): void { this.form.ctl = c; this.changingCtl = false; }
  setSense(s: SenseKind): void { this.form.sense = s; this.changingSense = false; }

  pickCtl(d: DiscoveredOutlet): void {
    // A no-relay plug has no contacts, so naming it here describes a collector
    // that can never start — validateTopology() rejects the document outright.
    // The picker cannot know that, so the sheet does, and says where it belongs
    // rather than just refusing.
    if ((d.kind ?? 'shelly') !== 'shelly') {
      this.note.emit(`${d.name || d.ip} has no relay, so it can't switch the collector — pair it under "Watched by" instead.`);
      return;
    }
    this.form.ctlPlug = {
      ip: d.ip, host: d.hostname, label: d.name || '',
      gen: d.generation || 2, kind: 'shelly',
    };
    this.changingCtl = false;
  }

  pickSense(d: DiscoveredOutlet): void {
    const kind = d.kind ?? 'shelly';
    this.form.sensePlug = {
      ip: d.ip, host: d.hostname, label: d.name || '',
      // `|| 2` only for a Shelly. A Tasmota reports generation 0 because it HAS
      // no generation, and coercing that to 2 writes a Shelly generation into a
      // document describing a device that has never heard of one.
      gen: kind === 'tasmota' ? 0 : (d.generation || 2),
      kind,
    };
    this.changingSense = false;
  }

  /** Remove said yes. The device half already ran inside the row; this is the
   *  layout half, and it commits immediately rather than waiting for Save — the
   *  plug has already been let go, so a layout still claiming it would be the
   *  two halves disagreeing. */
  unpairCtl(note: string): void {
    this.form.ctlPlug = { ip: '', host: '', label: '', gen: 2, kind: 'shelly' };
    this.form.ctl = 'none';
    this.changingCtl = false;
    if (note) this.note.emit(note);
    void this.save();
  }
  unpairSense(note: string): void {
    this.form.sensePlug = { ip: '', host: '', label: '', gen: 2, kind: 'shelly' };
    this.form.sense = 'none';
    this.changingSense = false;
    if (note) this.note.emit(note);
    void this.save();
  }

  /** Both rows, when on screen — ViewChildren rather than ViewChild because this
   *  sheet has two plug slots and only one of them being flushed is exactly the
   *  kind of half-fix that looks like it works.
   *
   *  Save waits on them so a rename still on the wire lands before the layout is
   *  written: blur fires BEFORE the click that caused it, so tapping Save straight
   *  after typing a name arrives here while that write is still out, and emitting
   *  now would store the OLD name against an outlet that already took the new one. */
  @ViewChildren(PairedOutletRowComponent) private outletRows?: QueryList<PairedOutletRowComponent>;

  async save(): Promise<void> {
    for (const row of this.outletRows ?? []) await row.flush();
    this.saved.emit(writeCollector(this.element, this.form));
  }
}
