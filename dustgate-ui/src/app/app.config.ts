import { ApplicationConfig } from '@angular/core';
import { provideRouter, withHashLocation } from '@angular/router';
import { provideHttpClient } from '@angular/common/http';
import { routes } from './app.routes';
import { ApiService } from './services/api.service';
import { DemoApiService } from './services/demo-api.service';
import { setAccessCode } from './services/access-code';

// Demo mode: active on any non-localhost host (i.e. the Vercel deployment),
// or when ?demo=true is present in the URL (for local dev testing).
const isDemo =
  !['localhost', '127.0.0.1'].includes(window.location.hostname) ||
  new URLSearchParams(window.location.search).has('demo');

// Pick up ?code=... once (e.g. a link shared with an interviewer) and persist
// it so future demo requests carry it without needing it in the URL again.
const codeParam = new URLSearchParams(window.location.search).get('code');
if (codeParam) {
  setAccessCode(codeParam);
}

export const appConfig: ApplicationConfig = {
  providers: [
    provideRouter(routes, withHashLocation()),
    provideHttpClient(),
    // In demo mode, substitute DemoApiService everywhere ApiService is injected.
    // All components and ClaudeService use ApiService — the override is transparent.
    ...(isDemo ? [{ provide: ApiService, useClass: DemoApiService }] : []),
  ]
};
