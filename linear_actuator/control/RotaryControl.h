// =============================================================================
// RotaryControl.h — 8-position SP8T rotary switch via resistor ladder (analog)
// Switch: https://www.microcenter.com/product/503961/
// Wiring: see WIRING.md for resistor values and voltage thresholds
// =============================================================================

#pragma once
#include "ControlInput.h"
#include "../config.h"

#ifdef CONTROL_ROTARY

class RotaryControl : public ControlInput {
public:
    RotaryControl();
    bool begin() override;
    int readRequestedStop() override;
    bool isEnabled() override;

private:
    int _lastStop;
    bool _lastEnabled;

    int readRotarySwitch();
    bool readToggleSwitch();
};

#endif // CONTROL_ROTARY
