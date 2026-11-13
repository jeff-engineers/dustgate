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
import { selectorsOnController, type ConfigurableSelector } from '../gates/selector-types';
import { busNumberOf, usualLoad } from './bus-servos';

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

    <div class="card" *ngIf="bus === undefined">
      <div class="line">No servo yet.</div>
      <div class="sub">Plug one into this board's chain and add it for this gate.</div>
      <div class="btns"><button class="btn" type="button" (click)="replace()">Add a servo</button></div>
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

      <div class="card">
        <div class="btns">
          <button class="btn" type="button" (click)="findIt()" [disabled]="busy">Find it</button>
          <button class="btn" type="button" (click)="testMove()" [disabled]="busy || !sel">Test move</button>
          <button class="btn" type="button" (click)="setPositions()" [disabled]="busy || !sel">Set positions</button>
          <button class="btn" type="button" (click)="replace()" [disabled]="busy || !sel">Replace this servo</button>
        </div>
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
  back(): void { void this.router.navigate(['/boards']); }
}
