// =============================================================================
// ControlInput.h — Abstract control input interface
// =============================================================================

#pragma once
#include <Arduino.h>

class ControlInput {
public:
    virtual ~ControlInput() {}

    // Initialize hardware. Call once in setup().
    virtual bool begin() = 0;

    // Call every loop(). Returns the currently requested stop index (0-7).
    // 0 = disabled/home. Returns -1 if no valid selection.
    virtual int readRequestedStop() = 0;

    // True if the system enable toggle is ON.
    virtual bool isEnabled() = 0;

    // Optional: called every loop() for control inputs that need background
    // processing (e.g. HTTP server polling). Default is a no-op.
    virtual void update() {}

    // Optional live-tuning accessors used by startHoming().
    // Return -1 / negative to use config.h defaults.
    virtual int   stallThreshold() const { return -1; }
    virtual float homeSpeed()      const { return -1.0f; }
};
