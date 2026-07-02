// =============================================================================
// AutoTuner.cpp — StallGuard threshold binary-search auto-tuner
// =============================================================================

#include "AutoTuner.h"

#ifdef MOTOR_STEPPER_TMC2209

#include "../utils/MotionMath.h"

AutoTuner::AutoTuner(StepperTMC2209Driver* motor)
    : _motor(motor),
      _phase(PHASE_IDLE),
      _phaseMs(0),
      _testSpeed(0),
      _lo(1), _hi(254), _mid(0),
      _bestSGTHRS(-1),
      _attemptCount(0),
      _startPos(0),
      _maxTravelSteps(0),
      _falseStallSteps(0)
{}

void AutoTuner::begin(float testSpeedStepsPerSec) {
    _testSpeed     = testSpeedStepsPerSec;
    _lo            = 1;
    _hi            = 254;
    _bestSGTHRS    = -1;
    _attemptCount  = 0;

    // Convert mm → steps using project geometry
    float spm             = stepsPerMM();
    _maxTravelSteps       = (long)(AUTOTUNE_SEARCH_MM * spm);
    _falseStallSteps      = (long)(_maxTravelSteps * AUTOTUNE_FALSE_STALL_PCT / 100.0f);

    Serial.println(F(""));
    Serial.println(F("=== StallGuard Auto-Tuner ==="));
    Serial.print(F("  Speed: ")); Serial.print(_testSpeed, 0); Serial.println(F(" steps/sec"));
    Serial.print(F("  Search window: ")); Serial.print(AUTOTUNE_SEARCH_MM, 1); Serial.println(F(" mm"));
    Serial.print(F("  False-stall threshold: ")); Serial.print(AUTOTUNE_FALSE_STALL_PCT); Serial.println(F("% of window"));
    Serial.print(F("  Max travel steps: ")); Serial.println(_maxTravelSteps);
    Serial.print(F("  False-stall steps: ")); Serial.println(_falseStallSteps);

    // Physical endstop position in AccelStepper coordinates (after homing):
    // setHome() zeroed the position HOME_BACKOFF_STEPS away from the endstop,
    // so the endstop is at +HOME_BACKOFF_STEPS * HOME_DIRECTION from position 0.
    long endstopPos = (long)(HOME_BACKOFF_STEPS) * HOME_DIRECTION;

    // Start position: AUTOTUNE_SEARCH_MM away from the PHYSICAL endstop (not from home).
    // Without this anchor, the drive window undershoots the endstop by the backoff distance.
    long searchSteps = (long)(AUTOTUNE_SEARCH_MM * spm);
    long setupPos = endstopPos + searchSteps * (-HOME_DIRECTION);

    // Max travel = retract distance + overshoot so approach always exceeds retract.
    // Ensures the motor contacts the endstop even if backlash causes a short-stop.
    // Travel limit fires only if no stall occurs within this window.
    _maxTravelSteps = searchSteps + (long)(AUTOTUNE_OVERSHOOT_MM * spm);

    Serial.print(F("  Endstop pos (steps): ")); Serial.println(endstopPos);
    Serial.print(F("  Moving to start position (step ")); Serial.print(setupPos); Serial.println(F(")..."));
    Serial.println(F("  Binary search SGTHRS 1–254. Please wait..."));
    Serial.println(F(""));
    _motor->moveTo(setupPos);
    enterPhase(PHASE_SETUP_MOVE);
}

void AutoTuner::update() {
    switch (_phase) {

        // ------------------------------------------------------------------
        case PHASE_IDLE:
        case PHASE_COMPLETE:
        case PHASE_ERROR:
            break;

        // ------------------------------------------------------------------
        case PHASE_SETUP_MOVE:
            // Wait for initial positioning move to complete, then begin first attempt
            if (!_motor->isMoving()) {
                startAttempt();
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_SETTLE:
            // Motor is already running. Wait for it to reach full speed before
            // checking stalls — prevents false triggers at low speed / standstill.
            if (millis() - _phaseMs >= AUTOTUNE_SETTLE_MS) {
                enterPhase(PHASE_ATTEMPT);
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_ATTEMPT: {
            // Check for stall or travel limit
            long travelSteps = abs(_motor->getPosition() - _startPos);
            bool stalled     = _motor->isStalled();

            if (stalled || travelSteps >= _maxTravelSteps) {
                _motor->stop();
                evaluateAttempt(travelSteps, stalled);
            }
            break;
        }

        // ------------------------------------------------------------------
        case PHASE_BACK_OFF:
            // Drive back to start position
            if (!_motor->isMoving()) {
                enterPhase(PHASE_BACK_OFF_PAUSE);
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_BACK_OFF_PAUSE:
            if (millis() - _phaseMs >= AUTOTUNE_BACK_OFF_PAUSE_MS) {
                if (_lo > _hi) {
                    // Binary search complete
                    if (_bestSGTHRS < 0) {
                        Serial.println(F("[AUTOTUNE] ERROR: no reliable stall found across full range."));
                        Serial.println(F("[AUTOTUNE] Try increasing motor current or homing speed."));
                        enterPhase(PHASE_ERROR);
                    } else {
                        printResults();
                        enterPhase(PHASE_COMPLETE);
                    }
                } else {
                    startAttempt(); // start motor then settle, not settle then start
                }
            }
            break;
    }
}

// -----------------------------------------------------------------------------
void AutoTuner::startAttempt() {
    // Capture position here — after any backoff move — so travel is measured correctly
    _startPos = _motor->getPosition();

    _mid = (_lo + _hi) / 2;
    _attemptCount++;

    Serial.print(F("[AUTOTUNE] #")); Serial.print(_attemptCount);
    Serial.print(F("  SGTHRS=")); Serial.print(_mid);
    Serial.print(F("  [lo=")); Serial.print(_lo);
    Serial.print(F(" hi=")); Serial.print(_hi);
    Serial.println(F("]  ..."));

    // Start motor toward endstop, then enter PHASE_SETTLE so it reaches speed
    // before stall detection begins. Starting detection immediately (old behavior)
    // caused false stalls at step 0 because DIAG fires at standstill.
    _motor->startHomingWithParams(_testSpeed, (uint8_t)_mid);
    enterPhase(PHASE_SETTLE);
}

// -----------------------------------------------------------------------------
void AutoTuner::evaluateAttempt(long stepsTraveled, bool stalled) {
    float travelMM = (float)stepsTraveled / stepsPerMM();

    if (!stalled) {
        // Traveled full distance without stalling — SGTHRS too low
        Serial.print(F("  → NO STALL after ")); Serial.print(travelMM, 1); Serial.println(F("mm  → raise SGTHRS  (lo = mid+1)"));
        _lo = _mid + 1;

    } else if (stepsTraveled <= _falseStallSteps) {
        // Stalled too early — false stall, SGTHRS too high
        Serial.print(F("  → FALSE STALL at ")); Serial.print(travelMM, 1); Serial.println(F("mm  → lower SGTHRS  (hi = mid-1)"));
        _hi = _mid - 1;

    } else {
        // Good stall — real endstop detection
        Serial.print(F("  → REAL STALL at ")); Serial.print(travelMM, 1); Serial.print(F("mm ✓  → recording best, trying lower"));
        _bestSGTHRS = _mid;
        _hi = _mid - 1;
        Serial.println(F("  (hi = mid-1)"));
    }

    // Return to start position for next attempt
    _motor->moveTo(_startPos);
    enterPhase(PHASE_BACK_OFF);
}

// -----------------------------------------------------------------------------
int AutoTuner::recommendedSGTHRS() const {
    if (_bestSGTHRS < 0) return -1;
    int rec = _bestSGTHRS + AUTOTUNE_MARGIN;
    return (rec > 255) ? 255 : rec;
}

// -----------------------------------------------------------------------------
void AutoTuner::printResults() const {
    int rec = recommendedSGTHRS();
    Serial.println(F(""));
    Serial.println(F("=== Auto-Tune Complete ==="));
    Serial.print(F("  Attempts:          ")); Serial.println(_attemptCount);
    Serial.print(F("  Min reliable SGTHRS: ")); Serial.println(_bestSGTHRS);
    Serial.print(F("  Safety margin:     +")); Serial.println(AUTOTUNE_MARGIN);
    Serial.print(F("  Recommended SGTHRS: ")); Serial.println(rec);
    Serial.print(F("  Test speed:        ")); Serial.print(_testSpeed, 0); Serial.println(F(" steps/sec"));
    Serial.println(F(""));
    Serial.println(F("  Copy these lines into config.h:"));
    Serial.println(F("  ┌─────────────────────────────────────────────────────┐"));
    Serial.print(F("  │ #define HOMING_SPEED_STEPS_PER_SEC   ")); Serial.print(_testSpeed, 0); Serial.println(F("f"));
    Serial.print(F("  │ #define TMC2209_STALL_THRESHOLD       ")); Serial.println(rec);
    Serial.println(F("  └─────────────────────────────────────────────────────┘"));
    Serial.println(F(""));
}

// -----------------------------------------------------------------------------
void AutoTuner::enterPhase(Phase p) {
    _phase   = p;
    _phaseMs = millis();
}

#endif // MOTOR_STEPPER_TMC2209
