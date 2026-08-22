/** The front door.
 *
 *  `/` doesn't render anything of its own for long: it asks the controller for the
 *  saved layout and forwards to wherever the person actually wants to be — the Live
 *  tool list if their shop is finished, the layout tool if it isn't.
 *
 *  Deliberately a component rather than a route guard. A guard blocks activation
 *  while it awaits, so a slow or absent controller leaves a blank screen with
 *  nothing to explain it. This way the "checking" state is real UI, and a
 *  controller we can't reach gets an honest message instead of a guess — sending
 *  someone into the layout tool while offline would invite them to edit a layout
 *  they can't compare against the device.
 */

import { Component, OnInit } from '@angular/core';
import { CommonModule } from '@angular/common';
import { Router, RouterLink } from '@angular/router';
import { ApiService } from '../services/api.service';
import { shopReadiness } from '../services/shop-ready';

@Component({
  selector: 'app-landing',
  standalone: true,
  imports: [CommonModule, RouterLink],
  styles: [`
    :host { display: flex; align-items: center; justify-content: center;
            min-height: 100dvh; min-height: 100vh; padding: 24px; }
    .card { width: 100%; max-width: 360px; display: flex; flex-direction: column;
            align-items: center; gap: 16px; text-align: center; }
    .spinner { width: 26px; height: 26px; border: 2.5px solid var(--border);
               border-top-color: var(--accent); border-radius: 50%;
               animation: spin 0.8s linear infinite; }
    @keyframes spin { to { transform: rotate(360deg); } }
    @media (prefers-reduced-motion: reduce) { .spinner { animation-duration: 2.4s; } }
    h1 { font-size: 17px; font-weight: 600; }
    p { color: var(--muted); font-size: 14px; line-height: 1.5; }
    .row { display: flex; gap: 10px; flex-wrap: wrap; justify-content: center; }
    button, a.btn { background: var(--surface); border: 1px solid var(--border);
                    color: var(--text); border-radius: var(--radius); padding: 10px 16px;
                    font-size: 14px; text-decoration: none; }
    a.primary { background: var(--accent); border-color: var(--accent); color: #1a1200; font-weight: 600; }
  `],
  template: `
    <div class="card" *ngIf="!error">
      <div class="spinner" role="status" aria-label="Checking your shop"></div>
      <p>Checking your shop…</p>
    </div>

    <div class="card" *ngIf="error">
      <h1>Can’t reach the dust collector</h1>
      <p>{{ error }}</p>
      <div class="row">
        <button (click)="decide()">Try again</button>
        <a class="btn" routerLink="/build">Open the layout tool</a>
      </div>
    </div>
  `,
})
export class LandingComponent implements OnInit {
  error = '';

  constructor(private api: ApiService, private router: Router) {}

  ngOnInit(): void { void this.decide(); }

  async decide(): Promise<void> {
    this.error = '';
    try {
      // The API service acquires its key asynchronously, and a fetch that races it
      // comes back 401 — which reads exactly like "fresh device" and would send
      // someone to the layout tool with a shop they already have. Every other
      // page waits for this; the front door has to as well.
      await this.api.whenReady();
      const topo = await this.api.getTopology();
      const { ready } = shopReadiness(topo);
      await this.router.navigate([ready ? '/shop' : '/build']);
    } catch (e: unknown) {
      // A 404 is the normal "nothing configured yet" answer, not a failure — that
      // person wants the layout tool. Anything else means we couldn't ask.
      const status = (e as { status?: number } | null)?.status;
      if (status === 404) { await this.router.navigate(['/build']); return; }
      this.error = status === 0 || status === undefined
        ? 'No answer from the controller. Check it’s powered on and on the same network.'
        : `The controller answered with an error (${status}).`;
    }
  }
}
