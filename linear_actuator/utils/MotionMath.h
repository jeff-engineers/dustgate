// =============================================================================
// MotionMath.h — Shared unit conversion utilities
//
// g_stopPositionsMM[] is the single runtime source of truth for stop positions.
// It is populated at startup in linear_actuator.ino from either:
//   - EEPROM (if valid calibration exists), or
//   - STOP_DISTANCES_MM in config.h (fallback)
// After training completes, it is updated in-place so normal operation
// immediately uses the new values without requiring a reflash.
// =============================================================================

#pragma once
#include <Arduino.h>
#include "../config.h"

// Declared here, defined in linear_actuator.ino
extern float g_stopPositionsMM[NUM_STOPS + 1];

// Steps ↔ mm conversion using config.h gear parameters
inline float stepsPerMM() {
    return (float)(STEPS_PER_REV * MICROSTEPS) /
           ((float)PINION_TEETH * RACK_PITCH_MM);
}

inline long mmToSteps(float mm) {
    return (long)(mm * stepsPerMM());
}

inline float stepsToMM(long steps) {
    return (float)steps / stepsPerMM();
}

// Canonical step position for a given stop index, using runtime calibration
inline long stepsForStop(int stopIndex) {
    return mmToSteps(g_stopPositionsMM[stopIndex]);
}
