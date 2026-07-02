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
// FEEDBACK TYPE — select exactly one
// -----------------------------------------------------------------------------
// #define FEEDBACK_SENSORLESS    // StallGuard sensorless endstops (disabled — SG unreliable on Adafruit #6121)
#define FEEDBACK_LIMIT_DISTANCE   // Home limit switch on left + step count for stop positions
// #define FEEDBACK_LIMIT_DETENT     // 2 endstops + switch per detent position

// Compile-time guard: sensorless homing requires TMC2209
#if defined(FEEDBACK_SENSORLESS) && !defined(MOTOR_STEPPER_TMC2209)
  #error "FEEDBACK_SENSORLESS requires MOTOR_STEPPER_TMC2209"
#endif

// PIN_ENDSTOP_HOME wiring: NC switch between D10 and GND, INPUT_PULLUP.
//   Normal (contacts closed): pin pulled to GND → LOW → readHomeSwitch() = false
//   Triggered (contacts open): pullup wins → HIGH → readHomeSwitch() = true
// Fail-safe: broken wire → HIGH → reads as triggered → motor stops.
// No max endstop required for current build — single-switch homing only.

// -----------------------------------------------------------------------------
// CONTROL INPUT — select exactly one
// -----------------------------------------------------------------------------
// #define CONTROL_ROTARY          // 8-position rotary switch (resistor ladder on A0)
#define CONTROL_SMART_OUTLET      // Shelly smart outlet polling (auto gate selection)
// #define CONTROL_APP             // Smartphone app (stub)
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
#define NUM_STOPS         2       // Number of selectable positions (max 7)
                                  // Position 0 = disabled/home

// Names for each stop (used in serial debug output)
#define STOP_NAMES { "Home/Disabled", "Stop 1", "Stop 2", "Stop 3", \
                     "Stop 4", "Stop 5", "Stop 6", "Stop 7" }

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

// -----------------------------------------------------------------------------
// GEOMETRY-BASED STOP POSITIONS (sensorless mode, no training required)
// These two parameters let the firmware compute gate positions analytically
// from the known gear geometry rather than from EEPROM calibration.
//
// When FEEDBACK_SENSORLESS is active and no EEPROM calibration is present,
// setup() calls computeStopPositions() to derive g_stopPositionsMM[] from:
//   gate_1_mm = ENDSTOP_MARGIN_TEETH × RACK_PITCH_MM − backoff_mm
//   gate_N_mm = gate_1_mm + (N−1) × STOP_SPACING_TEETH × RACK_PITCH_MM
//
// EEPROM calibration (training mode) always takes priority when present.
// -----------------------------------------------------------------------------

// Teeth of clearance between the physical hard endstop and gate 1 (and gate 7)
// Used for HOMING_MAX_TRAVEL_MM calculation only — computeStopPositions() uses
// ENDSTOP_MARGIN_STEPS (measured value) for the precise gate 1 position.
#define ENDSTOP_MARGIN_TEETH    1

// Measured distance from physical endstop to gate 1, in steps.
// Physically measured: gate 1 at step -51 from home → margin = 51 + HOME_BACKOFF_STEPS = 101
#define ENDSTOP_MARGIN_STEPS  101

// Pinion teeth of travel between adjacent gates (1 full rev + 5 teeth = 20)
#define STOP_SPACING_TEETH     20

// Fallback compile-time distances (used only if both EEPROM and geometry-compute
// are unavailable — should not normally be reached with FEEDBACK_SENSORLESS).
#define STOP_DISTANCES_MM { 0.0f, 3.7f, 86.6f, 169.5f, 252.4f, 335.3f, 418.2f, 501.1f }

// Homing: direction to drive toward home endstop
// 1 = positive step direction, -1 = negative step direction
#define HOME_DIRECTION      (1)

// Speed & acceleration
// steps/mm ≈ 102.94, so:
//   2000 steps/sec ≈ 19 mm/sec (normal moves)
//   1500 steps/sec ≈ 15 mm/sec (homing — StallGuard needs speed to trigger reliably)
#define MAX_SPEED_STEPS_PER_SEC      2000.0f
#define HOMING_SPEED_STEPS_PER_SEC   500.0f
#define ACCELERATION_STEPS_PER_SEC2  1000.0f

// Maximum travel during homing before position is forced to home regardless of stall detection.
// Set to full rack length + margin so a missed stall doesn't grind indefinitely.
#define HOMING_MAX_TRAVEL_MM  ((float)(NUM_STOPS - 1) * (float)(STOP_SPACING_TEETH) * RACK_PITCH_MM \
                               + 2.0f * (float)(ENDSTOP_MARGIN_TEETH) * RACK_PITCH_MM + 25.0f)

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
#define TMC2209_HOLD_CURRENT_MA  150    // Hold current — motor held between moves (low = cool)

// StallGuard threshold (0–255, higher = more sensitive / triggers more easily)
// Tune this empirically:
//   too high = false stalls mid-move
//   too low  = endstop not detected (motor grinds against stop)
// Start at 50 and increase by 10 until homing triggers reliably, then back off 5.
#define TMC2209_STALL_THRESHOLD   50

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

// -- Rotary switch (CONTROL_ROTARY) --
// 3.3V pull-up, 12-bit ADC (0–4095) — see RotaryControl.cpp for thresholds
#define PIN_ROTARY          A0

// -- Toggle switch (CONTROL_ROTARY) --
#define PIN_TOGGLE         12   // D12 — NC to GND; LOW = enabled

// -- E-stop button --
// NC momentary: one terminal to PIN_ESTOP, other to GND.
// Normal: pin LOW (contacts closed). Triggered/broken: pin HIGH → RISING interrupt.
// Any GPIO on ESP32-S2 supports interrupts — not restricted to a specific pin.
#define PIN_ESTOP           A3  // Interrupt-capable (all ESP32 GPIOs are)

// -- Relay output --
#define PIN_RELAY           A4  // HIGH = relay energized

// -- Status LED --
#define PIN_LED            13   // D13 — onboard LED on ESP32-S2 Feather

// -----------------------------------------------------------------------------
// RELAY OUTPUT CONFIGURATION
// -----------------------------------------------------------------------------
#define RELAY_ON_DELAY_MS       0     // ms after reaching stop before relay ON
#define RELAY_OFF_DELAY_MS    500     // ms after leaving stop before relay OFF
#define RELAY_ACTIVE_HIGH      true   // true = HIGH energizes relay

// -----------------------------------------------------------------------------
// TRAINING MODE PARAMETERS (TMC2209 sensorless calibration)
// -----------------------------------------------------------------------------
// Speed during training (steps/sec). Slow enough for reliable stall detection.
// Lower if stalls aren't detected consistently.
#define TRAINING_SPEED_STEPS_PER_SEC    300.0f

// Steps to back off from home endstop before zeroing position
#define TRAINING_HOME_BACKOFF_STEPS      20

// Verify pass tolerance: stall must occur within this fraction of trained distance
// 0.08 = ±8%. Increase if your mechanism has variability.
#define TRAINING_VERIFY_TOLERANCE       0.08f

// Settle time (ms) before stall detection activates after motion starts
#define TRAINING_STALL_SETTLE_MS        400

// Pause (ms) between verify steps to let motor fully stop
#define TRAINING_VERIFY_PAUSE_MS        600

// -----------------------------------------------------------------------------
// STALLGUARD AUTO-TUNE PARAMETERS
// autotune command: binary-searches SGTHRS to find the minimum value that
// reliably detects the endstop stall without false-stalling mid-travel.
// -----------------------------------------------------------------------------

// Retract distance: how far autotune backs away from the endstop before each attempt.
#define AUTOTUNE_SEARCH_MM          15.0f

// Extra approach distance past the retract point on each attempt (mm).
// The motor drives AUTOTUNE_SEARCH_MM + AUTOTUNE_OVERSHOOT_MM toward the endstop,
// so the approach always exceeds the retract — backlash cannot cause a short-stop.
// Increase if the motor sometimes fails to contact the endstop.
#define AUTOTUNE_OVERSHOOT_MM       10.0f

// Stall settle time before checking for stall each attempt (ms).
// Must be long enough for motor to reach full speed.
#define AUTOTUNE_SETTLE_MS          450

// If stall fires within the first N% of expected travel, classify as false stall.
// Lower = more tolerant of early stalls (better for non-rigid mechanisms).
// Higher = stricter (better for rigid mechanisms, separates zones more clearly).
// For a non-rigid mechanism start at 25 and decrease if false stalls are rejected.
#define AUTOTUNE_FALSE_STALL_PCT    25

// Safety margin added to the minimum working SGTHRS.
// Larger = more headroom against false stalls; smaller = more sensitive.
#define AUTOTUNE_MARGIN             15

// Pause after returning to start before next attempt (ms)
#define AUTOTUNE_BACK_OFF_PAUSE_MS  300

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
// WIFI CONTROL (CONTROL_WIFI only)
// Default: AP mode — ESP32 creates its own hotspot. Connect phone to the SSID
// below, then open http://192.168.4.1 in a browser.
//
// Station mode: uncomment WIFI_STA_SSID / WIFI_STA_PASS to join existing WiFi.
// The IP address will be printed to Serial on boot.
// -----------------------------------------------------------------------------
// Setup portal: shown when no WiFi credentials are stored (end-user provisioning).
// Connect to this SSID and visit http://192.168.4.1 to enter your network credentials.
#define WIFI_PORTAL_SSID    "DustGate-Setup"

// Web UI AP (CONTROL_WIFI only — used when in standalone hotspot mode)
#define WIFI_AP_SSID        "DustGate"
#define WIFI_AP_PASS        ""            // Empty = open (no password)

// Station mode credentials — uncomment to hardcode (developer / known network).
// Leave commented for end-user deployments: credentials are stored via the setup portal.
// #define WIFI_STA_SSID    "your-network-name"
// #define WIFI_STA_PASS    "your-password"

#define WIFI_PORT           80

// -----------------------------------------------------------------------------
// SERIAL / DEBUG
// -----------------------------------------------------------------------------
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
