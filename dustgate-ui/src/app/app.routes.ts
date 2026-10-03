import { Routes } from '@angular/router';
import { LandingComponent } from './landing/landing.component';
import { SettingsComponent } from './settings/settings.component';
import { LiveViewComponent } from './live/live.component';
import { BuildComponent } from './build/build.component';
import { ToolSetupComponent } from './tools/tool-setup.component';
import { BoardSetupComponent } from './boards/board-setup.component';
import { BrainLogComponent } from './boards/brain-log.component';
import { GateListComponent } from './gates/gate-list.component';

// EVERY SCREEN IS IN THE ONE BUNDLE (2026-10-03). These were lazy `import()`s,
// which makes the build emit a file per screen plus shared chunks — seventeen
// files, and a cold load asked for six at once. The app is served by a board with
// ~55 KB of free RAM and each open file costs it ~10 KB: that burst crashed it in
// a loop. Lazy loading saves a network that is not there (this is a LAN), so it
// was all cost.

// `/` asks the controller for the saved layout and forwards: a shop that's
// finished goes to the Live tool list, anything unfinished goes straight to the
// layout tool. See landing.component.ts.

export const routes: Routes = [
  { path: '',             component: LandingComponent },

  { path: 'shop',         component: LiveViewComponent },
  { path: 'build',        component: BuildComponent },
  { path: 'tools',        component: ToolSetupComponent },
  { path: 'boards',       component: BoardSetupComponent },
  // The brain's serial output over WiFi — a bench and support screen, reached
  // only from the brain's row in Your boards. docs/mockups/brain-log.html.
  { path: 'boards/log',   component: BrainLogComponent },
  { path: 'settings',     component: SettingsComponent },

  // /gates is BACK, and not as the screen that was retired. That one was a "set up
  // every gate" pass the canvas made redundant — the canvas shows which gates are
  // unset and configuring one is a tap on its badge. This is the other errand:
  // recalibrating a valve that got knocked, from the shop floor, without opening a
  // layout tool. Same calibration underneath. See docs/mockups/gates-list.html.
  { path: 'gates',        component: GateListComponent },

  // Retired paths. /setup was the old guided wizard; building the layout IS the
  // setup now.
  { path: 'setup',        pathMatch: 'full', redirectTo: 'build' },
  { path: 'setup/manual', redirectTo: 'build' },

  { path: '**',           redirectTo: '' },
];
