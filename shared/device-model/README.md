# Canonical device model

This directory is the **single source of truth for how a DustGate device
behaves** — its state, its command transitions, and its outlet ping/discover
simulation. It exists to solve one specific problem: the device's behaviour used
to be reimplemented three times (real firmware, local mock, hosted demo), and
the three drifted constantly.

```
shared/device-model/
  device-model.js     ← the canonical state machine (pure JS, no I/O, no timers)
  device-model.d.ts   ← TypeScript contract for the model's API
  conformance.js      ← executable HTTP contract that certifies any target
  README.md           ← you are here
```

## Who consumes it

| Consumer | Wraps the model with… | Notes |
|---|---|---|
| `tools/mock-api.js` | an HTTP + WebSocket server | Node `require`; owns `setTimeout` timing |
| `dustgate-ui/src/app/services/demo-api.service.ts` | an Angular service | imports via the `@device-model` tsconfig path alias; owns `await delay` timing |
| `linear_actuator/` firmware (C++) | — | **can't** import the JS; conforms to the same contract, verified by `conformance.js` |

Because both JS simulators call the *same* model, they can't drift from each
other. The firmware is kept honest by the conformance suite instead of by shared
code (see below).

## Design rules (for anyone editing `device-model.js`)

1. **Pure.** No HTTP, no WebSocket, no Angular, no wall-clock timers. Every
   function takes a device object `d` and mutates it synchronously.
2. **Caller owns timing.** Multi-step motions are split into `begin*` / `complete*`
   (e.g. `beginHome` / `completeHome`) so each consumer supplies its own delay
   between them. The model never calls `setTimeout` / `await` itself — that's how
   the Node (`setTimeout`) and Angular (`await delay`) async styles avoid fighting
   over shared code.
3. **Faithful to firmware, not to convenience.** Where the real device does
   something surprising (e.g. silently skipping a stop save that overlaps another
   gate — `MIN_STOP_SEPARATION_MM`), the model does the same. A mock that papers
   over real behaviour is worse than no mock.
4. **Keep `.d.ts` in step with `.js`.** The types are hand-written (so the Node
   mock needs no build step); update both together.

## Conformance — how firmware stays in sync

The firmware is C++ and can't share this code, so `conformance.js` is the sync
mechanism: **34 behavioural scenarios run over HTTP against any target that
claims to be a DustGate device.** The same suite certifies the mock in CI and a
real ESP32 on demand.

It asserts the **contract** — response shapes, validation (`400`/`401`), and
deterministic state transitions (home→homed, overlap-skip, clearcal reset). It
deliberately does **not** assert simulation-only details that legitimately differ
on real hardware (exact wattage, discovered device names/counts); those are
checked shape-only, and a couple of sim-specific state flips are gated to
localhost.

```bash
# Against the local mock (spawns it, runs, cleans up) — this is what CI runs:
cd tools && npm run conformance:ci

# Against an already-running mock:
cd tools && npm run conformance          # → http://localhost:3000

# Against REAL hardware (DESTRUCTIVE — homes, moves, wipes calibration):
node shared/device-model/conformance.js http://<device-ip> <api-key> --force
```

> ⚠️ The suite homes, jogs, moves, saves stops, and clears calibration. Against a
> real device that means **physical actuator motion and a wiped configuration**.
> For that reason it refuses any non-localhost target unless you pass `--force`
> (or set `CONFORMANCE_FORCE=1`).

If a run against real hardware fails, that's the point working as intended: the
firmware has drifted from the contract.

## Extending it

When adding a device command:
1. Add the pure transition to `device-model.js` (+ its type in `device-model.d.ts`).
2. Wire it into both wrappers (mock route + demo override) — thin, no logic.
3. Add a contract scenario to `conformance.js`.
4. Implement it in the firmware and confirm `conformance.js --force` stays green.
