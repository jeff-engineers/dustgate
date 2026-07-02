// =============================================================================
// LimitSwitchDistance.h — 2 endstops + rotation distance for stop positions
// Stepper motors only (requires step counting)
// =============================================================================

#pragma once
#include "FeedbackSystem.h"
#include "../config.h"

#ifdef FEEDBACK_LIMIT_DISTANCE

class LimitSwitchDistance : public FeedbackSystem {
public:
    LimitSwitchDistance();
    bool begin(MotorDriver* motor) override;
    void resetHoming() override;
    bool updateHoming() override;
    bool updateMoving(int targetStop) override;
    long stepsForStop(int stopIndex) override;
    bool isHomeTriggered() override;
    bool isMaxTriggered() override;
    bool isHomed() override;

private:
    MotorDriver* _motor;
    bool _homed;
    bool _backingOff;

    bool readHomeSwitch();
    bool readMaxSwitch();
    void pollEndstopLog();

    bool _lastHomeState;
};

#endif // FEEDBACK_LIMIT_DISTANCE
