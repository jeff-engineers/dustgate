// =============================================================================
// TrainingMode.cpp
// =============================================================================

#include "TrainingMode.h"
#include "../utils/MotionMath.h"

#if defined(MOTOR_STEPPER_TMC2209)

// All training constants are in config.h — adjust there if needed.
#define STALL_SETTLE_MS   TRAINING_STALL_SETTLE_MS
#define VERIFY_PAUSE_MS   TRAINING_VERIFY_PAUSE_MS
#define VERIFY_TOLERANCE  TRAINING_VERIFY_TOLERANCE
#define TRAIN_SPEED       TRAINING_SPEED_STEPS_PER_SEC
#define TRAIN_HOME_BACKOFF TRAINING_HOME_BACKOFF_STEPS

// -----------------------------------------------------------------------------
TrainingMode::TrainingMode(StepperTMC2209Driver* motor)
    : _motor(motor), _phase(PHASE_IDLE), _currentStop(0),
      _numStopsToTrain(NUM_STOPS),
      _stallSettleMs(0), _phaseEnteredMs(0), _waitingForEnter(false)
{
    memset(_stallSteps, 0, sizeof(_stallSteps));
    memset(_verifyPass, 0, sizeof(_verifyPass));
}

// -----------------------------------------------------------------------------
void TrainingMode::begin() {
    Serial.println(F(""));
    Serial.println(F("╔══════════════════════════════════════╗"));
    Serial.println(F("║     SENSORLESS TRAINING MODE         ║"));
    Serial.println(F("╠══════════════════════════════════════╣"));
    Serial.println(F("║ Press Enter at each prompt to        ║"));
    Serial.println(F("║ advance. Type 'q' + Enter to abort.  ║"));
    Serial.println(F("╚══════════════════════════════════════╝"));
    Serial.println(F(""));
    Serial.print(F("[TRAIN] How many stops to train? (1–"));
    Serial.print(NUM_STOPS);
    Serial.println(F(", then Enter):"));
    enterPhase(PHASE_COUNT_PROMPT);
}

// -----------------------------------------------------------------------------
void TrainingMode::update() {
    if (_phase == PHASE_COMPLETE || _phase == PHASE_ERROR) return;

    _motor->update();

    // Check for abort
    while (Serial.available()) {
        char c = (char)Serial.peek();
        if (c == 'q' || c == 'Q') {
            Serial.println(F("[TRAIN] Aborted."));
            _motor->stop();
            enterPhase(PHASE_ERROR);
            return;
        }
        break;
    }

    switch (_phase) {

        // ------------------------------------------------------------------
        case PHASE_COUNT_PROMPT:
            while (Serial.available()) {
                char c = (char)Serial.read();
                if (c == '\n' || c == '\r') {
                    int n = _inputBuffer.toInt();
                    _inputBuffer = "";
                    if (n >= 1 && n <= NUM_STOPS) {
                        _numStopsToTrain = (uint8_t)n;
                        Serial.print(F("[TRAIN] Training "));
                        Serial.print(_numStopsToTrain);
                        Serial.println(F(" stop(s)."));
                        Serial.println(F("[TRAIN] Driving to home endstop..."));
                        _motor->startHoming();
                        _stallSettleMs = millis();
                        enterPhase(PHASE_HOMING);
                    } else {
                        Serial.print(F("[TRAIN] Enter a number between 1 and "));
                        Serial.println(NUM_STOPS);
                    }
                } else if (c != '\r') {
                    _inputBuffer += c;
                }
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_HOMING:
            if (millis() - _stallSettleMs < STALL_SETTLE_MS) break;

            if (checkStall()) {
                _motor->stop();
                // Back off so we're clear of the physical endstop
                _motor->moveTo(TRAIN_HOME_BACKOFF * (-HOME_DIRECTION));
                while (_motor->isMoving()) _motor->update();
                _motor->setHome(); // zero position after backoff

                Serial.println(F("[TRAIN] Home endstop found."));
                Serial.println(F(""));
                Serial.print(F("[TRAIN] Engage detent for STOP 0, then press Enter..."));
                _waitingForEnter = true;
                _currentStop = 0;
                enterPhase(PHASE_HOMED_WAIT);
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_HOMED_WAIT:
            if (!_waitingForEnter) break;
            if (pollEnter()) {
                _waitingForEnter = false;
                Serial.println(F("[TRAIN] Driving to stop..."));
                startDriving(TRAIN_SPEED); // forward
                _stallSettleMs = millis();
                enterPhase(PHASE_SEEK_STOP);
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_SEEK_STOP:
            if (millis() - _stallSettleMs < STALL_SETTLE_MS) break;

            if (checkStall()) {
                _motor->stop();
                long pos = _motor->getPosition();
                _stallSteps[_currentStop + 1] = pos; // +1 because index 0 = home ref

                Serial.print(F("[TRAIN] Stop "));
                Serial.print(_currentStop);
                Serial.print(F(" at "));
                Serial.print(pos);
                Serial.print(F(" steps  ("));
                Serial.print(stepsToMM(pos), 2);
                Serial.println(F(" mm)"));

                if (_currentStop < (int)_numStopsToTrain - 1) {
                    Serial.println(F(""));
                    Serial.print(F("[TRAIN] Release detent, engage for STOP "));
                    Serial.print(_currentStop + 1);
                    Serial.print(F(", then press Enter..."));
                    _waitingForEnter = true;
                    enterPhase(PHASE_STOP_FOUND_WAIT);
                } else {
                    // All stops captured — now seek the far endstop
                    Serial.println(F(""));
                    Serial.println(F("[TRAIN] All stops captured."));
                    Serial.print(F("[TRAIN] Release the final detent, then press Enter to drive to far endstop..."));
                    _waitingForEnter = true;
                    enterPhase(PHASE_STOP_FOUND_WAIT);
                }
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_STOP_FOUND_WAIT:
            if (!_waitingForEnter) break;
            if (pollEnter()) {
                _waitingForEnter = false;
                _currentStop++;

                if (_currentStop >= (int)_numStopsToTrain) {
                    // Seek far endstop
                    Serial.println(F("[TRAIN] Driving to far endstop..."));
                    startDriving(TRAIN_SPEED);
                    _stallSettleMs = millis();
                    enterPhase(PHASE_SEEK_MAX);
                } else {
                    Serial.println(F("[TRAIN] Driving to next stop..."));
                    startDriving(TRAIN_SPEED);
                    _stallSettleMs = millis();
                    enterPhase(PHASE_SEEK_STOP);
                }
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_SEEK_MAX:
            if (millis() - _stallSettleMs < STALL_SETTLE_MS) break;

            if (checkStall()) {
                _motor->stop();
                long maxPos = _motor->getPosition();
                _stallSteps[_numStopsToTrain + 1] = maxPos;

                Serial.print(F("[TRAIN] Far endstop at "));
                Serial.print(maxPos);
                Serial.print(F(" steps  ("));
                Serial.print(stepsToMM(maxPos), 2);
                Serial.println(F(" mm)"));
                enterPhase(PHASE_MAX_FOUND);
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_MAX_FOUND:
            computeCalibration();
            CalibrationStore::print(_cal);
            CalibrationStore::printConfigSnippet(_cal);
            CalibrationStore::save(_cal);

            Serial.println(F(""));
            Serial.println(F("[TRAIN] Starting verify pass — driving back through all stops."));
            Serial.println(F("[TRAIN] Press Enter to begin verify..."));
            _waitingForEnter = true;
            _currentStop = 0;
            enterPhase(PHASE_VERIFY_INIT);
            break;

        // ------------------------------------------------------------------
        case PHASE_VERIFY_INIT:
            if (pollEnter()) {
                // Start verify from stop 0: drive forward, stall should fire
                // at the first recorded position.
                _motor->driveForwardWithStallGuard(TRAIN_SPEED);
                _stallSettleMs = millis();
                enterPhase(PHASE_VERIFY_SEEK);
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_VERIFY_SEEK: {
            if (millis() - _stallSettleMs < STALL_SETTLE_MS) break;

            long expectedSteps = _stallSteps[_currentStop + 1];
            long tolerance     = (long)(expectedSteps * VERIFY_TOLERANCE);

            if (checkStall()) {
                long actual = _motor->getPosition();
                long delta  = abs(actual - expectedSteps);
                _verifyPass[_currentStop] = (delta <= tolerance);

                Serial.print(F("[VERIFY] Stop "));
                Serial.print(_currentStop);
                Serial.print(F(": expected "));
                Serial.print(expectedSteps);
                Serial.print(F(" steps, got "));
                Serial.print(actual);
                Serial.print(F("  (Δ="));
                Serial.print(delta);
                Serial.print(F(")  "));
                Serial.println(_verifyPass[_currentStop] ? F("PASS ✓") : F("FAIL ✗"));

                _motor->stop();
                _phaseEnteredMs = millis();
                enterPhase(PHASE_VERIFY_WAIT);
            }

            // Safety: if we've traveled well past expected without stall, fail this stop
            if (_motor->getPosition() > expectedSteps + (tolerance * 3)) {
                Serial.print(F("[VERIFY] Stop "));
                Serial.print(_currentStop);
                Serial.println(F(": no stall detected — FAIL ✗"));
                _verifyPass[_currentStop] = false;
                _motor->stop();
                _phaseEnteredMs = millis();
                enterPhase(PHASE_VERIFY_WAIT);
            }
            break;
        }

        // ------------------------------------------------------------------
        case PHASE_VERIFY_WAIT:
            if (millis() - _phaseEnteredMs < VERIFY_PAUSE_MS) break;

            _currentStop++;
            if (_currentStop >= (int)_cal.numStops) {
                // Verify complete
                printVerifyResults();
                enterPhase(PHASE_COMPLETE);
            } else {
                // Drive to next stop — back off current stall point slightly first
                _motor->moveTo(_motor->getPosition() - 10);
                while (_motor->isMoving()) _motor->update();
                // Then run to next position
                startDriving(TRAIN_SPEED);
                _stallSettleMs = millis();
                enterPhase(PHASE_VERIFY_SEEK);
            }
            break;

        // ------------------------------------------------------------------
        case PHASE_COMPLETE:
        case PHASE_IDLE:
        case PHASE_ERROR:
            break;
    }
}

// -----------------------------------------------------------------------------
void TrainingMode::enterPhase(Phase p) {
    _phase = p;
    _phaseEnteredMs = millis();
}

void TrainingMode::startDriving(long /*speed*/) {
    // Drive forward with StallGuard active.
    // driveForwardWithStallGuard() enables SpreadCycle + StallGuard and uses
    // runSpeed() — motor runs until stop() is called (by checkStall() above).
    _motor->driveForwardWithStallGuard(TRAIN_SPEED);
}

bool TrainingMode::checkStall() {
    return _motor->isStalled();
}

bool TrainingMode::pollEnter() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            _inputBuffer = "";
            Serial.println(); // newline after user input
            return true;
        }
        if (c == 'q' || c == 'Q') {
            Serial.println(F("\n[TRAIN] Aborted by user."));
            _motor->stop();
            enterPhase(PHASE_ERROR);
            return false;
        }
        _inputBuffer += c;
    }
    return false;
}

void TrainingMode::computeCalibration() {
    _cal.magic    = CALIB_MAGIC;
    _cal.version  = CALIB_VERSION;
    _cal.numStops = _numStopsToTrain;

    _cal.stopMM[0] = 0.0f;
    for (int i = 1; i <= (int)_numStopsToTrain; i++) {
        _cal.stopMM[i] = stepsToMM(_stallSteps[i]);
    }

    long maxSteps = _stallSteps[_numStopsToTrain + 1];
    _cal.maxTravelMM = stepsToMM(maxSteps);

    // measuredStepsPerMM equals the theoretical value (same gear math).
    // Once the user measures the physical travel with calipers and updates
    // RACK_PITCH_MM / PINION_TEETH in config.h, retraining will produce
    // accurate mm values automatically.
    _cal.measuredStepsPerMM = stepsPerMM();
}

void TrainingMode::printVerifyResults() {
    Serial.println(F(""));
    Serial.println(F("=== Verify Results ==="));
    int passed = 0;
    for (int i = 0; i < (int)_cal.numStops; i++) {
        Serial.print(F("  Stop "));
        Serial.print(i);
        Serial.print(F(": "));
        Serial.println(_verifyPass[i] ? F("PASS ✓") : F("FAIL ✗"));
        if (_verifyPass[i]) passed++;
    }
    Serial.print(F("  Result: "));
    Serial.print(passed);
    Serial.print(F("/"));
    Serial.print((int)_cal.numStops);
    Serial.println(passed == (int)_cal.numStops ? F(" — ALL PASS") : F(" — SOME FAILED"));
    Serial.println(F("======================"));
    Serial.println(F("[TRAIN] Training complete. Returning to normal operation."));
    Serial.println(F(""));
}

#endif // MOTOR_STEPPER_TMC2209
