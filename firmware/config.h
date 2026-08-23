// =============================================================================
// config.h — DustGate Configuration
// Target hardware: Adafruit ESP32-S2 Feather + Adafruit TMC2209 Breakout (#6121)
// =============================================================================
// All project settings live here. Change values to match your build.
// Recompile after any change.
// =============================================================================

#pragma once

// -----------------------------------------------------------------------------
// MOTOR AND FEEDBACK TYPE — derived, not selected. See BOARD CAPABILITIES below.
//
// These were two hand-set #defines here (MOTOR_STEPPER_TMC2209 and
// FEEDBACK_LIMIT_DISTANCE) until 2026-08-22. They are now switched on by
// HAS_LINEAR, which comes from the board's pin map — because a board header that
// wires no stepper cannot be made to have one by a #define at the top of this
// file, and pretending otherwise is what made a rackless PRIMARY impossible to
// build. The defines still exist, still guard the same files, and are still what
// StepperTMC2209Driver.cpp and LimitSwitchDistance.cpp key off; they are just set
// further down, once the pin map is known.
//
// PIN_ENDSTOP_HOME wiring: NC switch between D10 and GND, INPUT_PULLUP.
//   Normal (contacts closed): pin pulled to GND → LOW → readHomeSwitch() = false
//   Triggered (contacts open): pullup wins → HIGH → readHomeSwitch() = true
// Fail-safe: broken wire → HIGH → reads as triggered → motor stops.
// PIN_ENDSTOP_MAX (far end) is wired identically (NC, INPUT_PULLUP, HIGH =
// triggered). It is REQUIRED on new builds: it provides over-travel safety and
// is the far reference for the self-calibrating reference sweep — see
// docs/dual-endstop-calibration.md.

// -----------------------------------------------------------------------------
// CONTROL INPUT — select exactly one
// -----------------------------------------------------------------------------
#define CONTROL_SMART_OUTLET      // Shelly smart outlet polling (auto gate selection)
// #define CONTROL_SERIAL_DEBUG    // Serial Monitor — open at SERIAL_BAUD, type 'help'

// -----------------------------------------------------------------------------
// HTTP API — enable to run the REST + WebSocket server alongside any control mode
// Required for the Angular front-end.
// Requires WiFi (set WIFI_STA_SSID / WIFI_STA_PASS, or use the setup portal).
// -----------------------------------------------------------------------------
#define ENABLE_HTTP_API

// -----------------------------------------------------------------------------
// POSITION CONFIGURATION
// -----------------------------------------------------------------------------
// Compile-time maximum — sets array sizes in CalibrationData and g_stopPositionsMM.
// The runtime count (g_numActiveStops) is set during setup and stored in NVS.
// Bumping this requires clearing calibration (clearcal) because CalibrationData changes size.
#define NUM_STOPS         16      // max selectable positions (position 0 = home)

// Minimum spacing (mm) between two saved gate positions. Authoritative backstop
// against saving two gates on top of each other (e.g. "forgot to jog" — saving
// gate N+1 without having moved off gate N). The Angular UI does a friendlier,
// port-size-aware version of this check before it ever calls /api/setstop; this
// firmware check catches any client that skips the UI (curl, scripts). Kept
// small so it only rejects genuine near-duplicates, never legitimately tight
// real-world gate spacing. Home (stop 0) is excluded from the check.
#define MIN_STOP_SEPARATION_MM   10.0f

// -----------------------------------------------------------------------------
// Manifold profiles + reference-sweep calibration (dual endstop)
// See docs/dual-endstop-calibration.md. A profile gives the mm geometry of a
// known manifold, referenced to the near (home) endstop trigger. The reference
// sweep measures the endstop-to-endstop step span, derives steps/mm, and places
// every gate by PROPORTION of the measured span (immune to steps/mm error).
// Keep these in step with shared/device-model MANIFOLD_PROFILES.
// -----------------------------------------------------------------------------
// Rockler Dust Right 2.5" — MEASURED on the reference build. Symmetric. Two direct
// measurements: trigger-to-trigger span = 84.9mm at 2 gates, gate-to-gate pitch =
// 82.9mm → trigger→gate offset = (84.9−82.9)/2 = 1mm/side. span(N) = 2 + (N−1)·82.9.
// NB: HOME_BACKOFF_STEPS does NOT affect pitch (cancels); it only shifts the
// steps/mm span — the sweep must add HOME_BACKOFF_STEPS back to the home→far step
// count before dividing by span mm.
#define MANIFOLD_2_5_FIRST_GATE_OFFSET_MM   1.0f
#define MANIFOLD_2_5_GATE_PITCH_MM          82.9f
#define MANIFOLD_2_5_END_MARGIN_MM          1.0f
// Rockler Dust Right 4": pitch derived from Rockler's 10" manifold width ÷ 2 gates
// = 5.000" = 127.0mm center-to-center (their 6.5"/2 = 3.25" ≈ measured 82.9mm on the
// 2.5" build confirms the width-per-gate method). Same rack pitch + endstop margin as
// the 2.5" slider, so offset/end-margin stay 1mm/side → span(N) = 2 + (N−1)·127. Only
// pitch drives sweep placement; the span is measured live. 4" path still disabled in
// the UI until the slider is built and one sweep confirms these numbers.
#define MANIFOLD_4_FIRST_GATE_OFFSET_MM     1.0f
#define MANIFOLD_4_GATE_PITCH_MM            127.0f
#define MANIFOLD_4_END_MARGIN_MM            1.0f

// steps/mm sanity bound: reject a measured sweep whose derived steps/mm deviates
// from the nominal geometric value by more than this — signals a wrong manifold
// profile or a mechanical fault rather than trusting a bad measurement.
#define STEPS_PER_MM_PLAUSIBILITY_PCT       15.0f
// Span re-check tolerance (mm): on re-home, a measured span off by more than this
// from the stored span flags possible lost steps (recalibrate).
#define SPAN_CHECK_TOLERANCE_MM             5.0f

// Names for each stop (used in serial debug output — extend as needed)
#define STOP_NAMES { "Home/Disabled", "Stop 1", "Stop 2", "Stop 3", \
                     "Stop 4", "Stop 5", "Stop 6", "Stop 7",        \
                     "Stop 8", "Stop 9", "Stop 10", "Stop 11",      \
                     "Stop 12", "Stop 13", "Stop 14", "Stop 15", "Stop 16" }

// -----------------------------------------------------------------------------
// MOTION PARAMETERS
// Hardware: LDO-42STH48-2004MAH motor + 15-tooth pinion + 20-tooth rack
// -----------------------------------------------------------------------------

// Standard 1.8° step angle → 200 native steps/rev
#define STEPS_PER_REV       200

// Microstepping divisor (set via TMC2209 UART at startup)
#define MICROSTEPS           16

// Pinion: 15 teeth
#define PINION_TEETH         15

// Rack: 82.9mm / 20 teeth = 4.145mm tooth pitch
#define RACK_PITCH_MM       4.145f

// Derived motion values (for reference):
//   Travel per revolution  = 15 × 4.145mm = 62.175mm
//   Steps per mm (16× µs)  = (200 × 16) / 62.175 = ~51.47 steps/mm
//   Steps per gate-to-gate = 82.9mm × 51.47 = ~4270 steps (measured: 4270 ✓)
//   Endstop to gate 1      = ~155 steps = 3.01mm (measured)

// Homing: direction to drive toward home endstop
// 1 = positive step direction, -1 = negative step direction
// Compile-time default — overridden at runtime by g_homeDirection (loaded from NVS).
// All files keep using HOME_DIRECTION unchanged; the macro now resolves to the global.
#define HOME_DIRECTION_DEFAULT  (1)
extern int g_homeDirection;        // defined in firmware.ino
#define HOME_DIRECTION           g_homeDirection

// Speed & acceleration
// steps/mm ≈ 102.94, so:
//   2000 steps/sec ≈ 19 mm/sec (normal moves)
//   1500 steps/sec ≈ 15 mm/sec (homing — StallGuard needs speed to trigger reliably)
#define MAX_SPEED_STEPS_PER_SEC      2000.0f
#define HOMING_SPEED_STEPS_PER_SEC   500.0f
#define ACCELERATION_STEPS_PER_SEC2  1000.0f

// Maximum travel during homing — safety cutoff if the home switch is never triggered.
// 700 mm covers an 8-gate installation (7 × 82.9 mm ≈ 580 mm) plus generous margin.
// At homing speed (~9.7 mm/sec) this limits runaway to ~72 s before the firmware
// forces the position to home regardless of the switch.
#define HOMING_MAX_TRAVEL_MM  700.0f

// After homing, back off this many steps before zeroing position.
// Endstop margin = 1 tooth = ~427 steps; backoff just needs to clear the switch.
// 50 steps ≈ 0.49mm — conservative, well within the 4.145mm margin.
#define HOME_BACKOFF_STEPS   50  // ~1mm at 51.47 steps/mm — clears backlash without overshooting gate 1

// -----------------------------------------------------------------------------
// TMC2209 PARAMETERS
// Adafruit TMC2209 Breakout (#6121) specifics:
//   - R_SENSE: 0.11Ω (verify on your board — check silkscreen or schematic)
//   - VDD: connect to 3.3V (Feather 3V3 pin) — board supports 3.3–5V logic
//   - Current pot: hardware ceiling; UART current setting cannot exceed pot limit
//   - UART: single-wire half-duplex on the board's "UART" pin
// -----------------------------------------------------------------------------
#define TMC2209_R_SENSE         0.11f   // Sense resistor (Ω) — verify on your board
#define TMC2209_CURRENT_MA       800    // Run current in mA — raise if stalls mid-travel
#define TMC2209_HOLD_CURRENT_MA   30    // Hold current — motor held between moves (low = cool).
                                        // Heat ~ I²: 30mA dissipates ~16% of what 75mA did. The
                                        // gate's rack-and-pinion friction holds position at idle;
                                        // set to 0 for a fully-freewheeling (coolest) standstill if
                                        // a little drift between moves is acceptable.

// Standstill power-down delay: clocks after the last step before the driver
// drops from run current (IRUN) to hold current (IHOLD). Set explicitly so the
// transition is guaranteed to engage promptly — otherwise a motor can linger at
// run current between moves and run hot even though the hold current is low.
// Range 0–255 (~0–5.6s); ~0.2s here.
#define TMC2209_TPOWERDOWN        10

// StallGuard threshold — not used for homing (physical limit switch) but left
// as a safety floor; TMC2209 still raises DIAG on severe overload/stall.
#define TMC2209_STALL_THRESHOLD   50

// Idle power-off: if no move/home command is issued for this many seconds,
// the driver is fully disabled (not just dropped to hold current) and the
// position is marked unknown, requiring a rehome before the next move.
// User-configurable at runtime via PUT /api/config/idle-timeout (0 = never
// sleep); this is only the default for a fresh device / after a NVS erase.
#define IDLE_TIMEOUT_SEC_DEFAULT 3600

// -----------------------------------------------------------------------------
// SYSTEM RESILIENCE — unattended-operation recovery
// -----------------------------------------------------------------------------
// How long WiFi may stay disconnected before we actively nudge a reconnect. The
// ESP32 core auto-reconnects on most drops, but a full AP outage can leave the
// radio idle indefinitely; WiFiProvisioner::maintain() retries at this cadence
// so the device rejoins on its own instead of needing a power cycle.
#define WIFI_RECONNECT_INTERVAL_MS  10000

// Task-watchdog timeout (seconds) for the main loop. If a loop iteration ever
// stalls this long, the device reboots itself rather than hanging silently.
// Generous on purpose: normal iterations are sub-millisecond (incremental motor
// steps + async I/O), and blocking network work runs on the poll task, not here.
#define WDT_TIMEOUT_SEC             10

// -----------------------------------------------------------------------------
// PHASE 2 — SERVO (ball-valve gates)
// -----------------------------------------------------------------------------
// Size of the PWM servo bank on one board. Mirrors MAX_SERVOS_PER_HOST in
// shared/device-model/topology.js — the schema refuses to place more servo gates on
// a controller than it has channels.
#define SERVO_COUNT                4

// Servo sweep duration: the driver eases from the current angle to the target
// over this long, rather than slamming the ~90° move in one command — gentler on
// the gate, the coupling, and the ball. ~2s feels deliberate without being slow.
// This is the CEILING, reached by a full-throw move; see SERVO_MS_PER_DEG.
#define SERVO_SWEEP_MS             2000

// Sweep pacing. The sweep duration is proportional to how far the servo actually has
// to travel, clamped to [SERVO_SWEEP_MIN_MS, SERVO_SWEEP_MS]. A fixed duration made a
// 90° throw and a 3° nudge take the same 2s, which is right for the throw and useless
// for the setup jog control — a nudge you can't see land is a nudge you press twice.
// 22ms/° puts a 90° quarter-turn at the full ~2s and a 3° nudge at the 80ms floor.
#define SERVO_MS_PER_DEG             22
#define SERVO_SWEEP_MIN_MS           80

// Post-move hold: keep the servo energized this long AFTER the sweep completes so
// an analog servo can actually catch up to the final commanded angle before we
// de-energize. Then auto-detach (the DEFAULT): analog servos groan/hunt while
// holding, and the ball valve holds position by friction/detent once seated.
#define SERVO_HOLD_MS              1000

// UART address (0–3, set by MS1/MS2 pins — Adafruit board default is 0)
#define TMC2209_ADDRESS            0

// -----------------------------------------------------------------------------
// PIN ASSIGNMENTS — selected per board
// The concrete pin map lives in a boards/*.h header chosen by a -DBOARD_* build
// flag (set per PlatformIO env). Everything downstream uses the PIN_* / SERIAL1_*
// macros unchanged. Primary target is the ESP32-DevKitC; the Feather is kept as
// an unadvertised variant. All GPIO are 3.3V logic.
// -----------------------------------------------------------------------------
#if defined(BOARD_DEVKITC)
  #include "boards/devkitc_wroom32.h"
#elif defined(BOARD_FEATHER_S2)
  #include "boards/feather_s2.h"
#elif defined(BOARD_QTPY_S3)
  #include "boards/qtpy_s3.h"
#elif defined(BOARD_XIAO_C5)
  #include "boards/xiao_c5.h"
#else
  // No board flag set (e.g. a bare Arduino IDE build) — default to the Feather,
  // which is the original hardware and has native-USB defaults.
  #include "boards/feather_s2.h"
#endif

// -----------------------------------------------------------------------------
// BOARD CAPABILITIES — derived, never hand-set.
//
// A DustGate board is defined by what it can physically drive, and that is
// already implied by its pin map. Deriving the capabilities from the pins means
// a board header can't claim hardware it doesn't wire up, and adding a new
// target is one file rather than a pin map plus a matching set of feature flags.
//
//   HAS_LINEAR  — a stepper + endstops (the rack). Absent on servo-only nodes.
//   HAS_SERVO   — the PWM servo bank.
//
// These replaced the old `#error "No feedback type defined"` / `"No control type
// defined"` walls in the sketch, which made a stepper-less build impossible to
// express at all.
// -----------------------------------------------------------------------------
#if defined(PIN_TMC_STEP)
  #define HAS_LINEAR 1
#else
  #define HAS_LINEAR 0
#endif

// ...and here is where HAS_LINEAR stopped being decorative (2026-08-22). These
// two macros guard StepperTMC2209Driver.{h,cpp} and LimitSwitchDistance.{h,cpp}
// in their entirety — they always did — so deriving them from the pin map is the
// whole of what compiles the rack out of a board that hasn't got one. The sketch
// picks NullMotorDriver / NullFeedback instead; see the header of
// motor/NullMotorDriver.h for why those are null objects and not #if at 46 call
// sites.
//
// The libraries follow: with the two .cpp files empty, nothing includes
// TMCStepper or AccelStepper, so a rackless env doesn't need them in lib_deps.
// That is what lets the XIAO C5 — one PWM-only pin map, no motor pins — build as
// a primary at all: neither library has ever been compiled for RISC-V or against
// Arduino core 3.x, and now neither has to be.
#if HAS_LINEAR
  #define MOTOR_STEPPER_TMC2209     // Stepper via TMC2209 (STEP/DIR + UART)
  #define FEEDBACK_LIMIT_DISTANCE   // Home + far limit switch, position by step count
#endif

#if defined(ENABLE_SERVO) && defined(SERVO_PWM_PIN_1)
  #define HAS_SERVO 1
#else
  #define HAS_SERVO 0
#endif

// -----------------------------------------------------------------------------
// NO_LINEAR_FITTED (-DNO_LINEAR_FITTED) — "this board's pin map has a rack, but
// no rack is physically attached."
//
// A master's job is the routing brain: hold the topology, poll the plugs,
// compute the transition, drive gates. None of that needs a stepper, and the
// direction of travel is servo ball valves, so requiring a TMC2209 on every
// brain taxes builds for hardware most of them will never carry.
//
// WHAT IT DOES, AND THE ONE THING IT DOESN'T
//   The TMC2209 UART health check still RUNS and still prints its full
//   diagnosis — that check is the only thing that separates a wiring fault from
//   a working driver, and deleting it would trade a loud failure at boot for a
//   silent one during the first move. What changes is only how the sketch REACTS
//   to it: a missing driver becomes the expected state rather than a fault, so
//   the board doesn't sit in STATE_ERROR with the status pixel pulsing red about
//   a stepper you chose not to fit. Motion stays disabled either way, via the
//   same g_hardwareFault latch — there is genuinely no motor to move.
//   That reaction now lives in one place, control/FaultPolicy.h: this flag is
//   the `rackFitted` argument, and "refuse motion but do not go red" is a row in
//   its table rather than a patch applied after the fault branch.
//
// WHY THIS IS A REACTION FLAG AND NOT HAS_LINEAR
//   HAS_LINEAR above is the RIGHT seam and is currently read by nothing. Making
//   it load-bearing means guarding StepperTMC2209Driver.cpp and
//   LimitSwitchDistance.cpp, adding a null MotorDriver, and gating the endstop
//   reads in firmware.ino and SerialDebugControl.cpp — a change to motion code
//   that has never run on hardware. Every servo-only board so far has been a
//   NODE, which sidesteps all of it by compiling from firmware/node/ instead, so
//   HAS_LINEAR == 0 has never actually been built for the primary sketch.
//
//   This flag is the deliberately small stand-in: it costs the servo build the
//   flash the stepper occupies and still prints "D10: TRIGGERED" for unwired
//   endstop pins (open reads as triggered on an NC switch), but it touches no
//   motion logic. The real HAS_LINEAR work is logged in TODO.md.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// SECONDARY ROLE (-DDUSTGATE_SECONDARY)
//
// A "dumb" actuator bank in the star: it accepts already-resolved NodeLink SET
// frames and moves a channel to an angle. It owns no topology, computes no
// routing, polls no Shelly plugs and serves no web UI — the primary does all of
// that. Building one is mostly SUBTRACTION from the normal firmware, which is
// what keeps the cheap variant from being a second codebase.
//
// Kept as one flag rather than a pile of per-feature switches so "is this board
// a secondary?" has exactly one answer.
// -----------------------------------------------------------------------------
// The secondary is built from its own tiny source root (firmware/node/),
// NOT by #ifdef-ing the primary sketch: the stepper, endstops, homing, the
// reference sweep, Shelly polling and the routing brain are woven through the
// main .ino in ~100 separate places, and splitting that with the preprocessor
// would leave a sketch nobody can read. See node/dustgate_node.cpp.
//
// This flag therefore only switches off the primary-only subsystems that the
// SHARED headers would otherwise pull in.
#ifdef DUSTGATE_SECONDARY
  // No plug polling: the primary owns tool sensing and tells us what to do.
  #undef CONTROL_SMART_OUTLET
  // No REST API / Angular bundle — a node's entire interface is /nodelink.
  #undef ENABLE_HTTP_API
#endif

// -----------------------------------------------------------------------------
// SMART OUTLET CONTROL (CONTROL_SMART_OUTLET)
// Requires WIFI_STA_SSID / WIFI_STA_PASS to be set below.
// Outlet-to-stop mappings are stored in NVS during setup.
// -----------------------------------------------------------------------------

// Maximum number of outlet slots (one per blast gate)
#define SMART_OUTLET_COUNT            7

// Maximum number of switchable COLLECTOR plugs — one per airflow system.
// A shop owns N systems (docs/shop-schema-rfc.md), each with its own blower:
// a 4" cyclone for the big machines and a 2.5" wall unit for the bench is the
// case that motivated this. Slot 0 is the one persisted in NVS and the one the
// pre-topology stop-index automation drives; the rest are RAM-only and rebuilt
// from the layout on every adopt, exactly like the tool slots.
// Three, not two: two is the realistic ceiling for a home shop and the third
// costs one pointer, so a third system doesn't silently lose its blower.
#define COLLECTOR_COUNT               3

// How often the poll task queries each outlet (ms)
#define OUTLET_POLL_INTERVAL_MS     500

// HTTP request timeout per outlet — must be shorter than OUTLET_POLL_INTERVAL_MS
// to avoid stalling the poll loop when a device is offline
#define OUTLET_HTTP_TIMEOUT_MS      400

// Timeout for RPC config writes (Ws.SetConfig / Switch.SetConfig / Sys.SetConfig).
// These persist to the plug's flash and can take far longer than a status read,
// so they get a generous window. Only runs at provisioning time (device add /
// boot), never on the fast poll path, so a long value is safe here.
#define OUTLET_RPC_WRITE_TIMEOUT_MS 3000

// Reachability probe timeout for the provisioning path. Unlike the 400ms poll
// probe (which must fit inside the poll interval), provisioning runs rarely and
// off the motor loop, so it can afford to wait for a marginal plug to answer its
// first request instead of failing it and deferring for a whole retry cycle.
#define OUTLET_PROVISION_PROBE_TIMEOUT_MS 2000

// How often to retry push-provisioning plugs that aren't yet configured (e.g. a
// plug that was briefly unreachable at boot). Each retry only fast-probes the
// unprovisioned plugs; already-pushing plugs are skipped, so this is cheap.
#define OUTLET_PROVISION_RETRY_MS   15000

// mDNS discovery (setup wizard's "Scan for outlets" / serial 'discover') is
// UDP-based and lossy — a single query commonly misses devices that answer
// on a repeat query. Re-querying a few times and merging by IP gives a much
// more complete/consistent list. Each query blocks for DISCOVER_MDNS_TIMEOUT_MS
// waiting for responses (see utils/MdnsQuery.h) — keep the total across all
// attempts well under a few seconds: on a local LAN, devices that are going
// to answer at all do so within tens of milliseconds, and blocking the main
// loop for too long risked a watchdog reset / stale HTTP request (see
// MdnsQuery.h for the full story).
#define DISCOVER_MDNS_ATTEMPTS       3
#define DISCOVER_MDNS_TIMEOUT_MS     400
#define DISCOVER_MDNS_RETRY_DELAY_MS 150
#define DISCOVER_MAX_RESULTS         16

// How long a tool must be drawing above threshold before the gate moves (ms).
// Prevents false triggers from motor startup inrush.
#define OUTLET_ON_DEBOUNCE_MS      1000

// How long all tools must be idle before returning to home (ms).
// Extra slack for tools with mechanical coast-down (router, bandsaw, etc.)
#define OUTLET_OFF_DEBOUNCE_MS     3000

// Default watts threshold for "tool is on". Overridden per-outlet in NVS.
// Set low enough to catch light tools (shop vac ≈ 1000W, soldering iron ≈ 60W).
// Set above standby draw of power strips / outlet transformers (typically < 2W).
#define OUTLET_DEFAULT_THRESHOLD_W  5.0f

// -----------------------------------------------------------------------------
// WIFI CREDENTIALS
// Station mode credentials — uncomment to hardcode (developer / known network).
// Leave commented for end-user deployments: credentials are stored via the setup portal.
// #define WIFI_STA_SSID    "your-network-name"
// #define WIFI_STA_PASS    "your-password"

// Setup portal SSID: shown when no WiFi credentials are stored.
// Connect to this hotspot and visit http://192.168.4.1 to enter your network credentials.
#define WIFI_PORTAL_SSID    "DustGate-Setup"

// -----------------------------------------------------------------------------
// HTTP API (ENABLE_HTTP_API)
// -----------------------------------------------------------------------------

// API key length (bytes) for auto-generated keys. 8 bytes = 16 hex chars.
#define API_KEY_BYTES   8

// Port for the HTTP API server (also serves WebSocket at /ws)
#define API_PORT        80

// How often the main loop re-serializes the routing view for
// GET /api/status. The view only changes on a tool on/off or a plan step, so
// rebuilding it every loop pass would burn heap churn for nothing; 250ms is
// well inside what the Live view needs to feel immediate.
#define V2_STATUS_PUBLISH_MS   250

// -----------------------------------------------------------------------------
// SERIAL COMMANDS
// Enables serial command processing (status, home, jog, help, etc.) alongside
// any control mode. Independent of CONTROL_SERIAL_DEBUG — you can type commands
// in the serial monitor even when CONTROL_SMART_OUTLET is the active mode.
// Disable to save flash if you never use the serial monitor in production.
// -----------------------------------------------------------------------------
#define ENABLE_SERIAL_COMMANDS

// -----------------------------------------------------------------------------
// SERIAL / DEBUG
// -----------------------------------------------------------------------------
#define SERIAL_BAUD       115200
#define DEBUG_ENABLED       true   // Set false to suppress all Serial output

#if DEBUG_ENABLED
  #define DEBUG_PRINT(x)    Serial.print(x)
  #define DEBUG_PRINTLN(x)  Serial.println(x)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
#endif
