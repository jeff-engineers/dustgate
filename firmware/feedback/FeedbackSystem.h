// =============================================================================
// FeedbackSystem.h — Abstract feedback/homing interface
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../motor/MotorDriver.h"

class FeedbackSystem {
public:
    virtual ~FeedbackSystem() {}

    // Initialize pins and state. Pass the motor driver for coordinated control.
    virtual bool begin(MotorDriver* motor) = 0;

    // Reset homing state before triggering a new homing sequence.
    // Must be called by startHoming() so updateHoming() re-runs from scratch.
    virtual void resetHoming() {}

    // Call every loop(). Returns true when homing is complete.
    //
    // THE IMPLEMENTATION STARTS THE SWEEP, and the caller must not (2026-08-28).
    // Homing has a phase before the sweep — getting OFF the datum switch if the
    // carriage is already on it — and only the feedback system knows whether
    // that is needed. A caller that commands motion first defeats it: on a
    // serial bus servo a new command does not cancel the one in flight, so a
    // sweep issued before the check cannot be called back, and `home` with the
    // datum made drives the carriage into it.
    //
    // So the contract is: resetHoming(), then call this every loop until it
    // returns true (or failed() goes true). Do not touch the motor in between.
    virtual bool updateHoming() = 0;

    // Call every loop() during normal motion.
    // Returns true when the target stop position has been reached.
    virtual bool updateMoving(int targetStop) = 0;

    // Compute the step target for a given stop index.
    virtual long stepsForStop(int stopIndex) = 0;
};
