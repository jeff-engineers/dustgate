// =============================================================================
// RelayOutput.cpp
// =============================================================================

#include "RelayOutput.h"

RelayOutput::RelayOutput()
    : _relayOn(false), _pendingOn(false), _pendingOff(false),
      _onRequestMs(0), _offRequestMs(0)
{}

bool RelayOutput::begin() {
    pinMode(PIN_RELAY, OUTPUT);
    setRelay(false);
    DEBUG_PRINTLN(F("RelayOutput initialized."));
    return true;
}

void RelayOutput::update(bool atStop, bool enabled) {
    bool shouldBeOn = atStop && enabled;

    if (shouldBeOn && !_relayOn) {
        // Request relay ON
        if (!_pendingOn) {
            _pendingOn = true;
            _pendingOff = false;
            _onRequestMs = millis();
        }
        // Apply ON delay
        if (millis() - _onRequestMs >= RELAY_ON_DELAY_MS) {
            setRelay(true);
            _pendingOn = false;
        }
    }
    else if (!shouldBeOn && _relayOn) {
        // Request relay OFF
        if (!_pendingOff) {
            _pendingOff = true;
            _pendingOn = false;
            _offRequestMs = millis();
        }
        // Apply OFF delay
        if (millis() - _offRequestMs >= RELAY_OFF_DELAY_MS) {
            setRelay(false);
            _pendingOff = false;
        }
    }
    else if (shouldBeOn && _pendingOff) {
        // Condition came back true before off-delay expired — cancel pending off
        _pendingOff = false;
    }
    else if (!shouldBeOn && _pendingOn) {
        // Condition went false before on-delay expired — cancel pending on
        _pendingOn = false;
    }
}

void RelayOutput::forceOff() {
    _pendingOn = false;
    _pendingOff = false;
    setRelay(false);
}

void RelayOutput::setRelay(bool on) {
    bool pinState = RELAY_ACTIVE_HIGH ? on : !on;
    digitalWrite(PIN_RELAY, pinState ? HIGH : LOW);

    if (on != _relayOn) {
        _relayOn = on;
        DEBUG_PRINT(F("Relay: "));
        DEBUG_PRINTLN(on ? F("ON") : F("OFF"));
    } else {
        _relayOn = on;
    }
}
