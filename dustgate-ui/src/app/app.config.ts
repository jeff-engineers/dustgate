import { ApplicationConfig } from '@angular/core';
import { provideRouter, withHashLocation } from '@angular/router';
import { provideHttpClient } from '@angular/common/http';
import { routes } from './app.routes';
import { ApiService } from './services/api.service';
import { DemoApiService } from './services/demo-api.service';

// Demo mode: active on any non-localhost host (i.e. the Vercel deployment),
// or when ?demo=true is present in the URL (for local dev testing).
const isDemo =
  !['localhost', '127.0.0.1'].includes(window.location.hostname) ||
  new URLSearchParams(window.location.search).has('demo');

export const appConfig: ApplicationConfig = {
  providers: [
    provideRouter(routes, withHashLocation()),
    provideHttpClient(),
    // In demo mode, substitute DemoApiService everywhere ApiService is injected.
    // All components and ClaudeService use ApiService — the override is transparent.
    ...(isDemo ? [{ provide: ApiService, useClass: DemoApiService }] : []),
  ]
};
