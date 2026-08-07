import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router } from '@angular/router';
import { ApiService } from '../services/api.service';
import type { Topology } from '@topology';
import { SelectorConfigComponent } from './selector-config.component';
import {
  ConfigurableSelector, configurableSelectorsOf, elementsOf, isCalibrated, isServoKind, kindLabel,
} from './selector-types';

// ── Gate setup pass ──────────────────────────────────────────────────────────
// The second-pass companion to /tools: once the plumbing is drawn, walk every gate —
// ball valve, manifold, or sliding gate — and teach it where its positions are. Same
// shape as the tool pass: fetch the topology, edit a list, PUT it back.
//
// The build canvas can also open one gate's config directly; both use
// SelectorConfigComponent so there's one place the fields live.

@Component({
  selector: 'app-gate-setup',
  standalone: true,
  imports: [CommonModule, SelectorConfigComponent],
  styles: [`
    :host { display: block; max-width: 460px; margin: 0 auto; padding: 16px 14px 40px; }
    .head { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
    .head .step { font-size: 12.5px; color: var(--muted); }

    .card { background: var(--surface); border: 1px solid var(--border); border-radius: 16px; padding: 6px 16px; }
    .row { display: flex; align-items: center; gap: 10px; padding: 12px 0; border-bottom: 1px solid var(--border); width: 100%;
           background: none; border-left: none; border-right: none; border-top: none; color: var(--text); text-align: left; }
    .row:last-child { border-bottom: none; }
    .row .nm { font-size: 14px; }
    .row .sub { font-size: 11.5px; color: var(--muted); margin-top: 2px; }
    .badge { font-size: 11.5px; padding: 3px 10px; border-radius: 20px; margin-left: auto; flex-shrink: 0; }
    .badge.ok { color: var(--success); background: rgba(60,190,110,0.12); }
    .badge.todo { color: var(--accent); background: rgba(240,165,0,0.12); }

    .empty { text-align: center; color: var(--muted); font-size: 13px; padding: 22px 10px; line-height: 1.6; }
    .err { font-size: 12.5px; color: var(--danger); margin-top: 12px; }

    .nav { display: flex; gap: 10px; margin-top: 18px; }
    .nav button { border-radius: var(--radius); padding: 12px 16px; font-size: 14px; }
    .nav .back { flex: 0 0 auto; background: var(--surface); border: 1px solid var(--border); color: var(--text); }
    .nav .next { flex: 1; background: var(--accent); border: none; color: #1a1200; font-weight: 600; }
    .nav .next:disabled { opacity: 0.5; }
  `],
  template: `
    <!-- Configuring one gate — the list steps aside. -->
    <ng-container *ngIf="editing as sel; else list">
      <div class="head">
        <span class="step">{{ sel.name || 'Gate' }}</span>
        <span class="step">{{ position() }}</span>
      </div>
      <app-selector-config [sel]="sel" [topo]="topo!"
                           (saved)="onSaved($event)" (cancelled)="editing = null">
      </app-selector-config>
      <p class="err" *ngIf="saveError">{{ saveError }}</p>
    </ng-container>

    <ng-template #list>
      <div class="head">
        <span class="step">Set up your gates</span>
        <span class="step" *ngIf="gates.length">{{ doneCount() }} of {{ gates.length }} done</span>
      </div>

      <div class="card" *ngIf="gates.length; else none">
        <button class="row" *ngFor="let g of gates" (click)="edit(g)">
          <div style="flex:1">
            <div class="nm">{{ g.name || g.id }}</div>
            <div class="sub">{{ subtitle(g) }}</div>
          </div>
          <span class="badge" [class.ok]="calibrated(g)" [class.todo]="!calibrated(g)">
            {{ calibrated(g) ? 'Configured' : 'Needs setup' }}
          </span>
        </button>
      </div>
      <ng-template #none>
        <div class="empty">
          No gates yet.<br/>Draw your plumbing first, then come back.
        </div>
      </ng-template>

      <p class="err" *ngIf="saveError">{{ saveError }}</p>

      <div class="nav">
        <button class="back" (click)="goBuild()">← Layout</button>
        <button class="next" (click)="nextUnconfigured()" [disabled]="!firstUnconfigured()">
          {{ firstUnconfigured() ? 'Next unconfigured →' : 'All set' }}
        </button>
      </div>
    </ng-template>
  `,
})
export class GateSetupComponent implements OnInit {
  gates: ConfigurableSelector[] = [];
  editing: ConfigurableSelector | null = null;
  topo: Topology | null = null;
  saveError = '';

  constructor(private api: ApiService, private router: Router) {}

  async ngOnInit(): Promise<void> {
    try {
      await this.api.whenReady();          // else the first fetch 401s and reads as "no gates"
      this.topo = JSON.parse(JSON.stringify(await this.api.getTopology())) as Topology;
    } catch {
      return;                                  // no topology yet — the empty state covers it
    }
    this.refresh();
  }

  calibrated(g: ConfigurableSelector): boolean { return isCalibrated(g); }
  doneCount(): number { return this.gates.filter(isCalibrated).length; }
  firstUnconfigured(): ConfigurableSelector | null { return this.gates.find((g) => !isCalibrated(g)) ?? null; }

  subtitle(g: ConfigurableSelector): string {
    const board = g.controllerId || 'no board';
    if (!isServoKind(g)) return `${kindLabel(g).toLowerCase()} · ${board}, stepper`;
    const ch = typeof g.servo?.channel === 'number' ? `servo ${g.servo.channel + 1}` : 'no servo assigned';
    return `${kindLabel(g).toLowerCase()} · ${board}, ${ch}`;
  }

  position(): string {
    const i = this.gates.findIndex((g) => g.id === this.editing?.id);
    return i < 0 ? '' : `${i + 1} of ${this.gates.length}`;
  }

  edit(g: ConfigurableSelector): void { this.saveError = ''; this.editing = g; }

  nextUnconfigured(): void {
    const next = this.firstUnconfigured();
    if (next) this.edit(next);
  }

  /** Splice the configured gate back into the topology and persist. The device
   *  validates too, so a rejection here is worth showing rather than swallowing. */
  async onSaved(updated: ConfigurableSelector): Promise<void> {
    if (!this.topo) return;
    const els = elementsOf(this.topo);
    const i = els.findIndex((e) => e.id === updated.id);
    if (i < 0) return;
    els[i] = updated as unknown as (typeof els)[number];
    try {
      await this.api.putTopology(this.topo);
      this.saveError = '';
      this.editing = null;
      this.refresh();
    } catch (e: unknown) {
      this.saveError = this.message(e);
    }
  }

  goBuild(): void { void this.router.navigate(['/build']); }

  private refresh(): void {
    this.gates = this.topo ? configurableSelectorsOf(this.topo) : [];
  }

  private message(e: unknown): string {
    const err = e as { error?: { error?: string; errors?: { message?: string }[] } };
    const first = err?.error?.errors?.[0]?.message;
    return first || err?.error?.error || 'Couldn\'t save — check the gate is reachable.';
  }
}
