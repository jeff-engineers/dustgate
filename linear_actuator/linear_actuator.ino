// =============================================================================
// linear_actuator.ino — Main sketch
// Target: Adafruit ESP32-S2 Feather + Adafruit TMC2209 Breakout (#6121)
//
// Configuration is entirely in config.h.
//
// Required libraries (install via Arduino Library Manager):
//   - AccelStepper (by Mike McCauley)
//   - TMCStepper (by teemuatlut)
//   - EEPROM (built-in with ESP32 Arduino core)
//
// State machine:
//   STARTUP → HOMING → IDLE → MOVING → AT_STOP
//                                  ↑___________|
// =============================================================================

#include <EEPROM.h>
#include "config.h"
#include "utils/MotionMath.h"
#include "motor/MotorDriver.h"
#include "feedback/FeedbackSystem.h"
#include "control/ControlInput.h"
#include "training/CalibrationStore.h"

// WiFi provisioning — included for any mode that needs network access.
// Handles first-boot captive portal and subsequent NVS credential lookup.
#if defined(CONTROL_WIFI) || defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
  #include "utils/WiFiProvisioner.h"
#endif

// HTTP API server — runs alongside any control mode when ENABLE_HTTP_API is set.
#ifdef ENABLE_HTTP_API
  #include "api/HttpApiServer.h"
  HttpApiServer apiServer;
#endif

// =============================================================================
// Runtime stop positions — single source of truth used by all feedback systems.
// Populated at startup from EEPROM (if valid) or STOP_DISTANCES_MM in config.h.
// =============================================================================
float g_stopPositionsMM[NUM_STOPS + 1];

// Highest stop index actually trained/saved — see MotionMath.h for why this
// is tracked separately from g_stopPositionsMM's raw values.
int g_numTrainedStops = 0;

// Runtime gate count — set by setup wizard via set_num_gates API, stored in NVS.
// MUST remain <= NUM_STOPS; array bounds are determined at compile time.
int g_numActiveStops = 0;   // 0 = unconfigured

// Homing direction — loaded from NVS so the user can flip it via setup wizard without
// recompiling.  All existing code references HOME_DIRECTION which now expands to this.
int g_homeDirection = HOME_DIRECTION_DEFAULT;

bool g_notHomedWarnShown = false; // suppress repeated "not homed" warnings

// Dual-endstop calibration + port roles (declared extern in MotionMath.h).
uint8_t g_stopRoles[NUM_STOPS + 1];
float   g_measuredStepsPerMM = 0.0f;   // 0 = not calibrated → status reports nominal
long    g_measuredSpanSteps  = 0;      // 0 = not calibrated
char    g_manifoldModel[16]  = "custom";

// Default all roles: home at 0, unassigned elsewhere.
static void resetStopRoles() {
    for (int i = 0; i <= NUM_STOPS; i++) g_stopRoles[i] = (i == 0) ? ROLE_HOME : ROLE_UNASSIGNED;
}

void loadCalibration() {
    CalibrationData cal;
    if (CalibrationStore::load(cal)) {
        for (int i = 0; i <= NUM_STOPS; i++) {
            g_stopPositionsMM[i] = (i <= (int)cal.numStops) ? cal.stopMM[i]
                                                             : cal.stopMM[cal.numStops];
        }
        // Restore active gate count from cal (may be overridden by NVS below in setup)
        if (cal.numStops > 0 && (int)cal.numStops <= NUM_STOPS)
            g_numActiveStops = (int)cal.numStops;
        g_numTrainedStops = (int)cal.numStops;
        // v2 fields: roles, manifold model, measured span/steps-per-mm.
        for (int i = 0; i <= NUM_STOPS; i++) g_stopRoles[i] = cal.stopRole[i];
        strlcpy(g_manifoldModel, cal.manifoldModel, sizeof(g_manifoldModel));
        g_measuredStepsPerMM = cal.measuredStepsPerMM;
        g_measuredSpanSteps  = (long)(cal.maxTravelMM * cal.measuredStepsPerMM);
        DEBUG_PRINTLN(F("Loaded calibration from EEPROM."));
        CalibrationStore::print(cal);
    } else {
        // No calibration yet — zero all positions. Setup wizard will call
        // save_stop for each gate to populate them via the HTTP API.
        memset(g_stopPositionsMM, 0, sizeof(g_stopPositionsMM));
        g_numTrainedStops = 0;
        resetStopRoles();
        strlcpy(g_manifoldModel, "custom", sizeof(g_manifoldModel));
        g_measuredStepsPerMM = 0.0f;
        g_measuredSpanSteps  = 0;
        DEBUG_PRINTLN(F("No calibration data — awaiting setup wizard."));
    }
}

// ── Manifold profile (mirror shared/device-model MANIFOLD_PROFILES) ──────────
// Fills gatesMm[1..gateCount] and spanMm for a known model. Returns false for
// 'custom'/unknown (→ manual jog, no auto-placement).
static bool manifoldProfile(const char* model, int gateCount, float* gatesMm, float& spanMm) {
    float first, pitch, endMargin;
    if (strcmp(model, "rockler-2.5") == 0) {
        first = MANIFOLD_2_5_FIRST_GATE_OFFSET_MM; pitch = MANIFOLD_2_5_GATE_PITCH_MM; endMargin = MANIFOLD_2_5_END_MARGIN_MM;
    } else if (strcmp(model, "rockler-4") == 0) {
        first = MANIFOLD_4_FIRST_GATE_OFFSET_MM;   pitch = MANIFOLD_4_GATE_PITCH_MM;   endMargin = MANIFOLD_4_END_MARGIN_MM;
    } else {
        return false;
    }
    for (int i = 1; i <= gateCount; i++) gatesMm[i] = first + (i - 1) * pitch;
    spanMm = first + (gateCount - 1) * pitch + endMargin;
    return true;
}

// Reference-sweep parameters, captured when a /api/calibrate request is consumed
// and used by the STATE_HOMING → STATE_CALIBRATING flow.
char  g_calModel[16] = "custom";
int   g_calGateCount = 0;
bool  g_calibratePending = false;   // calibrate requested → home, then sweep

// Physical gate-to-gate pitch (mm) for a manifold model, or 0 for custom/unknown.
static float manifoldPitchMm(const char* model) {
    if (strcmp(model, "rockler-2.5") == 0) return MANIFOLD_2_5_GATE_PITCH_MM;
    if (strcmp(model, "rockler-4")   == 0) return MANIFOLD_4_GATE_PITCH_MM;
    return 0.0f;
}

// A model string is "recognised" if it names a real profile or the explicit
// 'custom' fallback. Anything else (e.g. a typo like 'rockler2.5') still runs as
// custom — span recorded, no auto-placement — but the caller warns so the typo
// isn't silently swallowed.
static bool isKnownManifoldModel(const char* model) {
    return manifoldPitchMm(model) > 0.0f || strcmp(model, "custom") == 0;
}

// Finish the reference sweep: given the measured far-endstop trigger position (in
// steps, from home datum 0), place all gates and persist. See the placement
// derivation in docs/dual-endstop-calibration.md. Span-based: absorbs per-build
// steps/mm + endstop-location variance; pitch is the fixed manifold property.
static void finishCalibrationSweep(long farTriggerSteps) {
    long farSpanSteps     = farTriggerSteps < 0 ? -farTriggerSteps : farTriggerSteps; // home→far
    long triggerSpanSteps = farSpanSteps + HOME_BACKOFF_STEPS;   // near→far triggers
    float spm = stepsPerMM();                                    // nominal (validated ~0.3%)

    g_measuredSpanSteps  = triggerSpanSteps;
    g_measuredStepsPerMM = spm;
    g_numActiveStops     = g_calGateCount;
    strlcpy(g_manifoldModel, g_calModel, sizeof(g_manifoldModel));
    resetStopRoles();
    for (int i = 1; i <= NUM_STOPS; i++) g_stopPositionsMM[i] = 0.0f;

    float pitchMm = manifoldPitchMm(g_calModel);
    if (pitchMm > 0.0f) {
        // Center the (N-1)*pitch gate array in the measured trigger-to-trigger span.
        // Work in mm-from-home (positive = away from home). Near trigger sits
        // backoffMm toward home from the datum (negative); gate1 is slack/2 past it.
        float backoffMm = (float)HOME_BACKOFF_STEPS / spm;
        float slackMm   = ((float)triggerSpanSteps / spm) - (float)(g_calGateCount - 1) * pitchMm;
        float gate1Mm   = -backoffMm + slackMm / 2.0f;
        for (int i = 1; i <= g_calGateCount && i <= NUM_STOPS; i++) {
            g_stopPositionsMM[i] = gate1Mm + (float)(i - 1) * pitchMm;
        }
        g_numTrainedStops = g_calGateCount;
        DEBUG_PRINT(F("[CAL] placed gates: gate1=")); Serial.print(gate1Mm, 2);
        DEBUG_PRINT(F("mm pitch=")); Serial.print(pitchMm, 1);
        DEBUG_PRINT(F("mm span=")); Serial.print((float)triggerSpanSteps / spm, 1);
        DEBUG_PRINTLN(F("mm"));
    } else {
        // Custom manifold — record the span but leave gate positions for manual jog.
        g_numTrainedStops = 0;
        DEBUG_PRINTLN(F("[CAL] custom manifold — span recorded, gates via manual jog."));
    }

    CalibrationData cal;
    cal.magic   = CALIB_MAGIC;
    cal.version = CALIB_VERSION;
    cal.numStops = (uint8_t)g_calGateCount;
    cal.maxTravelMM = (float)triggerSpanSteps / spm;
    cal.measuredStepsPerMM = spm;
    for (int i = 0; i <= NUM_STOPS; i++) cal.stopMM[i]   = g_stopPositionsMM[i];
    for (int i = 0; i <= NUM_STOPS; i++) cal.stopRole[i] = g_stopRoles[i];
    cal.stopMM[0] = 0.0f;
    strlcpy(cal.manifoldModel, g_calModel, sizeof(cal.manifoldModel));
    CalibrationStore::save(cal);
}

// -- Motor driver (TMC2209) --
#include "motor/StepperTMC2209Driver.h"
StepperTMC2209Driver motor;

// -- Feedback system --
#ifdef FEEDBACK_LIMIT_DISTANCE
  #include "feedback/LimitSwitchDistance.h"
  LimitSwitchDistance feedback;
#else
  #error "No feedback type defined in config.h — define FEEDBACK_LIMIT_DISTANCE in config.h"
#endif

// -- Control input --
#ifdef CONTROL_SMART_OUTLET
  #include "control/SmartOutletControl.h"
  SmartOutletControl control;

  // Outlet discovery — used by the /api/outlets/discover handling in loop()
  // below. Must run on this (the main loop) task; see HttpApiServer.cpp's
  // /api/outlets/discover route comment for why.
  #include "utils/MdnsQuery.h"
  #include "outlets/ShellyGen1Outlet.h"
  #include "outlets/ShellyGen2Outlet.h"
  #include "outlets/ShellyDeviceName.h"
#elif defined(CONTROL_SERIAL_DEBUG)
  #include "control/SerialDebugControl.h"
  SerialDebugControl control;
#else
  #error "No control type defined in config.h"
#endif

// Supplemental serial command processor — active alongside any primary control
// mode when ENABLE_SERIAL_COMMANDS is set.  When CONTROL_SERIAL_DEBUG is the
// active mode the primary `control` object already handles serial input; no
// second instance is needed.
#if defined(ENABLE_SERIAL_COMMANDS) && !defined(CONTROL_SERIAL_DEBUG)
  #include "control/SerialDebugControl.h"
  SerialDebugControl _serialCmds;
#endif

// =============================================================================
// E-stop
// Software-only: set via the 'estop' serial command or HTTP API. No physical
// e-stop button — this hardware isn't powerful enough to need one.
// =============================================================================
volatile bool g_eStopTriggered = false;
bool          g_hardwareFault  = false; // set when begin() fails — not clearable without reset

// =============================================================================
// State machine
// =============================================================================
enum State {
    STATE_STARTUP,
    STATE_HOMING,
    STATE_IDLE,
    STATE_MOVING,
    STATE_AT_STOP,
    STATE_CALIBRATING,   // dual-endstop reference sweep (home already done)
    STATE_DISABLED,
    STATE_ERROR
};

State currentState = STATE_STARTUP;
int   currentStop  = -1;
int   targetStop   = 0;

// =============================================================================
// Idle power-off — see HttpApiServer::idleTimeoutSec(). Reset on every real
// move/home command; if it goes unrefreshed past the configured timeout while
// otherwise idle, the driver is fully disabled and the position marked
// unknown, forcing a rehome (reuses the existing "not homed" gating below)
// before the next move rather than sitting energized indefinitely.
// =============================================================================
unsigned long g_lastActivityMs = 0;
bool          g_driverAsleep   = false;

// Forward declarations
void issueMove(int stop);
void startHoming();

// =============================================================================
// setup()
// =============================================================================
void setup() {
    Serial.begin(SERIAL_BAUD);
    // ESP32-S2 native USB: wait until serial monitor connects, up to 5s.
    // Open Serial Monitor before or immediately after flashing to catch boot logs.
    unsigned long t0 = millis();
    while (!Serial && (millis() - t0) < 5000) { delay(10); }
    delay(100); // brief settle after connection
    DEBUG_PRINTLN(F("=== DustGate ==="));
    DEBUG_PRINTLN(F("Target: ESP32-S2 Feather + TMC2209"));

    // ESP32-S2 ADC: 12-bit (0-4095), 3.3V reference
    analogReadResolution(12);

    // WiFi provisioning — must run before any WiFi-dependent control mode.
    // If WIFI_STA_SSID is hardcoded in config.h it is used directly.
    // Otherwise stored NVS credentials are tried; if none exist or connection
    // fails, a captive portal AP ("DustGate-Setup") is started and this call
    // blocks until the user provides credentials (then reboots).
#if defined(CONTROL_WIFI) || defined(CONTROL_SMART_OUTLET) || defined(ENABLE_HTTP_API)
    WiFiProvisioner::begin();
#endif

    // Load calibration before feedback system initialises
    CalibrationStore::begin(); // Required on ESP32: allocates EEPROM flash region
    loadCalibration();

    // Load runtime config from NVS — takes priority over cal defaults above.
    // Uses the same namespace ("api_cfg") as HttpApiServer to avoid opening
    // multiple Preferences namespaces for the same flash partition.
    {
        Preferences prefs;
        prefs.begin("api_cfg", true);
        g_homeDirection  = prefs.getInt("home_dir",  HOME_DIRECTION_DEFAULT);
        int nvsGates     = prefs.getInt("num_gates", 0); // 0 = not saved yet
        prefs.end();
        if (nvsGates >= 1 && nvsGates <= NUM_STOPS) g_numActiveStops = nvsGates;
    }
    DEBUG_PRINT(F("[CFG] homeDirection=")); Serial.print(g_homeDirection);
    DEBUG_PRINT(F("  numActiveStops="));   Serial.println(g_numActiveStops);

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    bool ok = true;
    ok &= motor.begin();
    ok &= feedback.begin(&motor);
    ok &= control.begin();
#if defined(ENABLE_SERIAL_COMMANDS) && !defined(CONTROL_SERIAL_DEBUG)
    _serialCmds.begin();   // supplemental serial processor (non-fatal if begin() returns false)
#endif

    if (!ok) {
        DEBUG_PRINTLN(F("INIT FAILED — check wiring and config.h"));
        DEBUG_PRINTLN(F("Motion commands are disabled. Fix wiring and reset."));
        g_hardwareFault = true;
        currentState = STATE_ERROR;
        return;
    }

    DEBUG_PRINTLN(F("Init OK. Type 'enable' to home and start."));
    currentState = STATE_IDLE;

#ifdef ENABLE_HTTP_API
    if (!apiServer.begin()) {
        DEBUG_PRINTLN(F("[API] HTTP server failed to start."));
    } else {
        DEBUG_PRINT(F("[API] Listening on port 80.  Key: "));
        Serial.println(apiServer.apiKey());
    }
#endif
}

// =============================================================================
// loop()
// =============================================================================
void loop() {
    // Run background processing for control input (HTTP server, etc.)
    control.update();

    // -- Endstop over-travel safety — runs BEFORE motor.update() -----------------
    // Must run before the step, not after: motor.update() steps the carriage
    // regardless of currentState, so reacting *after* it let one step through per
    // loop — jogging into an already-triggered switch drifted the position +1 step
    // each time. Checking here clamps the target before any step is taken.
    // Directional: stops travel *toward* a triggered switch but allows backing
    // AWAY to release it. Homing drives into the home switch on purpose;
    // STATE_MOVING has its own endstop handling, so both are skipped here.
#ifdef FEEDBACK_LIMIT_DISTANCE
    // Applies to jogs AND commanded moves (STATE_MOVING). Homing drives into the
    // home switch on purpose; the calibration sweep manages the far switch itself.
    if (motor.isMoving() && currentState != STATE_HOMING && currentState != STATE_CALIBRATING) {
        long dtg = motor.distanceToGo();               // signed steps to target
        bool towardFar  = (dtg * (long)(-HOME_DIRECTION)) > 0; // away from home
        bool towardHome = (dtg * (long)( HOME_DIRECTION)) > 0;
        if ((digitalRead(PIN_ENDSTOP_MAX) == HIGH) && towardFar) {
            motor.stop();
            DEBUG_PRINTLN(F("[SAFETY] Far endstop triggered — halted travel toward far end."));
        } else if ((digitalRead(PIN_ENDSTOP_HOME) == HIGH) && towardHome) {
            motor.stop();
            DEBUG_PRINTLN(F("[SAFETY] Home endstop triggered — halted travel toward home."));
        }
    }
#endif

    motor.update();

    // -- Endstop transition logging — AFTER update() so the logged position is
    //    the post-step position at the actual trigger point. Change-gated (jogs
    //    included, since jogs never enter STATE_MOVING).
#ifdef FEEDBACK_LIMIT_DISTANCE
    {
        // Debounced transition logging. When parked exactly on the home datum the
        // switch sits right at its trigger edge and chatters open/triggered; a raw
        // change gate logs every flicker. Only log a transition once the new level
        // has held stable for ENDSTOP_DEBOUNCE_MS.
        static const unsigned long ENDSTOP_DEBOUNCE_MS = 40;
        static bool esInit = false, lastHome = false, lastFar = false;
        static bool candHome = false, candFar = false;
        static unsigned long candHomeSince = 0, candFarSince = 0;
        unsigned long nowMs = millis();
        bool home = (digitalRead(PIN_ENDSTOP_HOME) == HIGH); // HIGH = triggered (NC open)
        bool far  = (digitalRead(PIN_ENDSTOP_MAX)  == HIGH);
        if (!esInit) {
            lastHome = candHome = home; lastFar = candFar = far;
            candHomeSince = candFarSince = nowMs; esInit = true;
        }
        // Track how long the current raw reading has been steady.
        if (home != candHome) { candHome = home; candHomeSince = nowMs; }
        if (far  != candFar ) { candFar  = far;  candFarSince  = nowMs; }
        if (candHome != lastHome && (nowMs - candHomeSince) >= ENDSTOP_DEBOUNCE_MS) {
            lastHome = candHome;
            DEBUG_PRINT(F("[ENDSTOP] Home: ")); DEBUG_PRINT(lastHome ? F("TRIGGERED") : F("open"));
            DEBUG_PRINT(F("  pos=")); DEBUG_PRINTLN(motor.getPosition());
        }
        if (candFar != lastFar && (nowMs - candFarSince) >= ENDSTOP_DEBOUNCE_MS) {
            lastFar = candFar;
            DEBUG_PRINT(F("[ENDSTOP] Far: ")); DEBUG_PRINT(lastFar ? F("TRIGGERED") : F("open"));
            DEBUG_PRINT(F("  pos=")); DEBUG_PRINTLN(motor.getPosition());
        }
    }
#endif

    // -- E-stop (software-latched, no physical button): highest priority ------
    if (g_eStopTriggered) {
        motor.stop();
        motor.enable(false);
        if (currentState != STATE_ERROR) {
            currentState = STATE_ERROR;
            DEBUG_PRINTLN(F(""));
            DEBUG_PRINTLN(F("!!! E-STOP ACTIVE — motor disabled."));
            DEBUG_PRINTLN(F("!!! Type 'home' to clear and re-home."));
            DEBUG_PRINTLN(F(""));
        }
    }

    // -- Serial commands (CONTROL_SERIAL_DEBUG or supplemental ENABLE_SERIAL_COMMANDS) --
#if defined(CONTROL_SERIAL_DEBUG) || defined(ENABLE_SERIAL_COMMANDS)
    // _SC aliases the active serial command object:
    //   CONTROL_SERIAL_DEBUG  → primary `control` (already a SerialDebugControl)
    //   ENABLE_SERIAL_COMMANDS → supplemental `_serialCmds` instance
    #ifdef CONTROL_SERIAL_DEBUG
      #define _SC control
    #else
      #define _SC _serialCmds
    #endif

    _SC.readRequestedStop(); // process serial input before checking flags

    if (_SC.consumeEStop()) {
        if (!g_eStopTriggered) {
            DEBUG_PRINTLN(F(""));
            DEBUG_PRINTLN(F("!!! E-STOP command received."));
            DEBUG_PRINTLN(F("!!! Type 'home' to clear and re-home."));
            DEBUG_PRINTLN(F(""));
        }
        g_eStopTriggered = true;
    }

    if (_SC.consumeHomeRequest() && currentState != STATE_HOMING) {
        if (g_hardwareFault) {
            DEBUG_PRINTLN(F("[ERROR] Hardware fault — fix wiring and reset before homing."));
        } else {
            g_eStopTriggered = false;
            g_notHomedWarnShown = false;
            motor.enable(true);
            currentState = STATE_HOMING;
            startHoming();
        }
    }

    if (_SC.consumeGconfRequest()) {
        motor.printDriverRegs();
    }

    // Serial 'calibrate <model> <gates>' — same home→sweep flow as POST /api/calibrate.
    {
        char calModel[16]; int calGates = 0;
        if (_SC.consumeCalibrateRequest(calModel, sizeof(calModel), calGates)) {
            if (g_hardwareFault) {
                DEBUG_PRINTLN(F("[CAL] Hardware fault — fix wiring and reset first."));
            } else if (currentState == STATE_HOMING || currentState == STATE_CALIBRATING ||
                       currentState == STATE_MOVING) {
                DEBUG_PRINTLN(F("[CAL] Busy — retry when idle."));
            } else {
                if (!isKnownManifoldModel(calModel)) {
                    DEBUG_PRINT(F("[CAL] Unknown model '")); DEBUG_PRINT(calModel);
                    DEBUG_PRINTLN(F("' — treating as custom (span only, no auto-placement)."));
                }
                strlcpy(g_calModel, calModel, sizeof(g_calModel));
                g_calGateCount     = calGates;
                g_calibratePending = true;
                g_eStopTriggered   = false;
                motor.enable(true);
                DEBUG_PRINTLN(F("[CAL] Homing, then sweeping to far endstop..."));
                startHoming();
                currentState = STATE_HOMING;
            }
        }
    }

    {
        float jogMM = 0.0f;
        if (_SC.consumeJogRequest(jogMM)) {
            if (g_hardwareFault) {
                DEBUG_PRINTLN(F("[ERROR] Hardware fault — fix wiring and reset before jogging."));
            } else {
                // Jog is a raw calibration move — clears e-stop and error state so the
                // state machine doesn't immediately call motor.stop() on the next tick.
                g_eStopTriggered = false;
                currentState = STATE_IDLE;
                long delta = (long)(jogMM * stepsPerMM() * -HOME_DIRECTION);
                long target = motor.getPosition() + delta;
                motor.enable(true);
                motor.moveTo(target);
                Serial.print(F("[JOG] delta=")); Serial.print(delta);
                Serial.print(F(" steps  target=")); Serial.println(target);
            }
        }
    }

    if (_SC.consumeClearCalRequest()) {
        CalibrationStore::erase();
        loadCalibration();
        // Without this, currentStop (and therefore homed, which is derived
        // from it) survived a clearcal untouched — so the UI kept reporting
        // "homed at gate N" from before the reset even though numActiveStops
        // just dropped to 0 and no gates exist to be at. That stale combo is
        // exactly what made the manifold visualizer's flow arrow appear
        // pointing at a gate index with no corresponding DOM element.
        currentStop = -1;
        targetStop  = 0;
        DEBUG_PRINTLN(F("Calibration cleared. config.h defaults loaded."));
    }

    // Supplemental mode: translate serial position commands into direct moves.
    // In CONTROL_SERIAL_DEBUG mode STATE_IDLE already handles this via
    // control.readRequestedStop(). Only fires when the requested stop changes
    // (tracked via static) to avoid re-issuing on every loop tick.
    #if defined(ENABLE_SERIAL_COMMANDS) && !defined(CONTROL_SERIAL_DEBUG)
    {
        static int _scLastActioned = -1;
        int serialStop = _SC.readRequestedStop();
        // Fire from AT_STOP too, not just IDLE — otherwise after landing on a gate
        // (which leaves us in STATE_AT_STOP) the next stop command is ignored until
        // a re-home.
        if (serialStop >= 0 && serialStop != _scLastActioned && !g_eStopTriggered &&
            (currentState == STATE_IDLE || currentState == STATE_AT_STOP)) {
            _scLastActioned = serialStop;
            targetStop = serialStop;
            issueMove(serialStop);
            // In CONTROL_SMART_OUTLET mode the outlet poller runs concurrently.
            // Without a manual override it sees no active tool and returns home
            // after OUTLET_OFF_DEBOUNCE_MS.  setManualOverride() holds the stop
            // until a real outlet power event clears it.
            #ifdef CONTROL_SMART_OUTLET
            control.setManualOverride(serialStop);
            #endif
        }
    }
    #endif

    #undef _SC
#endif // CONTROL_SERIAL_DEBUG || ENABLE_SERIAL_COMMANDS

    // -- HTTP API commands ----------------------------------------------------
#ifdef ENABLE_HTTP_API
    if (apiServer.consumeEStopRequest()) {
        if (!g_eStopTriggered) {
            DEBUG_PRINTLN(F("!!! E-STOP (HTTP API)."));
        }
        g_eStopTriggered = true;
    }

    if (apiServer.consumeHomeRequest() && currentState != STATE_HOMING) {
        if (g_hardwareFault) {
            DEBUG_PRINTLN(F("[API] Hardware fault — reset before homing."));
        } else {
            g_eStopTriggered   = false;
            g_notHomedWarnShown = false;
            motor.enable(true);
            currentState = STATE_HOMING;
            startHoming();
        }
    }

    {
        int moveStop = -1;
        if (apiServer.consumeMoveRequest(moveStop) && !g_hardwareFault &&
            currentState != STATE_HOMING) {
            if (moveStop >= 1 && moveStop <= NUM_STOPS && g_stopRoles[moveStop] == ROLE_BLOCKED) {
                // Blocked ports (capped, or reserved as a v2 feed) are never move
                // targets — see docs/dual-endstop-calibration.md.
                DEBUG_PRINT(F("[API] Move: gate blocked, ignoring: "));
                DEBUG_PRINTLN(moveStop);
            } else if (moveStop >= 0 && moveStop <= g_numActiveStops) {
                targetStop = moveStop;
#ifdef CONTROL_SMART_OUTLET
                // In outlet mode, a manual move sets an override so the poll
                // task doesn't immediately revert to the outlet-selected stop.
                // Override clears automatically when the next tool powers on.
                control.setManualOverride(moveStop);
#endif
                issueMove(targetStop);
            } else {
                DEBUG_PRINT(F("[API] Move: stop out of range: "));
                DEBUG_PRINTLN(moveStop);
            }
        }
    }

    {
        float jogMM = 0.0f;
        if (apiServer.consumeJogRequest(jogMM) && !g_hardwareFault) {
            g_eStopTriggered = false;
            currentState = STATE_IDLE;
            long delta  = (long)(jogMM * stepsPerMM() * -HOME_DIRECTION);
            long target = motor.getPosition() + delta;
            motor.enable(true);
            motor.moveTo(target);
            DEBUG_PRINT(F("[API] Jog delta=")); Serial.print(delta);
            DEBUG_PRINT(F(" steps  target=")); Serial.println(target);
        }
    }

    if (apiServer.consumeClearCalRequest()) {
        CalibrationStore::erase();
        loadCalibration();
        g_numActiveStops = 0;  // return to unconfigured — setup wizard can restart
        // Same reasoning as the serial 'clearcal' handler above: currentStop
        // (and homed, derived from it) previously survived a clearcal
        // untouched, so the UI kept reporting "homed at gate N" from before
        // Start Over even though there are now zero gates to be at — which is
        // what made the manifold visualizer's flow arrow render pointing at a
        // gate index with no corresponding DOM element (or, on a later
        // restart, at a stale/mismatched position once gates existed again).
        currentStop = -1;
        targetStop  = 0;
#ifdef CONTROL_SMART_OUTLET
        // "Start Over" in the setup wizard means a full reset — without this,
        // the previous run's tool-to-gate outlet mappings (names, IPs,
        // thresholds) silently survived in NVS and kept driving gate
        // selection even after the wizard restarted from gate-count 0.
        control.clearAllOutlets();
#endif
        DEBUG_PRINTLN(F("[API] Calibration cleared. Gate count reset to 0."));
    }

    {
        int stopIdx = -1;
        if (apiServer.consumeSetStopRequest(stopIdx) && currentState == STATE_IDLE) {
            // Convert current motor position (steps) to mm.
            // HOME_DIRECTION inverts the step sign: positive steps are away from home.
            float currentMM = (float)motor.getPosition() / stepsPerMM() / (-HOME_DIRECTION);

            // Authoritative overlap guard (see MIN_STOP_SEPARATION_MM). Reject a
            // save that lands on top of another already-saved gate — home (0)
            // and the slot being (re)saved itself are excluded. Only meaningful
            // for gates within the active count; positions beyond it are stale.
            bool conflict = false;
            for (int j = 1; j <= g_numActiveStops && j <= NUM_STOPS; j++) {
                if (j == stopIdx) continue;
                if (g_stopPositionsMM[j] == 0.0f) continue; // unsaved slot
                if (fabsf(currentMM - g_stopPositionsMM[j]) < MIN_STOP_SEPARATION_MM) {
                    conflict = true;
                    DEBUG_PRINT(F("[API] Rejected stop "));  Serial.print(stopIdx);
                    DEBUG_PRINT(F(" at "));                   Serial.print(currentMM, 2);
                    DEBUG_PRINT(F(" mm — too close to stop ")); Serial.print(j);
                    DEBUG_PRINT(F(" ("));                     Serial.print(g_stopPositionsMM[j], 2);
                    DEBUG_PRINTLN(F(" mm). Jog further away and retry."));
                    break;
                }
            }
            // Only persist when the position clears the overlap guard —
            // otherwise leave calibration untouched (can't return here: the
            // rest of loop() must still run this iteration).
            if (!conflict) {
                g_stopPositionsMM[stopIdx] = currentMM;

                // Persist to CalibrationStore; reload other fields from existing data.
                CalibrationData cal;
                if (!CalibrationStore::load(cal)) {
                    // No valid cal yet — fill in what we know
                    cal.magic              = CALIB_MAGIC;
                    cal.version            = CALIB_VERSION;
                    cal.numStops           = 0;
                    cal.maxTravelMM        = 0.0f;
                    cal.measuredStepsPerMM = stepsPerMM();
                    memset(cal.stopMM, 0, sizeof(cal.stopMM));
                    // v2 fields: default roles (home at 0, unassigned elsewhere),
                    // custom manifold — so a save path never persists garbage.
                    for (int i = 0; i <= NUM_STOPS; i++) cal.stopRole[i] = (i == 0) ? ROLE_HOME : ROLE_UNASSIGNED;
                    strlcpy(cal.manifoldModel, "custom", sizeof(cal.manifoldModel));
                }
                cal.stopMM[stopIdx] = currentMM;
                if (stopIdx > (int)cal.numStops) cal.numStops = (uint8_t)stopIdx;
                CalibrationStore::save(cal);

                if (stopIdx > g_numTrainedStops) g_numTrainedStops = stopIdx;

                // Keep runtime count in sync (expand; never shrink during a session)
                if (stopIdx > g_numActiveStops) g_numActiveStops = stopIdx;

                DEBUG_PRINT(F("[API] Stop ")); Serial.print(stopIdx);
                DEBUG_PRINT(F(" saved at "));  Serial.print(currentMM, 2);
                DEBUG_PRINTLN(F(" mm"));
            }
        }
    }

    // Motor direction (runtime NVS override)
    {
        int newDir = 0;
        if (apiServer.consumeSetDirectionRequest(newDir)) {
            g_homeDirection = newDir;
            DEBUG_PRINT(F("[API] Motor direction: "));
            DEBUG_PRINTLN(newDir > 0 ? F("normal") : F("inverted"));
        }
    }

    // Active gate count (runtime NVS override)
    {
        int newGates = 0;
        if (apiServer.consumeSetNumGatesRequest(newGates)) {
            // Clear saved positions beyond the new count so a stale gate can't
            // reappear as a phantom proximity conflict if the count is later
            // raised again (positions live in RAM here; the EEPROM copy is
            // cleaned up below).
            for (int i = newGates + 1; i <= NUM_STOPS; i++) {
                g_stopPositionsMM[i] = 0.0f;
            }
            g_numActiveStops = newGates;

            // Trim the persisted calibration to match, so a reboot doesn't
            // restore the old (higher) gate count from cal.numStops.
            CalibrationData cal;
            if (CalibrationStore::load(cal) && (int)cal.numStops > newGates) {
                cal.numStops = (uint8_t)newGates;
                for (int i = newGates + 1; i <= NUM_STOPS; i++) {
                    cal.stopMM[i] = 0.0f;
                }
                CalibrationStore::save(cal);
            }

            DEBUG_PRINT(F("[API] Active gates: "));
            DEBUG_PRINTLN(g_numActiveStops);
        }
    }

    // Reference-sweep calibration (dual endstop). Kicks off a home → sweep flow:
    // this just records the request + re-homes; the sweep motion runs in
    // STATE_HOMING → STATE_CALIBRATING. See docs/dual-endstop-calibration.md.
    {
        char model[16]; int gateCount = 0;
        if (apiServer.consumeCalibrateRequest(model, sizeof(model), gateCount)) {
            if (g_hardwareFault) {
                DEBUG_PRINTLN(F("[CAL] Hardware fault — fix wiring and reset first."));
            } else if (gateCount < 1 || gateCount > NUM_STOPS) {
                DEBUG_PRINTLN(F("[CAL] Bad gate count — ignored."));
            } else if (currentState == STATE_HOMING || currentState == STATE_CALIBRATING ||
                       currentState == STATE_MOVING) {
                DEBUG_PRINTLN(F("[CAL] Busy — retry when idle."));
            } else {
                if (!isKnownManifoldModel(model)) {
                    DEBUG_PRINT(F("[CAL] Unknown model '")); DEBUG_PRINT(model);
                    DEBUG_PRINTLN(F("' — treating as custom (span only, no auto-placement)."));
                }
                strlcpy(g_calModel, model, sizeof(g_calModel));
                g_calGateCount     = gateCount;
                g_calibratePending = true;
                // Match the serial path: clear a latched e-stop and ensure the
                // driver is powered so calibrate works even from ERROR/idle-sleep.
                g_eStopTriggered   = false;
                motor.enable(true);
                DEBUG_PRINT(F("[CAL] Requested: ")); DEBUG_PRINT(model);
                DEBUG_PRINT(F(" x")); DEBUG_PRINT(gateCount);
                DEBUG_PRINTLN(F(" — homing, then sweeping to far endstop."));
                startHoming();
                currentState = STATE_HOMING;
            }
        }
    }

    // Port-role change (dual endstop / v2 topology).
    {
        int roleIdx = -1, roleVal = 0;
        if (apiServer.consumePortRoleRequest(roleIdx, roleVal)) {
            if (roleIdx >= 1 && roleIdx <= NUM_STOPS) {
                g_stopRoles[roleIdx] = (uint8_t)roleVal;
                CalibrationData cal;
                if (CalibrationStore::load(cal)) {
                    cal.stopRole[roleIdx] = (uint8_t)roleVal;
                    CalibrationStore::save(cal);
                }
                DEBUG_PRINT(F("[API] Port role: gate ")); DEBUG_PRINT(roleIdx);
                DEBUG_PRINT(F(" = ")); DEBUG_PRINTLN(roleVal);
            }
        }
    }

    // Enable / disable — TODO: add ControlInput::setEnabled() to the base class
    // so this works for all modes, not just serial debug.

#ifdef CONTROL_SMART_OUTLET
    {
        HttpApiServer::OutletConfigCmd cmd;
        if (apiServer.consumeOutletConfigRequest(cmd)) {
            control.configureOutlet(cmd.slot, cmd.generation, cmd.ip, cmd.name,
                                    cmd.stopIndex, cmd.thresholdW, cmd.host);
        }
        int delSlot = -1;
        if (apiServer.consumeOutletDeleteRequest(delSlot)) {
            control.removeOutlet(delSlot);
        }
        if (apiServer.consumeOutletSaveRequest()) {
            control.saveAll();
        }

        HttpApiServer::DustCollectorCmd dcCmd;
        if (apiServer.consumeDustCollectorConfigRequest(dcCmd)) {
            control.configureDustCollector(dcCmd.generation, dcCmd.ip, dcCmd.host);
        }
        if (apiServer.consumeDustCollectorDeleteRequest()) {
            control.removeDustCollector();
        }
        bool dcSwitchOn = false;
        if (apiServer.consumeDustCollectorSwitchRequest(dcSwitchOn)) {
            control.setDcManual(dcSwitchOn);
        }

        // Outlet discovery — runs synchronously here (main loop task) rather
        // than in a spawned FreeRTOS task; see HttpApiServer.cpp's
        // /api/outlets/discover route comment. Blocks the main loop for the
        // duration of the scan (mDNS query + a couple short HTTP probes per
        // match), which is fine since this only runs on an explicit,
        // infrequent wizard action while the system is otherwise idle.
        if (apiServer.consumeDiscoverRequest()) {
            DynamicJsonDocument doc(2048);
            JsonArray results = doc.to<JsonArray>();

            // mDNS/UDP query is lossy — retry a few times and merge unique
            // hosts by IP, so a device only needs to answer once across all
            // attempts to show up in the final list.
            String hitIp[DISCOVER_MAX_RESULTS];
            String hitHost[DISCOVER_MAX_RESULTS];
            int hitCount = 0;

            for (int attempt = 0; attempt < DISCOVER_MDNS_ATTEMPTS; attempt++) {
                MdnsHit mdnsHits[DISCOVER_MAX_RESULTS];
                int n = mdnsQueryHttpTcp(DISCOVER_MDNS_TIMEOUT_MS, mdnsHits, DISCOVER_MAX_RESULTS);
                DEBUG_PRINT(F("[DISCOVER] attempt "));
                DEBUG_PRINT(attempt + 1);
                DEBUG_PRINT(F("/"));
                DEBUG_PRINT(DISCOVER_MDNS_ATTEMPTS);
                DEBUG_PRINT(F(": mDNS _http._tcp query returned "));
                DEBUG_PRINT(n);
                DEBUG_PRINTLN(F(" host(s):"));

                for (int i = 0; i < n; i++) {
                    String host = mdnsHits[i].hostname;
                    String ip   = mdnsHits[i].ip;
                    String hostLower = host;
                    hostLower.toLowerCase();
                    bool matched = hostLower.indexOf("shelly") >= 0;

                    DEBUG_PRINT(F("  - "));
                    DEBUG_PRINT(host.length() ? host : String("(no hostname)"));
                    DEBUG_PRINT(F("  "));
                    DEBUG_PRINT(ip);
                    DEBUG_PRINTLN(matched ? F("  [matched \"shelly\"]") : F("  [skipped — no \"shelly\" in hostname]"));

                    if (!matched) continue;

                    bool dup = false;
                    for (int j = 0; j < hitCount; j++) {
                        if (hitIp[j] == ip) { dup = true; break; }
                    }
                    if (dup || hitCount >= DISCOVER_MAX_RESULTS) continue;
                    hitIp[hitCount]   = ip;
                    hitHost[hitCount] = host;
                    hitCount++;
                }

                if (attempt < DISCOVER_MDNS_ATTEMPTS - 1) delay(DISCOVER_MDNS_RETRY_DELAY_MS);
            }

            DEBUG_PRINT(F("[DISCOVER] "));
            DEBUG_PRINT(hitCount);
            DEBUG_PRINTLN(F(" unique host(s) across all attempts — probing:"));

            for (int i = 0; i < hitCount; i++) {
                const String& ip   = hitIp[i];
                const String& host = hitHost[i];

                // Gen 2 first — the vast majority of discoverable Shelly
                // hardware today (including this project's reference "Plug US
                // G4") speaks Gen2/RPC, so trying it first keeps discovery
                // fast instead of eating a timeout on every device before
                // falling back to Gen 1.
                ShellyGen2Outlet gen2(ip.c_str(), "discover");
                bool ok  = gen2.poll();
                float pw = gen2.getPowerW();
                int  gen = 2;
                if (!ok) {
                    ShellyGen1Outlet gen1(ip.c_str(), "discover");
                    ok  = gen1.poll();
                    pw  = gen1.getPowerW();
                    gen = 1;
                }
                String devName = ok ? fetchShellyDeviceName(ip.c_str(), gen) : String();
                DEBUG_PRINT(F("  - ")); DEBUG_PRINT(host); DEBUG_PRINT(F("  "));
                DEBUG_PRINT(ip);
                DEBUG_PRINT(F("  probe -> reachable="));
                DEBUG_PRINT(ok ? F("yes") : F("no"));
                DEBUG_PRINT(F(" gen="));
                DEBUG_PRINT(ok ? gen : 0);
                DEBUG_PRINT(F(" name="));
                DEBUG_PRINTLN(devName.length() ? devName : String("(none set)"));

                JsonObject o = results.createNestedObject();
                o["ip"]        = ip;
                o["hostname"]  = host;
                o["name"]      = devName;   // app-assigned Shelly device name, "" if unset
                o["reachable"] = ok;
                o["powerW"]    = pw;
                o["gen"]       = ok ? gen : 0;
            }
            if (hitCount == 0) {
                DEBUG_PRINTLN(F("  (no _http._tcp responders at all — check the outlets are powered, "
                                 "joined to WiFi, and that mDNS is enabled in the Shelly app's device settings)"));
            }

            String out; serializeJson(doc, out);
            apiServer.respondDiscover(out);
        }
    }

    // Outlet ping — probe a single IP on the main loop (see consumePingRequest
    // in HttpApiServer for why it's here rather than a spawned task).
    {
        char pingIp[40];
        if (apiServer.consumePingRequest(pingIp, sizeof(pingIp))) {
            // Gen 2 first — same reasoning as discover above: today's Shelly
            // hardware (incl. this project's reference Plug US G4) speaks
            // Gen2/RPC, so probing it first avoids eating a Gen 1 timeout on
            // every reachable device before falling back.
            ShellyGen2Outlet gen2(pingIp, "ping");
            bool ok  = gen2.poll();
            float pw = gen2.getPowerW();
            int  gen = 2;
            if (!ok) {
                ShellyGen1Outlet gen1(pingIp, "ping");
                ok  = gen1.poll();
                pw  = gen1.getPowerW();
                gen = 1;
            }
            String devName = ok ? fetchShellyDeviceName(pingIp, gen) : String();

            DEBUG_PRINT(F("[PING] ")); DEBUG_PRINT(pingIp);
            DEBUG_PRINT(F(" -> reachable=")); DEBUG_PRINT(ok ? F("yes") : F("no"));
            DEBUG_PRINT(F(" gen=")); DEBUG_PRINT(ok ? gen : 0);
            DEBUG_PRINT(F(" name=")); DEBUG_PRINTLN(devName.length() ? devName : String("(none set)"));

            StaticJsonDocument<192> resp;
            resp["reachable"] = ok;
            resp["powerW"]    = pw;
            resp["gen"]       = ok ? gen : 0;
            resp["name"]      = devName;  // app-assigned Shelly device name, "" if unset
            String out; serializeJson(resp, out);
            apiServer.respondPing(out);
        }
    }
#endif // CONTROL_SMART_OUTLET

    // ------------------------------------------------------------------
    // Idle power-off — only while genuinely at rest (home or a gate), never
    // mid-move/homing. Reuses the existing "not homed" gating (currentStop ==
    // -1) to force a rehome on the next move instead of adding a new state.
    // ------------------------------------------------------------------
    {
        int timeoutSec = apiServer.idleTimeoutSec();
        if (timeoutSec > 0 && !g_driverAsleep &&
            (currentState == STATE_IDLE || currentState == STATE_AT_STOP) &&
            currentStop != -1 &&
            (millis() - g_lastActivityMs) > (unsigned long)timeoutSec * 1000UL) {
            motor.enable(false);
            currentStop      = -1;
            currentState     = STATE_IDLE;
            g_driverAsleep   = true;
            g_notHomedWarnShown = false;
            DEBUG_PRINT(F("[Power] Idle ")); DEBUG_PRINT(timeoutSec);
            DEBUG_PRINTLN(F("s — driver powered off. Home to resume."));
        }
    }
#endif // ENABLE_HTTP_API

    switch (currentState) {

        case STATE_STARTUP:
            break;

        // ------------------------------------------------------------------
        case STATE_HOMING:
            digitalWrite(PIN_LED, (millis() / 250) % 2);
            if (feedback.updateHoming()) {
                currentStop = 0;
                digitalWrite(PIN_LED, LOW);
                if (g_calibratePending) {
                    // Homed → begin the reference sweep. Drive well past the
                    // largest plausible span toward the far end; the far endstop
                    // trips first (detected in STATE_CALIBRATING).
                    float pitch = manifoldPitchMm(g_calModel);
                    if (pitch <= 0.0f) pitch = MANIFOLD_2_5_GATE_PITCH_MM; // custom bound
                    float boundMm = 10.0f + (float)g_calGateCount * pitch + 40.0f;
                    long boundTarget = (long)(boundMm * stepsPerMM()) * (-HOME_DIRECTION);
                    // Sweep at the gentler homing speed — safer approach to the far
                    // switch. Restored to MAX_SPEED before the return-home move.
                    motor.setMaxSpeed(HOMING_SPEED_STEPS_PER_SEC);
                    motor.moveTo(boundTarget);
                    DEBUG_PRINT(F("[CAL] Homed. Sweeping to far endstop (bound "));
                    Serial.print(boundMm, 0); DEBUG_PRINTLN(F("mm)..."));
                    currentState = STATE_CALIBRATING;
                } else {
                    DEBUG_PRINTLN(F("Homed. Entering IDLE."));
                    currentState = STATE_IDLE;
                }
            }
            break;

        // ------------------------------------------------------------------
        case STATE_CALIBRATING: {
            // Sweeping toward the far endstop (moveTo issued when homing finished).
            // The endstop safety supervisor is disabled for this state so we can
            // detect the trigger ourselves. On trigger: record span, place gates,
            // then return to the home datum.
            static unsigned long calStart = 0;
            static uint8_t farHighStreak = 0;
            if (calStart == 0) { calStart = millis(); farHighStreak = 0; }
            digitalWrite(PIN_LED, (millis() / 120) % 2); // fast blink during sweep

            // Debounce the far switch: only accept the trigger after it reads HIGH
            // for CAL_FAR_CONFIRM_LOOPS consecutive loops. A single spurious NC-open
            // bounce (carriage vibration) would otherwise latch the sweep at a short
            // farPos and mis-place every gate. Loop runs fast, so the confirmation
            // delay is negligible against the sweep travel.
            static const uint8_t CAL_FAR_CONFIRM_LOOPS = 3;
            if (digitalRead(PIN_ENDSTOP_MAX) == HIGH) {
                if (farHighStreak < 255) farHighStreak++;
            } else {
                farHighStreak = 0;
            }

            if (farHighStreak >= CAL_FAR_CONFIRM_LOOPS) {   // far endstop confirmed
                farHighStreak = 0;
                long farPos = motor.getPosition();
                motor.stop();
                DEBUG_PRINT(F("[CAL] Far endstop at pos=")); DEBUG_PRINTLN(farPos);
                finishCalibrationSweep(farPos);
                g_calibratePending = false;
                calStart = 0;
                digitalWrite(PIN_LED, LOW);
                // Restore normal move speed for the return-home and all later moves.
                motor.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC);
                // Return to the home datum (releases the far switch on the way).
                targetStop = 0;
                issueMove(0);
            } else if (!motor.isMoving() || (millis() - calStart > 45000UL)) {
                // Reached the bound or timed out without the far switch — fault.
                motor.stop();
                DEBUG_PRINTLN(F("[CAL] Far endstop not found — calibration aborted."));
                g_calibratePending = false;
                calStart = 0;
                motor.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC); // restore normal speed
                currentState = STATE_ERROR;
            }
            break;
        }

        // ------------------------------------------------------------------
        case STATE_IDLE: {
            bool enabled = control.isEnabled();
            int requested = control.readRequestedStop();

            if (!enabled) {
                if (currentStop != 0 && currentStop != -1) {
                    targetStop = 0;
                    issueMove(0);
                }
                break;
            }

            // Not homed yet — require explicit 'home' command before accepting moves
            if (currentStop == -1) {
                if (!g_notHomedWarnShown) {
                    DEBUG_PRINTLN(F("Not homed. Type 'home' before issuing position commands."));
                    g_notHomedWarnShown = true;
                }
                break;
            }

            if (requested != currentStop && requested >= 0) {
                targetStop = requested;
                issueMove(targetStop);
            }
            break;
        }

        // ------------------------------------------------------------------
        case STATE_MOVING:
            if (!control.isEnabled()) {
                motor.stop();
                currentStop = -1;
                currentState = STATE_IDLE;
                DEBUG_PRINTLN(F("Disabled mid-move — stopped."));
                break;
            }

            // Far/home over-travel is enforced directionally by the always-on
            // endstop supervisor at the top of loop() (stops travel *into* a
            // triggered switch, allows travel away). updateMoving() just reports
            // arrival — which is also true once the supervisor has stopped us.
            if (feedback.updateMoving(targetStop)) {
                currentStop = targetStop;
                DEBUG_PRINT(F("Arrived at stop "));
                DEBUG_PRINTLN(currentStop);
                currentState = (currentStop == 0) ? STATE_IDLE : STATE_AT_STOP;
            }
            break;

        // ------------------------------------------------------------------
        case STATE_AT_STOP: {
            bool enabled = control.isEnabled();
            int requested = control.readRequestedStop();

            if (!enabled) {
                targetStop = 0;
                issueMove(0);
                break;
            }

            if (requested != currentStop && requested >= 0) {
                targetStop = requested;
                issueMove(targetStop);
            }
            break;
        }

        // ------------------------------------------------------------------
        case STATE_DISABLED:
            break;

        // ------------------------------------------------------------------
        case STATE_ERROR:
            motor.stop();
            motor.enable(false);
            digitalWrite(PIN_LED, (millis() / 100) % 2); // rapid blink

            // Physical recovery: toggle switch cycled (no hardware e-stop to release)
#ifndef CONTROL_SERIAL_DEBUG
            if (!g_eStopTriggered &&
                control.isEnabled()) {
                motor.enable(true);
                currentState = STATE_HOMING;
                startHoming();
                DEBUG_PRINTLN(F("E-stop cleared. Re-homing..."));
            }
#endif
            break;
    }

#ifdef ENABLE_HTTP_API
    {
        const char* stateStr = "UNKNOWN";
        switch (currentState) {
            case STATE_STARTUP:    stateStr = "STARTUP";    break;
            case STATE_HOMING:     stateStr = "HOMING";     break;
            case STATE_IDLE:       stateStr = "IDLE";       break;
            case STATE_MOVING:     stateStr = "MOVING";     break;
            case STATE_AT_STOP:    stateStr = "AT_STOP";    break;
            case STATE_CALIBRATING: stateStr = "CALIBRATING"; break;
            case STATE_DISABLED:   stateStr = "DISABLED";   break;

            case STATE_ERROR:      stateStr = "ERROR";      break;
        }
        ApiStatus s;
        s.stateName      = stateStr;
        s.currentStop    = currentStop;
        s.targetStop     = targetStop;
        s.positionSteps  = motor.getPosition();
        // Same sign convention as consumeSetStopRequest's currentMM below:
        // HOME_DIRECTION inverts the step sign so positive mm is away from home.
        s.positionMM     = (float)s.positionSteps / stepsPerMM() / (-HOME_DIRECTION);
        s.homed          = (currentStop != -1);
        s.enabled        = control.isEnabled();
        s.endstopHome    = (digitalRead(PIN_ENDSTOP_HOME) == HIGH); // HIGH = triggered (NC switch open)
        s.endstopMax     = (digitalRead(PIN_ENDSTOP_MAX)  == HIGH); // HIGH = triggered (NC switch open)
        s.numActiveStops = g_numActiveStops;
        s.measuredStepsPerMM = g_measuredStepsPerMM;
        s.measuredSpanSteps  = g_measuredSpanSteps;
        s.manifoldModel      = g_manifoldModel;
#ifdef CONTROL_SMART_OUTLET
        apiServer.update(s, &control);
#else
        apiServer.update(s);
#endif
    }
#endif // ENABLE_HTTP_API
}

// =============================================================================
// startHoming() — apply live-tuned speed/SGTHRS if set, otherwise use config.h
// defaults.  stallThreshold() / homeSpeed() return -1 when not overridden.
// =============================================================================
void startHoming() {
    g_lastActivityMs = millis();
    g_driverAsleep   = false;
    // Guarantee normal move speed as a baseline. The calibration sweep lowers
    // maxSpeed to homing speed and restores it on its own exits — but a global
    // e-stop during the sweep goes straight to STATE_ERROR without restoring it,
    // so a plain re-home here would otherwise leave every later move crawling.
    motor.setMaxSpeed(MAX_SPEED_STEPS_PER_SEC);
    feedback.resetHoming(); // clear _homed so updateHoming() actually runs
#ifdef MOTOR_STEPPER_TMC2209
    int   sg    = control.stallThreshold();
    float speed = control.homeSpeed();
    float appliedSpeed = (speed  < 0) ? HOMING_SPEED_STEPS_PER_SEC : speed;
    int   appliedSG    = (sg     < 0) ? TMC2209_STALL_THRESHOLD     : sg;
    DEBUG_PRINT(F("[HOME] speed=")); Serial.print(appliedSpeed, 0);
    DEBUG_PRINT(F(" steps/sec  SGTHRS=")); Serial.println(appliedSG);
    motor.startHomingWithParams(appliedSpeed, (uint8_t)appliedSG);
#else
    motor.startHoming();
#endif
}

// =============================================================================
// issueMove() — command motor to a stop position
// =============================================================================
void issueMove(int stop) {
    g_lastActivityMs = millis();
    DEBUG_PRINT(F("Moving to stop "));
    DEBUG_PRINTLN(stop);
    motor.moveTo(feedback.stepsForStop(stop));
    currentState = STATE_MOVING;
}
