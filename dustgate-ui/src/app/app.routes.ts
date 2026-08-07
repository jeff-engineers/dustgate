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
  { path: 'gates',        loadComponent: () => import('./gates/gate-setup.component').then(m => m.GateSetupComponent) },
  { path: '**',           redirectTo: '' }
];
