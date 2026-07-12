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

// Runtime gate count — set by setup wizard via set_num_gates API, stored in NVS.
// MUST remain <= NUM_STOPS; array bounds are determined at compile time.
int g_numActiveStops = 0;   // 0 = unconfigured

// Homing direction — loaded from NVS so the user can flip it via setup wizard without
// recompiling.  All existing code references HOME_DIRECTION which now expands to this.
int g_homeDirection = HOME_DIRECTION_DEFAULT;

bool g_notHomedWarnShown = false; // suppress repeated "not homed" warnings

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
        DEBUG_PRINTLN(F("Loaded calibration from EEPROM."));
        CalibrationStore::print(cal);
    } else {
        // No calibration yet — zero all positions. Setup wizard will call
        // save_stop for each gate to populate them via the HTTP API.
        memset(g_stopPositionsMM, 0, sizeof(g_stopPositionsMM));
        DEBUG_PRINTLN(F("No calibration data — awaiting setup wizard."));
    }
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
// Volatile flag set by ISR; checked at the top of every loop() iteration.
// ISR also directly de-energizes the TMC2209 EN pin for an immediate hardware
// fast-path (no software latency). IRAM_ATTR places ISR in IRAM on ESP32 for
// reliable execution regardless of flash cache state.
// =============================================================================
volatile bool g_eStopTriggered = false;
bool          g_hardwareFault  = false; // set when begin() fails — not clearable without reset

void IRAM_ATTR eStopISR() {
    // Fast-path: cut motor enable immediately.
    // State machine transition uses debounced polling in loop() to avoid false triggers.
    digitalWrite(PIN_TMC_EN, HIGH);
}

// =============================================================================
// State machine
// =============================================================================
enum State {
    STATE_STARTUP,
    STATE_HOMING,
    STATE_IDLE,
    STATE_MOVING,
    STATE_AT_STOP,
    STATE_DISABLED,
    STATE_ERROR
};

State currentState = STATE_STARTUP;
int   currentStop  = -1;
int   targetStop   = 0;

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

    // E-stop: hardware interrupt disabled — use 'stop' serial command instead.
    // Re-enable by uncommenting the two lines below once switch wiring is verified.
    // pinMode(PIN_ESTOP, INPUT_PULLUP);
    // attachInterrupt(digitalPinToInterrupt(PIN_ESTOP), eStopISR, RISING);

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

    motor.update();

    // -- Hardware e-stop: highest priority ------------------------------------
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
        } else if (digitalRead(PIN_ESTOP) == LOW) {
            DEBUG_PRINTLN(F("[ESTOP] Cleared. Re-homing..."));
            g_eStopTriggered = false;
            g_notHomedWarnShown = false;
            motor.enable(true);
            currentState = STATE_HOMING;
            startHoming();
        } else {
            DEBUG_PRINTLN(F("[ESTOP] Hardware button still active — release it first."));
        }
    }

    if (_SC.consumeGconfRequest()) {
        motor.printDriverRegs();
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
        if (serialStop >= 0 && serialStop != _scLastActioned &&
            currentState == STATE_IDLE && !g_eStopTriggered) {
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
            if (moveStop >= 0 && moveStop <= g_numActiveStops) {
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
        DEBUG_PRINTLN(F("[API] Calibration cleared. Gate count reset to 0."));
    }

    {
        int stopIdx = -1;
        if (apiServer.consumeSetStopRequest(stopIdx) && currentState == STATE_IDLE) {
            // Convert current motor position (steps) to mm.
            // HOME_DIRECTION inverts the step sign: positive steps are away from home.
            float currentMM = (float)motor.getPosition() / stepsPerMM() / (-HOME_DIRECTION);
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
            }
            cal.stopMM[stopIdx] = currentMM;
            if (stopIdx > (int)cal.numStops) cal.numStops = (uint8_t)stopIdx;
            CalibrationStore::save(cal);

            // Keep runtime count in sync (expand; never shrink during a session)
            if (stopIdx > g_numActiveStops) g_numActiveStops = stopIdx;

            DEBUG_PRINT(F("[API] Stop ")); Serial.print(stopIdx);
            DEBUG_PRINT(F(" saved at "));  Serial.print(currentMM, 2);
            DEBUG_PRINTLN(F(" mm"));
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

    // Enable / disable — TODO: add ControlInput::setEnabled() to the base class
    // so this works for all modes, not just serial debug.

#ifdef CONTROL_SMART_OUTLET
    {
        HttpApiServer::OutletConfigCmd cmd;
        if (apiServer.consumeOutletConfigRequest(cmd)) {
            control.configureOutlet(cmd.slot, cmd.generation, cmd.ip, cmd.name,
                                    cmd.stopIndex, cmd.thresholdW);
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
            control.configureDustCollector(dcCmd.generation, dcCmd.ip);
        }
        if (apiServer.consumeDustCollectorDeleteRequest()) {
            control.removeDustCollector();
        }
        bool dcSwitchOn = false;
        if (apiServer.consumeDustCollectorSwitchRequest(dcSwitchOn)) {
            control.setDcManual(dcSwitchOn);
        }
    }
#endif // CONTROL_SMART_OUTLET
#endif // ENABLE_HTTP_API

    switch (currentState) {

        case STATE_STARTUP:
            break;

        // ------------------------------------------------------------------
        case STATE_HOMING:
            digitalWrite(PIN_LED, (millis() / 250) % 2);
            if (feedback.updateHoming()) {
                currentStop = 0;
                DEBUG_PRINTLN(F("Homed. Entering IDLE."));
                currentState = STATE_IDLE;
                digitalWrite(PIN_LED, LOW);
            }
            break;

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

            if (feedback.updateMoving(targetStop)) {
                currentStop = targetStop;
                DEBUG_PRINT(F("Arrived at stop "));
                DEBUG_PRINTLN(currentStop);
                currentState = (currentStop == 0) ? STATE_IDLE : STATE_AT_STOP;
            }

            if (feedback.isMaxTriggered()) {
                motor.stop();
                DEBUG_PRINTLN(F("MAX endstop hit — emergency stop."));
                currentState = STATE_ERROR;
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

            // Physical recovery: e-stop released + toggle switch cycled
#ifndef CONTROL_SERIAL_DEBUG
            if (digitalRead(PIN_ESTOP) == LOW &&
                !g_eStopTriggered &&
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
        s.numActiveStops = g_numActiveStops;
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
    DEBUG_PRINT(F("Moving to stop "));
    DEBUG_PRINTLN(stop);
    motor.moveTo(feedback.stepsForStop(stop));
    currentState = STATE_MOVING;
}
