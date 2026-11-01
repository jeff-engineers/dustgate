// Add a servo — or replace one — for a gate on a bus board (2026-11-01; docs/mockups/bus-chain.html, "Add a servo").
//
// You pick the gate; the board picks the number. A new ST3215 answers as 1 (topology.js BUS_ID_UNASSIGNED), so the board
// asks who answers as 1, turns that one a little so you can see it, and gives it the next number free in the SHOP
// (shop.js nextBusId) — or, replacing, the number the old servo had. The number is then written into the gate in the
// layout, so it stays tied to that gate wherever the servo is plugged in. Numbers are shown only in developer mode
// (jeff: "obscure the numbering, tie them to a valve").
//
// Only gates already on the shop layout and wired to this board are offered: a gate is drawn before it is driven.
import { Component, OnDestroy, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ActivatedRoute, Router } from '@angular/router';
import { Subscription } from 'rxjs';
import { ApiService } from '../services/api.service';
import { DevModeService } from '../services/dev-mode';
import { failureReason, reportFailure } from '../services/failure';
import type { Topology } from '@topology';
import { nextBusId } from '@shop';
import { selectorsOnController, type ConfigurableSelector } from '../gates/selector-types';
import { busNumberOf } from './bus-servos';

type Step = 'pick' | 'plug' | 'looking' | 'none' | 'garbled' | 'saving' | 'done' | 'failed';

/** Tie servo `n` to a gate in a layout document (mutated). Its positions are cleared and must be set again: a new servo
 *  sits on the shaft its own way, and a gate that had a PWM servo had angles on a different scale. A bus valve turns a
 *  whole circle, so its range is opened to 359°. */
export function tieServo(gate: ConfigurableSelector, n: number): void {
  if (gate.kind === 'linear') {
    gate.linear = { ...(gate.linear ?? {}), bus: n };
    delete gate.linear.channel;
    return;
  }
  gate.servo = { ...gate.servo, bus: n };
  delete gate.servo.channel;
  if (gate.servo.maxAngle === undefined || gate.servo.maxAngle < 359) gate.servo.maxAngle = 359;
  delete gate.servo.referenceAngle;
}

@Component({
  selector: 'app-add-servo',
  standalone: true,
  imports: [CommonModule],
  styles: [`
    :host { display: block; max-width: 460px; margin: 0 auto; padding: 16px 14px 40px; }
    .back { background: none; border: 0; color: var(--accent); font: inherit; padding: 0; cursor: pointer; margin-bottom: 10px; }
    .head { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; margin-bottom: 10px; }
    .title { font-size: 16px; font-weight: 600; }
    .where { font-size: 12px; color: var(--muted); }
    .steps { display: flex; gap: 5px; margin-bottom: 12px; }
    .steps i { flex: 1; height: 3px; border-radius: 2px; background: var(--border); }
    .steps i.on { background: var(--accent); }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 14px; padding: 12px;
            display: flex; flex-direction: column; gap: 10px; }
    .big { font-size: 15px; font-weight: 600; }
    .sub { font-size: 12.5px; color: var(--muted); line-height: 1.5; }
    .gate { display: flex; justify-content: space-between; align-items: center; gap: 10px; text-align: left; width: 100%;
            background: none; border: 0; border-top: 1px solid var(--border); color: inherit; font: inherit; padding: 9px 0; cursor: pointer; }
    .gate .mark { color: var(--accent); }
    .btns { display: flex; flex-wrap: wrap; gap: 8px; }
    .btn { background: #262626; border: 1px solid var(--border); color: var(--text); border-radius: 10px; padding: 10px 12px;
           font: inherit; font-size: 13.5px; cursor: pointer; }
    .btn.primary { background: var(--accent); border-color: var(--accent); color: #1a1200; font-weight: 600; }
    .btn:disabled { opacity: .5; cursor: default; }
    .callout { font-size: 13px; border-radius: 10px; padding: 9px 10px; line-height: 1.5; }
    .callout.ok { background: #12261a; } .callout.warn { background: #2a2209; } .callout.bad { background: #2a1414; }
  `],
  template: `
    <button class="back" type="button" (click)="back()">← Your boards</button>
    <div class="head">
      <span class="title">{{ replacing ? 'Replace the ' + (gate?.name || 'gate') + '\\'s servo' : 'Add a servo' }}</span>
      <span class="where">{{ boardName }}</span>
    </div>
    <div class="steps"><i [class.on]="true"></i><i [class.on]="step !== 'pick'"></i><i [class.on]="step === 'done'"></i></div>

    <div class="card" *ngIf="step === 'pick'">
      <div class="big">Which gate is it for?</div>
      <ng-container *ngIf="candidates.length; else noGates">
        <button type="button" class="gate" *ngFor="let g of candidates" (click)="gate = g" [attr.aria-pressed]="gate === g">
          <span>{{ g.name || g.id }}<br><span class="sub">{{ kindWord(g) }} on this board, no servo yet</span></span>
          <span class="mark">{{ gate === g ? '●' : '○' }}</span>
        </button>
        <p class="sub" *ngIf="gate?.kind === 'linear'">A slider also needs its two endstops plugged into this board's endstop sockets.</p>
        <div class="btns"><button class="btn primary" type="button" [disabled]="!gate" (click)="step = 'plug'">Next</button></div>
      </ng-container>
      <ng-template #noGates>
        <p class="sub">Every gate wired to this board already has a servo. To add another, put the gate on the shop layout
          and wire it to this board first.</p>
        <div class="btns"><button class="btn" type="button" (click)="goLayout()">Open the shop layout</button></div>
      </ng-template>
    </div>

    <div class="card" *ngIf="step === 'plug' || step === 'looking'">
      <div class="big">{{ replacing ? 'Unplug the old servo and plug the new one into its cable' : 'Plug in its servo, and only that one' }}</div>
      <p class="sub">{{ replacing ? 'Leave every other servo plugged in.' : 'Leave the servos already working where they are. Add new ones one at a time.' }}</p>
      <div class="btns"><button class="btn primary" type="button" [disabled]="step === 'looking'" (click)="look()">
        {{ step === 'looking' ? 'Looking…' : 'It\\'s plugged in — find it' }}</button></div>
    </div>

    <div class="card" *ngIf="step === 'none' || step === 'garbled' || step === 'failed'">
      <div class="callout bad" *ngIf="step === 'garbled'"><b>Two new servos are plugged in.</b> Only one can be added at a
        time. Unplug all but one and look again.</div>
      <div class="callout warn" *ngIf="step === 'none'"><b>No new servo answered.</b> Check it is plugged into this board's
        chain and that the board's 12 V is on.</div>
      <div class="callout bad" *ngIf="step === 'failed'">{{ error }}</div>
      <div class="btns"><button class="btn primary" type="button" (click)="look()">Look again</button>
        <button class="btn" type="button" (click)="restart()">Back</button></div>
    </div>

    <div class="card" *ngIf="step === 'saving'"><div class="sub">Found it — giving it its number…</div></div>

    <div class="card" *ngIf="step === 'done'">
      <div class="callout ok"><b>Found it</b> — it turned a little so you could see which one. It now drives the
        <b>{{ gate?.name || gate?.id }}</b>, and remembers that if it is unplugged.</div>
      <p class="sub">Next, set its open and closed positions{{ replacing ? ' again: a new servo sits on its shaft a little differently' : '' }}.</p>
      <div class="dev-box" *ngIf="dev"><span class="dev-tag">Developer</span>
        <span class="sub">Found as number 1 (not yet assigned), given number {{ given }}{{ replacing ? ', the old servo\\'s' : ', the next free in the shop' }}.</span></div>
      <div class="btns">
        <button class="btn primary" type="button" (click)="setPositions()">Set its positions</button>
        <button class="btn" type="button" *ngIf="!replacing && candidates.length" (click)="restart()">Add another</button>
        <button class="btn" type="button" (click)="back()">Done</button>
      </div>
    </div>
  `,
})
export class AddServoComponent implements OnInit, OnDestroy {
  board = '';
  boardName = '';
  candidates: ConfigurableSelector[] = [];
  gate: ConfigurableSelector | null = null;
  replacing = false;
  step: Step = 'pick';
  given = 0;
  error = '';
  dev = false;
  private topo: Topology | null = null;
  private devSub: Subscription | null = null;

  constructor(private api: ApiService, private route: ActivatedRoute, private router: Router, private devMode: DevModeService) {}

  async ngOnInit(): Promise<void> {
    this.board = this.route.snapshot.paramMap.get('board') ?? '';
    const q = this.route.snapshot.queryParamMap;
    this.replacing = q.get('replace') === '1';
    this.devMode.load();
    this.devSub = this.devMode.on$.subscribe((on) => { this.dev = on; });
    try { this.topo = await this.api.getTopology(); } catch { this.topo = null; }
    try { const n = (await this.api.getNodes()).find((x) => x.id === this.board); this.boardName = n?.name || this.board; }
    catch { this.boardName = this.board; }
    this.loadCandidates();
    const want = q.get('gate');
    const g = want && this.topo ? selectorsOnController(this.topo, this.board).find((s) => s.id === want) : undefined;
    if (g) { this.gate = g; this.step = 'plug'; }
  }
  ngOnDestroy(): void { this.devSub?.unsubscribe(); }

  private loadCandidates(): void {
    this.candidates = this.topo ? selectorsOnController(this.topo, this.board).filter((s) => busNumberOf(s) === undefined) : [];
  }
  kindWord(g: ConfigurableSelector): string {
    return g.kind === 'linear' ? 'Slider' : g.kind === 'servoManifold' ? 'Manifold' : 'Ball valve';
  }

  async look(): Promise<void> {
    if (!this.gate) return;
    this.step = 'looking'; this.error = '';
    try {
      const found = await this.api.servoOp(this.board, 'find');
      if (!found.ok) throw new Error(found.err || 'The board could not look for it.');
      if (found.found !== 'one') { this.step = found.found === 'garbled' ? 'garbled' : 'none'; return; }
      this.step = 'saving';
      // Replacing keeps the gate's number (the old servo is unplugged, so it is free); adding takes the next free in the
      // shop. The board refuses a number that already answers, and says so.
      const old = busNumberOf(this.gate);
      const n = this.replacing && old !== undefined ? old : nextBusId(this.topo as unknown as Parameters<typeof nextBusId>[0]);
      if (n === null) throw new Error('This shop has used every servo number.');
      const r = await this.api.servoOp(this.board, 'assign', { to: n });
      if (!r.ok) {
        // Replacing, the number still answering is the old servo: say what to do about it, in those words.
        const stillThere = this.replacing && /already answers/.test(r.err || '');
        throw new Error(stillThere ? 'The old servo is still plugged in. Unplug it, then look again.' : r.err || 'The servo did not take its number.');
      }
      // Write it into the layout as read NOW, so whatever else changed since is kept.
      const doc = JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology;
      const g = selectorsOnController(doc, this.board).find((s) => s.id === this.gate!.id);
      if (!g) throw new Error('That gate is no longer on this board.');
      tieServo(g, n);
      await this.api.putTopology(doc);
      this.topo = doc;
      this.gate = g;
      this.given = n;
      this.loadCandidates();
      this.step = 'done';
    } catch (e) {
      this.error = reportFailure('Add a servo', e, failureReason(e));
      this.step = 'failed';
    }
  }
  restart(): void {
    this.error = '';
    if (this.replacing) { this.step = 'plug'; return; }
    this.gate = null; this.step = 'pick';
  }
  setPositions(): void { void this.router.navigate(['/gates'], { queryParams: { gate: this.gate?.id } }); }
  goLayout(): void { void this.router.navigate(['/build']); }
  back(): void { void this.router.navigate(['/boards']); }
}
