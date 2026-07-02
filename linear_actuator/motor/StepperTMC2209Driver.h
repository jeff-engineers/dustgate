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
    void update() override;
    bool isMoving() override;
    long getPosition() override;
    void setHome() override;
    void enable(bool on) override;

    // Sensorless homing: check StallGuard flag via UART
    bool isStalled();
    uint32_t getDrvStatus(); // raw DRV_STATUS register (for diagnostics)
    uint32_t getTStep();     // TSTEP register — actual step timing; 0 = motor stopped

    // Diagnostic: read GCONF, CHOPCONF, TCOOLTHRS from hardware and print to Serial.
    // Use the 'gconf' serial command to call this and verify writes are landing.
    void printDriverRegs();

    // Training mode: drive forward/reverse with StallGuard active.
    // Unlike moveTo(), these keep SpreadCycle + StallGuard enabled.
    // Call stop() when isStalled() returns true.
    void driveForwardWithStallGuard(float speedStepsPerSec);
    void driveReverseWithStallGuard(float speedStepsPerSec);

private:
    TMC2209Stepper _driver;
    AccelStepper _stepper;
    bool _homing;
    bool _enabled;
};

#endif // MOTOR_STEPPER_TMC2209
