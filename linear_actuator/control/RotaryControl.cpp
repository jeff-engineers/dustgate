// =============================================================================
// RotaryControl.cpp
//
// Resistor ladder wiring for SP8T rotary switch (8 positions, common to GND):
//
//   Pull-up: 10kΩ from A0 to 3.3V (Feather 3V3 pin)
//   ADC: ESP32-S2, 12-bit (0–4095), 3.3V reference
//
//   Position 0 (Home/Disabled): 0Ω direct to GND → 0.00V → ADC ~0
//   Position 1: 1kΩ divider    → 0.30V → ADC ~372
//   Position 2: 2kΩ            → 0.55V → ADC ~683
//   Position 3: 3kΩ            → 0.76V → ADC ~946
//   Position 4: 4kΩ            → 0.94V → ADC ~1170
//   Position 5: 5kΩ            → 1.10V → ADC ~1365
//   Position 6: 6kΩ            → 1.24V → ADC ~1537
//   Position 7: 7kΩ            → 1.36V → ADC ~1688
//
//   IMPORTANT — ESP32 ADC non-linearity:
//   The ESP32-S2 ADC is not perfectly linear, especially below ~0.1V and
//   above ~3.1V. Position 0 (0V) may read 20–150 instead of 0 due to noise.
//   Always calibrate: open Serial Monitor, set CONTROL_SERIAL_DEBUG, type
//   'status' while rotating through each position to see raw ADC values.
//   Update ROTARY_THRESHOLDS to midpoints between your observed readings.
//
//   See WIRING.md for the full schematic.
// =============================================================================

#include "RotaryControl.h"

#ifdef CONTROL_ROTARY

// ADC midpoint thresholds between positions (12-bit scale, 3.3V reference)
// These are calculated values — calibrate to your actual board (see above).
static const int ROTARY_THRESHOLDS[8] = {
    186,   // midpoint between pos 0 (~0)    and pos 1 (~372)
    528,   // midpoint between pos 1 (~372)  and pos 2 (~683)
    815,   // midpoint between pos 2 (~683)  and pos 3 (~946)
    1058,  // midpoint between pos 3 (~946)  and pos 4 (~1170)
    1268,  // midpoint between pos 4 (~1170) and pos 5 (~1365)
    1451,  // midpoint between pos 5 (~1365) and pos 6 (~1537)
    1613,  // midpoint between pos 6 (~1537) and pos 7 (~1688)
    4095   // sentinel: anything above pos 7
};

RotaryControl::RotaryControl()
    : _lastStop(0), _lastEnabled(false)
{}

bool RotaryControl::begin() {
    // analogReadResolution(12) is called once in setup() — not repeated here
    // Toggle switch: active-low (NC to GND when ON)
    pinMode(PIN_TOGGLE, INPUT_PULLUP);

    DEBUG_PRINTLN(F("RotaryControl initialized (12-bit ADC, 3.3V ref)."));
    DEBUG_PRINTLN(F("Tip: monitor raw ADC values via Serial to calibrate thresholds."));
    return true;
}

int RotaryControl::readRequestedStop() {
    _lastStop = readRotarySwitch();
    return _lastStop;
}

bool RotaryControl::isEnabled() {
    _lastEnabled = readToggleSwitch();
    return _lastEnabled;
}

int RotaryControl::readRotarySwitch() {
    int raw = analogRead(PIN_ROTARY);

    for (int i = 0; i < 8; i++) {
        if (raw < ROTARY_THRESHOLDS[i]) {
            return i; // 0 = home/disabled, 1-7 = stops
        }
    }
    return 0; // fallback
}

bool RotaryControl::readToggleSwitch() {
    // NC toggle: LOW = enabled (switch closed), HIGH = disabled (open)
    return digitalRead(PIN_TOGGLE) == LOW;
}

#endif // CONTROL_ROTARY
