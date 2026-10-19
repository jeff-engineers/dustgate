# shared/device-model — working here

This directory is the spec (the root `CLAUDE.md` has the anti-drift rule). Read `README.md` here before changing
anything: its design rules (pure, caller owns timing, faithful to firmware, `.d.ts` in step) apply to every file.

**Matched tests.** Where a JS test here asserts a number, its C++ twin in `firmware/test/` asserts the same number —
same cases, same order. Pairs: `nodelink.test.js` ↔ `test_nodebus.cpp` (the reference), `manual-blower` ↔
`test_manual_blower`, `layout-save` ↔ `test_layout_save`, `collector-plug` ↔ `test_collector_plug`, `bin-sensor` ↔
`test_binsensor` (same rule, deliberately not the same numbers), `board-id` ↔ `test_boardid`, `shop` ↔ `test_shop`,
`problems` ↔ the "problems —" block of `test_nodebus`, `plug-identity` ↔ the "S" block of `test_relocate` (is the plug at a
saved address still that plug — the app and the brain must agree). **Change one, change both.**

**Constants shared across the boundary.** `shared/device-model/constant-pairs.json` is the CHECKED list
(`tools/check-constant-pairs.js`, run by `npm run model:test`): it fails when a group disagrees or a listed constant
can't be found. This table says WHY each is a pair. **A new pair goes in both places, in the same change.** Only the
**default applied when the document or frame is silent** is a pair; a value read from the document at runtime is not.

| JS (`shared/device-model/`) | C++ (`firmware/`) | What, and why it must agree |
|---|---|---|
| `DEFAULT_COLLECTOR_OFF_DELAY_MS` (topology-device.js) | `kDefaultCollectorOffDelayMs` (TopologyRuntime.h) | collector coast-down default |
| `DEFAULT_THRESHOLD_W` (topology-device.js) | `kDefaultThresholdW` (TopologyController.h) | machine-on wattage default |
| the `* 3` in the demo's `setMachineManual()` | the `* 3.0f` in `manualWattsFor()` (TopologyRuntime.h) | synthetic watts for a hand switch-on; switching a tool on releases other hand-switched tools in its system (`releaseManualsFor()`, mock-api.js `/api/tool`) |
| `MAX_SERVOS_PER_HOST` / `MAX_LINEAR_PER_HOST` (topology.js) | `SERVO_COUNT` (config.h) | servo channels per board: **2** (gate, fob). Also `SERVO_PORTS` (wire-geometry.ts), `SERVO_CHANNELS_PER_BOARD` (selector-types.ts), `servoPortsPerBoard` (board-drives.ts) — five places; the canvas port strip IS the budget. Test fixtures must not name channels they don't need |
| `NUM_STOPS` (device-model.js) | `NUM_STOPS` (config.h) | max stops on a sliding gate, **8**, = `MAX_SLIDE_BRANCHES` (topology.js) and `SLIDE_MAX_OUTLETS` (UI). Must stay EVEN; changing it changes `CalibrationData` — bump `CALIB_VERSION` |
| `MIN_STOP_SEPARATION_MM`, `IDLE_TIMEOUT_SEC_DEFAULT` (device-model.js) | same names (config.h) | stop overlap backstop; idle power-off default |
| `MANIFOLD_PROFILES` (device-model.js) | `MANIFOLD_2_5_GATE_PITCH_MM` etc. (config.h) | Rockler geometry; the reference sweep centres the gates on `gatePitchMm`, so a one-sided edit misplaces every gate |
| default for an absent outlet `kind` (topology.js) | `outletKindFromName()`, `OutletConfig` (outlets/) | must be `shelly` on both sides; an unknown string degrades the same way |
| `COLLECTOR_RUNNING_W` / `COLLECTOR_SPINUP_GRACE_MS` (topology-device.js) | `kCollectorRunningW` / `kCollectorSpinupGraceMs` (CollectorPlugState.h) | is the blower ACTUALLY running; every way of commanding it is stateless, so feedback is judged on both sides. 25 W for now (held: may become per-collector) |
| `NODELINK_VERSION`, `PING_INTERVAL_MS`, `PONG_TIMEOUT_MS`, `RECONNECT_MIN/MAX_MS` | `kVersion`, `kPingIntervalMs`, `kPongTimeoutMs`, `kReconnectMin/MaxMs` (NodeLink.h) | protocol timing |
| `SENSE_REPEAT_MS` / `SENSE_STALE_MS` | `kSenseRepeatMs` / `kSenseStaleMs` | SENSE repeats so one dropped frame can't stick; stale is 3× the repeat, asserted as a ratio |
| `caps.ct`, `caps.plug`, `caps.join`, `caps.rf`, `caps.bin` — default when absent (nodelink.js) | the `buildWelcome` parameter defaults, `_cap*` (RemoteActuatorBus) | what a board declares in its WELCOME. **Absent = NO on both sides**, or an older board is sent frames it refuses |
| bounds on `tripRatio` / `minCounts` / `clearRatio` (validateFrame) | the same in `parseConfigFrame` (NodeLink.h) | CT tuning rides CONFIG; the BOUNDS must agree or a node refuses the whole frame. Validate on PRESENCE (0 means "not sent") |
| `MAX_SENSORS_PER_NODE`, `MAX_RST_LEN`, `MAX_STATE_ERR_LEN`, `MAX_PLUG_THRESHOLD_W`/`MAX_PLUG_WATTS`/`MAX_PLUG_IP_LEN`, `MAX_OTA_PATH`/`MIN/MAX_OTA_BYTES`, `MAX_WHERE_IP_LEN`/`REFUSE_REASONS`/`BEACON_PORT`, `MIN/MAX_RF_TICK_US`/`MAX_RF_REPEATS` | `kMaxSensorsPerNode`, `kMaxRstLen`, `kMaxStateErrLen`, `kMaxPlug*`, `kMaxOtaPath`/`kMin/MaxOtaBytes`, `kMaxWhereIpLen`/`kRefuseReasons`/`kBeaconPort`, `kMin/MaxRfTickUs`/`kMaxRfRepeats` | frame bounds. Either side refuses an out-of-bounds frame WHOLE, and a refused frame looks like one never sent — so they must agree exactly. The OTA size window's top is also in `partitions-xiao-c5-node.csv` |
| `DEFAULT_SERVO_MIN/MAX_US`, `MIN/MAX_SERVO_US`, `MIN_SERVO_SPAN_US` | `kDefaultServoMin/MaxUs`, `kMin/MaxServoUs`, `kMinServoSpanUs` | the shop's PWM pulse range (Settings, the layout's `servo` block, sent on every servo SET) |
| `isOptionalBoard` (shop.js) | `isOptionalBoard` (Shop.h) | a board with no gate and none of a collector's jobs is optional: off raises nothing |
| `bareHost` / `isOwnBoard` / `sameBoard` (board-id.js) | the same (BoardId.h) | the ONE rule for "which board is this id"; the C++ is the reference |
| `PROBLEM_TEXT` (topology-device.js) | `kProblem*` (TopologyRuntime.h) | the problem texts derived from the collector's reading, asserted literally |

**Not pairs, deliberately** (exist once, change alone): `kMoveTimeoutMs` (NodeLink.h — the primary's own wait, never on
the wire; must exceed `HOMING_TIMEOUT_MS`, which the node static_asserts), `HOMING_TIMEOUT_MS` (derived from the rail
and speed; `HOMING_MAX_TRAVEL_MM` must exceed the largest rack's `spanMm`), `kBinDebounceMs`, the CT sampling cadence
and learned floor (`sensing/CtTrip.h`, one file shared by node and primary — never copy it). `firmware/test/shim/Arduino.h`
fakes just enough clock and ADC for CtTrip; it must not grow. The UI bundle's copy of the model is checked by
`tools/check-ui-fresh.js`.
