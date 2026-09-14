// demo-mode.ts — is this app talking to a real board, or simulating one?
//
// Lived inside app.config.ts until 2026-09-14, where it decided one thing: which
// ApiService to provide. It moved here because the ANSWER is now needed on
// screen as well — people shown the Vercel deployment have asked, more than
// once, whether they were switching on someone's real dust collector. A page
// that cannot tell you it is a simulation is a page that has to be explained
// out loud every time.
//
// Its own module rather than an export from app.config: app.config pulls in the
// router and every provider, and a component importing that to ask one boolean
// is a dependency nobody wants to reason about.

// Demo mode is active on the public deployment, or when ?demo=true is present
// (for local dev testing). NOT active for any way of reaching a real device —
// localhost, its mDNS hostname (*.local), or a LAN IP — since the UI is served
// directly from the device itself and real users reach it by exactly those
// addresses.
//
// A plain "hostname !== localhost" check would (and previously did) misclassify
// every real device as the demo, silently swapping in the fully-simulated
// DemoApiService instead of talking to the actual firmware — homing and moves
// would appear to succeed with zero physical motion.
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

/**
 * True when nothing on screen is connected to real hardware.
 *
 * Evaluated ONCE, at module load, and deliberately so: it decides which
 * ApiService is provided, and a value that could change afterwards would mean
 * the banner and the service could disagree about the same session. Nothing
 * here can change without a reload anyway — the hostname cannot, and the
 * sessionStorage flag is only written during this same evaluation.
 */
export const IS_DEMO: boolean =
  !isLocalNetworkHost(window.location.hostname) || readForcedDemo();
