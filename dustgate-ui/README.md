# dustgate-ui

Angular 17 web UI for [DustGate](../README.md) — the dashboard, manual setup wizard,
and AI setup assistant that run on the ESP32 (and, in demo mode, standalone in a
browser with no hardware at all).

The app is served *from* the device itself, so it uses hash-based routing
(`/#/setup`, not `/setup`) — there's no server-side router to fall back to.

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

### 2. Mock firmware server — closer to the real device

`tools/mock-api.js` is a small Node server that mimics the ESP32's HTTP + WebSocket
API. Use this when you want to exercise the real `ApiService` / proxy path instead
of the in-browser demo:

```bash
# one-time setup
cd ../tools && npm install

# terminal 1
node mock-api.js

# terminal 2
cd dustgate-ui
npm run start:mock
```

Then open http://localhost:4200 (no `?demo=true` — this uses the real `ApiService`
proxied through `proxy.conf.json` to `localhost:3000`).

> **Common mistake:** running plain `npm start` here instead of `npm run start:mock`
> looks like it works (the app loads), but `ng serve` isn't using `proxy.conf.json`
> in that mode, so every `/api/*` call — including the AI Setup chatbot — fails
> with `ECONNREFUSED` in the console. If you see a wall of `ws proxy error` /
> `http proxy error: ECONNREFUSED` messages, check that (a) `node mock-api.js`
> is running in another terminal and (b) you started Angular with
> `npm run start:mock`, not `npm start`.
>
> For the AI Setup assistant to return real Claude responses instead of canned
> mock replies, set `ANTHROPIC_KEY` in `tools/.env` before starting `mock-api.js`.

### 3. Against a real device

Point `proxy.conf.json`'s `target` values at the ESP32's IP address, then:

```bash
npm start
```

Open http://localhost:4200 — API calls proxy to the device over your LAN.

## Build

```bash
npm run build          # production build → dist/dustgate-ui/browser
npm run deploy          # build + gzip + copy into ../linear_actuator/data (for flashing)
```

After `npm run deploy`, upload the filesystem image from the repo root with
`pio run --target uploadfs`.

## Project structure

```
src/app/
  dashboard/              Operational view — tool buttons, HOME, dust collector toggle
  setup/                  AI setup assistant (chat interface, powered by Claude)
  setup-manual/           Manual setup wizard (step-by-step, no AI)
  outlet-configurator/    Reusable form for assigning a Shelly outlet to a gate
  gate-positioner/        Reusable jog widget for positioning the actuator at a stop
  visualizer/             Manifold visualizer — gate boxes, slider, dust collector
  services/
    api.service.ts            Talks to the real device (HTTP + WebSocket)
    demo-api.service.ts       In-memory drop-in replacement, used in demo mode
    claude.service.ts         Tool-calling loop for the AI setup assistant
    unit-preference.service.ts     mm/inches display preference
    hardware-profile.service.ts    Port size (2.5"/4") — seeds expected gate spacing
```

`ApiService` is swapped for `DemoApiService` via a DI override in `app.config.ts`
— every component just injects `ApiService` and doesn't need to know which one
it's actually talking to.

## Tests

No test suite yet.
