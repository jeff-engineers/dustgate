// =============================================================================
// LimitSwitchDistance.cpp
// =============================================================================

#include "LimitSwitchDistance.h"
#include "../utils/MotionMath.h"

#ifdef FEEDBACK_LIMIT_DISTANCE

LimitSwitchDistance::LimitSwitchDistance()
    : _motor(nullptr), _homed(false), _backingOff(false),
      _lastHomeState(false), _lastMaxState(false)
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
    _lastMaxState  = readMaxSwitch();
    DEBUG_PRINT(F("LimitSwitchDistance initialized. Home: "));
    DEBUG_PRINT(_lastHomeState ? F("TRIGGERED") : F("open"));
    DEBUG_PRINT(F("  Far: "));
    DEBUG_PRINTLN(_lastMaxState ? F("TRIGGERED") : F("open"));
    return true;
}

void LimitSwitchDistance::pollEndstopLog() {
    // Endstop transition logging now lives in the main loop's always-on endstop
    // supervisor (linear_actuator.ino) so it also covers jogs, which never enter
    // STATE_MOVING. Kept as a no-op to preserve the call sites in
    // updateHoming()/updateMoving() without double-printing.
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
    // Same NC + INPUT_PULLUP wiring as the home switch: triggered (contacts
    // open) → pin pulled HIGH; also fail-safe (broken wire → HIGH → triggered).
    // (Was `== LOW`, which is inverted — it only appeared to work while no max
    // switch was installed and the floating pin read HIGH. With a real NC switch
    // that inversion reads "triggered" whenever the switch is closed/normal.)
    return digitalRead(PIN_ENDSTOP_MAX) == HIGH;
}

// mmToSteps() now delegated to MotionMath.h

#endif // FEEDBACK_LIMIT_DISTANCE
