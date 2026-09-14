import { ApplicationConfig } from '@angular/core';
import { provideRouter, withHashLocation, withInMemoryScrolling } from '@angular/router';
import { provideHttpClient } from '@angular/common/http';
import { routes } from './app.routes';
import { ApiService } from './services/api.service';
import { DemoApiService } from './services/demo-api.service';
// WHAT counts as demo mode lives in services/demo-mode.ts, because the answer is
// now needed on screen (the banner) as well as here.
import { IS_DEMO } from './services/demo-mode';

export const appConfig: ApplicationConfig = {
  providers: [
    // Every route is its own screen, so every navigation starts at the top of it.
    // Without this Angular leaves the window where the PREVIOUS screen left it:
    // scrolling down the Live list and tapping "Shop layout" landed on the build
    // canvas with the toolbar and guide bar already scrolled off the top, and the
    // canvas fills the rest, so there was nothing left to scroll back UP with.
    // 'enabled' (not 'top') so the back button still returns you to where you were.
    provideRouter(routes, withHashLocation(), withInMemoryScrolling({
      scrollPositionRestoration: 'enabled',
    })),
    provideHttpClient(),
    // In demo mode, substitute DemoApiService everywhere ApiService is injected.
    // Every component injects ApiService — the override is transparent.
    ...(IS_DEMO ? [{ provide: ApiService, useClass: DemoApiService }] : []),
  ]
};
