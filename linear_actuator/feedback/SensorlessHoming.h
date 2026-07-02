// =============================================================================
// SensorlessHoming.h — Back-EMF / StallGuard homing via TMC2209
// Only compiled when MOTOR_STEPPER_TMC2209 + FEEDBACK_SENSORLESS are selected
// =============================================================================

#pragma once
#include "FeedbackSystem.h"
#include "../config.h"

#if defined(MOTOR_STEPPER_TMC2209) && defined(FEEDBACK_SENSORLESS)

#include "../motor/StepperTMC2209Driver.h"

class SensorlessHoming : public FeedbackSystem {
public:
    SensorlessHoming();
    bool begin(MotorDriver* motor) override;
    void resetHoming();          // call before each new homing sequence
    bool updateHoming() override;
    bool updateMoving(int targetStop) override;
    long stepsForStop(int stopIndex) override;
    bool isHomeTriggered() override;
    bool isMaxTriggered() override;
    bool isHomed() override;

private:
    StepperTMC2209Driver* _motor;
    bool _homed;
    bool _stallDetected;
    unsigned long _homingStartMs;
    long _homingStartPos;

    // Minimum time before stall detection activates (let motor reach speed AND
    // let TMC2209 StallGuard velocity filter complete at least a few full
    // electrical cycles before sampling SG_RESULT)
    static const unsigned long STALL_GUARD_SETTLE_MS = 1500;
};

#endif // TMC2209 + SENSORLESS
