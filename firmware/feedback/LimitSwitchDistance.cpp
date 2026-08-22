// =============================================================================
// LimitSwitchDistance.cpp
// =============================================================================

#include "LimitSwitchDistance.h"
#include "../utils/MotionMath.h"

#ifdef FEEDBACK_LIMIT_DISTANCE

LimitSwitchDistance::LimitSwitchDistance()
    : _motor(nullptr), _homed(false), _backingOff(false)
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

    DEBUG_PRINT(F("LimitSwitchDistance initialized. D10: "));
    DEBUG_PRINT(readHomeSwitch() ? F("TRIGGERED") : F("open"));
    DEBUG_PRINT(F("  D11: "));
    DEBUG_PRINTLN(readMaxSwitch() ? F("TRIGGERED") : F("open"));
    return true;
}

bool LimitSwitchDistance::updateHoming() {
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

    // Driving toward the HOME DATUM (the user's-left endstop — D10 or D11 depending
    // on g_homeIsMaxEndstop). Stop when that switch triggers. The far switch firing
    // instead means the motor is wired backwards; that's detected in the main loop's
    // homing state, not here.
    bool datumTriggered = g_homeIsMaxEndstop ? readMaxSwitch() : readHomeSwitch();
    if (datumTriggered) {
        _motor->stop();
        delay(20);

        // Backoff is RELATIVE to the trigger position.
        // moveTo(absolute) from an arbitrary position would crash the carriage.
        long backoffTarget = _motor->getPosition() + HOME_BACKOFF_STEPS * (-HOME_DIRECTION);
        _motor->moveTo(backoffTarget);
        _backingOff = true;

        DEBUG_PRINTLN(F("Home datum endstop hit, backing off..."));
    }

    return false;
}

bool LimitSwitchDistance::updateMoving(int /*targetStop*/) {
    // Over-travel is enforced directionally by the main-loop endstop supervisor
    // (stops travel INTO a triggered switch, allows travel away — needed so a
    // move can leave the far switch, e.g. returning home after a sweep). Here we
    // only report arrival: true once the stepper has reached target or been
    // stopped.
    return !_motor->isMoving();
}

long LimitSwitchDistance::stepsForStop(int stopIndex) {
    // Negate by HOME_DIRECTION: with HOME_DIRECTION=1, gates are in the negative
    // step direction from home (motor backs away from endstop toward gates).
    return ::mmToSteps(g_stopPositionsMM[stopIndex]) * (-HOME_DIRECTION);
}

bool LimitSwitchDistance::readHomeSwitch() {
    // NC switch wired between pin and GND, INPUT_PULLUP:
    //   Normal (contacts closed): pin pulled to GND → LOW
    //   Triggered (contacts open): pullup wins → HIGH
    // Also fail-safe: broken wire → pin HIGH → reads as triggered → motor stops.
    return digitalRead(PIN_ENDSTOP_HOME) == HIGH;
}

bool LimitSwitchDistance::readMaxSwitch() {
    // Same NC + INPUT_PULLUP wiring as the home switch: triggered (contacts
    // open) → pin pulled HIGH; also fail-safe (broken wire → HIGH → triggered).
    // (Was `== LOW`, which is inverted — it only appeared to work while no max
    // switch was installed and the floating pin read HIGH. With a real NC switch
    // that inversion reads "triggered" whenever the switch is closed/normal.)
    return digitalRead(PIN_ENDSTOP_MAX) == HIGH;
}

// mmToSteps() now delegated to MotionMath.h

#endif // FEEDBACK_LIMIT_DISTANCE
