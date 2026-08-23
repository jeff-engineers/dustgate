---
name: verify
description: Run the right DustGate checks for whatever changed — UI suites, JS model tests, C++ host tests, conformance suites, firmware compiles. Use after editing anything in this repo, when asked to "verify", "check", "run the tests", or before committing.
---

# Verify a DustGate change

Pick checks by what actually changed. Don't run everything by reflex — the
firmware compiles are slow. Don't run less than the table says either; the whole
point of the matched-pair tests is that a change in one place breaks another.

## 1. What changed

```bash
git status --porcelain && git diff --stat HEAD
```

## 2. Map it to checks

| Changed | Run |
|---|---|
| `dustgate-ui/**` | `cd dustgate-ui && npm test` |
| `shared/device-model/**` | **all three**: `cd tools && npm run model:test`, `npm run firmware:test`, and `cd dustgate-ui && npm test` |
| `firmware/**` (`.h`/`.cpp` control logic) | `cd tools && npm run firmware:test`, then the compiles below |
| `firmware/**` (pins, boards, `.ino`, `config.h`) | the compiles below |
| HTTP/WS contract on either side | the conformance suites below |
| `platformio.ini`, `boards/*.h` | the compiles below |
| `dev.sh`, `deploy.sh`, `tools/*.sh` | `bash -n <file>` (syntax only — flashing needs hardware) |
| docs, mockups, README only | nothing |

**`shared/device-model/` is why the row above is three commands.** Its JS tests
and the C++ host tests in `firmware/test/` are a matched pair asserting the same
numbers, and the UI imports the model through a tsconfig path alias that only
type-checks on build. A change there can break any of the three.

### Conformance suites

Run these when the device API contract moved — new/changed route, changed
payload shape, changed state transition. Each spins up its own mock and tears it
down. Run them one at a time; they bind fixed ports.

```bash
cd tools
npm run conformance:ci            # HTTP device contract vs mock-api.js
npm run topology:conformance:ci   # topology routes
npm run nodelink:conformance:ci   # primary↔secondary WS conversation vs mock-node.js
```

### Firmware compiles

Two envs, which is every env — one board, two roles. They build together:

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5_primary -e xiao_c5
```

The core dir is REQUIRED: the pioarduino platform lives there, and a bare
`pio run` won't set it (`dev.sh`/`deploy.sh` do).

`xiao_c5_primary` builds the full sketch and catches the most. Build both anyway —
~45 s together — unless the change is provably UI-only or JS-only. Any command
naming an `esp32dev_*`, `adafruit_feather_*` or `dustgate_node` env is stale.

## 3. Report

State what you ran and what happened. If something failed, show the actual
output — don't summarize a failure into "some tests failed". If you skipped a
category the table called for (e.g. left `xiao_c5` unbuilt), say which and why.

Compiling and passing host tests is **not** hardware verification. This project
is hardware-untested; never upgrade "builds clean" into "works".
