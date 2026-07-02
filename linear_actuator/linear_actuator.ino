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
#include "output/RelayOutput.h"
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

// Runtime gate count (may differ from compile-time NUM_STOPS when using CONTROL_WIFI).
// MUST remain <= NUM_STOPS; array bounds are determined at compile time.
int g_numActiveStops = NUM_STOPS;

// Last autotune recommendation — set when STATE_AUTOTUNING completes.
int   g_lastTuneSGTHRS = -1;
float g_lastTuneSpeed  = -1.0f;
bool  g_autotuneDone   = false;
bool  g_notHomedWarnShown = false; // suppress repeated "not homed" warnings

// Compute stop positions analytically from known gear geometry.
// Called when FEEDBACK_SENSORLESS is active and no EEPROM data is present.
// Positions are relative to code home (position 0), which is set after homing
// backoff: the motor stalls at the physical endstop, then reverses HOME_BACKOFF_STEPS.
//
// gate_1_mm = (measured steps from endstop to gate 1 − backoff steps) / stepsPerMM()
//           = (ENDSTOP_MARGIN_STEPS − HOME_BACKOFF_STEPS) / stepsPerMM()
// gate_N_mm = gate_1_mm + (N−1) × STOP_SPACING_TEETH × RACK_PITCH_MM
//
// ENDSTOP_MARGIN_STEPS is measured empirically (see config.h).
// Tune HOME_BACKOFF_STEPS if gates feel slightly off — each step ≈ 0.019mm.
void computeStopPositions() {
    float backoffMM   = (float)HOME_BACKOFF_STEPS / stepsPerMM();
    float gate1MM     = (float)(ENDSTOP_MARGIN_STEPS - HOME_BACKOFF_STEPS) / stepsPerMM();
    float spacingMM   = (float)STOP_SPACING_TEETH   * RACK_PITCH_MM;

    g_stopPositionsMM[0] = 0.0f; // home/parked (code origin, just past endstop)
    for (int i = 1; i <= NUM_STOPS; i++) {
        g_stopPositionsMM[i] = gate1MM + (float)(i - 1) * spacingMM;
    }

    DEBUG_PRINTLN(F("Computed stop positions from gear geometry:"));
    DEBUG_PRINT(F("  Backoff      = ")); Serial.print(backoffMM, 3); DEBUG_PRINTLN(F(" mm"));
    DEBUG_PRINT(F("  Gate 1       = ")); Serial.print(gate1MM,   2); DEBUG_PRINTLN(F(" mm from home"));
    DEBUG_PRINT(F("  Gate spacing = ")); Serial.print(spacingMM, 2); DEBUG_PRINTLN(F(" mm"));
    DEBUG_PRINT(F("  Gate 7       = ")); Serial.print(g_stopPositionsMM[NUM_STOPS], 2); DEBUG_PRINTLN(F(" mm"));
}

void loadCalibration() {
    CalibrationData cal;
    if (CalibrationStore::load(cal)) {
        // EEPROM training data always takes priority
        for (int i = 0; i <= NUM_STOPS; i++) {
            g_stopPositionsMM[i] = (i <= (int)cal.numStops) ? cal.stopMM[i]
                                                             : cal.stopMM[cal.numStops];
        }
        DEBUG_PRINTLN(F("Loaded calibration from EEPROM."));
        CalibrationStore::print(cal);
    } else {
        // No EEPROM data — derive positions from gear geometry.
        // Works for both sensorless and limit-switch modes: the math only
        // depends on the physical rack/pinion dimensions, not how home is detected.
        computeStopPositions();
    }
}

// -- Motor driver (TMC2209) --
#include "motor/StepperTMC2209Driver.h"
StepperTMC2209Driver motor;

// -- Feedback system --
#ifdef FEEDBACK_SENSORLESS
  #include "feedback/SensorlessHoming.h"
  SensorlessHoming feedback;
#elif defined(FEEDBACK_LIMIT_DISTANCE)
  #include "feedback/LimitSwitchDistance.h"
  LimitSwitchDistance feedback;
#elif defined(FEEDBACK_LIMIT_DETENT)
  #include "feedback/LimitSwitchDetent.h"
  LimitSwitchDetent feedback;
#else
  #error "No feedback type defined in config.h"
#endif

// -- Control input --
#ifdef CONTROL_ROTARY
  #include "control/RotaryControl.h"
  RotaryControl control;
#elif defined(CONTROL_SMART_OUTLET)
  #include "control/SmartOutletControl.h"
  SmartOutletControl control;
#elif defined(CONTROL_APP)
  #include "control/AppControl.h"
  AppControl control;
#elif defined(CONTROL_SERIAL_DEBUG)
  #include "control/SerialDebugControl.h"
  SerialDebugControl control;
#elif defined(CONTROL_WIFI)
  #include "control/WiFiControl.h"
  WiFiControl control;
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

// -- Relay output --
RelayOutput relay;

// -- Training mode --
#include "training/TrainingMode.h"
TrainingMode* trainer = nullptr;

// -- StallGuard auto-tuner --
#ifdef MOTOR_STEPPER_TMC2209
  #include "training/AutoTuner.h"
  AutoTuner* autotuner = nullptr;
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
    STATE_TRAINING,
    STATE_AUTOTUNING,
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

    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);

    bool ok = true;
    ok &= motor.begin();
    ok &= feedback.begin(&motor);
    ok &= control.begin();
    ok &= relay.begin();
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
        relay.forceOff();
        motor.enable(false);
        if (currentState != STATE_ERROR) {
            currentState = STATE_ERROR;
            DEBUG_PRINTLN(F(""));
            DEBUG_PRINTLN(F("!!! E-STOP ACTIVE — motor disabled, relay off."));
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

    if (_SC.consumeTrainRequest() &&
        currentState != STATE_TRAINING &&
        currentState != STATE_HOMING) {
        if (g_hardwareFault) {
            DEBUG_PRINTLN(F("[ERROR] Hardware fault — fix wiring and reset before training."));
        } else {
            if (trainer) { delete trainer; trainer = nullptr; }
            trainer = new TrainingMode(&motor);
            trainer->begin();
            relay.forceOff();
            currentState = STATE_TRAINING;
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

#ifdef MOTOR_STEPPER_TMC2209
    if (_SC.consumeAutotuneRequest() &&
        currentState != STATE_AUTOTUNING &&
        currentState != STATE_HOMING) {
        if (g_hardwareFault) {
            DEBUG_PRINTLN(F("[ERROR] Hardware fault — fix wiring and reset before autotuning."));
        } else {
            if (autotuner) { delete autotuner; autotuner = nullptr; }
            autotuner = new AutoTuner(&motor);
            float speed = _SC.homeSpeed();
            float tuneSpeed = (speed < 0) ? HOMING_SPEED_STEPS_PER_SEC : speed;
            g_autotuneDone = false;
            autotuner->begin(tuneSpeed);
            relay.forceOff();
            motor.enable(true);
            currentState = STATE_AUTOTUNING;
        }
    }
#endif

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
        DEBUG_PRINTLN(F("[API] Calibration cleared."));
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
    }
#endif // CONTROL_SMART_OUTLET
#endif // ENABLE_HTTP_API

    // -- WiFi control commands ------------------------------------------------
#ifdef CONTROL_WIFI
    if (control.consumeEStop()) {
        if (!g_eStopTriggered) {
            DEBUG_PRINTLN(F("!!! E-STOP (web command)."));
        }
        g_eStopTriggered = true;
    }

    if (control.consumeHomeRequest() && currentState != STATE_HOMING) {
        if (digitalRead(PIN_ESTOP) == LOW) {
            g_eStopTriggered = false;
            motor.enable(true);
            currentState = STATE_HOMING;
            startHoming();
        }
    }

    // Gate count change from setup UI
    {
        int ng = control.pendingGateCount();
        if (ng > 0 && ng <= NUM_STOPS) {
            g_numActiveStops = ng;
            control.clearPendingGateCount();
            DEBUG_PRINT(F("[WiFi] Active gates set to ")); DEBUG_PRINTLN(g_numActiveStops);
        }
    }

#ifdef MOTOR_STEPPER_TMC2209
    if (control.consumeAutotuneRequest() &&
        currentState != STATE_AUTOTUNING &&
        currentState != STATE_HOMING) {
        if (autotuner) { delete autotuner; autotuner = nullptr; }
        autotuner = new AutoTuner(&motor);
        float tuneSpeed = HOMING_SPEED_STEPS_PER_SEC;
        g_autotuneDone = false;
        autotuner->begin(tuneSpeed);
        relay.forceOff();
        motor.enable(true);
        currentState = STATE_AUTOTUNING;
    }
#endif

    if (control.consumeSaveRequest()) {
        control.performSave(g_numActiveStops, g_lastTuneSGTHRS, g_lastTuneSpeed);
        // Recompute positions in case gate count changed
        computeStopPositions();
    }

    if (control.consumeReconfigureRequest()) {
        control.clearConfiguration();
        g_lastTuneSGTHRS = -1;
        g_lastTuneSpeed  = -1.0f;
        g_autotuneDone   = false;
    }

    // Push current status to web UI (cheap struct copy, safe every loop)
    {
        const char* stateStr = "UNKNOWN";
        switch (currentState) {
            case STATE_STARTUP:    stateStr = "STARTUP";    break;
            case STATE_HOMING:     stateStr = "HOMING";     break;
            case STATE_IDLE:       stateStr = "IDLE";       break;
            case STATE_MOVING:     stateStr = "MOVING";     break;
            case STATE_AT_STOP:    stateStr = "AT_STOP";    break;
            case STATE_DISABLED:   stateStr = "DISABLED";   break;
            case STATE_TRAINING:   stateStr = "TRAINING";   break;
            case STATE_AUTOTUNING: stateStr = "AUTOTUNING"; break;
            case STATE_ERROR:      stateStr = "ERROR";      break;
        }
        control.pushStatus(
            stateStr,
            g_numActiveStops,
            currentStop,
            relay.isOn(),
            control.isEnabled(),
            g_eStopTriggered,
            currentState == STATE_AUTOTUNING,
            g_autotuneDone,
            g_lastTuneSGTHRS,
            g_lastTuneSpeed
        );
    }
#endif // CONTROL_WIFI

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
                relay.update(false, false);
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
            } else {
                relay.update(currentStop > 0, enabled);
            }
            break;
        }

        // ------------------------------------------------------------------
        case STATE_MOVING:
            relay.forceOff();

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

            relay.update(true, enabled);

            if (!enabled) {
                relay.update(false, false);
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
        case STATE_TRAINING:
            relay.forceOff();
            if (trainer) {
                trainer->update();
                if (trainer->isDone()) {
                    if (!trainer->hasError()) {
                        const CalibrationData& cal = trainer->getResults();
                        for (int i = 0; i <= (int)cal.numStops; i++) {
                            g_stopPositionsMM[i] = cal.stopMM[i];
                        }
                        DEBUG_PRINTLN(F("Training complete — positions updated."));
                    }
                    delete trainer;
                    trainer = nullptr;
                    currentStop = 0;
                    currentState = STATE_IDLE;
                }
            }
            break;

        // ------------------------------------------------------------------
        case STATE_AUTOTUNING:
            relay.forceOff();
#ifdef MOTOR_STEPPER_TMC2209
            if (autotuner) {
                autotuner->update();
                if (autotuner->isDone() || autotuner->hasError()) {
                    if (autotuner->isDone()) {
                        g_lastTuneSGTHRS = autotuner->recommendedSGTHRS();
                        g_lastTuneSpeed  = autotuner->testedSpeed();
                        g_autotuneDone   = true;
                        DEBUG_PRINT(F("[AUTOTUNE] Recommended SGTHRS="));
                        DEBUG_PRINT(g_lastTuneSGTHRS);
                        DEBUG_PRINT(F("  speed="));
                        Serial.println(g_lastTuneSpeed, 0);
                        DEBUG_PRINTLN(F("[AUTOTUNE] Done. Home to apply new settings."));
                    }
                    delete autotuner;
                    autotuner = nullptr;
                    currentStop = -1; // motor moved — require re-home
                    currentState = STATE_IDLE;
                }
            }
#endif
            break;

        // ------------------------------------------------------------------
        case STATE_DISABLED:
            relay.forceOff();
            break;

        // ------------------------------------------------------------------
        case STATE_ERROR:
            relay.forceOff();
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

    relay.update(
        currentState == STATE_AT_STOP,
        control.isEnabled()
    );

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
            case STATE_TRAINING:   stateStr = "TRAINING";   break;
            case STATE_AUTOTUNING: stateStr = "AUTOTUNING"; break;
            case STATE_ERROR:      stateStr = "ERROR";      break;
        }
        ApiStatus s;
        s.stateName     = stateStr;
        s.currentStop   = currentStop;
        s.targetStop    = targetStop;
        s.positionSteps = motor.getPosition();
        s.homed         = (currentStop != -1);
        s.enabled       = control.isEnabled();
        s.endstopHome   = (digitalRead(PIN_ENDSTOP_HOME) == HIGH); // HIGH = triggered (NC switch open)
#ifdef CONTROL_SMART_OUTLET
        apiServer.update(s, &control);
#else
        apiServer.update(s);
#endif
    }
#endif // ENABLE_HTTP_API
}

// =============================================================================
// startHoming() — apply live-tuned or saved parameters if available
// control.stallThreshold() / control.homeSpeed() return -1 to use config.h
// defaults; both SerialDebugControl and WiFiControl implement these.
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
