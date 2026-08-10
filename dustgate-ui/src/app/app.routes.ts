import { Routes } from '@angular/router';
import { LandingComponent } from './landing/landing.component';
import { SettingsComponent } from './settings/settings.component';

// ── Phase 2 is the product; phase 1 is kept, not linked ──────────────────────
//
// `/` no longer renders the old dashboard. It asks the controller for the saved
// layout and forwards: a shop that's finished goes to the Live tool list, anything
// unfinished goes straight to the layout tool. See landing.component.ts.
//
// The phase-1 dashboard and its two setup wizards are DEPRECATED, not deleted.
// Nothing links to them and their old paths redirect into the new flow, but they
// stay reachable under /legacy/* and stay in the build. That's deliberate: phase 1
// is still the only UI that has been through a complete setup on real hardware,
// and phase 2 has not been on a bench yet. Delete them once it has — at which
// point setup.component, setup-manual.component, dashboard.component and the
// helpers only they use (dust-collector-configurator, gate-positioner,
// outlet-configurator, visualizer/manifold-visualizer, dashboard/manual-controls)
// can all go together.

export const routes: Routes = [
  { path: '',             component: LandingComponent },

  // Phase 2
  { path: 'shop',         loadComponent: () => import('./live/live.component').then(m => m.LiveViewComponent) },
  { path: 'build',        loadComponent: () => import('./build/build.component').then(m => m.BuildComponent) },
  { path: 'tools',        loadComponent: () => import('./tools/tool-setup.component').then(m => m.ToolSetupComponent) },
  { path: 'boards',       loadComponent: () => import('./boards/board-setup.component').then(m => m.BoardSetupComponent) },
  { path: 'settings',     component: SettingsComponent },

  // Retired paths. /gates was a separate "set up every gate" pass — the canvas now
  // shows which gates are unset and configuring one is a tap on its badge. /setup
  // and /setup/manual were the phase-1 wizards; building the layout IS the setup now.
  { path: 'gates',        redirectTo: 'build' },
  { path: 'setup',        pathMatch: 'full', redirectTo: 'build' },
  { path: 'setup/manual', redirectTo: 'build' },

  // Deprecated phase-1 UI: lazy so it costs nothing until someone asks for it.
  { path: 'legacy',           pathMatch: 'full', redirectTo: 'legacy/dashboard' },
  { path: 'legacy/dashboard', loadComponent: () => import('./dashboard/dashboard.component').then(m => m.DashboardComponent) },
  { path: 'legacy/setup',     loadComponent: () => import('./setup/setup.component').then(m => m.SetupComponent) },
  { path: 'legacy/setup/manual', loadComponent: () => import('./setup-manual/setup-manual.component').then(m => m.ManualSetupComponent) },

  { path: '**',           redirectTo: '' },
];
