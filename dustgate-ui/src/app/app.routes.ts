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

  // Retired paths. /gates was a separate "set up every gate" pass — the canvas now
  // shows which gates are unset and configuring one is a tap on its badge. /setup
  // was the old guided wizard; building the layout IS the setup now.
  { path: 'gates',        redirectTo: 'build' },
  { path: 'setup',        pathMatch: 'full', redirectTo: 'build' },
  { path: 'setup/manual', redirectTo: 'build' },

  { path: '**',           redirectTo: '' },
];
