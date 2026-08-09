import { Routes } from '@angular/router';
import { DashboardComponent } from './dashboard/dashboard.component';
import { SetupComponent } from './setup/setup.component';
import { ManualSetupComponent } from './setup-manual/setup-manual.component';
import { SettingsComponent } from './settings/settings.component';

export const routes: Routes = [
  { path: '',             component: DashboardComponent },
  { path: 'setup',        component: SetupComponent },
  { path: 'setup/manual', component: ManualSetupComponent },
  { path: 'settings',     component: SettingsComponent },
  // v2 Live view — silent lazy route, nothing links to it yet (see docs/v2-ui-design.md).
  { path: 'shop',         loadComponent: () => import('./live/live.component').then(m => m.LiveViewComponent) },
  { path: 'build',        loadComponent: () => import('./build/build.component').then(m => m.BuildComponent) },
  { path: 'tools',        loadComponent: () => import('./tools/tool-setup.component').then(m => m.ToolSetupComponent) },
  { path: 'boards',       loadComponent: () => import('./boards/board-setup.component').then(m => m.BoardSetupComponent) },
  // /gates was a separate "set up every gate" pass. Retired: the canvas shows
  // which gates are unset (orange dot) and configuring one is a tap on that dot,
  // so the pass was a second place to do the same thing. Redirect kept for
  // bookmarks and for anything still linking here.
  { path: 'gates',        redirectTo: 'build' },
  { path: '**',           redirectTo: '' }
];
