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

const isDemo =
  !isLocalNetworkHost(window.location.hostname) ||
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
