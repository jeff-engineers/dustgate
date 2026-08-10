import { ApplicationConfig } from '@angular/core';
import { provideRouter, withHashLocation } from '@angular/router';
import { provideHttpClient } from '@angular/common/http';
import { routes } from './app.routes';
import { ApiService } from './services/api.service';
import { DemoApiService } from './services/demo-api.service';
import { setAccessCode } from './services/access-code';

// Demo mode: active on the public Vercel deployment, or when ?demo=true is
// present (for local dev testing). NOT active for any way of reaching a real
// device — localhost, its mDNS hostname (*.local), or a LAN IP — since the
// UI is served directly from the device itself and real users reach it by
// exactly those addresses. A plain "hostname !== localhost" check would
// (and previously did) misclassify every real device as the demo, silently
// swapping in the fully-simulated DemoApiService instead of talking to the
// actual firmware — homing/moves would appear to succeed with zero physical
// motion.
function isLocalNetworkHost(hostname: string): boolean {
  if (hostname === 'localhost' || hostname === '127.0.0.1') return true;
  if (hostname.endsWith('.local')) return true; // mDNS, e.g. dustgate.local
  if (/^10\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(hostname)) return true;
  if (/^192\.168\.\d{1,3}\.\d{1,3}$/.test(hostname)) return true;
  if (/^172\.(1[6-9]|2\d|3[0-1])\.\d{1,3}\.\d{1,3}$/.test(hostname)) return true;
  return false;
}

// Demo can be forced with ?demo=true (and cleared with ?demo=false). Hash routing
// rewrites the address bar to "/#/route" on navigation, dropping the pre-hash
// query string — so a one-shot ?demo=true would be lost on the next navigate or
// reload. Persist it in sessionStorage (per-tab) so it sticks once set.
const DEMO_KEY = 'dustgate_demo';
function readForcedDemo(): boolean {
  try {
    const params = new URLSearchParams(window.location.search);
    if (params.has('demo')) {
      if (params.get('demo') === 'false') sessionStorage.removeItem(DEMO_KEY);
      else sessionStorage.setItem(DEMO_KEY, '1');
    }
    return sessionStorage.getItem(DEMO_KEY) === '1';
  } catch {
    // Private mode / storage disabled — fall back to the raw query param.
    return new URLSearchParams(window.location.search).has('demo');
  }
}

// The ONLY thing the host decides is whether the API is real or simulated. It used
// to also decide which UI you got — the public demo was pinned to the phase-1
// dashboard — but phase 2 is the product now and every deploy routes through it.
const isDemo = !isLocalNetworkHost(window.location.hostname) || readForcedDemo();

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
