import { Component, Input, Output, EventEmitter } from '@angular/core';
import { CommonModule } from '@angular/common';

export interface ToolButton {
  stop: number;
  name: string;
  powerW: number;
  active: boolean;
  reachable: boolean;
}

/**
 * ManualControlsComponent — the original button-list dashboard controls
 * (HOME button, one row per tool, dust collector toggle).
 *
 * Retired in favor of making the manifold visualizer itself the interactive
 * control surface (tap a gate / the dust collector directly — see
 * ManifoldVisualizerComponent's `interactive` input and DashboardComponent).
 * Kept here, fully wired and compiling, in case that decision needs
 * reverting: drop `<app-manual-controls>` back into the dashboard template
 * in place of the interactive visualizer and wire up its outputs to the
 * same api.home() / api.moveToStop() / api.setDustCollector() calls.
 */
@Component({
  selector: 'app-manual-controls',
  standalone: true,
  imports: [CommonModule],
  styles: [`
    :host {
      display: flex;
      flex-direction: column;
      gap: var(--gap);
    }

    .section-label {
      font-size: 11px;
      font-weight: 600;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--muted);
      margin-top: 4px;
    }

    .tool-btn {
      width: 100%;
      min-height: var(--btn-h);
      border-radius: var(--radius);
      background: var(--surface);
      border: 2px solid var(--border);
      color: var(--text);
      display: flex;
      align-items: center;
      padding: 0 20px;
      gap: 14px;
      transition: border-color 0.15s, background 0.15s;
      text-align: left;
    }

    .tool-btn .btn-name {
      font-size: 20px;
      font-weight: 600;
      flex: 1;
    }

    .tool-btn .btn-power {
      font-size: 13px;
      color: var(--muted);
      white-space: nowrap;
    }

    .tool-btn .btn-dot {
      width: 10px; height: 10px;
      border-radius: 50%;
      background: var(--border);
      flex-shrink: 0;
    }

    .tool-btn.current {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 10%, var(--surface));
    }
    .tool-btn.current .btn-dot { background: var(--accent); }

    .tool-btn.tool-on .btn-dot { background: var(--success); }
    .tool-btn.tool-on .btn-power { color: var(--success); }

    .tool-btn.offline { opacity: 0.45; }

    .home-btn {
      background: var(--surface);
      border: 2px solid var(--border);
      border-radius: var(--radius);
      color: var(--text);
      width: 100%;
      min-height: var(--btn-h);
      font-size: 20px;
      font-weight: 700;
      letter-spacing: 0.05em;
    }
    .home-btn.current {
      border-color: var(--accent);
      background: color-mix(in srgb, var(--accent) 10%, var(--surface));
      color: var(--accent);
    }

    .dc-btn {
      width: 100%;
      min-height: var(--btn-h);
      border-radius: var(--radius);
      display: flex;
      align-items: center;
      padding: 0 20px;
      gap: 14px;
      font-size: 20px;
      font-weight: 600;
      border: 2px solid var(--border);
      background: var(--surface);
      color: var(--text);
    }
    .dc-btn .dc-icon { font-size: 22px; }
    .dc-btn.dc-on {
      border-color: var(--success);
      background: color-mix(in srgb, var(--success) 10%, var(--surface));
      color: var(--success);
    }
    .dc-btn .dc-status {
      margin-left: auto;
      font-size: 13px;
      color: var(--muted);
      font-weight: 400;
    }
    .dc-btn.dc-on .dc-status { color: var(--success); }
    .dc-btn:disabled { opacity: 0.5; }
  `],
  template: `
    <!-- HOME -->
    <span class="section-label">Position</span>
    <button class="home-btn"
            [class.current]="currentStop === 0"
            (click)="home.emit()">
      HOME
    </button>

    <!-- Tool buttons -->
    <span class="section-label" *ngIf="toolButtons.length > 0">Tools</span>
    <button *ngFor="let t of toolButtons"
            class="tool-btn"
            [class.current]="currentStop === t.stop"
            [class.tool-on]="t.active"
            [class.offline]="!t.reachable"
            (click)="move.emit(t.stop)">
      <span class="btn-dot"></span>
      <span class="btn-name">{{ t.name }}</span>
      <span class="btn-power" *ngIf="t.reachable">{{ t.powerW | number:'1.0-0' }} W</span>
      <span class="btn-power" *ngIf="!t.reachable">offline</span>
    </button>

    <!-- Dust collector -->
    <span class="section-label">Dust Collector</span>
    <button class="dc-btn"
            [class.dc-on]="dcOn"
            [disabled]="!dcConfigured"
            (click)="toggleDc.emit()">
      <span class="dc-icon">{{ dcOn ? '💨' : '🌀' }}</span>
      <span>Dust Collector</span>
      <span class="dc-status">{{ dcConfigured ? (dcOn ? 'ON' : 'OFF') : 'no plug' }}</span>
    </button>
  `
})
export class ManualControlsComponent {
  @Input() currentStop: number | null = null;
  @Input() toolButtons: ToolButton[] = [];
  @Input() dcOn = false;
  @Input() dcConfigured = false;

  @Output() home = new EventEmitter<void>();
  @Output() move = new EventEmitter<number>();
  @Output() toggleDc = new EventEmitter<void>();
}
