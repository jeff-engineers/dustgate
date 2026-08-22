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

Default pair — the primary target and the node target:

```bash
pio run -e esp32dev_wroom32 -e dustgate_node
```

Everything the CI job covers (still one command, all on the espressif32 platform):

```bash
pio run -e esp32dev_servo -e esp32dev_wroom32 -e adafruit_feather_esp32s2 -e dustgate_node
```

`xiao_c5` **must** be built alone, and against its own core directory — it rides
the pioarduino platform, which collides with espressif32 over package names. The
core dir is one env var per process, so it cannot share a `pio run`. It does not
disturb `~/.platformio`:

```bash
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e xiao_c5
```

Only build `xiao_c5` when the change touches C5 pin maps, `boards/xiao_c5.h`, or
node firmware. Say so if you skip it.

## 3. Report

State what you ran and what happened. If something failed, show the actual
output — don't summarize a failure into "some tests failed". If you skipped a
category the table called for (e.g. left `xiao_c5` unbuilt), say which and why.

Compiling and passing host tests is **not** hardware verification. This project
is hardware-untested; never upgrade "builds clean" into "works".
