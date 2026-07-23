// =============================================================================
// StepperTMC2209Driver.h — Stepper via TMC2209 (STEP/DIR + UART)
// Requires: TMCStepper library, AccelStepper library
// =============================================================================

#pragma once
#include "MotorDriver.h"
#include "../config.h"

#ifdef MOTOR_STEPPER_TMC2209

#include <TMCStepper.h>
#include <AccelStepper.h>

class StepperTMC2209Driver : public MotorDriver {
public:
    StepperTMC2209Driver();
    bool begin() override;
    void startHoming() override;
    void startHomingWithParams(float speedStepsPerSec, uint8_t stallThreshold);
    void moveTo(long targetSteps) override;
    void stop() override;

    // Override the AccelStepper max speed for subsequent moveTo() moves. Used to
    // run the calibration reference sweep at the gentler homing speed. Pass
    // MAX_SPEED_STEPS_PER_SEC to restore normal move speed.
    void setMaxSpeed(float speedStepsPerSec);
    void update() override;
    bool isMoving() override;
    long getPosition() override;
    void setHome() override;
    void enable(bool on) override;

    // Signed steps remaining to the current target (sign = direction of travel).
    // Used by the main-loop endstop supervisor to allow backing off a switch.
    long distanceToGo();

    // Diagnostic: read GCONF, CHOPCONF, TCOOLTHRS from hardware and print to Serial.
    // Use the 'gconf' serial command to call this and verify writes are landing.
    void printDriverRegs();

private:
    TMC2209Stepper _driver;
    AccelStepper _stepper;
    bool _homing;
    bool _enabled;
};

#endif // MOTOR_STEPPER_TMC2209
