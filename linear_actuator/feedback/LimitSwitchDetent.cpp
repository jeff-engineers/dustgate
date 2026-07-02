// =============================================================================
// LimitSwitchDetent.cpp
// =============================================================================

#include "LimitSwitchDetent.h"

#ifdef FEEDBACK_LIMIT_DETENT

// Debounce time for detent switch reading
static const unsigned long DETENT_DEBOUNCE_MS = 50;

LimitSwitchDetent::LimitSwitchDetent()
    : _motor(nullptr), _homed(false), _backingOff(false),
      _lastDetent(-1), _detentDebounceMs(0)
{}

bool LimitSwitchDetent::begin(MotorDriver* motor) {
    _motor = motor;

    pinMode(PIN_ENDSTOP_HOME, INPUT_PULLUP);
    pinMode(PIN_ENDSTOP_MAX, INPUT_PULLUP);

    // --- Option A: Individual pins (one per detent stop) ---
    // Uses Mega pins 22–28. For Nano, see Option B below.
    for (int i = 0; i < NUM_STOPS; i++) {
        pinMode(PIN_DETENT_BASE + i, INPUT_PULLUP);
    }

    // --- Option B: Resistor ladder on analog pin (Nano-friendly) ---
    // Uncomment this block and comment out Option A above.
    // See WIRING.md for resistor values.
    // pinMode(A1, INPUT);  // Analog read in readDetentPosition()

    DEBUG_PRINTLN(F("LimitSwitchDetent initialized."));
    return true;
}

bool LimitSwitchDetent::updateHoming() {
    if (_homed) return true;

    if (_backingOff) {
        // Wait for backoff move to complete, then zero position
        if (!_motor->isMoving()) {
            _motor->setHome();
            _homed = true;
            _backingOff = false;
            _lastDetent = 0;
            DEBUG_PRINTLN(F("Homing complete (detent mode)."));
            return true;
        }
        return false;
    }

    // Driving toward home — stop when endstop triggers
    if (readHomeSwitch()) {
        _motor->stop();
        delay(20);
        // Back off so the switch is no longer held open
        long backoffTarget = HOME_BACKOFF_STEPS * (-HOME_DIRECTION);
        _motor->moveTo(backoffTarget);
        _backingOff = true;
        DEBUG_PRINTLN(F("Home endstop hit, backing off..."));
    }

    return false;
}

bool LimitSwitchDetent::updateMoving(int targetStop) {
    // Safety: stop at max endstop regardless of target
    if (readMaxSwitch()) {
        _motor->stop();
        DEBUG_PRINTLN(F("Max endstop hit during detent travel."));
        return true;
    }

    // Check if current detent matches target
    int current = readDetentPosition();
    if (current == targetStop) {
        _motor->stop();
        _lastDetent = current;
        DEBUG_PRINT(F("Arrived at stop: "));
        DEBUG_PRINTLN(current);
        return true;
    }

    return false;
}

long LimitSwitchDetent::stepsForStop(int /*stopIndex*/) {
    // Detent mode doesn't use step-based positioning
    // Motor runs until detent switch triggers; return 0
    return 0L;
}

bool LimitSwitchDetent::isHomeTriggered() {
    return readHomeSwitch();
}

bool LimitSwitchDetent::isMaxTriggered() {
    return readMaxSwitch();
}

bool LimitSwitchDetent::isHomed() {
    return _homed;
}

int LimitSwitchDetent::readDetentPosition() {
    // --- Option A: Individual digital pins ---
    // Home position: check home endstop
    if (readHomeSwitch()) return 0;

    for (int i = 0; i < NUM_STOPS; i++) {
        // NC switch opens (pin goes HIGH) when at detent position
        if (digitalRead(PIN_DETENT_BASE + i) == HIGH) {
            return i + 1; // stops are 1-indexed
        }
    }
    return -1; // between stops

    // --- Option B: Resistor ladder on A1 ---
    // Each detent stop connects to a different resistor divider, producing
    // a unique analog voltage. Thresholds below assume 5V supply and
    // resistor values from WIRING.md.
    //
    // int raw = analogRead(A1);
    // if (raw < 50)  return 0; // home (switch to GND directly)
    // if (raw < 180) return 1;
    // if (raw < 310) return 2;
    // if (raw < 440) return 3;
    // if (raw < 570) return 4;
    // if (raw < 700) return 5;
    // if (raw < 830) return 6;
    // if (raw < 960) return 7;
    // return -1; // no switch triggered (between stops)
}

bool LimitSwitchDetent::readHomeSwitch() {
    return digitalRead(PIN_ENDSTOP_HOME) == LOW; // NC: LOW = triggered
}

bool LimitSwitchDetent::readMaxSwitch() {
    return digitalRead(PIN_ENDSTOP_MAX) == LOW;
}

#endif // FEEDBACK_LIMIT_DETENT
