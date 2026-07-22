// =============================================================================
// config.h — DustGate Configuration
// Target hardware: Adafruit ESP32-S2 Feather + Adafruit TMC2209 Breakout (#6121)
// =============================================================================
// All project settings live here. Change values to match your build.
// Recompile after any change.
// =============================================================================

#pragma once

// -----------------------------------------------------------------------------
// MOTOR TYPE — TMC2209 stepper driver
// -----------------------------------------------------------------------------
#define MOTOR_STEPPER_TMC2209  // Stepper via TMC2209 (STEP/DIR + UART)

// -----------------------------------------------------------------------------
// FEEDBACK TYPE
// -----------------------------------------------------------------------------
#define FEEDBACK_LIMIT_DISTANCE   // Home limit switch + step count for gate positions

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
// Required for the setup agent and Angular front-end.
// Requires WiFi (set WIFI_STA_SSID / WIFI_STA_PASS, or use the setup portal).
// -----------------------------------------------------------------------------
#define ENABLE_HTTP_API

// -----------------------------------------------------------------------------
// POSITION CONFIGURATION
// -----------------------------------------------------------------------------
// Compile-time maximum — sets array sizes in CalibrationData and g_stopPositionsMM.
// The runtime count (g_numActiveStops) is set via the setup agent and stored in NVS.
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
// Rockler Dust Right 4" — PLACEHOLDER (no 4" hardware to measure yet). The 4"
// path is disabled in the UI; these values are unused until real measurements.
#define MANIFOLD_4_FIRST_GATE_OFFSET_MM     3.0f
#define MANIFOLD_4_GATE_PITCH_MM            120.0f
#define MANIFOLD_4_END_MARGIN_MM            3.0f

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
extern int g_homeDirection;        // defined in linear_actuator.ino
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
#define TMC2209_HOLD_CURRENT_MA   75    // Hold current — motor held between moves (low = cool)

// StallGuard threshold — not used for homing (physical limit switch) but left
// as a safety floor; TMC2209 still raises DIAG on severe overload/stall.
#define TMC2209_STALL_THRESHOLD   50

// Idle power-off: if no move/home command is issued for this many seconds,
// the driver is fully disabled (not just dropped to hold current) and the
// position is marked unknown, requiring a rehome before the next move.
// User-configurable at runtime via PUT /api/config/idle-timeout (0 = never
// sleep); this is only the default for a fresh device / after a NVS erase.
#define IDLE_TIMEOUT_SEC_DEFAULT 3600

// UART address (0–3, set by MS1/MS2 pins — Adafruit board default is 0)
#define TMC2209_ADDRESS            0

// -----------------------------------------------------------------------------
// PIN ASSIGNMENTS — Adafruit ESP32-S2 Feather
// All GPIO are 3.3V logic. Any pin can trigger interrupts.
// Serial1 (RX/TX header pins) used for TMC2209 UART — no SoftwareSerial needed.
// -----------------------------------------------------------------------------

// -- TMC2209 control pins --
#define PIN_TMC_STEP        5   // D5
#define PIN_TMC_DIR         6   // D6
#define PIN_TMC_EN          9   // D9  (active LOW)
#define PIN_TMC_DIAG        A2  // DIAG — TMC2209 drives HIGH when SG_RESULT < SGTHRS*2
// TMC2209 UART: wire Feather TX → 1kΩ → board UART pin; Feather RX → same node
// Serial1 is automatically on the RX/TX header pins — no pin defines needed

// -- Endstop pins (FEEDBACK_LIMIT_DISTANCE and FEEDBACK_LIMIT_DETENT) --
#define PIN_ENDSTOP_HOME   10   // D10 — NC switch, pulls LOW when triggered
#define PIN_ENDSTOP_MAX    11   // D11 — NC switch, pulls LOW when triggered

// -- Detent switch pins (FEEDBACK_LIMIT_DETENT only) --
// Use resistor ladder on A1 (single analog pin) — see WIRING.md
#define PIN_DETENT_ANALOG   A1  // Resistor ladder for up to 7 detent switches

// -- Status LED --
#define PIN_LED            13   // D13 — onboard LED on ESP32-S2 Feather

// -----------------------------------------------------------------------------
// SMART OUTLET CONTROL (CONTROL_SMART_OUTLET)
// Requires WIFI_STA_SSID / WIFI_STA_PASS to be set below.
// Outlet-to-stop mappings are stored in NVS by the setup agent.
// -----------------------------------------------------------------------------

// Maximum number of outlet slots (one per blast gate)
#define SMART_OUTLET_COUNT            7

// How often the poll task queries each outlet (ms)
#define OUTLET_POLL_INTERVAL_MS     500

// HTTP request timeout per outlet — must be shorter than OUTLET_POLL_INTERVAL_MS
// to avoid stalling the poll loop when a device is offline
#define OUTLET_HTTP_TIMEOUT_MS      400

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
