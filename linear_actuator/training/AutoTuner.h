// =============================================================================
// AutoTuner.h — StallGuard threshold binary-search auto-tuner
//
// Usage (serial commands):
//   autotune          — run at current homespeed setting
//
// Algorithm:
//   Binary-searches SGTHRS (0–254) to find the MINIMUM value that reliably
//   detects the endstop stall without false-stalling mid-travel.
//
//   Each attempt:
//     1. Drive toward endstop at test speed with current SGTHRS
//     2. Measure how far the motor traveled when stall fired
//     3. No stall after AUTOTUNE_SEARCH_MM  → SGTHRS too low  → lo = mid+1
//        Stall in first AUTOTUNE_FALSE_STALL_PCT%  → false stall → hi = mid-1
//        Stall in remaining travel                 → real stall  → record, hi = mid-1
//     4. Back off to start position and repeat
//
//   Recommends: min_working_SGTHRS + AUTOTUNE_MARGIN
//   Prints config.h snippet at completion.
//
// Prerequisites:
//   - Motor must be homed (position 0 = home endstop)
//   - Motor must be positioned within AUTOTUNE_SEARCH_MM of the home endstop
//     before begin() is called (i.e. at a positive step count ≤ search range)
// =============================================================================

#pragma once
#include "../config.h"

#ifdef MOTOR_STEPPER_TMC2209

#include "../motor/StepperTMC2209Driver.h"

class AutoTuner {
public:
    explicit AutoTuner(StepperTMC2209Driver* motor);

    // Start the tuner. testSpeed: steps/sec used for every attempt.
    void begin(float testSpeedStepsPerSec);

    // Call every loop() while STATE_AUTOTUNING.
    void update();

    bool isDone()    const { return _phase == PHASE_COMPLETE; }
    bool hasError()  const { return _phase == PHASE_ERROR; }

    // Valid after isDone() and !hasError()
    int   recommendedSGTHRS() const;
    float testedSpeed()       const { return _testSpeed; }

private:
    enum Phase {
        PHASE_IDLE,
        PHASE_SETUP_MOVE,     // move to autotune start position (away from endstop)
        PHASE_SETTLE,         // motor running — wait for it to reach speed
        PHASE_ATTEMPT,        // check for stall / travel limit
        PHASE_BACK_OFF,       // return to start position
        PHASE_BACK_OFF_PAUSE, // brief pause before next attempt
        PHASE_COMPLETE,
        PHASE_ERROR
    };

    StepperTMC2209Driver* _motor;
    Phase         _phase;
    unsigned long _phaseMs;

    float _testSpeed;
    int   _lo, _hi, _mid;
    int   _bestSGTHRS;   // lowest SGTHRS confirmed to produce a real stall
    int   _attemptCount;

    long _startPos;           // position at start of each attempt (set in startAttempt)
    long _maxTravelSteps;     // AUTOTUNE_SEARCH_MM in steps
    long _falseStallSteps;    // travel below this threshold = false stall

    void enterPhase(Phase p);
    void startAttempt();
    void evaluateAttempt(long stepsTraveled, bool stalled);
    void printResults() const;
};

#endif // MOTOR_STEPPER_TMC2209
