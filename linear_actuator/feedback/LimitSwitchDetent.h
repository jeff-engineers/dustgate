// =============================================================================
// LimitSwitchDetent.h — 2 endstops + travelling detent switch per stop
// Works with stepper, servo, and DC motors.
// NC detent switch: normally LOW (closed to GND), opens HIGH at each stop.
// =============================================================================

#pragma once
#include "FeedbackSystem.h"
#include "../config.h"

#ifdef FEEDBACK_LIMIT_DETENT

class LimitSwitchDetent : public FeedbackSystem {
public:
    LimitSwitchDetent();
    bool begin(MotorDriver* motor) override;
    bool updateHoming() override;
    bool updateMoving(int targetStop) override;
    long stepsForStop(int stopIndex) override;
    bool isHomeTriggered() override;
    bool isMaxTriggered() override;
    bool isHomed() override;

    // Returns the current detent index (0 = home, 1-7 = stops, -1 = between stops)
    int readDetentPosition();

private:
    MotorDriver* _motor;
    bool _homed;
    bool _backingOff;   // true while executing post-endstop backoff
    int _lastDetent;
    unsigned long _detentDebounceMs;

    bool readHomeSwitch();
    bool readMaxSwitch();

    // Detent switches: one per stop, wired as NC (open = HIGH when at stop)
    // Can be wired individually (PIN_DETENT_BASE + i) or as resistor ladder (A1)
    // Edit readDetentPosition() to match your wiring choice
};

#endif // FEEDBACK_LIMIT_DETENT
