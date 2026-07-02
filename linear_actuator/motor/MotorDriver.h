// =============================================================================
// MotorDriver.h — Abstract motor driver interface
// =============================================================================

#pragma once
#include <Arduino.h>

class MotorDriver {
public:
    virtual ~MotorDriver() {}

    // Initialize hardware. Call once in setup().
    virtual bool begin() = 0;

    // Move motor toward home until stopped externally (by feedback system).
    // Returns immediately; caller polls isMoving() or uses callback.
    virtual void startHoming() = 0;

    // Move to absolute position in steps (stepper) or mm (servo/DC use mm).
    virtual void moveTo(long targetSteps) = 0;

    // Stop motion immediately.
    virtual void stop() = 0;

    // Call this every loop() iteration.
    virtual void update() = 0;

    // True while motor is in motion.
    virtual bool isMoving() = 0;

    // Current position in steps (stepper) or arbitrary units (servo/DC).
    virtual long getPosition() = 0;

    // Set current position as zero (call after homing complete).
    virtual void setHome() = 0;

    // Enable/disable motor power. Disabled = coils de-energized.
    virtual void enable(bool on) = 0;
};
