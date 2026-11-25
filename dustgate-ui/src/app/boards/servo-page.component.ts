// One bus servo's page (2026-11-01; docs/mockups/bus-chain.html, "A servo"): the gate it drives, the peak load of its
// last moves against what it usually needs, where it is and what it reads — and the everyday controls (Find it, Test
// move, Set positions, Replace). Developer mode adds the raw numbers and the controls that can leave a gate somewhere it
// shouldn't be (relax, give back its number).
//
// Load is LOGGED, not judged (jeff, 2026-11-01): the line is drawn, nothing turns amber, until real shop numbers say
// where "working harder than usual" starts.
import { Component, OnDestroy, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { ActivatedRoute, Router } from '@angular/router';
import { Subscription } from 'rxjs';
import { ApiService, BusServoState, NodeLinkState } from '../services/api.service';
import { DevModeService } from '../services/dev-mode';
import { failureReason, reportFailure } from '../services/failure';
import type { Topology } from '@topology';
import { absoluteAngles } from '@topology';
import { configurableSelectorsOf, selectorsOnController, type ConfigurableSelector } from '../gates/selector-types';
import { busNumberOf, usualLoad } from './bus-servos';
import { setServoNumber } from '@shop-edit';

@Component({
  selector: 'app-servo-page',
  standalone: true,
  imports: [CommonModule],
  styles: [`
    :host { display: block; max-width: 460px; margin: 0 auto; padding: 16px 14px 40px; }
    .head { display: flex; justify-content: space-between; align-items: baseline; gap: 8px; margin-bottom: 12px; }
    .title { font-size: 16px; font-weight: 600; }
    .where { font-size: 12px; color: var(--muted); }
    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 14px; padding: 12px;
            display: flex; flex-direction: column; gap: 8px; margin-bottom: 12px; }
    .line { display: flex; align-items: center; gap: 8px; font-size: 13px; }
    .sub { font-size: 12px; color: var(--muted); line-height: 1.5; }
    .spark { width: 100%; height: 72px; display: block; }
    .kv { display: grid; grid-template-columns: 1fr auto; gap: 4px 10px; font-size: 13px; }
    .kv span:nth-child(odd) { color: var(--muted); }
    .kv span:nth-child(even) { text-align: right; font-variant-numeric: tabular-nums; }
    .btns { display: flex; flex-wrap: wrap; gap: 8px; }
    .btn { background: #262626; border: 1px solid var(--border); color: var(--text); border-radius: 10px; padding: 10px 12px;
           font: inherit; font-size: 13.5px; cursor: pointer; }
    .btn:disabled { opacity: .5; cursor: default; }
    .msg { font-size: 12.5px; color: var(--muted); }
    .err { font-size: 12.5px; color: var(--danger); }
    .back { background: none; border: 0; color: var(--accent); font: inherit; padding: 0; cursor: pointer; margin-bottom: 10px; }
  `],
  template: `
    <button class="back" type="button" (click)="back()">← Your boards</button>
    <div class="head">
      <span class="title">{{ name }}</span>
      <span class="where">{{ dev && bus !== undefined ? 'servo #' + bus + ' · ' : '' }}{{ boardName }}</span>
    </div>

    <!-- A GATE WITH NO SERVO (2026-11-24). Its servo may already be on the board, answering but tied to no gate — a gate
         re-drawn or a layout restored loses the number the servo still holds. Find its servo turns each such servo a
         little, one at a time, and the person says which one moved this gate. Add a new servo is for one out of the box. -->
    <div class="card" *ngIf="bus === undefined && sel">
      <ng-container *ngIf="finding === null">
        <div class="line">No servo yet.</div>
        <div class="sub" *ngIf="untied.length === 1">This board has a servo not tied to any gate. If it turns this gate,
          <b>Find its servo</b> shows you.</div>
        <div class="sub" *ngIf="untied.length > 1">This board has {{ untied.length }} servos not tied to any gate. If one of them
          turns this gate, <b>Find its servo</b> shows you each in turn.</div>
        <div class="sub" *ngIf="!untied.length">Plug its servo into this board's chain. A servo already set up shows up here to
          find; a new one out of the box is added with <b>Add a new servo</b>.</div>
        <div class="btns">
          <button class="btn" type="button" *ngIf="untied.length" (click)="findItsServo()" [disabled]="busy">Find its servo</button>
          <button class="btn" type="button" (click)="replace()" [disabled]="busy">Add a new servo</button>
        </div>
      </ng-container>
      <ng-container *ngIf="finding !== null">
        <div class="line">Did the {{ name }} just move?</div>
        <div class="sub">A servo on this board turned a little and back.</div>
        <div class="btns">
          <button class="btn" type="button" (click)="itMoved()" [disabled]="busy">Yes, that's its servo</button>
          <button class="btn" type="button" (click)="notThisOne()" [disabled]="busy">No{{ findQueue.length ? ', try the next' : '' }}</button>
          <button class="btn" type="button" (click)="showAgain()" [disabled]="busy">Show me again</button>
        </div>
      </ng-container>
      <div class="msg" *ngIf="msg">{{ msg }}</div>
      <div class="err" *ngIf="error">{{ error }}</div>
    </div>

    <!-- A SERVO NO GATE HOLDS: say which gate it turns, from the gates on this board that have none. -->
    <div class="card" *ngIf="bus !== undefined && !sel">
      <div class="line">Not tied to a gate.</div>
      <div class="sub" *ngIf="servoless.length"><b>Find it</b> turns it a little and back, so you can see which gate it is on.
        Then tap that gate.</div>
      <div class="sub" *ngIf="!servoless.length">Every gate on this board has a servo. Draw its gate on the shop layout and wire
        it to this board, then come back to tie it.</div>
      <div class="btns" *ngIf="servoless.length">
        <button class="btn" type="button" (click)="findIt()" [disabled]="busy">Find it</button>
      </div>
      <div class="btns" *ngIf="servoless.length">
        <button class="btn" type="button" *ngFor="let g of servoless" (click)="tieTo(g)" [disabled]="busy">It turns the {{ g.name }}</button>
      </div>
      <div class="msg" *ngIf="msg">{{ msg }}</div>
      <div class="err" *ngIf="error">{{ error }}</div>
    </div>

    <ng-container *ngIf="bus !== undefined">
      <div class="card">
        <div class="line"><b>Load, last {{ history.length || '' }} moves</b><span class="sub" style="margin-left:auto">peak of each move</span></div>
        <svg class="spark" viewBox="0 0 320 72" preserveAspectRatio="none" *ngIf="history.length > 1; else noHistory"
             role="img" [attr.aria-label]="'Peak load of the last ' + history.length + ' moves'">
          <line *ngIf="usual !== null" x1="4" [attr.x2]="316" [attr.y1]="y(usual)" [attr.y2]="y(usual)"
                stroke="#e8e8e8" stroke-opacity="0.35" stroke-dasharray="4 4"/>
          <polyline [attr.points]="points()" fill="none" stroke="#3cbe6e" stroke-width="2"/>
          <circle [attr.cx]="lastX()" [attr.cy]="y(history[history.length - 1])" r="3.5" fill="#3cbe6e"/>
        </svg>
        <ng-template #noHistory><div class="sub">No moves reported yet. Each move adds its peak here.</div></ng-template>
        <div class="kv">
          <span>Last move</span><span>{{ state && state.moves ? state.peak + '%' : '—' }}</span>
          <span>Usually needs</span><span>{{ usual !== null ? usual + '%' : 'after a few moves' }}</span>
          <span>Position</span><span>{{ state?.ok ? (state?.deg ?? 0 | number:'1.0-0') + '°' : (state ? 'not answering' : '—') }}</span>
          <span>Supply at the servo</span><span>{{ state?.ok ? (state?.v ?? 0 | number:'1.1-1') + ' V' : '—' }}</span>
          <span>Temperature</span><span>{{ state?.ok ? state?.temp + ' °C' : '—' }}</span>
        </div>
      </div>

      <!-- The everyday controls are a gate's: an untied servo's page has its own Find it, in the card above. -->
      <div class="card" *ngIf="sel">
        <div class="btns">
          <button class="btn" type="button" (click)="findIt()" [disabled]="busy">Find it</button>
          <button class="btn" type="button" (click)="testMove()" [disabled]="busy || !sel">Test move</button>
          <button class="btn" type="button" (click)="setPositions()" [disabled]="busy || !sel">Set positions</button>
          <button class="btn" type="button" (click)="replace()" [disabled]="busy || !sel">Replace this servo</button>
        </div>
        <div class="sub" *ngIf="sel && !hasPositions()">Its positions aren't set yet: <b>Set positions</b> before it runs a tool.</div>
        <div class="sub"><b>Find it</b> turns it a little and back, so you can see which one this is. <b>Test move</b> runs
          it through its positions and back to closed.</div>
        <div class="msg" *ngIf="msg">{{ msg }}</div>
        <div class="err" *ngIf="error">{{ error }}</div>
      </div>

      <div class="dev-box" *ngIf="dev">
        <span class="dev-tag">Developer</span>
        <div class="dev-kv">
          <span>Servo number</span><span>#{{ bus }}</span>
          <span>Raw position</span><span>{{ state?.ok ? (state?.deg ?? 0 | number:'1.1-1') + '°' : '—' }}</span>
          <span>Load now</span><span>{{ state?.ok ? state?.load + '%' : '—' }}</span>
          <span>Moves since the board started</span><span>{{ state?.moves ?? '—' }}</span>
          <span>Reads missed</span><span>{{ state ? state.missed + ' of ' + state.reads : '—' }}</span>
        </div>
        <div class="btns">
          <button class="btn" type="button" (click)="torque(false)" [disabled]="busy">Relax (turn by hand)</button>
          <button class="btn" type="button" (click)="torque(true)" [disabled]="busy">Hold</button>
          <button class="btn" type="button" (click)="giveBack()" [disabled]="busy">Give back its number</button>
        </div>
      </div>
    </ng-container>
  `,
})
export class ServoPageComponent implements OnInit, OnDestroy {
  board = '';
  boardName = '';
  gateKey = '';
  sel: ConfigurableSelector | null = null;
  name = 'Servo';
  bus: number | undefined;
  state: BusServoState | undefined;
  history: number[] = [];
  usual: number | null = null;
  dev = false;
  busy = false;
  msg = '';
  error = '';
  /** Servo numbers answering on this board that no gate in the shop holds. */
  untied: number[] = [];
  /** Gates on this board with no servo number. */
  servoless: ConfigurableSelector[] = [];
  /** Find its servo: the servo being shown now, and the ones still to show. */
  finding: number | null = null;
  findQueue: number[] = [];
  private topo: Topology | null = null;
  private poll: ReturnType<typeof setInterval> | null = null;
  private devSub: Subscription | null = null;

  constructor(private api: ApiService, private route: ActivatedRoute, private router: Router, private devMode: DevModeService) {}

  async ngOnInit(): Promise<void> {
    this.board = this.route.snapshot.paramMap.get('board') ?? '';
    this.gateKey = this.route.snapshot.paramMap.get('gate') ?? '';
    this.devMode.load();
    this.devSub = this.devMode.on$.subscribe((on) => { this.dev = on; });
    try { this.topo = await this.api.getTopology(); } catch { this.topo = null; }
    this.resolveGate();
    await this.refresh();
    this.poll = setInterval(() => void this.refresh(), 3000);
  }
  ngOnDestroy(): void {
    if (this.poll) clearInterval(this.poll);
    this.devSub?.unsubscribe();
  }

  private resolveGate(): void {
    // `bus-N` is a servo no gate holds (Boards lists it as "not tied to a gate"); anything else is the gate's id.
    if (this.gateKey.startsWith('bus-')) { this.bus = Number(this.gateKey.slice(4)); this.name = 'A servo not tied to a gate'; return; }
    this.sel = this.topo ? selectorsOnController(this.topo, this.board).find((s) => s.id === this.gateKey) ?? null : null;
    this.name = this.sel?.name || this.gateKey;
    this.bus = this.sel ? busNumberOf(this.sel) : undefined;
  }

  private async refresh(): Promise<void> {
    let nodes: NodeLinkState[] = [];
    try { nodes = await this.api.getNodes(); } catch { return; }   // a missed poll says nothing new
    const n = nodes.find((x) => x.id === this.board);
    this.boardName = n?.name || this.board;
    this.state = this.bus !== undefined ? n?.bus?.find((b) => b.id === this.bus) : undefined;
    const held = new Set<number>();
    for (const s of this.topo ? configurableSelectorsOf(this.topo) : []) { const b = busNumberOf(s); if (b !== undefined) held.add(b); }
    this.untied = (n?.bus ?? []).filter((b) => b.ok && !held.has(b.id)).map((b) => b.id);
    this.servoless = this.topo ? selectorsOnController(this.topo, this.board).filter((s) => busNumberOf(s) === undefined) : [];
    this.history = this.state?.history ?? [];
    this.usual = usualLoad(this.history);
  }

  // ── the line: 0..max% of load over the chart's height ──
  private max(): number { return Math.max(50, ...this.history) * 1.1; }
  y(v: number): number { return 66 - (v / this.max()) * 60; }
  points(): string {
    const n = this.history.length;
    return this.history.map((v, i) => `${(4 + (i / (n - 1)) * 312).toFixed(1)},${this.y(v).toFixed(1)}`).join(' ');
  }
  lastX(): number { return this.history.length > 1 ? 316 : 160; }

  // ── controls ──
  private async act(label: string, f: () => Promise<string | void>): Promise<void> {
    this.busy = true; this.msg = ''; this.error = '';
    try { this.msg = (await f()) || ''; }
    catch (e) { this.error = reportFailure(`Servo · ${label}`, e, failureReason(e)); }
    finally { this.busy = false; }
  }
  findIt(): Promise<void> {
    return this.act('find it', async () => {
      const r = await this.api.servoOp(this.board, 'wiggle', { bus: this.bus });
      if (!r.ok) throw new Error(r.err || 'It did not move.');
      return 'It turned a little and back.';
    });
  }
  testMove(): Promise<void> {
    return this.act('test move', async () => {
      const sel = this.sel;
      if (!sel || sel.kind === 'linear') throw new Error('Test a slider from its own setup screen.');
      const angles = absoluteAngles(sel) as Record<string, number> | null;
      const closed = sel.states.find((s) => s.isClosed);
      if (!angles || !closed || angles[closed.id] === undefined) throw new Error('Set its positions first.');
      const order = [...sel.states.filter((s) => !s.isClosed), closed];
      for (const st of order) {
        await this.api.jogGate(sel, Math.round(angles[st.id]));
        await new Promise((r) => setTimeout(r, 2500));   // a quarter turn takes about 1.7 s at the board's speed
      }
      return 'Done — it went through each position and back to closed.';
    });
  }
  setPositions(): void { void this.router.navigate(['/gates'], { queryParams: { gate: this.sel?.id } }); }
  replace(): void {
    void this.router.navigate(['/boards/add-servo', this.board], this.sel ? { queryParams: { gate: this.sel.id, replace: this.bus !== undefined ? 1 : null } } : {});
  }
  torque(on: boolean): Promise<void> {
    return this.act(on ? 'hold' : 'relax', async () => {
      const r = await this.api.servoOp(this.board, 'torque', { bus: this.bus, on });
      if (!r.ok) throw new Error(r.err || 'The servo did not answer.');
      return on ? 'Holding.' : 'Relaxed — it can be turned by hand. Hold it again, or move the gate, to make it hold.';
    });
  }
  giveBack(): Promise<void> {
    return this.act('give back its number', async () => {
      const r = await this.api.servoOp(this.board, 'reset', { bus: this.bus });
      if (!r.ok) throw new Error(r.err || 'The servo did not answer.');
      // The layout forgets it too, or the gate would name a number nothing answers to.
      if (this.sel && this.topo) {
        const doc = JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology;
        const g = selectorsOnController(doc, this.board).find((s) => s.id === this.sel!.id);
        if (g) { if (g.kind === 'linear') delete g.linear?.bus; else delete g.servo.bus; }
        await this.api.putTopology(doc);
        this.topo = doc;
      }
      this.bus = undefined;
      return 'It is a new servo again (number 1). Add it for a gate to use it.';
    });
  }
  hasPositions(): boolean {
    const sv = this.sel && this.sel.kind !== 'linear' ? this.sel.servo : undefined;
    return !!sv && typeof sv.referenceAngle === 'number';
  }

  // ── Find its servo: show each untied servo in turn until the person says which one turned this gate ──
  findItsServo(): Promise<void> {
    this.findQueue = [...this.untied];
    return this.showNext();
  }
  private showNext(): Promise<void> {
    const n = this.findQueue.shift();
    if (n === undefined) {
      this.finding = null;
      this.msg = '';
      this.error = `None of this board's untied servos turned the ${this.name}. Check its cable is plugged into the board's chain, or add it as a new servo.`;
      return Promise.resolve();
    }
    this.finding = n;
    return this.wiggle(n);
  }
  showAgain(): Promise<void> { return this.finding === null ? Promise.resolve() : this.wiggle(this.finding); }
  notThisOne(): Promise<void> { return this.showNext(); }
  private wiggle(n: number): Promise<void> {
    return this.act('find its servo', async () => {
      const r = await this.api.servoOp(this.board, 'wiggle', { bus: n });
      if (!r.ok) throw new Error(r.err || 'That servo did not move.');
    });
  }
  itMoved(): Promise<void> {
    const n = this.finding; const sel = this.sel;
    if (n === null || !sel) return Promise.resolve();
    return this.act('tie its servo', async () => {
      await this.tie(sel.id, n);
      this.finding = null; this.findQueue = [];
      this.resolveGate();
      await this.refresh();
      return this.hasPositions() ? `Tied. The ${this.name} is ready to run.` : `Tied. Now set its positions.`;
    });
  }
  /** From an untied servo's page: it turns this gate. */
  tieTo(g: ConfigurableSelector): Promise<void> {
    const n = this.bus;
    if (n === undefined) return Promise.resolve();
    return this.act('tie to a gate', async () => {
      await this.tie(g.id, n);
      // The same page, now the gate's: the router keeps this component when only the address changes, so it is pointed
      // at the gate here rather than waiting for an ngOnInit that will not run again.
      void this.router.navigate(['/boards/servo', this.board, g.id], { replaceUrl: true });
      this.gateKey = g.id;
      this.resolveGate();
      await this.refresh();
      return this.hasPositions() ? `Tied. The ${this.name} is ready to run.` : `Tied. Now set its positions.`;
    });
  }
  /** Write the servo's number into the gate, in the layout as read NOW, so whatever else changed since is kept. */
  private async tie(gateId: string, n: number): Promise<void> {
    const doc = JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology;
    setServoNumber(doc, gateId, n);
    await this.api.putTopology(doc);
    this.topo = doc;
  }

  back(): void { void this.router.navigate(['/boards']); }
}
