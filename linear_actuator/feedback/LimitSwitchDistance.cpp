// =============================================================================
// LimitSwitchDistance.cpp
// =============================================================================

#include "LimitSwitchDistance.h"
#include "../utils/MotionMath.h"

#ifdef FEEDBACK_LIMIT_DISTANCE

LimitSwitchDistance::LimitSwitchDistance()
    : _motor(nullptr), _homed(false), _backingOff(false), _lastHomeState(false)
{}

void LimitSwitchDistance::resetHoming() {
    _homed      = false;
    _backingOff = false;
}

bool LimitSwitchDistance::begin(MotorDriver* motor) {
    _motor = motor;

    // NC switches: wire between pin and GND
    // Pin reads HIGH normally, LOW when switch opens (triggered)
    pinMode(PIN_ENDSTOP_HOME, INPUT_PULLUP);
    pinMode(PIN_ENDSTOP_MAX, INPUT_PULLUP);

    _lastHomeState = readHomeSwitch();
    DEBUG_PRINT(F("LimitSwitchDistance initialized. Home endstop: "));
    DEBUG_PRINTLN(_lastHomeState ? F("TRIGGERED") : F("open"));
    return true;
}

void LimitSwitchDistance::pollEndstopLog() {
    bool current = readHomeSwitch();
    if (current != _lastHomeState) {
        _lastHomeState = current;
        DEBUG_PRINT(F("[ENDSTOP] Home: "));
        DEBUG_PRINTLN(current ? F("TRIGGERED") : F("open"));
    }
}

bool LimitSwitchDistance::updateHoming() {
    pollEndstopLog();
    if (_homed) return true;

    if (_backingOff) {
        // Wait for backoff move to complete
        if (!_motor->isMoving()) {
            _motor->setHome();
            _homed = true;
            _backingOff = false;
            DEBUG_PRINTLN(F("Homing complete (limit switch + distance)."));
            return true;
        }
        return false;
    }

    // Driving toward home — stop when endstop triggered
    if (readHomeSwitch()) {
        _motor->stop();
        delay(20);

        // Backoff is RELATIVE to the trigger position.
        // moveTo(absolute) from an arbitrary position would crash the carriage.
        long backoffTarget = _motor->getPosition() + HOME_BACKOFF_STEPS * (-HOME_DIRECTION);
        _motor->moveTo(backoffTarget);
        _backingOff = true;

        DEBUG_PRINTLN(F("Home endstop hit, backing off..."));
    }

    return false;
}

bool LimitSwitchDistance::updateMoving(int /*targetStop*/) {
    pollEndstopLog();
    // Stop if max endstop triggered during normal motion (safety)
    if (readMaxSwitch()) {
        _motor->stop();
        DEBUG_PRINTLN(F("Max endstop triggered during motion — stopped."));
        return true; // treat as "arrived" to prevent re-trigger loop
    }

    // Motion complete when stepper reaches target
    return !_motor->isMoving();
}

long LimitSwitchDistance::stepsForStop(int stopIndex) {
    // Negate by HOME_DIRECTION: with HOME_DIRECTION=1, gates are in the negative
    // step direction from home (motor backs away from endstop toward gates).
    return ::mmToSteps(g_stopPositionsMM[stopIndex]) * (-HOME_DIRECTION);
}

bool LimitSwitchDistance::isHomeTriggered() {
    return readHomeSwitch();
}

bool LimitSwitchDistance::isMaxTriggered() {
    return readMaxSwitch();
}

bool LimitSwitchDistance::isHomed() {
    return _homed;
}

bool LimitSwitchDistance::readHomeSwitch() {
    // NC switch wired between pin and GND, INPUT_PULLUP:
    //   Normal (contacts closed): pin pulled to GND → LOW
    //   Triggered (contacts open): pullup wins → HIGH
    // Also fail-safe: broken wire → pin HIGH → reads as triggered → motor stops.
    return digitalRead(PIN_ENDSTOP_HOME) == HIGH;
}

bool LimitSwitchDistance::readMaxSwitch() {
    return digitalRead(PIN_ENDSTOP_MAX) == LOW;
}

// mmToSteps() now delegated to MotionMath.h

#endif // FEEDBACK_LIMIT_DISTANCE
