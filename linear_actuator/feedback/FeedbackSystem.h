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
    virtual bool updateHoming() = 0;

    // Call every loop() during normal motion.
    // Returns true when the target stop position has been reached.
    virtual bool updateMoving(int targetStop) = 0;

    // Compute the step target for a given stop index.
    virtual long stepsForStop(int stopIndex) = 0;
};
