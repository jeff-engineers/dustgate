import { Routes } from '@angular/router';
import { DashboardComponent } from './dashboard/dashboard.component';
import { SetupComponent } from './setup/setup.component';
import { ManualSetupComponent } from './setup-manual/setup-manual.component';

export const routes: Routes = [
  { path: '',             component: DashboardComponent },
  { path: 'setup',        component: SetupComponent },
  { path: 'setup/manual', component: ManualSetupComponent },
  { path: '**',           redirectTo: '' }
];
