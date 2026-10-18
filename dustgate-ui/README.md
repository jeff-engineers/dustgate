# dustgate-ui

Angular 17 web UI for [DustGate](../README.md) — the Live tool list, the shop
layout canvas, and the setup screens that run on the ESP32 (and, in demo mode,
standalone in a browser with no hardware at all).

The app is served *from* the device itself, so it uses hash-based routing
(`/#/build`, not `/build`) — there's no server-side router to fall back to.

## Prerequisites

- Node.js 18+ and npm

## Install

```bash
npm install
```

## Running locally

There are three ways to run the UI, depending on whether you have real hardware:

### 1. Demo mode — no backend at all

Start any static/dev server and open the app with `?demo=true` in the URL:

```bash
npm start
# then open http://localhost:4200/?demo=true
```

This swaps in `DemoApiService` (see `src/app/services/demo-api.service.ts`), which
simulates the whole device in-memory — homing, jogging, outlets, everything. No
proxy, no mock server, no device required. This is also what runs automatically
on the Vercel deployment (demo mode activates on any non-localhost hostname).

### 2. Mock firmware server — real transport + CI conformance target

`tools/mock-api.js` is a small Node server that mimics the ESP32's HTTP + WebSocket
API. It's a thin wrapper over the canonical device model in `../shared/device-model`
— **the exact same simulated model the in-browser demo uses** — so its "outlets" are
fabricated, not real plugs on your network (discovery invents random `192.168.87.x`
Shelly devices; nothing is actually scanned). Its data is no more real than demo mode.

What it adds over demo mode is **transport**: demo mode swaps `ApiService` out for an
in-browser stub, whereas the mock keeps the real `ApiService` and serves genuine
HTTP/WebSocket through `proxy.conf.json`. Two things need that, and neither is served
by demo mode:

- **Developing the networking layer** — `ApiService`, WebSocket reconnect, HTTP
  status/error handling, streaming — without flashing a device.
- **CI conformance** — `.github/workflows/ci.yml` runs `shared/device-model/conformance.js`
  against this mock on every commit (`npm run conformance:ci`). That's the load-bearing
  job keeping the mock, demo, and firmware honoring one API contract; the same suite can
  be pointed at a real board (`node conformance.js http://<device-ip> <key> --force`) to
  certify actual firmware, which can't import the JS model.

If you're only building UI features, **demo mode is enough** — clicking through the
mock is redundant with it since the data is identically fake. Reach for the mock when
you're touching the transport layer or the API contract:

```bash
# one-time setup (run from dustgate-ui/)
cd ../tools && npm install

# terminal 1 — start the mock firmware server (serves localhost:3000)
cd ../tools && node mock-api.js

# terminal 2 — start Angular with the proxy pointed at the mock (from dustgate-ui/)
npm run start:mock
```

Then open http://localhost:4200 (no `?demo=true` — this uses the real `ApiService`
proxied through `proxy.conf.json` to `localhost:3000`).

> **Common mistake:** running plain `npm start` here instead of `npm run start:mock`
> looks like it works (the app loads), but `ng serve` isn't using `proxy.conf.json`
> in that mode, so every `/api/*` call fails with `ECONNREFUSED` in the console.
> If you see a wall of `ws proxy error` / `http proxy error: ECONNREFUSED`
> messages, check that (a) `node mock-api.js` is running in another terminal and
> (b) you started Angular with `npm run start:mock`, not `npm start`.

### 3. Against a real device

`bash dev.sh live [host]` from the repo root (default `dustgate.local`) serves the app with hot reload, proxied to the
brain. By hand: point `proxy.conf.json`'s `target` values at the brain's address, then:

```bash
npm start
```

Open http://localhost:4200 — API calls proxy to the device over your LAN.

## Build

```bash
npm run build          # production build → dist/dustgate-ui/browser
npm run deploy          # build + gzip + copy into ../firmware/data (for flashing)
```

Don't flash it by hand: the app shares the ESP32's filesystem with the saved layout, and a raw `uploadfs` erases the
shop. Use `bash dev.sh flash --ui` for an ESP32 brain (it saves and restores the layout), or
`bash native/pi/deploy.sh <user>@dustgate.local` for the Pi, which builds the app itself.

## Project structure

```
src/app/
  landing/                The front door — reads the stored layout and forwards to
                          /shop (finished) or /build (unfinished)
  live/                   Live tool list — which tool is collecting; manual override
  build/                  Shop layout canvas: place the collector, run duct, attach
                          gates and tools. routing/ + wiring/ hold its geometry
  gates/                  Gate calibration — sliding gate (ST3215 slider, homing sweep) and ball
                          valve (servo angle capture), plus the shared selector types
  tools/                  Tools and the collector: plugs, clamps, the collector's switch and board
  plugs/                  Every smart plug on the network: pair, rename, release, take over
  boards/                 Pair boards, update their firmware over WiFi, the brain's log
  settings/               The servo pulse range, forget-WiFi, resets
  services/
    api.service.ts            Talks to the real device (HTTP + WebSocket)
    demo-api.service.ts       In-memory drop-in replacement used in demo mode —
                              a thin async wrapper over the canonical device model
                              in ../../shared/device-model (imported as @device-model),
                              the same model that drives tools/mock-api.js
    demo-topology.ts          Pre-built layout that seeds demo mode
    shop-ready.ts             Is this layout finished enough to run?
    shop-doc.ts               The typed seam onto the shop document the screens edit
    failure.ts                What to say when a request fails, and the [DustGate] console log
```

`ApiService` is swapped for `DemoApiService` via a DI override in `app.config.ts`
— every component just injects `ApiService` and doesn't need to know which one
it's actually talking to.

## Tests

```bash
npm test     # the Angular-free specs (spec-runner.js), the router and the wiring geometry, under plain node
```

Specs are listed by hand in `tsconfig.spec.json` and `spec-runner.js` (a known gap, in `TODO/TODO.md`): a new
`*.spec.ts` runs only once it is added to both.
