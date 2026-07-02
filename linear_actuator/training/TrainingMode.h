// =============================================================================
// TrainingMode.h — Sensorless calibration wizard (TMC2209 + stepper only)
//
// Drives to each hard stop using StallGuard, records step positions,
// converts to mm, saves to EEPROM, and runs a verify pass.
//
// Requires MOTOR_STEPPER_TMC2209 — compile guard enforced below.
//
// Usage (from main sketch):
//   TrainingMode trainer(&motor);
//   trainer.begin();               // call once to start
//   while (!trainer.isDone())
//       trainer.update();          // call each loop()
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

#if defined(MOTOR_STEPPER_TMC2209)

#include "../motor/StepperTMC2209Driver.h"
#include "CalibrationStore.h"

class TrainingMode {
public:
    explicit TrainingMode(StepperTMC2209Driver* motor);

    void begin();
    void update();

    bool isDone()    const { return _phase == PHASE_COMPLETE || _phase == PHASE_ERROR; }
    bool hasError()  const { return _phase == PHASE_ERROR; }

    // Valid after isDone() && !hasError()
    const CalibrationData& getResults() const { return _cal; }

private:
    enum Phase {
        PHASE_IDLE,
        PHASE_COUNT_PROMPT,     // asking user how many stops to train
        PHASE_HOMING,           // driving to home physical endstop
        PHASE_HOMED_WAIT,       // found home, waiting for user to engage stop 0
        PHASE_SEEK_STOP,        // driving forward, waiting for stall at stop N
        PHASE_STOP_FOUND_WAIT,  // stall recorded, waiting for user to release + engage next
        PHASE_SEEK_MAX,         // driving to far endstop
        PHASE_MAX_FOUND,        // all stops recorded, saving + printing
        PHASE_VERIFY_INIT,      // starting verify pass
        PHASE_VERIFY_SEEK,      // driving to a recorded position for verification
        PHASE_VERIFY_WAIT,      // pause between verify steps
        PHASE_COMPLETE,
        PHASE_ERROR
    };

    StepperTMC2209Driver* _motor;
    Phase   _phase;
    int     _currentStop;       // which stop we're currently seeking/verifying
    long    _stallSteps[NUM_STOPS + 2]; // [0]=home ref, [1..N]=stops, [N+1]=max endstop
    bool    _verifyPass[NUM_STOPS + 1]; // verify result per stop

    CalibrationData _cal;
    uint8_t _numStopsToTrain;       // set by user at start of training

    unsigned long _stallSettleMs;   // millis() when homing started (for settle delay)
    unsigned long _phaseEnteredMs;  // millis() when current phase started
    bool _waitingForEnter;          // true when waiting for user to press Enter

    String _inputBuffer;

    // -- Phase transitions --
    void enterPhase(Phase p);
    void startDriving(long speedDir); // positive = forward, negative = reverse

    // -- StallGuard --
    bool checkStall();

    // -- Serial helpers --
    bool pollEnter();           // returns true once user presses Enter
    void prompt(const __FlashStringHelper* msg);
    void println(const __FlashStringHelper* msg);

    // -- Computation --
    void computeCalibration();
    void printVerifyResults();
};

#endif // MOTOR_STEPPER_TMC2209
