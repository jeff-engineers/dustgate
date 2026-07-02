// =============================================================================
// RelayOutput.h — External relay control with configurable delay
// Relay ON conditions: system enabled AND actuator at a valid stop (not homing)
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

class RelayOutput {
public:
    RelayOutput();
    bool begin();

    // Call every loop() with current system state.
    // atStop: true if actuator is stationary at a valid stop position (not home/0)
    // enabled: true if toggle switch is ON
    void update(bool atStop, bool enabled);

    // Force relay off immediately (emergency stop, homing, etc.)
    void forceOff();

    bool isOn() const { return _relayOn; }

private:
    bool _relayOn;
    bool _pendingOn;
    bool _pendingOff;
    unsigned long _onRequestMs;
    unsigned long _offRequestMs;

    void setRelay(bool on);
};
