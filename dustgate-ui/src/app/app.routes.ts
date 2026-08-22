import { Routes } from '@angular/router';
import { LandingComponent } from './landing/landing.component';
import { SettingsComponent } from './settings/settings.component';

// `/` asks the controller for the saved layout and forwards: a shop that's
// finished goes to the Live tool list, anything unfinished goes straight to the
// layout tool. See landing.component.ts.

export const routes: Routes = [
  { path: '',             component: LandingComponent },

  { path: 'shop',         loadComponent: () => import('./live/live.component').then(m => m.LiveViewComponent) },
  { path: 'build',        loadComponent: () => import('./build/build.component').then(m => m.BuildComponent) },
  { path: 'tools',        loadComponent: () => import('./tools/tool-setup.component').then(m => m.ToolSetupComponent) },
  { path: 'boards',       loadComponent: () => import('./boards/board-setup.component').then(m => m.BoardSetupComponent) },
  { path: 'settings',     component: SettingsComponent },

  // /gates is BACK, and not as the screen that was retired. That one was a "set up
  // every gate" pass the canvas made redundant — the canvas shows which gates are
  // unset and configuring one is a tap on its badge. This is the other errand:
  // recalibrating a valve that got knocked, from the shop floor, without opening a
  // layout tool. Same calibration underneath. See docs/mockups/gates-list.html.
  { path: 'gates',        loadComponent: () => import('./gates/gate-list.component').then(m => m.GateListComponent) },

  // Retired paths. /setup was the old guided wizard; building the layout IS the
  // setup now.
  { path: 'setup',        pathMatch: 'full', redirectTo: 'build' },
  { path: 'setup/manual', redirectTo: 'build' },

  { path: '**',           redirectTo: '' },
];
